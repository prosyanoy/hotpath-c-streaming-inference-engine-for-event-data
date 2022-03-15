#include <gtest/gtest.h>

#include "hotpath/histogram.hpp"
#include "hotpath/metrics.hpp"

using namespace hotpath;

TEST(Histogram, IndexMonotonicAndBucketsContainValue) {
  int prev = -1;
  for (std::uint64_t v = 0; v < 5'000'000; v += (v < 100 ? 1 : 997)) {
    const int i = Histogram::index(v);
    ASSERT_GE(i, prev);
    prev = i;
    ASSERT_LE(HistogramSnapshot::lower_bound(i), v);
    if (i + 1 < HistogramSnapshot::kBuckets) {
      ASSERT_GT(HistogramSnapshot::lower_bound(i + 1), v);
    }
  }
}

TEST(Histogram, QuantilesWithinBucketError) {
  Histogram h;
  for (std::uint64_t v = 1; v <= 10000; ++v) h.record(v * 1000);  // 1us..10ms uniform
  const auto s = h.snapshot();
  EXPECT_EQ(s.count, 10000u);
  EXPECT_EQ(s.max, 10'000'000u);
  EXPECT_NEAR(static_cast<double>(s.quantile(0.50)), 5'000'000.0, 5'000'000.0 * 0.07);
  EXPECT_NEAR(static_cast<double>(s.quantile(0.99)), 9'900'000.0, 9'900'000.0 * 0.07);
  EXPECT_LE(s.quantile(1.0), s.max);
}

TEST(Histogram, MergeAddsUp) {
  Histogram a, b;
  a.record(100);
  b.record(1'000'000);
  auto s = a.snapshot();
  s.merge(b.snapshot());
  EXPECT_EQ(s.count, 2u);
  EXPECT_EQ(s.max, 1'000'000u);
  EXPECT_EQ(s.count_le(200), 1u);
  EXPECT_EQ(s.count_le(2'000'000), 2u);
}

TEST(Histogram, EmptyQuantileIsZero) { EXPECT_EQ(Histogram().snapshot().quantile(0.99), 0u); }

TEST(Metrics, PrometheusExpositionHasExpectedSeries) {
  Metrics m(2);
  m.events_scored.store(42);
  m.event_latency(0).record(150'000);  // 150us
  m.event_latency(1).record(3'000'000);
  Gauges g;
  g.pool_total_batches = 8;
  g.ready = true;
  const std::string text = m.render_prometheus(g);
  EXPECT_NE(text.find("# TYPE hotpath_events_scored_total counter\nhotpath_events_scored_total 42\n"), std::string::npos);
  EXPECT_NE(text.find("hotpath_pool_total_batches 8"), std::string::npos);
  EXPECT_NE(text.find("hotpath_ready 1"), std::string::npos);
  EXPECT_NE(text.find("hotpath_event_latency_seconds_bucket{le=\"0.00025\"} 1"), std::string::npos);
  EXPECT_NE(text.find("hotpath_event_latency_seconds_bucket{le=\"+Inf\"} 2"), std::string::npos);
  EXPECT_NE(text.find("hotpath_event_latency_seconds_count 2"), std::string::npos);
}
