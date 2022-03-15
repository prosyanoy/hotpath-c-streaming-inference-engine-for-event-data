#include "hotpath/metrics.hpp"

#include <cstdio>

namespace hotpath {
namespace {

HistogramSnapshot merged(const std::unique_ptr<Histogram[]>& h, std::size_t n) {
  HistogramSnapshot s;
  for (std::size_t i = 0; i < n; ++i) s.merge(h[i].snapshot());
  return s;
}

void counter(std::string& out, const char* name, const char* help, std::uint64_t v) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "# HELP %s %s\n# TYPE %s counter\n%s %llu\n", name, help, name, name,
                static_cast<unsigned long long>(v));
  out += buf;
}

void gauge(std::string& out, const char* name, const char* help, double v) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "# HELP %s %s\n# TYPE %s gauge\n%s %.17g\n", name, help, name, name, v);
  out += buf;
}

// Histograms are recorded in nanoseconds and exported in seconds (Prometheus convention).
void histogram(std::string& out, const char* name, const char* help, const HistogramSnapshot& s) {
  static constexpr double kBoundsSeconds[] = {50e-6, 100e-6, 250e-6, 500e-6, 1e-3, 2.5e-3, 5e-3,
                                              10e-3, 25e-3,  50e-3,  100e-3, 250e-3, 500e-3, 1.0, 5.0};
  char buf[256];
  std::snprintf(buf, sizeof buf, "# HELP %s %s\n# TYPE %s histogram\n", name, help, name);
  out += buf;
  for (double b : kBoundsSeconds) {
    const auto n = s.count_le(static_cast<std::uint64_t>(b * 1e9));
    std::snprintf(buf, sizeof buf, "%s_bucket{le=\"%g\"} %llu\n", name, b, static_cast<unsigned long long>(n));
    out += buf;
  }
  std::snprintf(buf, sizeof buf, "%s_bucket{le=\"+Inf\"} %llu\n%s_sum %.9f\n%s_count %llu\n", name,
                static_cast<unsigned long long>(s.count), name, static_cast<double>(s.sum) / 1e9, name,
                static_cast<unsigned long long>(s.count));
  out += buf;
}

}  // namespace

HistogramSnapshot Metrics::event_latency_snapshot() const { return merged(event_latency_, workers_); }
HistogramSnapshot Metrics::inference_time_snapshot() const { return merged(inference_time_, workers_); }

std::string Metrics::render_prometheus(const Gauges& g) const {
  auto ld = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  std::string out;
  out.reserve(8192);
  counter(out, "hotpath_events_ingested_total", "Events accepted into the ingest ring.", ld(events_ingested));
  counter(out, "hotpath_events_dropped_total", "Events dropped because the ingest ring was full.", ld(events_dropped));
  counter(out, "hotpath_events_malformed_total", "Ingest lines that failed to parse.", ld(events_malformed));
  counter(out, "hotpath_events_scored_total", "Events scored by the model.", ld(events_scored));
  counter(out, "hotpath_batches_scored_total", "Inference batches executed.", ld(batches_scored));
  counter(out, "hotpath_inference_errors_total", "Inference batches that failed.", ld(inference_errors));
  counter(out, "hotpath_pool_waits_total", "Times the batcher waited for a free batch (back-pressure).", ld(pool_waits));
  counter(out, "hotpath_anomalies_total", "Events scored as anomalous.", ld(anomalies));
  counter(out, "hotpath_alerts_dropped_total", "Alerts dropped before delivery.", ld(alerts_dropped));
  counter(out, "hotpath_alerts_sent_total", "Alerts accepted by the HEC endpoint.", ld(alerts_sent));
  counter(out, "hotpath_alerts_failed_total", "Alerts abandoned after retries.", ld(alerts_failed));
  counter(out, "hotpath_hec_requests_total", "HEC POST requests attempted.", ld(hec_requests));
  counter(out, "hotpath_hec_retries_total", "HEC POST retries.", ld(hec_retries));
  gauge(out, "hotpath_ingest_ring_depth", "Events waiting in the ingest ring.", static_cast<double>(g.ingest_ring_depth));
  gauge(out, "hotpath_batch_queue_depth", "Batches waiting for an inference worker.", static_cast<double>(g.batch_queue_depth));
  gauge(out, "hotpath_alert_queue_depth", "Alerts waiting for the HEC sink.", static_cast<double>(g.alert_queue_depth));
  gauge(out, "hotpath_pool_free_batches", "Batches available in the memory pool.", static_cast<double>(g.pool_free_batches));
  gauge(out, "hotpath_pool_total_batches", "Batches pre-allocated in the memory pool.", static_cast<double>(g.pool_total_batches));
  gauge(out, "hotpath_ready", "1 once the pipeline is running.", g.ready ? 1 : 0);
  histogram(out, "hotpath_event_latency_seconds", "Ingest-to-scored latency per event.", event_latency_snapshot());
  histogram(out, "hotpath_inference_batch_seconds", "Model execution time per batch.", inference_time_snapshot());
  return out;
}

}  // namespace hotpath
