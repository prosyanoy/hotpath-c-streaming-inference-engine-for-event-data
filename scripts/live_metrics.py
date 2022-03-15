#!/usr/bin/env python3
"""Live terminal view of a HotPath /metrics endpoint (stdlib only).

Usage: python scripts/live_metrics.py [url] [seconds]   (default http://localhost:9100/metrics, forever)
"""
import re
import sys
import time
import urllib.request

url = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:9100/metrics"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else float("inf")


def scrape():
    text = urllib.request.urlopen(url, timeout=2).read().decode()
    vals, buckets = {}, []
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        m = re.match(r'(\w+)(?:\{le="([^"]+)"\})? (\S+)', line)
        if not m:
            continue
        name, le, v = m.group(1), m.group(2), float(m.group(3))
        if name == "hotpath_event_latency_seconds_bucket":
            buckets.append((float("inf") if le == "+Inf" else float(le), v))
        else:
            vals[name] = v
    return vals, buckets


def p99_ms(prev, cur):
    """Approximate p99 over the last interval from the delta of cumulative histogram buckets."""
    delta = [(le, c - p) for (le, c), (_, p) in zip(cur, prev)]
    total = delta[-1][1]
    if total <= 0:
        return float("nan")
    for le, c in delta:
        if c >= 0.99 * total:
            return le * 1000 if le != float("inf") else float("inf")
    return float("nan")


print(f"{'t':>4} {'events/s':>10} {'anom/s':>8} {'alerts/s':>9} {'p99<=ms':>8} {'ring':>6} {'pool free':>10} {'dropped':>8}")
prev, prev_b, t0 = None, None, time.time()
while time.time() - t0 < duration:
    try:
        v, b = scrape()
    except OSError as e:
        print(f"scrape failed: {e}")
        time.sleep(1)
        continue
    if prev:
        d = lambda k: v.get(k, 0) - prev.get(k, 0)  # noqa: E731
        print(f"{time.time() - t0:4.0f} {d('hotpath_events_scored_total'):>10,.0f} {d('hotpath_anomalies_total'):>8,.0f} "
              f"{d('hotpath_alerts_sent_total'):>9,.0f} {p99_ms(prev_b, b):>8.2f} {v['hotpath_ingest_ring_depth']:>6.0f} "
              f"{v['hotpath_pool_free_batches']:>4.0f}/{v['hotpath_pool_total_batches']:<5.0f} {v['hotpath_events_dropped_total']:>8.0f}",
              flush=True)
    prev, prev_b = v, b
    time.sleep(1)
