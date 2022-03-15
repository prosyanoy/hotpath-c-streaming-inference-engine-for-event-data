#include "hotpath/hec_sink.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace hotpath {
namespace {

void append_escaped(std::string& out, const std::string& s) {
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
}

void append_number(std::string& out, float v) {
  if (!std::isfinite(v)) {
    out += "null";
    return;
  }
  char b[32];
  std::snprintf(b, sizeof b, "%.6g", static_cast<double>(v));
  out += b;
}

}  // namespace

void append_hec_event(std::string& out, const Alert& a, const Config& cfg) {
  char b[96];
  std::snprintf(b, sizeof b, "{\"time\":%llu.%03llu,\"host\":\"host-%u\",\"source\":\"",
                static_cast<unsigned long long>(a.unix_ns / 1000000000ull),
                static_cast<unsigned long long>(a.unix_ns / 1000000ull % 1000ull), a.host);
  out += b;
  append_escaped(out, cfg.hec_source);
  out += "\",\"sourcetype\":\"hotpath:anomaly\",\"index\":\"";
  append_escaped(out, cfg.hec_index);
  std::snprintf(b, sizeof b, "\",\"event\":{\"id\":%llu,\"score\":", static_cast<unsigned long long>(a.id));
  out += b;
  append_number(out, a.score);
  out += ",\"features\":{";
  for (std::size_t i = 0; i < kFeatures; ++i) {
    if (i) out += ',';
    out += '"';
    out += kFeatureNames[i];
    out += "\":";
    append_number(out, a.features[i]);
  }
  out += "}}}";
}

HecSink::HecSink(const Config& cfg, MpmcQueue<Alert>& queue, Metrics& metrics)
    : cfg_(cfg), queue_(queue), metrics_(metrics) {
  if (cfg.hec_enabled()) url_ = parse_url(cfg.hec_url);  // fail fast on a bad URL
}

bool HecSink::deliver_(HttpClient& client, const std::string& body, std::size_t n) {
  const std::string auth = "Splunk " + cfg_.hec_token;
  for (int attempt = 1; attempt <= cfg_.hec_max_attempts; ++attempt) {
    metrics_.hec_requests.fetch_add(1, std::memory_order_relaxed);
    const HttpResponse r = client.post(body, auth);
    if (r.status == 200) {
      metrics_.alerts_sent.fetch_add(n, std::memory_order_relaxed);
      return true;
    }
    const bool retryable = r.status == 0 || r.status >= 500 || r.status == 408 || r.status == 429;
    if (!retryable || attempt == cfg_.hec_max_attempts) break;
    metrics_.hec_retries.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(25u << attempt));  // 50, 100, 200 ms ...
  }
  metrics_.alerts_failed.fetch_add(n, std::memory_order_relaxed);
  return false;
}

void HecSink::run(const std::atomic<bool>& producers_done) {
  using Clock = std::chrono::steady_clock;
  Alert a;
  if (!cfg_.hec_enabled()) {  // keep the queue moving; nothing to deliver to
    for (;;) {
      const bool done = producers_done.load(std::memory_order_acquire);
      bool got = false;
      while (queue_.try_pop(a)) {
        got = true;
        metrics_.alerts_dropped.fetch_add(1, std::memory_order_relaxed);
      }
      if (!got) {
        if (done) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }

  HttpClient client(url_, cfg_.hec_timeout_ms);
  std::string body;
  body.reserve(cfg_.hec_batch * 400);
  std::size_t n = 0;
  Clock::time_point batch_started;
  for (;;) {
    const bool done = producers_done.load(std::memory_order_acquire);
    bool got = false;
    while (n < cfg_.hec_batch && queue_.try_pop(a)) {
      if (n == 0) batch_started = Clock::now();
      append_hec_event(body, a, cfg_);
      ++n;
      got = true;
    }
    const bool timed_out =
        n > 0 && Clock::now() - batch_started >= std::chrono::milliseconds(cfg_.hec_flush_ms);
    if (n >= cfg_.hec_batch || (n > 0 && (timed_out || (done && !got)))) {
      deliver_(client, body, n);
      body.clear();
      n = 0;
    }
    if (!got) {
      if (done) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

}  // namespace hotpath
