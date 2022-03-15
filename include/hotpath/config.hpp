#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace hotpath {

enum class Overflow { Block, Drop };

/// Runtime configuration, read from HOTPATH_* environment variables (see .env.example).
struct Config {
  std::string model_path = "models/anomaly.onnx";
  std::string meta_path;                  // defaults to <model>.meta
  std::string source = "synthetic";       // "synthetic" | "tcp"
  double rate = 5000;                     // synthetic events/sec; 0 = as fast as possible
  std::uint64_t total_events = 0;         // synthetic: stop after N events; 0 = run until signalled
  double anomaly_rate = 0.01;             // synthetic: fraction of injected anomalies
  std::uint64_t seed = 42;
  std::size_t batch_size = 256;
  std::uint32_t batch_timeout_us = 2000;  // flush a partial batch after this long
  std::size_t workers = 2;
  std::size_t ring_capacity = 65536;      // ingest SPSC ring (rounded up to a power of two)
  std::size_t pool_batches = 64;          // pre-allocated batches in flight
  std::size_t alert_queue_capacity = 16384;
  Overflow overflow = Overflow::Block;    // synthetic default; tcp defaults to Drop
  std::string hec_url = "http://127.0.0.1:8088/services/collector/event";  // "" or "off" disables
  std::string hec_token = "dev-token-not-secret";  // local-only placeholder, never a real secret
  std::string hec_source = "hotpath";
  std::string hec_index = "main";
  std::size_t hec_batch = 100;            // alerts per HTTP POST
  std::uint32_t hec_flush_ms = 200;
  std::uint32_t hec_timeout_ms = 2000;
  int hec_max_attempts = 3;
  int metrics_port = 9100;                // 0 disables the metrics endpoint
  std::string metrics_bind = "0.0.0.0";
  int tcp_port = 9200;
  bool print_summary = true;

  bool hec_enabled() const { return !hec_url.empty() && hec_url != "off"; }

  using GetEnv = std::function<const char*(const char*)>;
  /// Build a Config from the environment (or a custom lookup, used by tests).
  /// Throws std::invalid_argument on malformed or out-of-range values.
  static Config from_env(const GetEnv& getenv_fn = nullptr);
  void validate() const;
};

}  // namespace hotpath
