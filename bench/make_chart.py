#!/usr/bin/env python3
"""Render docs/benchmark.svg from bench/results/latest.json (stdlib only).

Usage: python bench/make_chart.py [results.json] [out.svg]
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "bench" / "results" / "latest.json"
dst = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "docs" / "benchmark.svg"
d = json.loads(src.read_text())
cpu = d.get("cpus", "?")
mt, pc = d["max_throughput"], d["paced"]
rate = d["config"]["paced_rate"]

C_CPP, C_PY, INK, MUTED = "#1f6feb", "#e8710a", "#1b1f24", "#57606a"
W, H = 900, 420


def bars(x0, title, subtitle, groups, fmt, unit):
    """groups: [(label, cpp_value, py_value)] -> grouped bars in a 400px-wide panel."""
    top, base, ph = 95, 340, 245
    vmax = max(max(c, p) for _, c, p in groups) * 1.15
    out = [f'<text x="{x0}" y="62" font-size="16" font-weight="600" fill="{INK}">{title}</text>',
           f'<text x="{x0}" y="82" font-size="12" fill="{MUTED}">{subtitle}</text>',
           f'<line x1="{x0}" y1="{base}" x2="{x0 + 400}" y2="{base}" stroke="{MUTED}" stroke-width="1"/>']
    gw = 400 / len(groups)
    for i, (label, c, p) in enumerate(groups):
        gx = x0 + i * gw + gw / 2
        for j, (v, col, name) in enumerate(((c, C_CPP, "C++"), (p, C_PY, "Python"))):
            h = ph * v / vmax
            bx = gx - 56 + j * 58
            out.append(f'<rect x="{bx:.1f}" y="{base - h:.1f}" width="52" height="{h:.1f}" rx="3" fill="{col}"/>')
            out.append(f'<text x="{bx + 26:.1f}" y="{base - h - 6:.1f}" font-size="12" font-weight="600" '
                       f'text-anchor="middle" fill="{INK}">{fmt(v)}</text>')
        out.append(f'<text x="{gx:.1f}" y="{base + 20}" font-size="12" text-anchor="middle" fill="{INK}">{label}</text>')
    out.append(f'<text x="{x0}" y="{base + 42}" font-size="11" fill="{MUTED}">{unit}</text>')
    return out


k = lambda v: f"{v / 1000:,.0f}k"          # noqa: E731
ms = lambda v: f"{v / 1000:.1f} ms"        # noqa: E731
ratio_t = mt["cpp"]["events_per_sec"] / mt["python"]["events_per_sec"]
ratio_l = pc["python"]["p99_us"] / pc["cpp"]["p99_us"]

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
       f'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif" role="img" '
       f'aria-label="HotPath C++ vs Python baseline: events per second and latency">',
       f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
       f'<text x="40" y="30" font-size="18" font-weight="700" fill="{INK}">HotPath (C++) vs Python baseline: same model, same pipeline</text>',
       f'<rect x="640" y="16" width="12" height="12" rx="2" fill="{C_CPP}"/><text x="658" y="27" font-size="12" fill="{INK}">C++</text>',
       f'<rect x="710" y="16" width="12" height="12" rx="2" fill="{C_PY}"/><text x="728" y="27" font-size="12" fill="{INK}">Python</text>']
svg += bars(40, f"Throughput: {ratio_t:.1f}x", "events/sec, unbounded ingest, higher is better",
            [("end to end", mt["cpp"]["events_per_sec"], mt["python"]["events_per_sec"])], k, "events per second")
svg += bars(480, f"Latency at {rate // 1000}k events/sec: p99 {ratio_l:.1f}x lower", "ingest to scored, lower is better",
            [("p50", pc["cpp"]["p50_us"], pc["python"]["p50_us"]), ("p99", pc["cpp"]["p99_us"], pc["python"]["p99_us"])],
            ms, "milliseconds")
svg += [f'<text x="40" y="{H - 16}" font-size="11" fill="{MUTED}">{cpu}-CPU VM, 2 workers, batch 256, median of '
        f'{d["config"]["repeats"]} runs. Reproduce: make bench, then make chart</text>', "</svg>"]
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text("\n".join(svg))
print(f"wrote {dst}")
