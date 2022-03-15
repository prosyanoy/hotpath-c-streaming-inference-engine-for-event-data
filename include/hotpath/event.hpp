#pragma once
#include <array>
#include <cstdint>

namespace hotpath {

inline constexpr std::size_t kFeatures = 8;
inline constexpr std::array<const char*, kFeatures> kFeatureNames = {
    "cpu_pct", "mem_pct", "latency_ms", "req_rate", "error_rate", "bytes_in_kb", "bytes_out_kb", "conn_count"};

/// One telemetry event as it travels through the ingest ring.
struct Event {
  std::uint64_t id = 0;
  std::uint64_t ingest_ns = 0;  // steady-clock time the event entered the service
  std::uint32_t host = 0;
  float features[kFeatures] = {};
};

/// Per-event bookkeeping kept next to a batch's feature matrix.
struct EventMeta {
  std::uint64_t id;
  std::uint64_t ingest_ns;
  std::uint32_t host;
};

/// A scored event that crossed the anomaly threshold; fixed-size so it can ride a lock-free queue.
struct Alert {
  std::uint64_t id = 0;
  std::uint64_t unix_ns = 0;
  std::uint32_t host = 0;
  float score = 0;
  float features[kFeatures] = {};
};

}  // namespace hotpath
