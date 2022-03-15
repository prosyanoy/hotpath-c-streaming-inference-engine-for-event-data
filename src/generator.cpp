#include "hotpath/generator.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace hotpath {
namespace {

// Keep in sync with python/hotpath/synthetic.py.
constexpr double kMean[kFeatures] = {35.0, 55.0, 40.0, 500.0, 0.5, 200.0, 800.0, 120.0};
constexpr double kStd[kFeatures] = {8.0, 7.0, 8.0, 70.0, 0.25, 30.0, 120.0, 15.0};
constexpr double kUpper[kFeatures] = {100.0, 100.0, 1e9, 1e9, 1e9, 1e9, 1e9, 1e9};
constexpr double kAnomalyMinSigma = 5.0;
constexpr double kAnomalyMaxSigma = 9.0;
constexpr std::uint32_t kAnomalyMaxFeatures = 3;
constexpr std::uint32_t kHosts = 64;

std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

}  // namespace

Rng::Rng(std::uint64_t seed) noexcept {
  // splitmix64 expands the seed into the xoshiro state.
  for (auto& s : s_) {
    seed += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    s = z ^ (z >> 31);
  }
}

std::uint64_t Rng::next_u64() noexcept {
  const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return result;
}

double Rng::uniform() noexcept { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }

double Rng::normal() noexcept {
  if (has_spare_) {
    has_spare_ = false;
    return spare_;
  }
  double u1;
  do {
    u1 = uniform();
  } while (u1 <= 1e-300);
  const double u2 = uniform();
  const double r = std::sqrt(-2.0 * std::log(u1));
  const double theta = 6.283185307179586 * u2;
  spare_ = r * std::sin(theta);
  has_spare_ = true;
  return r * std::cos(theta);
}

std::uint32_t Rng::below(std::uint32_t n) noexcept {
  return static_cast<std::uint32_t>((next_u64() >> 32) * n >> 32);
}

bool SyntheticGenerator::next(float* features) noexcept {
  double v[kFeatures];
  for (std::size_t i = 0; i < kFeatures; ++i) v[i] = kMean[i] + kStd[i] * rng_.normal();
  const bool anomaly = rng_.uniform() < anomaly_rate_;
  if (anomaly) {
    std::uint32_t idx[kFeatures];
    std::iota(idx, idx + kFeatures, 0u);
    const std::uint32_t k = 1 + rng_.below(kAnomalyMaxFeatures);
    for (std::uint32_t j = 0; j < k; ++j) {  // partial Fisher-Yates: k distinct features
      std::swap(idx[j], idx[j + rng_.below(static_cast<std::uint32_t>(kFeatures) - j)]);
      const std::size_t c = idx[j];
      const double sigma = kAnomalyMinSigma + (kAnomalyMaxSigma - kAnomalyMinSigma) * rng_.uniform();
      // Features that would clip at zero when shifted down only shift up (see synthetic.py).
      const bool can_down = kMean[c] - kAnomalyMinSigma * kStd[c] > 0.0;
      const double sign = can_down && rng_.uniform() < 0.5 ? -1.0 : 1.0;
      v[c] = kMean[c] + sign * sigma * kStd[c];
    }
  }
  for (std::size_t i = 0; i < kFeatures; ++i) features[i] = static_cast<float>(std::clamp(v[i], 0.0, kUpper[i]));
  return anomaly;
}

bool SyntheticGenerator::next(Event& e) noexcept {
  e.id = counter_++;
  e.host = rng_.below(kHosts);
  return next(e.features);
}

}  // namespace hotpath
