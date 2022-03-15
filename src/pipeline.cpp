#include "hotpath/pipeline.hpp"

#include <cstdio>
#include <stdexcept>

#include "hotpath/generator.hpp"
#include "hotpath/tcp_source.hpp"

namespace hotpath {
namespace {

/// Escalating idle strategy: spin briefly, then yield, then nap. Keeps latency low under load and CPU
/// low when idle.
class Backoff {
 public:
  void idle() {
    if (n_ < 64) cpu_relax();
    else if (n_ < 256) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::microseconds(50));
    if (n_ < 1000000) ++n_;
  }
  void reset() { n_ = 0; }

 private:
  unsigned n_ = 0;
};

void store_max(std::atomic<std::uint64_t>& a, std::uint64_t v) {
  std::uint64_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

}  // namespace

std::string Summary::to_json() const {
  char buf[640];
  std::snprintf(buf, sizeof buf,
                "{\"impl\":\"cpp\",\"events\":%llu,\"elapsed_s\":%.6f,\"events_per_sec\":%.1f,"
                "\"latency_us\":{\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},\"anomalies\":%llu,"
                "\"alerts_sent\":%llu,\"alerts_failed\":%llu,\"events_dropped\":%llu}",
                static_cast<unsigned long long>(events), elapsed_s, events_per_sec, p50_us, p99_us, max_us,
                static_cast<unsigned long long>(anomalies), static_cast<unsigned long long>(alerts_sent),
                static_cast<unsigned long long>(alerts_failed), static_cast<unsigned long long>(events_dropped));
  return buf;
}

Pipeline::Pipeline(Config cfg, ScoreFn score, float threshold)
    : cfg_(std::move(cfg)),
      score_(std::move(score)),
      threshold_(threshold),
      metrics_(cfg_.workers),
      ring_(cfg_.ring_capacity),
      pool_(cfg_.pool_batches, cfg_.batch_size),
      batch_q_(cfg_.pool_batches),  // >= pool size, so pushing an in-flight batch can never fail
      alert_q_(cfg_.alert_queue_capacity) {
  cfg_.validate();
  sink_ = std::make_unique<HecSink>(cfg_, alert_q_, metrics_);
}

Pipeline::~Pipeline() {
  if (started_ && !joined_) {
    request_stop();
    finish_input();
    join();
  }
}

void Pipeline::start() {
  start_stages_();
  source_thread_ = std::thread([this] { source_main_(); });
}

void Pipeline::start_without_source() { start_stages_(); }

void Pipeline::start_stages_() {
  if (started_) throw std::logic_error("pipeline already started");
  started_ = true;
  if (cfg_.metrics_port > 0) {
    metrics_server_ = std::make_unique<MetricsServer>(
        cfg_.metrics_bind, cfg_.metrics_port, [this] { return metrics_.render_prometheus(gauges()); },
        [this] { return ready_.load(std::memory_order_acquire); });
  }
  workers_running_.store(cfg_.workers);
  sink_thread_ = std::thread([this] { sink_main_(); });
  for (std::size_t i = 0; i < cfg_.workers; ++i) workers_.emplace_back([this, i] { worker_main_(i); });
  batcher_thread_ = std::thread([this] { batcher_main_(); });
  ready_.store(true, std::memory_order_release);
}

void Pipeline::request_stop() {
  ready_.store(false, std::memory_order_release);
  stop_.store(true, std::memory_order_release);
}

void Pipeline::join() {
  if (joined_) return;
  if (source_thread_.joinable()) source_thread_.join();
  if (batcher_thread_.joinable()) batcher_thread_.join();
  for (auto& w : workers_) w.join();
  if (sink_thread_.joinable()) sink_thread_.join();
  if (metrics_server_) metrics_server_->stop();
  joined_ = true;
  finished_.store(true, std::memory_order_release);
}

Gauges Pipeline::gauges() const {
  Gauges g;
  g.ingest_ring_depth = ring_.size_approx();
  g.batch_queue_depth = batch_q_.size_approx();
  g.alert_queue_depth = alert_q_.size_approx();
  g.pool_total_batches = pool_.total();
  g.pool_free_batches = pool_.free_approx();
  g.ready = ready_.load(std::memory_order_acquire);
  return g;
}

Summary Pipeline::summary() const {
  Summary s;
  auto ld = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  s.events = ld(metrics_.events_scored);
  const std::uint64_t t0 = ld(metrics_.first_ingest_ns), t1 = ld(metrics_.last_scored_ns);
  s.elapsed_s = t1 > t0 ? static_cast<double>(t1 - t0) / 1e9 : 0.0;
  s.events_per_sec = s.elapsed_s > 0 ? static_cast<double>(s.events) / s.elapsed_s : 0.0;
  const HistogramSnapshot lat = metrics_.event_latency_snapshot();
  s.p50_us = static_cast<double>(lat.quantile(0.50)) / 1e3;
  s.p99_us = static_cast<double>(lat.quantile(0.99)) / 1e3;
  s.max_us = static_cast<double>(lat.max) / 1e3;
  s.anomalies = ld(metrics_.anomalies);
  s.alerts_sent = ld(metrics_.alerts_sent);
  s.alerts_failed = ld(metrics_.alerts_failed);
  s.events_dropped = ld(metrics_.events_dropped);
  return s;
}

// ---- source -----------------------------------------------------------------------------------------

bool Pipeline::submit(Event e) {
  e.id = next_id_++;
  e.ingest_ns = now_ns();
  if (next_id_ == 1) metrics_.first_ingest_ns.store(e.ingest_ns, std::memory_order_relaxed);
  if (!ring_.try_push(e)) {
    if (cfg_.overflow == Overflow::Drop) {
      metrics_.events_dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    Backoff b;
    while (!ring_.try_push(e)) b.idle();  // back-pressure: the producer slows to the pipeline's pace
  }
  metrics_.events_ingested.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void Pipeline::source_main_() {
  try {
    if (cfg_.source == "tcp") {
      run_tcp_source(
          cfg_.tcp_port, stop_, [this](const Event& e) { submit(e); },
          [this] { metrics_.events_malformed.fetch_add(1, std::memory_order_relaxed); },
          [](int port) { std::fprintf(stderr, "hotpath: tcp ingest listening on port %d\n", port); });
    } else {
      SyntheticGenerator gen(cfg_.seed, cfg_.anomaly_rate);
      Event e;
      std::uint64_t emitted = 0;
      const std::uint64_t start = now_ns();
      while (!stop_.load(std::memory_order_acquire) && (cfg_.total_events == 0 || emitted < cfg_.total_events)) {
        std::uint64_t burst = 256;
        if (cfg_.rate > 0) {
          const double elapsed_s = static_cast<double>(now_ns() - start) / 1e9;
          const double due = elapsed_s * cfg_.rate + 1.0 - static_cast<double>(emitted);
          if (due < 1.0) {
            const double wait_us = (static_cast<double>(emitted) / cfg_.rate - elapsed_s) * 1e6;
            if (wait_us > 300) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(wait_us) - 100));
            else std::this_thread::yield();
            continue;
          }
          burst = due > 1024 ? 1024 : static_cast<std::uint64_t>(due);
        }
        for (std::uint64_t i = 0; i < burst && (cfg_.total_events == 0 || emitted < cfg_.total_events); ++i) {
          gen.next(e);
          submit(e);
          ++emitted;
        }
      }
    }
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "hotpath: source failed: %s\n", ex.what());
  }
  finish_input();
}

