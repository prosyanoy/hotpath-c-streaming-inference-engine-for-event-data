#pragma once
// A tiny in-process HEC receiver for tests (sequential keep-alive connections).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class FakeHec {
 public:
  explicit FakeHec(std::string token = "tok") : token_(std::move(token)) {
    lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(lfd_, 8);
    socklen_t len = sizeof a;
    getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { serve(); });
  }
  ~FakeHec() {
    stop_ = true;
    thread_.join();
    ::close(lfd_);
  }

  int port() const { return port_; }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/services/collector/event"; }

  std::atomic<int> fail_next{0};  // answer this many requests with 503
  std::vector<std::string> bodies() {
    std::lock_guard<std::mutex> l(m_);
    return bodies_;
  }
  std::vector<std::string> auths() {
    std::lock_guard<std::mutex> l(m_);
    return auths_;
  }
  std::size_t events() {
    std::lock_guard<std::mutex> l(m_);
    return events_;
  }
  int requests() const { return requests_.load(); }

 private:
  void serve() {
    while (!stop_) {
      pollfd p{lfd_, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      int fd = ::accept(lfd_, nullptr, nullptr);
      if (fd < 0) continue;
      handle(fd);
      ::close(fd);
    }
  }
  void handle(int fd) {
    std::string buf;
    char tmp[8192];
    while (!stop_) {
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
      if (n <= 0) return;
      buf.append(tmp, static_cast<size_t>(n));
      for (;;) {
        auto he = buf.find("\r\n\r\n");
        if (he == std::string::npos) break;
        auto cl = buf.find("Content-Length: ");
        size_t len = cl == std::string::npos ? 0 : std::stoul(buf.substr(cl + 16));
        if (buf.size() < he + 4 + len) break;
        std::string head = buf.substr(0, he), body = buf.substr(he + 4, len);
        buf.erase(0, he + 4 + len);
        std::string auth;
        if (auto a = head.find("Authorization: "); a != std::string::npos)
          auth = head.substr(a + 15, head.find("\r\n", a) - a - 15);
        int status = 200;
        const char* text = "OK";
        if (auth != "Splunk " + token_) { status = 401; text = "Unauthorized"; }
        else if (fail_next.load() > 0) { --fail_next; status = 503; text = "Service Unavailable"; }
        if (status == 200) {
          std::lock_guard<std::mutex> l(m_);
          bodies_.push_back(body);
          auths_.push_back(auth);
          for (size_t pos = 0; (pos = body.find("{\"time\":", pos)) != std::string::npos; ++pos) ++events_;
        }
        ++requests_;
        std::string payload = status == 200 ? "{\"text\":\"Success\",\"code\":0}" : "{\"text\":\"err\"}";
        std::string r = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: application/json\r\nContent-Length: " +
                        std::to_string(payload.size()) + "\r\n\r\n" + payload;
        ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
      }
    }
  }

  std::string token_;
  int lfd_ = -1, port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> requests_{0};
  std::mutex m_;
  std::vector<std::string> bodies_, auths_;
  std::size_t events_ = 0;
  std::thread thread_;
};
