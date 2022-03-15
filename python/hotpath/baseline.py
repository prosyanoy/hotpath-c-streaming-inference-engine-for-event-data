"""Python reference implementation of the HotPath pipeline, used as the benchmark baseline.

Same stages as the C++ service (ingest -> batch -> ONNX inference -> HEC sink), same model,
same synthetic distribution and the same HOTPATH_* environment variables. It is written the way
a typical Python service would be: threads joined by ``queue.Queue`` and numpy batches.
"""
from __future__ import annotations

import http.client
import json
import os
import queue
import random
import threading
import time
from urllib.parse import urlparse

import numpy as np
import onnxruntime as ort

from . import synthetic

_STOP = object()


def read_meta(path: str) -> dict:
    meta = {}
    for line in open(path):
        if "=" in line:
            k, v = line.strip().split("=", 1)
            meta[k] = v
    return meta


def env(name: str, default, cast=str):
    v = os.environ.get(name)
    return cast(v) if v not in (None, "") else default


class Config:
    def __init__(self):
        self.model = env("HOTPATH_MODEL", "models/anomaly.onnx")
        self.meta = env("HOTPATH_MODEL_META", self.model.rsplit(".", 1)[0] + ".meta")
        self.rate = env("HOTPATH_RATE", 0, float)  # events/sec, 0 = unbounded
        self.total = env("HOTPATH_TOTAL_EVENTS", 200_000, int)
        self.batch_size = env("HOTPATH_BATCH_SIZE", 256, int)
        self.batch_timeout_us = env("HOTPATH_BATCH_TIMEOUT_US", 2000, int)
        self.workers = env("HOTPATH_WORKERS", 2, int)
        self.ring_capacity = env("HOTPATH_RING_CAPACITY", 65536, int)
        self.anomaly_rate = env("HOTPATH_ANOMALY_RATE", 0.01, float)
        self.seed = env("HOTPATH_SEED", 42, int)
        self.hec_url = env("HOTPATH_HEC_URL", "http://127.0.0.1:8088/services/collector/event")
        self.hec_token = env("HOTPATH_HEC_TOKEN", "dev-token-not-secret")
        self.hec_batch = env("HOTPATH_HEC_BATCH", 100, int)
        self.hec_flush_ms = env("HOTPATH_HEC_FLUSH_MS", 200, int)


class Generator:
    """Pure-python per-event generator mirroring ``hotpath.synthetic``."""

    def __init__(self, seed: int, anomaly_rate: float):
        self.rng = random.Random(seed)
        self.anomaly_rate = anomaly_rate
        self.mean = synthetic.MEAN.tolist()
        self.std = synthetic.STD.tolist()
        self.lo = synthetic.LOWER.tolist()
        self.hi = synthetic.UPPER.tolist()

    def next(self) -> list:
        g = self.rng.gauss
        v = [g(m, s) for m, s in zip(self.mean, self.std)]
        if self.rng.random() < self.anomaly_rate:
            k = self.rng.randint(1, synthetic.ANOMALY_MAX_FEATURES)
            for c in self.rng.sample(range(synthetic.N_FEATURES), k):
                sigma = self.rng.uniform(synthetic.ANOMALY_MIN_SIGMA, synthetic.ANOMALY_MAX_SIGMA)
                sign = self.rng.choice((-1.0, 1.0)) if bool(synthetic.CAN_SHIFT_DOWN[c]) else 1.0
                v[c] = self.mean[c] + sign * sigma * self.std[c]
        return [min(max(x, lo), hi) for x, lo, hi in zip(v, self.lo, self.hi)]


