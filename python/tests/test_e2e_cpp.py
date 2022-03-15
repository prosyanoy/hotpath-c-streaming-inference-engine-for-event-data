"""End-to-end: the real C++ binary + real ONNX model + mock HEC receiver (skipped until built)."""
import json
import os
import signal
import subprocess
import threading
import time
import urllib.request
from pathlib import Path

import pytest

from hotpath import mock_hec, train

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "build" / "hotpath"
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="build the C++ engine first (make build)")


@pytest.fixture(scope="module")
def model_dir(tmp_path_factory):
    d = tmp_path_factory.mktemp("model")
    model, threshold = train.train(seed=11, n_train=15000, n_estimators=50)
    train.export(model, threshold, d)
    return d


@pytest.fixture()
def hec():
    server, state = mock_hec.make_server("127.0.0.1", 0, "e2e-token")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_address[1]}/services/collector/event", state
    server.shutdown(); server.server_close()


def env_for(model_dir, hec_url, **extra):
    return {**os.environ, "HOTPATH_MODEL": str(model_dir / "anomaly.onnx"), "HOTPATH_HEC_URL": hec_url,
            "HOTPATH_HEC_TOKEN": "e2e-token", "HOTPATH_METRICS_PORT": "0", **extra}


def test_finite_run_delivers_every_anomaly_to_hec(model_dir, hec):
    url, state = hec
    out = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120, check=True,
                         env=env_for(model_dir, url, HOTPATH_RATE="0", HOTPATH_TOTAL_EVENTS="30000",
                                     HOTPATH_ANOMALY_RATE="0.02"))
    summary = json.loads(out.stdout.strip().splitlines()[-1])
    assert summary["events"] == 30000
    assert 300 < summary["anomalies"] < 1500
    assert summary["alerts_sent"] == summary["anomalies"] == state.events_received
    assert summary["alerts_failed"] == 0
    ev = state.recent[-1]
    assert ev["sourcetype"] == "hotpath:anomaly" and set(ev["event"]["features"]) >= {"cpu_pct", "conn_count"}


def test_live_run_serves_metrics_and_stops_on_sigterm(model_dir, hec):
    url, state = hec
    port = 19255
    proc = subprocess.Popen([str(BINARY)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                            env=env_for(model_dir, url, HOTPATH_RATE="3000", HOTPATH_METRICS_PORT=str(port),
                                        HOTPATH_METRICS_BIND="127.0.0.1", HOTPATH_ANOMALY_RATE="0.05"))
    try:
        body = ""
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                body = urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=2).read().decode()
                scored = [l for l in body.splitlines() if l.startswith("hotpath_events_scored_total ")]
                if scored and float(scored[0].split()[1]) > 500:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        assert "hotpath_events_scored_total" in body and "hotpath_event_latency_seconds_bucket" in body
        assert urllib.request.urlopen(f"http://127.0.0.1:{port}/readyz", timeout=2).status == 200
        proc.send_signal(signal.SIGTERM)
        out, _ = proc.communicate(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert proc.returncode == 0
    assert json.loads(out.strip().splitlines()[-1])["events"] > 500
    assert state.events_received > 0
