#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <limits>
#include <thread>

#include "fake_hec.hpp"
#include "hotpath/hec_sink.hpp"
#include "hotpath/http.hpp"
#include "hotpath/tcp_source.hpp"

using namespace hotpath;

TEST(ParseUrl, AcceptsHttpForms) {
  auto u = parse_url("http://hec.example:8088/services/collector/event");
  EXPECT_EQ(u.host, "hec.example");
  EXPECT_EQ(u.port, 8088);
  EXPECT_EQ(u.path, "/services/collector/event");
  u = parse_url("http://localhost");
  EXPECT_EQ(u.port, 80);
  EXPECT_EQ(u.path, "/");
}

TEST(ParseUrl, RejectsBadUrls) {
  EXPECT_THROW(parse_url("https://x/y"), std::invalid_argument);
  EXPECT_THROW(parse_url("ftp://x"), std::invalid_argument);
  EXPECT_THROW(parse_url("http://:80/"), std::invalid_argument);
  EXPECT_THROW(parse_url("http://h:99999/"), std::invalid_argument);
  EXPECT_THROW(parse_url("http://h:abc/"), std::invalid_argument);
}

TEST(HecFormat, ProducesValidEnvelope) {
  Config cfg;
  cfg.hec_source = "src\"x";
  Alert a;
  a.id = 12;
  a.host = 3;
  a.unix_ns = 1730000000'123456789ull;
  a.score = -0.25f;
  for (std::size_t i = 0; i < kFeatures; ++i) a.features[i] = static_cast<float>(i) + 0.5f;
  std::string out;
  append_hec_event(out, a, cfg);
  EXPECT_EQ(out.rfind("{\"time\":1730000000.123,\"host\":\"host-3\",\"source\":\"src\\\"x\"", 0), 0u) << out;
  EXPECT_NE(out.find("\"sourcetype\":\"hotpath:anomaly\",\"index\":\"main\""), std::string::npos);
  EXPECT_NE(out.find("\"event\":{\"id\":12,\"score\":-0.25,\"features\":{\"cpu_pct\":0.5,"), std::string::npos);
  EXPECT_NE(out.find("\"conn_count\":7.5}}}"), std::string::npos);
  a.score = std::numeric_limits<float>::quiet_NaN();
  out.clear();
  append_hec_event(out, a, cfg);
  EXPECT_NE(out.find("\"score\":null"), std::string::npos);  // never emit invalid JSON
}

TEST(HttpClient, PostsWithAuthAndReusesConnection) {
  FakeHec hec("secret");
  HttpClient c(parse_url(hec.url()), 1000);
  auto r1 = c.post("{\"a\":1}", "Splunk secret");
  auto r2 = c.post("{\"a\":2}", "Splunk secret");
  EXPECT_EQ(r1.status, 200);
  EXPECT_EQ(r2.status, 200);
  EXPECT_NE(r1.body.find("Success"), std::string::npos);
  EXPECT_EQ(hec.bodies().size(), 2u);
  EXPECT_EQ(c.post("{}", "Splunk wrong").status, 401);
}

TEST(HttpClient, ReportsTransportErrorWhenNobodyListens) {
  int port;
  {
    FakeHec hec;
    port = hec.port();
  }
  HttpClient c(Url{"127.0.0.1", port, "/"}, 200);
  EXPECT_EQ(c.post("x", "a").status, 0);
}

namespace {
struct SinkFixture {
  Config cfg;
  Metrics metrics{1};
  MpmcQueue<Alert> q{1024};
  std::atomic<bool> done{false};
  void push(int n) {
    for (int i = 0; i < n; ++i) {
      Alert a;
      a.id = static_cast<std::uint64_t>(i);
      a.unix_ns = 1730000000'000000000ull;
      ASSERT_TRUE(q.try_push(a));
    }
  }
};
}  // namespace

TEST(HecSink, BatchesAlertsIntoConcatenatedBodies) {
  FakeHec hec("tok");
  SinkFixture f;
  f.cfg.hec_url = hec.url();
  f.cfg.hec_token = "tok";
  f.cfg.hec_batch = 10;
  f.push(25);
  f.done = true;
  HecSink sink(f.cfg, f.q, f.metrics);
  sink.run(f.done);
  EXPECT_EQ(hec.events(), 25u);
  EXPECT_EQ(hec.requests(), 3);  // 10 + 10 + 5
  EXPECT_EQ(f.metrics.alerts_sent.load(), 25u);
  EXPECT_EQ(hec.auths()[0], "Splunk tok");
}

TEST(HecSink, RetriesTransientFailures) {
  FakeHec hec("tok");
  hec.fail_next = 2;
  SinkFixture f;
  f.cfg.hec_url = hec.url();
  f.cfg.hec_token = "tok";
  f.push(5);
  f.done = true;
  HecSink sink(f.cfg, f.q, f.metrics);
  sink.run(f.done);
  EXPECT_EQ(hec.events(), 5u);
  EXPECT_EQ(f.metrics.hec_retries.load(), 2u);
  EXPECT_EQ(f.metrics.alerts_sent.load(), 5u);
  EXPECT_EQ(f.metrics.alerts_failed.load(), 0u);
}