// ---- batcher ----------------------------------------------------------------------------------------

void Pipeline::flush_(Batch* b) {
  Backoff bo;
  while (!batch_q_.try_push(b)) bo.idle();
}

void Pipeline::batcher_main_() {
  const std::uint64_t timeout_ns = static_cast<std::uint64_t>(cfg_.batch_timeout_us) * 1000;
  Batch* cur = nullptr;
  Event e;
  Backoff idle;
  for (;;) {
    const bool done = ingest_done_.load(std::memory_order_acquire);  // read BEFORE popping (see below)
    if (ring_.try_pop(e)) {
      idle.reset();
      if (!cur) {
        Backoff wait;
        bool counted = false;
        while (!(cur = pool_.acquire())) {  // every batch in flight: back-pressure the ring
          if (!counted) {
            metrics_.pool_waits.fetch_add(1, std::memory_order_relaxed);
            counted = true;
          }
          wait.idle();
        }
        cur->opened_ns = now_ns();
      }
      cur->append(e);
      if (cur->full()) {
        flush_(cur);
        cur = nullptr;
      }
      continue;
    }
    // The ring was empty *after* we observed done==true, so the producer's final push (which
    // happens-before its done store) is already visible: nothing more can arrive.
    if (done) break;
    if (cur && now_ns() - cur->opened_ns >= timeout_ns) {
      flush_(cur);
      cur = nullptr;
    }
    idle.idle();
  }
  if (cur) flush_(cur);
  batcher_done_.store(true, std::memory_order_release);
}

