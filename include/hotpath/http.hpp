#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace hotpath {

struct Url {
  std::string host;
  int port = 80;
  std::string path = "/";
};
/// Parses http://host[:port][/path]. Throws std::invalid_argument for anything else (incl. https).
Url parse_url(const std::string& url);

struct HttpResponse {
  int status = 0;  // 0 => transport error
  std::string body;
};

/// Tiny blocking HTTP/1.1 client with keep-alive, used by the HEC sink. One instance per thread.
class HttpClient {
 public:
  HttpClient(Url url, std::uint32_t timeout_ms);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  /// POSTs `body`; reconnects transparently if the kept-alive socket went stale.
  HttpResponse post(const std::string& body, const std::string& authorization,
                    const std::string& content_type = "application/json");

 private:
  bool connect_();
  void close_();
  HttpResponse round_trip_(const std::string& request);
  Url url_;
  std::uint32_t timeout_ms_;
  int fd_ = -1;
};

/// Minimal single-threaded HTTP server serving GET /metrics, /healthz and /readyz.
class MetricsServer {
 public:
  using Render = std::function<std::string()>;
  using Ready = std::function<bool()>;
  /// port 0 picks an ephemeral port (see port()). Throws std::runtime_error if binding fails.
  MetricsServer(const std::string& bind_addr, int port, Render render, Ready ready);
  ~MetricsServer();
  int port() const { return port_; }
  void stop();

 private:
  void serve_();
  int listen_fd_ = -1;
  int port_ = 0;
  Render render_;
  Ready ready_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace hotpath
