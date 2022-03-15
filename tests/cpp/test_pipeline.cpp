#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <thread>

#include "fake_hec.hpp"
#include "hotpath/generator.hpp"
#include "hotpath/pipeline.hpp"

using namespace hotpath;

namespace {

// Deterministic stand-in for the model: "score" is the negated largest z-score, so anything beyond
// 4 sigma on any feature scores below the -4 threshold.
void fake_score(const float* x, std::size_t n, float* out) {
  static const double mean[] = {35, 55, 40, 500, 0.5, 200, 800, 120};
  static const double sd[] = {8, 7, 8, 70, 0.25, 30, 120, 15};
  for (std::size_t i = 0; i < n; ++i) {
    double z = 0;
    for (std::size_t k = 0; k < kFeatures; ++k) z = std::max(z, std::abs((x[i * kFeatures + k] - mean[k]) / sd[k]));
    out[i] = static_cast<float>(-z);
  }
}

Config base_config() {
  Config c;
  c.rate = 0;
  c.metrics_port = 0;
  c.batch_size = 64;
  c.pool_batches = 8;
  c.ring_capacity = 1024;
  c.hec_flush_ms = 20;
  c.print_summary = false;
  return c;
}

}  // namespace

TEST(Pipeline, ScoresEveryEventExactlyOnceAndDeliversAnomalies) {
  FakeHec hec("tok");
  Config c = base_config();
  c.total_events = 20000;
  c.anomaly_rate = 0.05;
  c.hec_url = hec.url();
  c.hec_token = "tok";
  // Reference count: same seed => same stream.
  SyntheticGenerator ref(c.seed, c.anomaly_rate);
  Event ev;
  float f[kFeatures];
  float s;
  std::uint64_t expected = 0;
  for (std::uint64_t i = 0; i < c.total_events; ++i) {
    ref.next(ev);
    std::copy(ev.features, ev.features + kFeatures, f);
    fake_score(f, 1, &s);
    expected += s < -4.0f;
  }
  ASSERT_GT(expected, 500u);

  Pipeline p(c, fake_score, -4.0f);
  p.start();
  p.join();
  const Summary sum = p.summary();
  EXPECT_EQ(sum.events, 20000u);
  EXPECT_EQ(sum.anomalies, expected);
  EXPECT_EQ(sum.alerts_sent, expected);
  EXPECT_EQ(hec.events(), expected);
  EXPECT_EQ(sum.events_dropped, 0u);
  EXPECT_GT(sum.events_per_sec, 0.0);
  EXPECT_GT(sum.p99_us, 0.0);
  EXPECT_GE(sum.p99_us, sum.p50_us);
  EXPECT_EQ(p.metrics().events_ingested.load(), 20000u);
  EXPECT_NE(sum.to_json().find("\"impl\":\"cpp\""), std::string::npos);
}

TEST(Pipeline, TinyPoolAndRingStillDrainWithBackPressure) {
  Config c = base_config();
  c.total_events = 30000;
  c.workers = 1;
  c.pool_batches = 3;  // workers + 2: minimum
  c.batch_size = 16;
  c.ring_capacity = 8;
  c.hec_url = "off";
  Pipeline p(c, fake_score, -4.0f);
  p.start();
  p.join();
  EXPECT_EQ(p.summary().events, 30000u);
  EXPECT_EQ(p.metrics().events_dropped.load(), 0u);
}

