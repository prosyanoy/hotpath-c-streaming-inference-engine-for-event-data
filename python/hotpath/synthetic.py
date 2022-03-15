"""Synthetic telemetry shared by training, the Python baseline and (by spec) the C++ generator.

The C++ generator in ``src/generator.cpp`` implements the same distribution; keep the
tables below in sync with ``FeatureSpec`` there.
"""
from __future__ import annotations

import numpy as np

FEATURES = (
    "cpu_pct",
    "mem_pct",
    "latency_ms",
    "req_rate",
    "error_rate",
    "bytes_in_kb",
    "bytes_out_kb",
    "conn_count",
)
N_FEATURES = len(FEATURES)

MEAN = np.array([35.0, 55.0, 40.0, 500.0, 0.5, 200.0, 800.0, 120.0], dtype=np.float32)
STD = np.array([8.0, 7.0, 8.0, 70.0, 0.25, 30.0, 120.0, 15.0], dtype=np.float32)
LOWER = np.zeros(N_FEATURES, dtype=np.float32)
UPPER = np.array([100.0, 100.0, 1e9, 1e9, 1e9, 1e9, 1e9, 1e9], dtype=np.float32)

# Anomalies shift 1..3 features by 5..9 standard deviations in a random direction. Features whose
# lower clip bound lies within ANOMALY_MIN_SIGMA of the mean (cpu, latency, error rate) only shift
# upward, otherwise clipping at zero would turn the "anomaly" back into a plausible value.
ANOMALY_MIN_SIGMA = 5.0
ANOMALY_MAX_SIGMA = 9.0
ANOMALY_MAX_FEATURES = 3
CAN_SHIFT_DOWN = (MEAN - ANOMALY_MIN_SIGMA * STD) > LOWER


def generate(n: int, anomaly_rate: float, rng: np.random.Generator):
    """Return ``(X float32 [n, 8], y int8 [n])`` where y=1 marks an injected anomaly."""
    x = rng.normal(MEAN, STD, size=(n, N_FEATURES)).astype(np.float32)
    y = (rng.random(n) < anomaly_rate).astype(np.int8)
    for i in np.flatnonzero(y):
        k = int(rng.integers(1, ANOMALY_MAX_FEATURES + 1))
        cols = rng.choice(N_FEATURES, size=k, replace=False)
        sigma = rng.uniform(ANOMALY_MIN_SIGMA, ANOMALY_MAX_SIGMA, size=k)
        sign = np.where(CAN_SHIFT_DOWN[cols], rng.choice([-1.0, 1.0], size=k), 1.0)
        x[i, cols] = MEAN[cols] + sign * sigma * STD[cols]
    np.clip(x, LOWER, UPPER, out=x)
    return x, y
