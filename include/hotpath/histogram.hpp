#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

namespace hotpath {

/// Plain (non-atomic) snapshot of a Histogram; mergeable across per-worker histograms.
struct HistogramSnapshot {
  static constexpr int kSubBits = 4;  // 16 sub-buckets per power of two => <= 6.25% bucket width
  static constexpr int kBuckets = 640;

  std::array<std::uint64_t, kBuckets> counts{};
  std::uint64_t count = 0;
  std::uint64_t sum = 0;
  std::uint64_t max = 0;

  /// Smallest value in bucket `idx`; bucket idx covers [lower(idx), lower(idx + 1)).
  static std::uint64_t lower_bound(int idx) noexcept {
    if (idx < (1 << kSubBits)) return static_cast<std::uint64_t>(idx);
    const int k = idx - (1 << kSubBits);
    const int msb = k / (1 << kSubBits) + kSubBits;
    const std::uint64_t sub = static_cast<std::uint64_t>(k % (1 << kSubBits));
    return ((1ull << kSubBits) + sub) << (msb - kSubBits);
  }

  void merge(const HistogramSnapshot& o) noexcept {
    for (int i = 0; i < kBuckets; ++i) counts[i] += o.counts[i];
    count += o.count;
    sum += o.sum;
    if (o.max > max) max = o.max;
  }

  /// Value at quantile q in [0,1]; reports the upper edge of the containing bucket (never
  /// under-reports), capped by the exact observed maximum.
  std::uint64_t quantile(double q) const noexcept {
    if (count == 0) return 0;
    auto target = static_cast<std::uint64_t>(q * static_cast<double>(count));
    if (target >= count) target = count - 1;
    std::uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
      seen += counts[i];
      if (seen > target) {
        const std::uint64_t upper = i + 1 < kBuckets ? lower_bound(i + 1) - 1 : max;
        return upper < max ? upper : max;
      }
    }
    return max;
  }

  /// Number of observations <= `bound` (bucket-granular, conservative).
  std::uint64_t count_le(std::uint64_t bound) const noexcept {
    std::uint64_t n = 0;
    for (int i = 0; i < kBuckets; ++i) {
      const std::uint64_t upper = i + 1 < kBuckets ? lower_bound(i + 1) - 1 : max;
      if (upper > bound) break;
      n += counts[i];
    }
    return n;
  }
};

/// Lock-free log-linear histogram of non-negative integers (nanoseconds). record() is a handful of
/// relaxed atomic adds; give each writer thread its own instance to avoid cache-line contention.
class Histogram {
 public:
  void record(std::uint64_t v) noexcept {
    counts_[index(v)].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(v, std::memory_order_relaxed);
    std::uint64_t m = max_.load(std::memory_order_relaxed);
    while (v > m && !max_.compare_exchange_weak(m, v, std::memory_order_relaxed)) {
    }
  }

  HistogramSnapshot snapshot() const noexcept {
    HistogramSnapshot s;
    for (int i = 0; i < HistogramSnapshot::kBuckets; ++i) s.counts[i] = counts_[i].load(std::memory_order_relaxed);
    s.count = count_.load(std::memory_order_relaxed);
    s.sum = sum_.load(std::memory_order_relaxed);
    s.max = max_.load(std::memory_order_relaxed);
    return s;
  }

  static int index(std::uint64_t v) noexcept {
    constexpr int sb = HistogramSnapshot::kSubBits;
    if (v < (1u << sb)) return static_cast<int>(v);
    const int msb = 63 - std::countl_zero(v);
    const int idx = (1 << sb) + (msb - sb) * (1 << sb) + static_cast<int>((v >> (msb - sb)) & ((1u << sb) - 1));
    return idx < HistogramSnapshot::kBuckets ? idx : HistogramSnapshot::kBuckets - 1;
  }

 private:
  std::array<std::atomic<std::uint64_t>, HistogramSnapshot::kBuckets> counts_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_{0};
  std::atomic<std::uint64_t> max_{0};
};

}  // namespace hotpath
