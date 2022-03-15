#!/usr/bin/env python3
"""Benchmark the C++ engine against the equivalent Python service.

Runs both implementations through the same pipeline (synthetic ingest -> batch -> ONNX -> HEC POST to a
local mock receiver), same model, same batch size and worker count:

  * max-throughput: unbounded ingest, N events  -> events/sec (latency is queue-dominated here)
  * paced:          fixed ingest rate           -> p50/p99 end-to-end latency at a sustainable load
  * raw inference:  ONNX Runtime on one batch   -> C++ (Google Benchmark) vs Python (timeit)

Usage: python bench/compare.py [--events 300000] [--paced-rate 20000] [--repeats 3]
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

import numpy as np  # noqa: E402
import onnxruntime as ort  # noqa: E402

from hotpath import mock_hec, synthetic  # noqa: E402


def run_impl(impl: str, env_extra: dict, build_dir: Path) -> dict:
    env = {**os.environ, **env_extra}
    if impl == "cpp":
        cmd = [str(build_dir / "hotpath")]
    else:
        cmd = [sys.executable, "-m", "hotpath.baseline"]
        env["PYTHONPATH"] = str(ROOT / "python")
    out = subprocess.run(cmd, env=env, cwd=ROOT, capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        raise RuntimeError(f"{impl} failed:\n{out.stderr}")
    return json.loads(out.stdout.strip().splitlines()[-1])


def median_run(impl, env, build_dir, repeats):
    runs = [run_impl(impl, env, build_dir) for _ in range(repeats)]
    pick = lambda f: statistics.median(f(r) for r in runs)  # noqa: E731
    return {
        "events": runs[0]["events"],
        "events_per_sec": pick(lambda r: r["events_per_sec"]),
        "p50_us": pick(lambda r: r["latency_us"]["p50"]),
        "p99_us": pick(lambda r: r["latency_us"]["p99"]),
        "alerts_sent": runs[0]["alerts_sent"],
    }


def python_inference(model: str, batch: int, seconds: float = 1.5) -> float:
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 1
    opts.inter_op_num_threads = 1
    sess = ort.InferenceSession(model, opts, providers=["CPUExecutionProvider"])
    x, _ = synthetic.generate(batch, 0.01, np.random.default_rng(1))
    for _ in range(20):
        sess.run(["scores"], {"X": x})
    n, t0 = 0, time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        sess.run(["scores"], {"X": x})
        n += 1
    return n * batch / (time.perf_counter() - t0)


def cpp_inference(build_dir: Path) -> dict:
    out = subprocess.run([str(build_dir / "bench" / "hotpath_bench"), "--benchmark_filter=BM_Inference",
                          "--benchmark_format=json"], capture_output=True, text=True, check=True, cwd=ROOT)
    res = {}
    for b in json.loads(out.stdout)["benchmarks"]:
        res[int(b["name"].split("/")[1])] = b["items_per_second"]
    return res


def table(rows, headers):
    w = [max(len(str(r[i])) for r in [headers, *rows]) for i in range(len(headers))]
    line = lambda r: "| " + " | ".join(str(c).ljust(w[i]) for i, c in enumerate(r)) + " |"  # noqa: E731
    return "\n".join([line(headers), "|" + "|".join("-" * (x + 2) for x in w) + "|", *map(line, rows)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build")
    ap.add_argument("--model", default=str(ROOT / "models" / "anomaly.onnx"))
    ap.add_argument("--events", type=int, default=300_000, help="events per max-throughput run")
    ap.add_argument("--paced-rate", type=int, default=40_000, help="events/sec for the latency run")
    ap.add_argument("--paced-seconds", type=int, default=10)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results" / "latest.json")
    args = ap.parse_args()

    server, state = mock_hec.make_server("127.0.0.1", 0, "bench-token")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    common = {
        "HOTPATH_MODEL": args.model,
        "HOTPATH_HEC_URL": f"http://127.0.0.1:{server.server_address[1]}/services/collector/event",
        "HOTPATH_HEC_TOKEN": "bench-token",
        "HOTPATH_WORKERS": str(args.workers),
        "HOTPATH_BATCH_SIZE": str(args.batch_size),
        "HOTPATH_METRICS_PORT": "0",
        "HOTPATH_ANOMALY_RATE": "0.01",
    }
    results: dict = {"config": vars(args) | {"build_dir": str(args.build_dir), "out": str(args.out)}, "cpus": os.cpu_count()}
    for mode, extra in (
        ("max_throughput", {"HOTPATH_RATE": "0", "HOTPATH_TOTAL_EVENTS": str(args.events)}),
        ("paced", {"HOTPATH_RATE": str(args.paced_rate),
                   "HOTPATH_TOTAL_EVENTS": str(args.paced_rate * args.paced_seconds)}),
    ):
        results[mode] = {}
        for impl in ("cpp", "python"):
            print(f"[{mode}] running {impl} x{args.repeats} ...", flush=True)
            results[mode][impl] = median_run(impl, {**common, **extra}, args.build_dir, args.repeats)

    print("[raw inference] ...", flush=True)
    cpp = cpp_inference(args.build_dir)
    results["inference_rows_per_sec"] = {
        str(b): {"cpp": cpp[b], "python": python_inference(args.model, b)} for b in sorted(cpp)}
    server.shutdown()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(results, indent=2, default=str))

    def ratio(a, b):
        return f"{a / b:.1f}x"

    mt = results["max_throughput"]
    print(f"\n### Max throughput ({args.events:,} events, {args.workers} workers, batch {args.batch_size}, {os.cpu_count()} CPUs)\n")
    print(table([[i, f"{mt[i]['events_per_sec']:,.0f}", f"{mt[i]['p99_us'] / 1e3:,.1f}"] for i in ("cpp", "python")],
                ["impl", "events/sec", "p99 latency ms (saturated)"]))
    print(f"\nC++ / Python throughput: {ratio(mt['cpp']['events_per_sec'], mt['python']['events_per_sec'])}")
    pc = results["paced"]
    print(f"\n### Paced at {args.paced_rate:,} events/sec for {args.paced_seconds}s\n")
    print(table([[i, f"{pc[i]['events_per_sec']:,.0f}", f"{pc[i]['p50_us']:,.0f}", f"{pc[i]['p99_us']:,.0f}"] for i in ("cpp", "python")],
                ["impl", "events/sec", "p50 latency us", "p99 latency us"]))
    print(f"\nC++ p99 is {ratio(pc['python']['p99_us'], pc['cpp']['p99_us'])} lower than Python.")
    print("\n### Raw ONNX Runtime inference (rows/sec, 1 thread)\n")
    print(table([[b, f"{v['cpp']:,.0f}", f"{v['python']:,.0f}"] for b, v in results["inference_rows_per_sec"].items()],
                ["batch", "C++", "Python"]))
    print(f"\nraw results: {args.out}")


if __name__ == "__main__":
    main()
