#include "hotpath/http.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace hotpath {

Url parse_url(const std::string& url) {
  constexpr const char* kScheme = "http://";
  if (url.rfind(kScheme, 0) != 0)
    throw std::invalid_argument("HOTPATH_HEC_URL must start with http:// (TLS is not built in; terminate TLS in a "
                                "sidecar or forwarder): '" + url + "'");
  Url u;
  std::string rest = url.substr(std::strlen(kScheme));
  const auto slash = rest.find('/');
  std::string hostport = rest.substr(0, slash);
  if (slash != std::string::npos) u.path = rest.substr(slash);
  const auto colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    const std::string port = hostport.substr(colon + 1);
    hostport = hostport.substr(0, colon);
    if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), ::isdigit))
      throw std::invalid_argument("bad port in URL: '" + url + "'");
    u.port = std::stoi(port);
    if (u.port < 1 || u.port > 65535) throw std::invalid_argument("port out of range in URL: '" + url + "'");
  }
  if (hostport.empty()) throw std::invalid_argument("missing host in URL: '" + url + "'");
  u.host = hostport;
  return u;
}

// ---- client ---------------------------------------------------------------------------------------

HttpClient::HttpClient(Url url, std::uint32_t timeout_ms) : url_(std::move(url)), timeout_ms_(timeout_ms) {}
HttpClient::~HttpClient() { close_(); }

void HttpClient::close_() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool HttpClient::connect_() {
  close_();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(url_.host.c_str(), std::to_string(url_.port).c_str(), &hints, &res) != 0) return false;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    timeval tv{static_cast<time_t>(timeout_ms_ / 1000), static_cast<suseconds_t>((timeout_ms_ % 1000) * 1000)};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      fd_ = fd;
      break;
    }
    ::close(fd);
  }
  freeaddrinfo(res);
  return fd_ >= 0;
}

HttpResponse HttpClient::round_trip_(const std::string& request) {
  HttpResponse bad;  // status 0
  for (std::size_t sent = 0; sent < request.size();) {
    const ssize_t n = ::send(fd_, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return bad;
    sent += static_cast<std::size_t>(n);
  }
  std::string buf;
  char tmp[4096];
  std::size_t header_end = std::string::npos;
  while ((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
    const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
    if (n <= 0) return bad;
    buf.append(tmp, static_cast<std::size_t>(n));
    if (buf.size() > (1u << 20)) return bad;
  }
  HttpResponse resp;
  if (buf.size() < 12 || buf.compare(0, 5, "HTTP/") != 0) return bad;
  resp.status = std::atoi(buf.c_str() + buf.find(' ') + 1);

  std::string headers = buf.substr(0, header_end);
  std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return std::tolower(c); });
  std::size_t content_length = 0;
  bool have_length = false;
  if (auto p = headers.find("content-length:"); p != std::string::npos) {
    content_length = static_cast<std::size_t>(std::strtoull(headers.c_str() + p + 15, nullptr, 10));
    have_length = true;
  }
  const bool close_after = headers.find("connection: close") != std::string::npos || !have_length;
  resp.body = buf.substr(header_end + 4);
  while (resp.body.size() < content_length) {
    const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
    if (n <= 0) return bad;
    resp.body.append(tmp, static_cast<std::size_t>(n));
  }
  if (close_after) close_();
  return resp;
}

HttpResponse HttpClient::post(const std::string& body, const std::string& authorization,
                              const std::string& content_type) {
  std::string req = "POST " + url_.path + " HTTP/1.1\r\nHost: " + url_.host + ":" + std::to_string(url_.port) +
                    "\r\nUser-Agent: hotpath/1.0\r\nAuthorization: " + authorization +
                    "\r\nContent-Type: " + content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: keep-alive\r\n\r\n";
  req += body;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool reused = fd_ >= 0;
    if (!reused && !connect_()) return {};
    HttpResponse r = round_trip_(req);
    if (r.status != 0) return r;
    close_();
    if (!reused) return r;  // fresh connection failed: let the caller's retry policy decide
    // A kept-alive socket may have been closed by the peer; retry once on a new connection.
  }
  return {};
}

// ---- metrics server -----------------------------------------------------------------------------------

MetricsServer::MetricsServer(const std::string& bind_addr, int port, Render render, Ready ready)
    : render_(std::move(render)), ready_(std::move(ready)) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error("metrics: socket() failed");
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
    ::close(listen_fd_);
    throw std::runtime_error("metrics: bad bind address '" + bind_addr + "'");
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd_, 16) != 0) {
    const std::string err = std::strerror(errno);
    ::close(listen_fd_);
    throw std::runtime_error("metrics: cannot listen on " + bind_addr + ":" + std::to_string(port) + ": " + err);
  }
  socklen_t len = sizeof addr;
  getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  thread_ = std::thread([this] { serve_(); });
}

MetricsServer::~MetricsServer() { stop(); }

void MetricsServer::stop() {
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  listen_fd_ = -1;
}

void MetricsServer::serve_() {
  while (!stop_.load()) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    if (::poll(&pfd, 1, 100) <= 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    std::string req;
    char tmp[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
      const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
      if (n <= 0) break;
      req.append(tmp, static_cast<std::size_t>(n));
    }
    int status = 404;
    std::string body = "not found\n", type = "text/plain";
    const bool is_get = req.rfind("GET ", 0) == 0;
    const std::string target = is_get ? req.substr(4, req.find(' ', 4) - 4) : "";
    if (is_get && target == "/metrics") {
      status = 200;
      body = render_();
      type = "text/plain; version=0.0.4; charset=utf-8";
    } else if (is_get && target == "/healthz") {
      status = 200;
      body = "ok\n";
    } else if (is_get && target == "/readyz") {
      const bool ready = ready_();
      status = ready ? 200 : 503;
      body = ready ? "ready\n" : "not ready\n";
    } else if (!is_get) {
      status = 405;
      body = "method not allowed\n";
    }
    const char* text = status == 200 ? "OK" : status == 503 ? "Service Unavailable" : status == 405 ? "Method Not Allowed" : "Not Found";
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + type +
                       "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    for (std::size_t sent = 0; sent < resp.size();) {
      const ssize_t n = ::send(fd, resp.data() + sent, resp.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) break;
      sent += static_cast<std::size_t>(n);
    }
    ::close(fd);
  }
}

}  // namespace hotpath
