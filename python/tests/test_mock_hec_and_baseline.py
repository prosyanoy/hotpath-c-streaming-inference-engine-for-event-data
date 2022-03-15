import json
import threading
import urllib.request
import urllib.error

import pytest

from hotpath import baseline, mock_hec, train


@pytest.fixture()
def hec():
    server, state = mock_hec.make_server("127.0.0.1", 0, "tok")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_address[1]}", state
    server.shutdown(); server.server_close()


def post(url, body, token="tok"):
    req = urllib.request.Request(url + "/services/collector/event", data=body,
                                 headers={"Authorization": f"Splunk {token}"})
    return urllib.request.urlopen(req)


def test_parse_concatenated_json():
    assert mock_hec.parse_concatenated_json(b'{"event":1}{"event":2}\n {"event":3}') == [
        {"event": 1}, {"event": 2}, {"event": 3}]
    with pytest.raises(ValueError):
        mock_hec.parse_concatenated_json(b"{not json")


def test_hec_accepts_batches_and_rejects_bad_requests(hec):
    url, state = hec
    assert post(url, b'{"event":{"a":1}}{"event":{"a":2}}').status == 200
    assert state.events_received == 2
    with pytest.raises(urllib.error.HTTPError) as e:
        post(url, b'{"event":1}', token="wrong")
    assert e.value.code == 401
    with pytest.raises(urllib.error.HTTPError) as e:
        post(url, b'{"nope":1}')
    assert e.value.code == 400
    stats = json.load(urllib.request.urlopen(url + "/stats"))
    assert stats["events_received"] == 2 and stats["rejected"] == 2


def test_baseline_end_to_end(hec, tmp_path, monkeypatch):
    url, state = hec
    model, threshold = train.train(seed=5, n_train=10000, n_estimators=30)
    path = train.export(model, threshold, tmp_path)
    for k, v in {"HOTPATH_MODEL": str(path), "HOTPATH_TOTAL_EVENTS": "5000",
                 "HOTPATH_HEC_URL": url + "/services/collector/event",
                 "HOTPATH_HEC_TOKEN": "tok", "HOTPATH_ANOMALY_RATE": "0.05"}.items():
        monkeypatch.setenv(k, v)
    summary = baseline.Service(baseline.Config()).run()
    assert summary["events"] == 5000
    assert summary["anomalies"] > 100
    assert summary["alerts_sent"] == summary["anomalies"] == state.events_received
    assert summary["latency_us"]["p99"] >= summary["latency_us"]["p50"] > 0
