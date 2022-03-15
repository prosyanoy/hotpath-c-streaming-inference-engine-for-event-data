#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

#include "hotpath/batch_pool.hpp"
#include "hotpath/mpmc_queue.hpp"
#include "hotpath/spsc_ring.hpp"

using namespace hotpath;

TEST(SpscRing, CapacityRoundsUpAndBoundsPushes) {
  SpscRing<int> r(5);
  EXPECT_EQ(r.capacity(), 8u);
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(r.try_push(i));
  EXPECT_FALSE(r.try_push(99));
  int v;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, WrapsAround) {
  SpscRing<int> r(4);
  int v;
  for (int round = 0; round < 100; ++round) {
    ASSERT_TRUE(r.try_push(round));
    ASSERT_TRUE(r.try_pop(v));
    ASSERT_EQ(v, round);
  }
  EXPECT_EQ(r.size_approx(), 0u);
}

TEST(SpscRing, ThreadedTransferPreservesOrder) {
  constexpr int kN = 500000;
  SpscRing<std::uint64_t> r(1024);
  std::thread prod([&] {
    for (std::uint64_t i = 0; i < kN; ++i)
      while (!r.try_push(i)) cpu_relax();
  });
  std::uint64_t expect = 0, v;
  while (expect < kN) {
    if (r.try_pop(v)) {
      ASSERT_EQ(v, expect);
      ++expect;
    }
  }
  prod.join();
}

TEST(MpmcQueue, FifoAndFullEmpty) {
  MpmcQueue<int> q(4);
  int v;
  EXPECT_FALSE(q.try_pop(v));
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(4));
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, i);
  }
}

TEST(MpmcQueue, ManyProducersManyConsumersLoseNothing) {
  constexpr int kProducers = 3, kConsumers = 3, kPer = 100000;
  MpmcQueue<std::uint32_t> q(256);
  std::atomic<int> producers_left{kProducers};
  std::vector<std::thread> threads;
  std::vector<std::vector<std::uint32_t>> got(kConsumers);
  for (int p = 0; p < kProducers; ++p)
    threads.emplace_back([&, p] {
      for (int i = 0; i < kPer; ++i)
        while (!q.try_push(static_cast<std::uint32_t>(p * kPer + i))) cpu_relax();
      producers_left.fetch_sub(1);
    });
  for (int c = 0; c < kConsumers; ++c)
    threads.emplace_back([&, c] {
      std::uint32_t v;
      for (;;) {
        const bool done = producers_left.load() == 0;
        if (q.try_pop(v)) got[c].push_back(v);
        else if (done) break;
      }
    });
  for (auto& t : threads) t.join();
  std::vector<int> seen(kProducers * kPer, 0);
  std::size_t total = 0;
  for (auto& g : got)
    for (auto v : g) {
      ++seen[v];
      ++total;
    }
  EXPECT_EQ(total, static_cast<std::size_t>(kProducers * kPer));
  for (int c : seen) ASSERT_EQ(c, 1);  // exactly once
}

TEST(BatchPool, AcquireReleaseCyclesAndExhausts) {
  BatchPool pool(3, 16);
  Batch* a = pool.acquire();
  Batch* b = pool.acquire();
  Batch* c = pool.acquire();
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ(std::set<Batch*>({a, b, c}).size(), 3u);
  EXPECT_EQ(pool.acquire(), nullptr);
  EXPECT_EQ(pool.free_approx(), 0u);

  Event e;
  for (std::size_t i = 0; i < kFeatures; ++i) e.features[i] = static_cast<float>(i + 1);
  e.id = 7;
  a->append(e);
  EXPECT_EQ(a->count, 1u);
  EXPECT_EQ(a->features[3], 4.0f);
  EXPECT_EQ(a->meta[0].id, 7u);
  pool.release(a);
  Batch* again = pool.acquire();
  EXPECT_EQ(again, a);
  EXPECT_EQ(again->count, 0u);  // reset on acquire
}

TEST(BatchPool, BatchesDoNotOverlap) {
  BatchPool pool(4, 8);
  std::vector<Batch*> bs;
  while (Batch* b = pool.acquire()) bs.push_back(b);
  ASSERT_EQ(bs.size(), 4u);
  Event e;
  for (std::size_t i = 0; i < bs.size(); ++i) {
    for (int r = 0; r < 8; ++r) {
      for (auto& f : e.features) f = static_cast<float>(i);
      bs[i]->append(e);
    }
    EXPECT_TRUE(bs[i]->full());
  }
  for (std::size_t i = 0; i < bs.size(); ++i)
    for (int r = 0; r < 8; ++r)
      for (std::size_t f = 0; f < kFeatures; ++f) ASSERT_EQ(bs[i]->features[r * kFeatures + f], static_cast<float>(i));
}
