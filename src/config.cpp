#include "hotpath/config.hpp"

#include <charconv>
#include <cstdlib>
#include <stdexcept>

namespace hotpath {
namespace {

[[noreturn]] void bad(const char* name, const std::string& why) {
  throw std::invalid_argument(std::string(name) + ": " + why);
}

template <class T>
void read_int(const Config::GetEnv& get, const char* name, T& out) {
  const char* v = get(name);
  if (!v || !*v) return;
  T parsed{};
  const char* end = v + std::char_traits<char>::length(v);
  auto [p, ec] = std::from_chars(v, end, parsed);
  if (ec != std::errc() || p != end) bad(name, std::string("not a valid integer: '") + v + "'");
  out = parsed;
}

void read_double(const Config::GetEnv& get, const char* name, double& out) {
  const char* v = get(name);
  if (!v || !*v) return;
  char* end = nullptr;
  const double parsed = std::strtod(v, &end);
  if (end == v || *end != '\0') bad(name, std::string("not a valid number: '") + v + "'");
  out = parsed;
}

void read_str(const Config::GetEnv& get, const char* name, std::string& out) {
  const char* v = get(name);
  if (v) out = v;  // an explicitly empty value is meaningful (e.g. HOTPATH_HEC_URL= disables HEC)
}

}  // namespace

Config Config::from_env(const GetEnv& getenv_fn) {
  const GetEnv get = getenv_fn ? getenv_fn : GetEnv([](const char* n) { return std::getenv(n); });
  Config c;
  read_str(get, "HOTPATH_MODEL", c.model_path);
  c.meta_path = c.model_path.substr(0, c.model_path.rfind('.')) + ".meta";
  read_str(get, "HOTPATH_MODEL_META", c.meta_path);
  read_str(get, "HOTPATH_SOURCE", c.source);
  read_double(get, "HOTPATH_RATE", c.rate);
  read_int(get, "HOTPATH_TOTAL_EVENTS", c.total_events);
  read_double(get, "HOTPATH_ANOMALY_RATE", c.anomaly_rate);
  read_int(get, "HOTPATH_SEED", c.seed);
  read_int(get, "HOTPATH_BATCH_SIZE", c.batch_size);
  read_int(get, "HOTPATH_BATCH_TIMEOUT_US", c.batch_timeout_us);
  read_int(get, "HOTPATH_WORKERS", c.workers);
  read_int(get, "HOTPATH_RING_CAPACITY", c.ring_capacity);
  read_int(get, "HOTPATH_POOL_BATCHES", c.pool_batches);
  read_int(get, "HOTPATH_ALERT_QUEUE_CAPACITY", c.alert_queue_capacity);
  c.overflow = c.source == "tcp" ? Overflow::Drop : Overflow::Block;
  if (const char* o = get("HOTPATH_OVERFLOW"); o && *o) {
    const std::string s = o;
    if (s == "block") c.overflow = Overflow::Block;
    else if (s == "drop") c.overflow = Overflow::Drop;
    else bad("HOTPATH_OVERFLOW", "expected 'block' or 'drop'");
  }
  read_str(get, "HOTPATH_HEC_URL", c.hec_url);
  read_str(get, "HOTPATH_HEC_TOKEN", c.hec_token);
  read_str(get, "HOTPATH_HEC_SOURCE", c.hec_source);
  read_str(get, "HOTPATH_HEC_INDEX", c.hec_index);
  read_int(get, "HOTPATH_HEC_BATCH", c.hec_batch);
  read_int(get, "HOTPATH_HEC_FLUSH_MS", c.hec_flush_ms);
  read_int(get, "HOTPATH_HEC_TIMEOUT_MS", c.hec_timeout_ms);
  read_int(get, "HOTPATH_HEC_MAX_ATTEMPTS", c.hec_max_attempts);
  read_int(get, "HOTPATH_METRICS_PORT", c.metrics_port);
  read_str(get, "HOTPATH_METRICS_BIND", c.metrics_bind);
  read_int(get, "HOTPATH_TCP_PORT", c.tcp_port);
  int summary = c.print_summary ? 1 : 0;
  read_int(get, "HOTPATH_PRINT_SUMMARY", summary);
  c.print_summary = summary != 0;
  c.validate();
  return c;
}

void Config::validate() const {
  if (source != "synthetic" && source != "tcp") bad("HOTPATH_SOURCE", "expected 'synthetic' or 'tcp'");
  if (rate < 0) bad("HOTPATH_RATE", "must be >= 0");
  if (anomaly_rate < 0 || anomaly_rate > 1) bad("HOTPATH_ANOMALY_RATE", "must be in [0, 1]");
  if (batch_size == 0 || batch_size > 65536) bad("HOTPATH_BATCH_SIZE", "must be in [1, 65536]");
  if (workers == 0 || workers > 256) bad("HOTPATH_WORKERS", "must be in [1, 256]");
  if (ring_capacity < 2) bad("HOTPATH_RING_CAPACITY", "must be >= 2");
  if (pool_batches < workers + 2) bad("HOTPATH_POOL_BATCHES", "must be >= workers + 2");
  if (alert_queue_capacity < 2) bad("HOTPATH_ALERT_QUEUE_CAPACITY", "must be >= 2");
  if (hec_batch == 0) bad("HOTPATH_HEC_BATCH", "must be >= 1");
  if (hec_max_attempts < 1) bad("HOTPATH_HEC_MAX_ATTEMPTS", "must be >= 1");
  if (metrics_port < 0 || metrics_port > 65535) bad("HOTPATH_METRICS_PORT", "must be in [0, 65535]");
  if (tcp_port < 0 || tcp_port > 65535) bad("HOTPATH_TCP_PORT", "must be in [0, 65535]");
}

}  // namespace hotpath
