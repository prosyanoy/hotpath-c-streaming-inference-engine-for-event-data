#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "hotpath/common.hpp"
#include "hotpath/histogram.hpp"

namespace hotpath {

/// Point-in-time gauges supplied by the pipeline when /metrics is rendered.
struct Gauges {
  std::size_t ingest_ring_depth = 0;
  std::size_t batch_queue_depth = 0;
  std::size_t alert_queue_depth = 0;
  std::size_t pool_free_batches = 0;
  std::size_t pool_total_batches = 0;
  bool ready = false;
};

/// All counters are monotonic and updated with relaxed atomics from the pipeline threads.
class Metrics {
 public:
  explicit Metrics(std::size_t workers)
      : workers_(workers),
        event_latency_(new Histogram[workers]),
        inference_time_(new Histogram[workers]) {}

  std::atomic<std::uint64_t> events_ingested{0};
  std::atomic<std::uint64_t> events_dropped{0};     // ring full under the "drop" overflow policy
  std::atomic<std::uint64_t> events_malformed{0};   // unparsable TCP lines
  std::atomic<std::uint64_t> events_scored{0};
  std::atomic<std::uint64_t> batches_scored{0};
  std::atomic<std::uint64_t> inference_errors{0};
  std::atomic<std::uint64_t> pool_waits{0};         // batcher found the pool empty (back-pressure)
  std::atomic<std::uint64_t> anomalies{0};
  std::atomic<std::uint64_t> alerts_dropped{0};     // alert queue full or sink disabled
  std::atomic<std::uint64_t> alerts_sent{0};
  std::atomic<std::uint64_t> alerts_failed{0};      // gave up after retries / non-retryable
  std::atomic<std::uint64_t> hec_requests{0};
  std::atomic<std::uint64_t> hec_retries{0};
  std::atomic<std::uint64_t> first_ingest_ns{0};
  std::atomic<std::uint64_t> last_scored_ns{0};

  /// Per-worker histograms (no cross-thread contention); merge with the snapshot helpers.
  Histogram& event_latency(std::size_t worker) { return event_latency_[worker]; }
  Histogram& inference_time(std::size_t worker) { return inference_time_[worker]; }
  HistogramSnapshot event_latency_snapshot() const;
  HistogramSnapshot inference_time_snapshot() const;

  /// Prometheus text exposition format (version 0.0.4).
  std::string render_prometheus(const Gauges& g) const;

 private:
  std::size_t workers_;
  std::unique_ptr<Histogram[]> event_latency_;
  std::unique_ptr<Histogram[]> inference_time_;
};

}  // namespace hotpath
