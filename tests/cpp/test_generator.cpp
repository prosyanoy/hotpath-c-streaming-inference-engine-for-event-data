#include <gtest/gtest.h>

#include <cmath>

#include "hotpath/generator.hpp"

using namespace hotpath;

TEST(Rng, DeterministicForSeed) {
  Rng a(5), b(5), c(6);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(a.next_u64(), b.next_u64());
  EXPECT_NE(Rng(5).next_u64(), c.next_u64());
}

TEST(Rng, NormalHasUnitMoments) {
  Rng r(1);
  double sum = 0, sq = 0;
  constexpr int n = 200000;
  for (int i = 0; i < n; ++i) {
    const double x = r.normal();
    sum += x;
    sq += x * x;
  }
  EXPECT_NEAR(sum / n, 0.0, 0.02);
  EXPECT_NEAR(sq / n, 1.0, 0.03);
}

TEST(SyntheticGenerator, NormalSamplesMatchSpecAndAnomaliesAreFar) {
  SyntheticGenerator normal(3, 0.0), anomalous(3, 1.0);
  double cpu = 0;
  float f[kFeatures];
  constexpr int n = 50000;
  for (int i = 0; i < n; ++i) {
    ASSERT_FALSE(normal.next(f));
    cpu += f[0];
    for (float v : f) ASSERT_GE(v, 0.0f);
    ASSERT_LE(f[0], 100.0f);
  }
  EXPECT_NEAR(cpu / n, 35.0, 0.2);

  const double mean[] = {35, 55, 40, 500, 0.5, 200, 800, 120};
  const double sd[] = {8, 7, 8, 70, 0.25, 30, 120, 15};
  int far = 0;
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(anomalous.next(f));
    double maxz = 0;
    for (std::size_t k = 0; k < kFeatures; ++k) maxz = std::max(maxz, std::abs((f[k] - mean[k]) / sd[k]));
    far += maxz > 4.9;  // always a >= 5 sigma shift (float rounding aside)
  }
  EXPECT_GT(far, 1990);
}

TEST(SyntheticGenerator, AnomalyRateIsRespected) {
  SyntheticGenerator g(11, 0.05);
  float f[kFeatures];
  int hits = 0;
  constexpr int n = 100000;
  for (int i = 0; i < n; ++i) hits += g.next(f);
  EXPECT_NEAR(static_cast<double>(hits) / n, 0.05, 0.004);
}

TEST(SyntheticGenerator, EventsGetSequentialIdsAndHosts) {
  SyntheticGenerator g(1, 0.0);
  Event e;
  for (std::uint64_t i = 0; i < 10; ++i) {
    g.next(e);
    EXPECT_EQ(e.id, i);
    EXPECT_LT(e.host, 64u);
  }
}
