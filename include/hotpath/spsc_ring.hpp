#pragma once
#include <atomic>
#include <memory>

#include "hotpath/common.hpp"

namespace hotpath {

/// Bounded single-producer / single-consumer ring buffer (wait-free).
///
/// Head and tail live on separate cache lines and each side keeps a cached copy of the other's
/// index, so in steady state a push or pop touches only its own line.
/// T must be default-constructible and cheap to copy (it is stored by value, pre-allocated).
template <class T>
class SpscRing {
 public:
  explicit SpscRing(std::size_t capacity)
      : capacity_(round_up_pow2(capacity)), mask_(capacity_ - 1), slots_(new T[capacity_]) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  /// Producer thread only.
  bool try_push(const T& value) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == capacity_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == capacity_) return false;
    }
    slots_[tail & mask_] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Consumer thread only.
  bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return false;
    }
    out = slots_[head & mask_];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Approximate occupancy; safe from any thread (used for metrics only).
  std::size_t size_approx() const noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_relaxed);
    return t >= h ? t - h : 0;
  }
  std::size_t capacity() const noexcept { return capacity_; }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer-owned
  std::size_t tail_cache_{0};                              // consumer's view of tail_
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer-owned
  std::size_t head_cache_{0};                              // producer's view of head_
};

}  // namespace hotpath
