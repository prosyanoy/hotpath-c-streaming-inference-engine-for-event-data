#include "hotpath/tcp_source.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hotpath {
namespace {

constexpr std::size_t kMaxLine = 4096;
constexpr std::size_t kMaxConns = 256;

std::uint32_t fnv1a(std::string_view s) {
  std::uint32_t h = 2166136261u;
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

struct Conn {
  int fd;
  std::string pending;
};

}  // namespace

std::optional<Event> parse_event_line(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
  Event e;
  std::size_t field = 0;
  while (true) {
    const auto comma = line.find(',');
    const std::string_view tok = line.substr(0, comma);
    if (tok.empty() || field > kFeatures) return std::nullopt;
    if (field == 0) {
      std::uint32_t host = 0;
      auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), host);
      e.host = (ec == std::errc() && p == tok.data() + tok.size()) ? host : fnv1a(tok);
    } else {
      float v = 0;
      auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
      if (ec != std::errc() || p != tok.data() + tok.size() || !std::isfinite(v)) return std::nullopt;
      e.features[field - 1] = v;
    }
    ++field;
    if (comma == std::string_view::npos) break;
    line.remove_prefix(comma + 1);
  }
  if (field != kFeatures + 1) return std::nullopt;
  return e;
}

void run_tcp_source(int port, const std::atomic<bool>& stop, const std::function<void(const Event&)>& emit,
                    const std::function<void()>& on_malformed, const std::function<void(int)>& ready) {
  const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) throw std::runtime_error("tcp source: socket() failed");
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(lfd, 64) != 0) {
    ::close(lfd);
    throw std::runtime_error("tcp source: cannot listen on port " + std::to_string(port));
  }
  socklen_t len = sizeof addr;
  getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
  if (ready) ready(ntohs(addr.sin_port));

  std::vector<Conn> conns;
  std::vector<pollfd> fds;
  std::vector<char> buf(1 << 16);
  while (!stop.load(std::memory_order_acquire)) {
    fds.assign(1, pollfd{lfd, POLLIN, 0});
    for (const Conn& c : conns) fds.push_back(pollfd{c.fd, POLLIN, 0});
    if (::poll(fds.data(), fds.size(), 50) <= 0) continue;

    if (fds[0].revents & POLLIN) {
      const int cfd = ::accept(lfd, nullptr, nullptr);
      if (cfd >= 0) {
        if (conns.size() >= kMaxConns) ::close(cfd);
        else conns.push_back(Conn{cfd, {}});
      }
    }
    for (std::size_t i = fds.size() - 1; i >= 1; --i) {  // reverse so erase() keeps indices valid
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      Conn& c = conns[i - 1];
      const ssize_t n = ::recv(c.fd, buf.data(), buf.size(), 0);
      bool drop = n <= 0;
      if (n > 0) {
        std::string_view chunk(buf.data(), static_cast<std::size_t>(n));
        while (!chunk.empty()) {
          const auto nl = chunk.find('\n');
          if (nl == std::string_view::npos) {
            c.pending.append(chunk);
            if (c.pending.size() > kMaxLine) {  // a line this long is garbage; hang up
              on_malformed();
              drop = true;
            }
            break;
          }
          std::string_view line = chunk.substr(0, nl);
          std::string joined;
          if (!c.pending.empty()) {
            joined = std::move(c.pending);
            joined.append(line);
            c.pending.clear();
            line = joined;
          }
          if (!line.empty()) {
            if (auto ev = parse_event_line(line)) emit(*ev);
            else on_malformed();
          }
          chunk.remove_prefix(nl + 1);
        }
      }
      if (drop) {
        ::close(c.fd);
        conns.erase(conns.begin() + static_cast<std::ptrdiff_t>(i - 1));
      }
    }
  }
  for (const Conn& c : conns) ::close(c.fd);
  ::close(lfd);
}

}  // namespace hotpath
