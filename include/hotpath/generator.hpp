#pragma once
#include <cstdint>

#include "hotpath/event.hpp"

namespace hotpath {

/// xoshiro256** PRNG with a Box-Muller normal sampler: fast, deterministic, platform independent.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept;
  std::uint64_t next_u64() noexcept;
  double uniform() noexcept;  // [0, 1)
  double normal() noexcept;   // N(0, 1)
  std::uint32_t below(std::uint32_t n) noexcept;

 private:
  std::uint64_t s_[4];
  double spare_ = 0;
  bool has_spare_ = false;
};

/// Synthetic telemetry matching python/hotpath/synthetic.py (same means, deviations, clipping and
/// anomaly injection), so the Python-trained model sees the distribution it was trained on.
class SyntheticGenerator {
 public:
  SyntheticGenerator(std::uint64_t seed, double anomaly_rate) noexcept : rng_(seed), anomaly_rate_(anomaly_rate) {}

  /// Fill `features` with one sample; returns true if an anomaly was injected.
  bool next(float* features) noexcept;
  /// Fill a full Event (id/host/features); `ingest_ns` is left for the caller.
  bool next(Event& e) noexcept;

 private:
  Rng rng_;
  double anomaly_rate_;
  std::uint64_t counter_ = 0;
};

}  // namespace hotpath