TEST(HecSink, GivesUpAfterMaxAttemptsAndCountsFailures) {
  FakeHec hec("tok");
  hec.fail_next = 100;
  SinkFixture f;
  f.cfg.hec_url = hec.url();
  f.cfg.hec_token = "tok";
  f.cfg.hec_max_attempts = 2;
  f.push(4);
  f.done = true;
  HecSink sink(f.cfg, f.q, f.metrics);
  sink.run(f.done);
  EXPECT_EQ(f.metrics.alerts_failed.load(), 4u);
  EXPECT_EQ(f.metrics.alerts_sent.load(), 0u);
}

TEST(HecSink, BadTokenIsNotRetried) {
  FakeHec hec("tok");
  SinkFixture f;
  f.cfg.hec_url = hec.url();
  f.cfg.hec_token = "nope";
  f.push(3);
  f.done = true;
  HecSink sink(f.cfg, f.q, f.metrics);
  sink.run(f.done);
  EXPECT_EQ(hec.requests(), 1);
  EXPECT_EQ(f.metrics.hec_retries.load(), 0u);
  EXPECT_EQ(f.metrics.alerts_failed.load(), 3u);
}

TEST(HecSink, RejectsHttpsUrlAtConstruction) {
  SinkFixture f;
  f.cfg.hec_url = "https://splunk:8088/services/collector";
  EXPECT_THROW(HecSink(f.cfg, f.q, f.metrics), std::invalid_argument);
}

TEST(HecSink, DisabledSinkDrainsQueue) {
  SinkFixture f;
  f.cfg.hec_url = "off";
  f.push(7);
  f.done = true;
  HecSink sink(f.cfg, f.q, f.metrics);
  sink.run(f.done);
  EXPECT_EQ(f.metrics.alerts_dropped.load(), 7u);
}

namespace {
std::string http_get(int port, const std::string& path) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return "";
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string out;
  char b[4096];
  ssize_t n;
  while ((n = ::recv(fd, b, sizeof b, 0)) > 0) out.append(b, static_cast<size_t>(n));
  ::close(fd);
  return out;
}
}  // namespace

TEST(MetricsServer, ServesMetricsHealthAndReadiness) {
  std::atomic<bool> ready{false};
  MetricsServer s("127.0.0.1", 0, [] { return std::string("hotpath_x 1\n"); }, [&] { return ready.load(); });
  ASSERT_GT(s.port(), 0);
  auto m = http_get(s.port(), "/metrics");
  EXPECT_NE(m.find("200 OK"), std::string::npos);
  EXPECT_NE(m.find("version=0.0.4"), std::string::npos);
  EXPECT_NE(m.find("hotpath_x 1"), std::string::npos);
  EXPECT_NE(http_get(s.port(), "/healthz").find("200 OK"), std::string::npos);
  EXPECT_NE(http_get(s.port(), "/readyz").find("503"), std::string::npos);
  ready = true;
  EXPECT_NE(http_get(s.port(), "/readyz").find("200 OK"), std::string::npos);
  EXPECT_NE(http_get(s.port(), "/nope").find("404"), std::string::npos);
}

TEST(TcpSource, ParseEventLine) {
  auto e = parse_event_line("12,1,2,3,4,5,6,7,8.5\r");
  ASSERT_TRUE(e);
  EXPECT_EQ(e->host, 12u);
  EXPECT_FLOAT_EQ(e->features[0], 1.0f);
  EXPECT_FLOAT_EQ(e->features[7], 8.5f);
  EXPECT_TRUE(parse_event_line("web-1,1,2,3,4,5,6,7,8"));  // non-numeric hosts are hashed
  EXPECT_FALSE(parse_event_line("1,2,3"));                    // too few
  EXPECT_FALSE(parse_event_line("1,1,2,3,4,5,6,7,8,9"));      // too many
  EXPECT_FALSE(parse_event_line("1,1,2,3,4,5,6,7,abc"));
  EXPECT_FALSE(parse_event_line("1,1,2,3,4,5,6,7,nan"));
  EXPECT_FALSE(parse_event_line("1,1,2,3,4,5,6,7,"));
  EXPECT_FALSE(parse_event_line(""));
}

TEST(TcpSource, ReceivesLinesAcrossPacketsAndCountsMalformed) {
  std::atomic<bool> stop{false};
  std::atomic<int> port{0};
  std::vector<Event> got;
  int bad = 0;
  std::thread t([&] {
    run_tcp_source(0, stop, [&](const Event& e) { got.push_back(e); }, [&] { ++bad; }, [&](int p) { port = p; });
  });
  while (port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port.load()));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
  auto send_str = [&](const char* s) { ::send(fd, s, std::strlen(s), 0); };
  send_str("1,1,2,3,4,5,6,7,8\n2,9,9,9,");           // second line split across sends
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  send_str("9,9,9,9,9\ngarbage\n3,1,1,1,1,1,1,1,1\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ::close(fd);
  stop = true;
  t.join();
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].host, 1u);
  EXPECT_EQ(got[1].host, 2u);
  EXPECT_FLOAT_EQ(got[1].features[7], 9.0f);
  EXPECT_EQ(got[2].host, 3u);
  EXPECT_EQ(bad, 1);
}
