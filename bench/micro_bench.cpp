// Google Benchmark micro-benchmarks for the building blocks of the hot path.
#include <benchmark/benchmark.h>

#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

#include "hotpath/batch_pool.hpp"
#include "hotpath/generator.hpp"
#include "hotpath/histogram.hpp"
#include "hotpath/model.hpp"
#include "hotpath/mpmc_queue.hpp"
#include "hotpath/spsc_ring.hpp"

using namespace hotpath;

static void BM_SpscRing_PushPop(benchmark::State& st) {
  SpscRing<Event> r(1024);
  Event e, o;
  for (auto _ : st) {
    r.try_push(e);
    benchmark::DoNotOptimize(r.try_pop(o));
  }
  st.SetItemsProcessed(st.iterations());
}
BENCHMARK(BM_SpscRing_PushPop);

// One producer thread and one consumer thread moving events through the ring.
static void BM_SpscRing_TwoThreads(benchmark::State& st) {
  static SpscRing<Event>* ring;
  static std::atomic<bool> stop;
  static std::thread consumer;
  if (st.thread_index() == 0) {
    ring = new SpscRing<Event>(65536);
    stop = false;
    consumer = std::thread([] {
      Event o;
      while (!stop.load(std::memory_order_relaxed)) ring->try_pop(o);
    });
  }
  Event e;
  for (auto _ : st)
    while (!ring->try_push(e)) cpu_relax();
  st.SetItemsProcessed(st.iterations());
  stop = true;
  consumer.join();
  delete ring;
}
BENCHMARK(BM_SpscRing_TwoThreads)->UseRealTime();

static void BM_MpmcQueue_PushPop(benchmark::State& st) {
  MpmcQueue<Batch*> q(1024);
  Batch* b = nullptr;
  Batch* o;
  for (auto _ : st) {
    q.try_push(b);
    benchmark::DoNotOptimize(q.try_pop(o));
  }
  st.SetItemsProcessed(st.iterations());
}
BENCHMARK(BM_MpmcQueue_PushPop);

// Contended: N threads each push then pop on a shared queue.
static void BM_MpmcQueue_Contended(benchmark::State& st) {
  static MpmcQueue<std::uint64_t>* q;
  if (st.thread_index() == 0) q = new MpmcQueue<std::uint64_t>(4096);
  std::uint64_t v = 0;
  for (auto _ : st) {
    q->try_push(v);
    benchmark::DoNotOptimize(q->try_pop(v));
  }
  st.SetItemsProcessed(st.iterations());
  if (st.thread_index() == 0) delete q;
}
BENCHMARK(BM_MpmcQueue_Contended)->Threads(1)->Threads(2)->UseRealTime();

static void BM_BatchPool_AcquireRelease(benchmark::State& st) {
  BatchPool pool(64, 256);
  for (auto _ : st) {
    Batch* b = pool.acquire();
    benchmark::DoNotOptimize(b);
    pool.release(b);
  }
  st.SetItemsProcessed(st.iterations());
}
BENCHMARK(BM_BatchPool_AcquireRelease);

static void BM_Batch_Append(benchmark::State& st) {
  BatchPool pool(2, 256);
  Batch* b = pool.acquire();
  Event e;
  for (auto _ : st) {
    b->count = 0;
    for (int i = 0; i < 256; ++i) b->append(e);
    benchmark::ClobberMemory();
  }
  st.SetItemsProcessed(st.iterations() * 256);
}
BENCHMARK(BM_Batch_Append);

static void BM_Generator(benchmark::State& st) {
  SyntheticGenerator g(1, 0.01);
  Event e;
  for (auto _ : st) {
    g.next(e);
    benchmark::DoNotOptimize(e);
  }
  st.SetItemsProcessed(st.iterations());
}
BENCHMARK(BM_Generator);

static void BM_Histogram_Record(benchmark::State& st) {
  Histogram h;
  std::uint64_t v = 1000;
  for (auto _ : st) h.record(v += 37);
  st.SetItemsProcessed(st.iterations());
}
BENCHMARK(BM_Histogram_Record);

// ONNX Runtime scoring of one batch (needs `make train`); the Python equivalent is in bench/compare.py.
static void BM_Inference(benchmark::State& st) {
  const std::string model = HOTPATH_BENCH_MODEL;
  if (!std::ifstream(model)) {
    st.SkipWithError("model not found; run `make train`");
    return;
  }
  Scorer scorer(model, ModelMeta::load(model.substr(0, model.rfind('.')) + ".meta"));
  const auto n = static_cast<std::size_t>(st.range(0));
  std::vector<float> x(n * kFeatures), s(n);
  SyntheticGenerator g(1, 0.01);
  for (std::size_t i = 0; i < n; ++i) g.next(&x[i * kFeatures]);
  for (auto _ : st) {
    scorer.score(x.data(), n, s.data());
    benchmark::DoNotOptimize(s.data());
  }
  st.SetItemsProcessed(st.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_Inference)->Arg(1)->Arg(32)->Arg(256)->Arg(1024);

BENCHMARK_MAIN();
