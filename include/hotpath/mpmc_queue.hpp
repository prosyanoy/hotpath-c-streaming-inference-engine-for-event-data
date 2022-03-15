#pragma once
#include <atomic>
#include <memory>

#include "hotpath/common.hpp"

namespace hotpath {

/// Bounded multi-producer / multi-consumer queue (Dmitry Vyukov's sequence-stamped ring).
///
/// Each cell carries a sequence number that tells producers and consumers whose turn it is, so
/// the only contended operations are one CAS on the enqueue or dequeue index. Lock-free, no
/// allocation after construction. T is stored by value and must be cheap to copy.
template <class T>
class MpmcQueue {
 public:
  explicit MpmcQueue(std::size_t capacity)
      : capacity_(round_up_pow2(capacity)), mask_(capacity_ - 1), cells_(new Cell[capacity_]) {
    for (std::size_t i = 0; i < capacity_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }

  MpmcQueue(const MpmcQueue&) = delete;
  MpmcQueue& operator=(const MpmcQueue&) = delete;

  bool try_push(const T& value) noexcept {
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    Cell* cell;
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (diff < 0) {
        return false;  // full
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
    cell->value = value;
    cell->seq.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    Cell* cell;
    for (;;) {
      cell = &cells_[pos & mask_];
      const std::size_t seq = cell->seq.load(std::memory_order_acquire);
      const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      } else if (diff < 0) {
        return false;  // empty
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
    out = cell->value;
    cell->seq.store(pos + mask_ + 1, std::memory_order_release);
    return true;
  }

  std::size_t size_approx() const noexcept {
    const std::size_t e = enqueue_pos_.load(std::memory_order_relaxed);
    const std::size_t d = dequeue_pos_.load(std::memory_order_relaxed);
    return e >= d ? e - d : 0;
  }
  std::size_t capacity() const noexcept { return capacity_; }

 private:
  struct Cell {
    std::atomic<std::size_t> seq;
    T value;
  };

  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
};

}  // namespace hotpath