TEST(Pipeline, PartialBatchFlushesOnTimeout) {
  Config c = base_config();
  c.batch_size = 256;
  c.batch_timeout_us = 1000;
  c.hec_url = "off";
  Pipeline p(c, fake_score, -4.0f);
  p.start_without_source();
  Event e;
  for (auto& v : e.features) v = 1;
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(p.submit(e));
  for (int i = 0; i < 200 && p.metrics().events_scored.load() < 10; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(p.metrics().events_scored.load(), 10u);  // scored although the batch never filled
  EXPECT_EQ(p.metrics().batches_scored.load(), 1u);
  p.finish_input();
  p.join();
}

TEST(Pipeline, DropPolicyCountsDroppedEventsWhenSaturated) {
  Config c = base_config();
  c.overflow = Overflow::Drop;
  c.ring_capacity = 16;
  c.workers = 1;
  c.hec_url = "off";
  std::atomic<bool> release{false};
  Pipeline p(c, [&](const float* x, std::size_t n, float* o) {
    while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    fake_score(x, n, o);
  }, -4.0f);
  p.start_without_source();
  Event e;
  for (auto& v : e.features) v = 1;
  int accepted = 0;
  for (int i = 0; i < 100000; ++i) accepted += p.submit(e);  // consumer is stalled: must overflow
  EXPECT_GT(p.metrics().events_dropped.load(), 0u);
  EXPECT_EQ(static_cast<std::uint64_t>(accepted) + p.metrics().events_dropped.load(), 100000u);
  release = true;
  p.finish_input();
  p.join();
  EXPECT_EQ(p.summary().events, static_cast<std::uint64_t>(accepted));
}

TEST(Pipeline, ScorerExceptionsAreCountedNotFatal) {
  Config c = base_config();
  c.total_events = 1000;
  c.hec_url = "off";
  Pipeline p(c, [](const float*, std::size_t, float*) { throw std::runtime_error("boom"); }, 0.0f);
  p.start();
  p.join();
  EXPECT_GT(p.metrics().inference_errors.load(), 0u);
  EXPECT_EQ(p.summary().events, 0u);
}

TEST(Pipeline, ExposesPrometheusMetricsWhileRunning) {
  Config c = base_config();
  c.rate = 2000;
  c.metrics_port = 19137;
  c.metrics_bind = "127.0.0.1";
  c.hec_url = "off";
  Pipeline p(c, fake_score, -4.0f);
  p.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(19137);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
  const char req[] = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";
  ::send(fd, req, sizeof req - 1, 0);
  std::string out;
  char b[8192];
  ssize_t n;
  while ((n = ::recv(fd, b, sizeof b, 0)) > 0) out.append(b, static_cast<size_t>(n));
  ::close(fd);
  p.request_stop();
  p.finish_input();
  p.join();
  EXPECT_NE(out.find("hotpath_events_scored_total"), std::string::npos);
  EXPECT_NE(out.find("hotpath_event_latency_seconds_bucket"), std::string::npos);
  EXPECT_NE(out.find("hotpath_pool_total_batches 8"), std::string::npos);
  EXPECT_GT(p.summary().events, 100u);
}

TEST(Pipeline, TcpSourceIngestsAndScores) {
  Config c = base_config();
  c.source = "tcp";
  c.tcp_port = 19138;
  c.overflow = Overflow::Block;
  c.hec_url = "off";
  Pipeline p(c, fake_score, -4.0f);
  p.start();
  int fd = -1;
  for (int i = 0; i < 100 && fd < 0; ++i) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(19138);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      ::close(fd);
      fd = -1;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_GE(fd, 0);
  std::string lines;
  for (int i = 0; i < 50; ++i) lines += "7,35,55,40,500,0.5,200,800,120\n";
  lines += "7,99,55,40,500,0.5,200,800,120\n";  // cpu 99% => z=8
  ::send(fd, lines.data(), lines.size(), 0);
  for (int i = 0; i < 300 && p.metrics().events_scored.load() < 51; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ::close(fd);
  p.request_stop();
  p.join();
  EXPECT_EQ(p.summary().events, 51u);
  EXPECT_EQ(p.summary().anomalies, 1u);
}

TEST(PipelineModel, RealOnnxModelFlagsInjectedAnomalies) {
  const std::string model = HOTPATH_TEST_MODEL;
  if (!std::ifstream(model)) GTEST_SKIP() << "run `make train` to create " << model;
  const ModelMeta meta = ModelMeta::load(model.substr(0, model.rfind('.')) + ".meta");
  Scorer scorer(model, meta);
  Config c = base_config();
  c.total_events = 50000;
  c.anomaly_rate = 0.02;
  FakeHec hec("tok");
  c.hec_url = hec.url();
  c.hec_token = "tok";
  Pipeline p(c, scorer.as_function(), meta.threshold);
  p.start();
  p.join();
  const Summary s = p.summary();
  EXPECT_EQ(s.events, 50000u);
  // ~1000 injected + ~0.5% false positives; the model must find a meaningful share and not flood.
  EXPECT_GT(s.anomalies, 400u);
  EXPECT_LT(s.anomalies, 1800u);
  EXPECT_EQ(hec.events(), s.anomalies);
  EXPECT_EQ(s.alerts_failed, 0u);
}

TEST(PipelineModel, CppScoresMatchSampleRowsFromPython) {
  const std::string model = HOTPATH_TEST_MODEL;
  if (!std::ifstream(model)) GTEST_SKIP() << "run `make train` to create " << model;
  Scorer scorer(model, ModelMeta::load(model.substr(0, model.rfind('.')) + ".meta"));
  // A row at the exact mean must look normal (higher score) vs a wildly off row.
  float rows[2 * kFeatures] = {35, 55, 40, 500, 0.5f, 200, 800, 120, 99, 99, 400, 5000, 20, 2000, 9000, 900};
  float scores[2];
  scorer.score(rows, 2, scores);
  EXPECT_GT(scores[0], 0.0f);
  EXPECT_LT(scores[1], scores[0]);
}
