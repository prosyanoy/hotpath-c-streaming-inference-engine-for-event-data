#pragma once
#include <string>

#include "hotpath/config.hpp"
#include "hotpath/event.hpp"
#include "hotpath/http.hpp"
#include "hotpath/metrics.hpp"
#include "hotpath/mpmc_queue.hpp"

namespace hotpath {

/// Appends one Splunk HEC event envelope for `a` to `out` (no trailing newline). HEC accepts many
/// envelopes concatenated in a single request body.
void append_hec_event(std::string& out, const Alert& a, const Config& cfg);

/// Drains the alert queue and POSTs batches to the HEC endpoint with keep-alive, retry and
/// exponential backoff. Run `run()` on its own thread; it returns once producers are done and the
/// queue is empty.
class HecSink {
 public:
  HecSink(const Config& cfg, MpmcQueue<Alert>& queue, Metrics& metrics);
  void run(const std::atomic<bool>& producers_done);

 private:
  bool deliver_(HttpClient& client, const std::string& body, std::size_t n_events);
  const Config& cfg_;
  MpmcQueue<Alert>& queue_;
  Metrics& metrics_;
  Url url_;
};

}  // namespace hotpath
