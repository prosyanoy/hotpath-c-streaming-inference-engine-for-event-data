#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace hotpath {

inline constexpr std::size_t kCacheLine = 64;

/// Monotonic nanoseconds; the clock used for every latency measurement.
inline std::uint64_t now_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// Wall-clock nanoseconds since the Unix epoch (for HEC "time").
inline std::uint64_t unix_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

/// Politely spin: hint the CPU that we are in a busy-wait loop.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

constexpr std::size_t round_up_pow2(std::size_t v) noexcept {
  std::size_t p = 2;
  while (p < v) p <<= 1;
  return p;
}

}  // namespace hotpath
