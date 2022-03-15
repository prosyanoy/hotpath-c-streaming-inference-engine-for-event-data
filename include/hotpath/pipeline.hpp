#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hotpath/batch_pool.hpp"
#include "hotpath/config.hpp"
#include "hotpath/event.hpp"
#include "hotpath/hec_sink.hpp"
#include "hotpath/http.hpp"
#include "hotpath/metrics.hpp"
#include "hotpath/model.hpp"
#include "hotpath/mpmc_queue.hpp"
#include "hotpath/spsc_ring.hpp"

namespace hotpath {

struct Summary {
  std::uint64_t events = 0;
  double elapsed_s = 0;
  double events_per_sec = 0;
  double p50_us = 0, p99_us = 0, max_us = 0;
  std::uint64_t anomalies = 0, alerts_sent = 0, alerts_failed = 0, events_dropped = 0;
  std::string to_json() const;
};

/// The streaming engine:
///
///   source --SPSC ring--> batcher --MPMC queue--> N inference workers --MPMC queue--> HEC sink
///                            ^                          |
///                            +---- BatchPool free list -+      (batches are recycled, never freed)
class Pipeline {
 public:
  Pipeline(Config cfg, ScoreFn score, float threshold);
  ~Pipeline();

  void start();
  /// Ask the source to stop; the rest of the pipeline drains and shuts down in order.
  void request_stop();
  void join();
  /// True once the source finished (finite synthetic run) and everything drained.
  bool finished() const { return finished_.load(std::memory_order_acquire); }

  Metrics& metrics() { return metrics_; }
  Gauges gauges() const;
  Summary summary() const;
  /// Port the metrics server is bound to (0 if disabled).
  int metrics_port() const { return metrics_server_ ? metrics_server_->port() : 0; }

  /// Producer-side API for embedding/tests: single producer thread only. Honors the overflow policy.
  bool submit(Event e);
  /// Mark the producer side complete (used instead of start()'s built-in source when embedding).
  void finish_input() { ingest_done_.store(true, std::memory_order_release); }
  /// Start every stage except the built-in source; feed with submit() + finish_input().
  void start_without_source();

 private:
  void start_stages_();
  void source_main_();
  void batcher_main_();
  void worker_main_(std::size_t idx);
  void sink_main_();
  void flush_(Batch* b);
  void emit_(const Event& e);

  Config cfg_;
  ScoreFn score_;
  float threshold_;
  Metrics metrics_;
  SpscRing<Event> ring_;
  BatchPool pool_;
  MpmcQueue<Batch*> batch_q_;
  MpmcQueue<Alert> alert_q_;
  std::unique_ptr<HecSink> sink_;
  std::unique_ptr<MetricsServer> metrics_server_;

  std::atomic<bool> stop_{false};
  std::atomic<bool> ingest_done_{false};
  std::atomic<bool> batcher_done_{false};
  std::atomic<std::size_t> workers_running_{0};
  std::atomic<bool> workers_done_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> ready_{false};
  std::uint64_t next_id_ = 0;  // owned by the producer thread

  std::thread source_thread_, batcher_thread_, sink_thread_;
  std::vector<std::thread> workers_;
  bool started_ = false;
  bool joined_ = false;
};

}  // namespace hotpath