class Service:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        meta = read_meta(cfg.meta)
        self.threshold = float(meta["threshold"])
        self.input_name = meta["input_name"]
        self.score_output = meta["score_output"]
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 1
        opts.inter_op_num_threads = 1
        self.session = ort.InferenceSession(cfg.model, opts, providers=["CPUExecutionProvider"])
        self.events_q: queue.Queue = queue.Queue(maxsize=cfg.ring_capacity)
        self.batch_q: queue.Queue = queue.Queue(maxsize=64)
        self.alert_q: queue.Queue = queue.Queue()
        self.latencies: list = []
        self.lat_lock = threading.Lock()
        self.scored = 0
        self.anomalies = 0
        self.alerts_sent = 0
        self.alerts_failed = 0
        self.first_ns = 0
        self.last_ns = 0

    # --- stages -----------------------------------------------------------------------------
    def _ingest(self):
        cfg = self.cfg
        gen = Generator(cfg.seed, cfg.anomaly_rate)
        interval = 1.0 / cfg.rate if cfg.rate > 0 else 0.0
        start = time.perf_counter()
        self.first_ns = time.perf_counter_ns()
        for i in range(cfg.total):
            if interval:
                delay = start + i * interval - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
            self.events_q.put((i, time.perf_counter_ns(), gen.next()))
        self.events_q.put(_STOP)

    def _batch(self):
        cfg = self.cfg
        timeout = cfg.batch_timeout_us / 1e6
        ids, ts, rows = [], [], []
        deadline = None
        while True:
            wait = None if deadline is None else max(deadline - time.perf_counter(), 0)
            try:
                item = self.events_q.get(timeout=wait) if wait is not None else self.events_q.get()
            except queue.Empty:
                item = None
            if item is _STOP:
                if rows:
                    self.batch_q.put((ids, ts, rows))
                break
            if item is not None:
                if not rows:
                    deadline = time.perf_counter() + timeout
                ids.append(item[0]); ts.append(item[1]); rows.append(item[2])
            if rows and (len(rows) >= cfg.batch_size or time.perf_counter() >= deadline):
                self.batch_q.put((ids, ts, rows))
                ids, ts, rows, deadline = [], [], [], None
        for _ in range(cfg.workers):
            self.batch_q.put(_STOP)

    def _worker(self):
        while True:
            item = self.batch_q.get()
            if item is _STOP:
                break
            ids, ts, rows = item
            x = np.asarray(rows, dtype=np.float32)
            scores = self.session.run([self.score_output], {self.input_name: x})[0].ravel()
            now = time.perf_counter_ns()
            lat = [now - t for t in ts]
            hits = np.flatnonzero(scores < self.threshold)
            with self.lat_lock:
                self.latencies.extend(lat)
                self.scored += len(ids)
                self.anomalies += len(hits)
                self.last_ns = max(self.last_ns, now)
            for h in hits:
                self.alert_q.put((ids[h], time.time(), float(scores[h]), rows[h]))

    def _sink(self):
        cfg = self.cfg
        url = urlparse(cfg.hec_url)
        conn = None
        buf: list = []
        last = time.monotonic()

        def flush():
            nonlocal conn, buf
            if not buf:
                return
            body = "".join(
                json.dumps({
                    "time": round(t, 3), "host": f"host-{eid % 64}", "source": "hotpath-baseline",
                    "sourcetype": "hotpath:anomaly", "index": "main",
                    "event": {"id": eid, "score": round(s, 6),
                              "features": dict(zip(synthetic.FEATURES, (round(x, 4) for x in f)))},
                }) for eid, t, s, f in buf).encode()
            for attempt in range(3):
                try:
                    if conn is None:
                        conn = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=5)
                    conn.request("POST", url.path or "/", body,
                                 {"Authorization": f"Splunk {cfg.hec_token}",
                                  "Content-Type": "application/json"})
                    resp = conn.getresponse(); resp.read()
                    if resp.status == 200:
                        self.alerts_sent += len(buf)
                        buf = []
                        return
                except OSError:
                    pass
                conn = None
                time.sleep(0.05 * (attempt + 1))
            self.alerts_failed += len(buf)
            buf = []

        while True:
            try:
                item = self.alert_q.get(timeout=cfg.hec_flush_ms / 1000)
            except queue.Empty:
                item = None
            if item is _STOP:
                break
            if item is not None:
                buf.append(item)
            if len(buf) >= cfg.hec_batch or (buf and time.monotonic() - last >= cfg.hec_flush_ms / 1000):
                flush(); last = time.monotonic()
        flush()

    # --- driver -----------------------------------------------------------------------------
    def run(self) -> dict:
        cfg = self.cfg
        sink = threading.Thread(target=self._sink)
        workers = [threading.Thread(target=self._worker) for _ in range(cfg.workers)]
        batcher = threading.Thread(target=self._batch)
        ingest = threading.Thread(target=self._ingest)
        for t in (sink, *workers, batcher, ingest):
            t.start()
        ingest.join(); batcher.join()
        for w in workers:
            w.join()
        elapsed = (self.last_ns - self.first_ns) / 1e9
        self.alert_q.put(_STOP)
        sink.join()
        lat = np.asarray(self.latencies, dtype=np.float64) / 1e3
        return {
            "impl": "python",
            "events": self.scored,
            "elapsed_s": elapsed,
            "events_per_sec": self.scored / elapsed if elapsed > 0 else 0.0,
            "latency_us": {
                "p50": float(np.percentile(lat, 50)),
                "p99": float(np.percentile(lat, 99)),
                "max": float(lat.max()),
            },
            "anomalies": self.anomalies,
            "alerts_sent": self.alerts_sent,
            "alerts_failed": self.alerts_failed,
        }


def main():
    summary = Service(Config()).run()
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