// ---- workers ----------------------------------------------------------------------------------------

void Pipeline::worker_main_(std::size_t idx) {
  std::vector<float> scores(cfg_.batch_size);  // per-worker output buffer, allocated once
  Histogram& lat = metrics_.event_latency(idx);
  Histogram& inf = metrics_.inference_time(idx);
  Backoff idle;
  for (;;) {
    const bool done = batcher_done_.load(std::memory_order_acquire);
    Batch* b = nullptr;
    if (!batch_q_.try_pop(b)) {
      if (done) break;
      idle.idle();
      continue;
    }
    idle.reset();

    const std::uint64_t t0 = now_ns();
    try {
      score_(b->features, b->count, scores.data());
    } catch (const std::exception& ex) {
      metrics_.inference_errors.fetch_add(1, std::memory_order_relaxed);
      std::fprintf(stderr, "hotpath: inference failed: %s\n", ex.what());
      pool_.release(b);
      continue;
    }
    const std::uint64_t t1 = now_ns();
    inf.record(t1 - t0);
    const std::uint64_t unix_at_t1 = unix_ns();

    std::uint64_t anomalies = 0, dropped = 0;
    for (std::uint32_t i = 0; i < b->count; ++i) {
      const EventMeta& m = b->meta[i];
      lat.record(t1 - m.ingest_ns);
      if (scores[i] < threshold_) {
        ++anomalies;
        Alert a;
        a.id = m.id;
        a.unix_ns = unix_at_t1 - (t1 - m.ingest_ns);  // event time, not delivery time
        a.host = m.host;
        a.score = scores[i];
        for (std::size_t f = 0; f < kFeatures; ++f) a.features[f] = b->features[i * kFeatures + f];
        if (!alert_q_.try_push(a)) ++dropped;
      }
    }
    metrics_.events_scored.fetch_add(b->count, std::memory_order_relaxed);
    metrics_.batches_scored.fetch_add(1, std::memory_order_relaxed);
    metrics_.anomalies.fetch_add(anomalies, std::memory_order_relaxed);
    if (dropped) metrics_.alerts_dropped.fetch_add(dropped, std::memory_order_relaxed);
    store_max(metrics_.last_scored_ns, t1);
    pool_.release(b);
  }
  if (workers_running_.fetch_sub(1, std::memory_order_acq_rel) == 1) workers_done_.store(true, std::memory_order_release);
}

void Pipeline::sink_main_() {
  sink_->run(workers_done_);
  finished_.store(true, std::memory_order_release);  // everything upstream has drained
}

}  // namespace hotpath
