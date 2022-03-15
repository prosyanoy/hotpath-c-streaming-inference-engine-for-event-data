#pragma once
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "hotpath/event.hpp"
#include "hotpath/mpmc_queue.hpp"

namespace hotpath {

/// A fixed-capacity micro-batch: a row-major [count x kFeatures] float matrix (the exact layout the
/// ONNX input tensor wants, so inference reads it in place) plus per-row metadata.
struct Batch {
  float* features = nullptr;
  EventMeta* meta = nullptr;
  std::uint32_t count = 0;
  std::uint32_t capacity = 0;
  std::uint64_t opened_ns = 0;  // when the first event was added

  void append(const Event& e) noexcept {
    float* row = features + static_cast<std::size_t>(count) * kFeatures;
    for (std::size_t i = 0; i < kFeatures; ++i) row[i] = e.features[i];
    meta[count] = EventMeta{e.id, e.ingest_ns, e.host};
    ++count;
  }
  bool full() const noexcept { return count == capacity; }
};

/// Pre-allocated pool of batches. All memory is one aligned slab acquired at construction; at run
/// time batches circulate between the pool's lock-free free list and the pipeline, so the hot path
/// never calls malloc.
class BatchPool {
 public:
  BatchPool(std::size_t num_batches, std::size_t batch_capacity)
      : free_(num_batches), batches_(num_batches), total_(num_batches) {
    const std::size_t feat_bytes = num_batches * batch_capacity * kFeatures * sizeof(float);
    const std::size_t meta_bytes = num_batches * batch_capacity * sizeof(EventMeta);
    feat_bytes_ = round_up(feat_bytes);
    slab_.reset(static_cast<char*>(std::aligned_alloc(kCacheLine, feat_bytes_ + round_up(meta_bytes))));
    if (!slab_) throw std::bad_alloc();
    auto* feats = reinterpret_cast<float*>(slab_.get());
    auto* metas = reinterpret_cast<EventMeta*>(slab_.get() + feat_bytes_);
    for (std::size_t i = 0; i < num_batches; ++i) {
      Batch& b = batches_[i];
      b.features = feats + i * batch_capacity * kFeatures;
      b.meta = metas + i * batch_capacity;
      b.capacity = static_cast<std::uint32_t>(batch_capacity);
      free_.try_push(&b);
    }
  }

  /// Returns nullptr when every batch is in flight (the caller applies back-pressure).
  Batch* acquire() noexcept {
    Batch* b = nullptr;
    if (!free_.try_pop(b)) return nullptr;
    b->count = 0;
    return b;
  }
  void release(Batch* b) noexcept {
    // The free list holds exactly `total_` slots, so this cannot fail for batches from this pool.
    free_.try_push(b);
  }

  std::size_t total() const noexcept { return total_; }
  std::size_t free_approx() const noexcept { return free_.size_approx(); }

 private:
  static std::size_t round_up(std::size_t n) { return (n + kCacheLine - 1) / kCacheLine * kCacheLine; }
  struct FreeDeleter {
    void operator()(char* p) const noexcept { std::free(p); }
  };

  MpmcQueue<Batch*> free_;
  std::vector<Batch> batches_;
  std::unique_ptr<char, FreeDeleter> slab_;
  std::size_t feat_bytes_ = 0;
  std::size_t total_;
};

}  // namespace hotpath
