// spsc_ring.h
//
// Lock-free Single-Producer / Single-Consumer ring buffer for the
// market-data ingest hot path.
//
// One thread (the "feed thread") calls `try_push`. A second, distinct
// thread (the "eval thread") calls `try_pop`. No other configuration
// is supported; the absence of contention between multiple producers
// (or multiple consumers) is what lets this implementation:
//   - use plain `acquire`/`release` semantics on the head/tail indices
//     (no CAS, no fetch_add),
//   - cache the opposing index in a producer-private (resp. consumer-
//     private) shadow that is read on EVERY hot-path call but
//     refreshed against the live atomic only when the cached value
//     says we are at the empty/full boundary,
//   - keep the slots themselves on dedicated cache lines so the
//     producer never has to invalidate a line the consumer is reading
//     (and vice versa).
//
// Memory order rationale (canonical SPSC pattern, Rigtorp / Vyukov
// SPSC-degenerate-of-MPMC):
//   * Producer publishes a slot before publishing the head: the
//     `slots_[h & mask] = value` write happens-before the
//     `head_.store(h+1, release)`. The consumer's
//     `head_.load(acquire)` synchronizes with that release, making
//     the slot data visible.
//   * Consumer publishes a freed slot before publishing the tail:
//     the `out = slots_[t & mask]` read happens-before the
//     `tail_.store(t+1, release)`. The producer's
//     `tail_.load(acquire)` synchronizes with that release.
//   * The cached-shadow read does not need any synchronization: it is
//     an over-conservative estimate. If the producer's cached_tail_
//     says the ring is full, we re-read the live tail_; if the live
//     tail_ says we're full too, we report full. We never report
//     "not full" when we are full (because the live read is always
//     done before declaring success).
//
// Why this matters: the benchmark in bench/spsc_jit_pipeline_bench.cpp
// pins producer and consumer on distinct physical cores and measures
// enqueue-to-signal-output latency. The cache-line padding is what
// keeps the head and tail false-sharing-free, which is the single
// largest factor in achieving stable sub-microsecond p99 latency on
// the consumer side.
//
// Allocation discipline: the ring's storage is a fixed std::array
// inside the object. Construction allocates nothing on the heap; no
// hot-path call allocates anything. This is what
// `test/spsc_ring_test.cpp` gates against the operator-new hook.

#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace jitse {

namespace detail {

// On x86 a cache line is 64 bytes on every shipping CPU. ARM big.LITTLE
// and Apple Silicon use 128 byte lines but x86 is what we benchmark on,
// so 64 is the right padding constant here. C++17's
// `std::hardware_destructive_interference_size` would be the portable
// answer but it's still a hint, not a guarantee, on most toolchains.
constexpr std::size_t kCacheLineBytes = 64;

}  // namespace detail

// A lock-free SPSC ring buffer with fixed capacity `N` (must be a
// power of two so the index-to-slot mapping is a single AND-mask,
// not a divmod). `T` should be trivially copyable for the hot path
// to be alloc-free; the ring stores by value, no indirection.
template <typename T, std::size_t N>
class SpscRing {
  static_assert(N >= 2, "SpscRing capacity must be >= 2");
  static_assert((N & (N - 1)) == 0, "SpscRing capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing requires T to be trivially copyable to keep the "
                "hot path branch- and alloc-free");

 public:
  static constexpr std::size_t kCapacity = N;
  static constexpr std::size_t kMask = N - 1;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer side. Returns true on success; false iff the ring is full.
  // Caller is expected to spin/back-off on false.
  inline bool try_push(const T& value) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    // First check against producer-private cached tail. If the cached
    // estimate already shows the ring as full, refresh from the live
    // atomic (acquire to synchronize with the consumer's slot release).
    if (h - cached_tail_ == N) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h - cached_tail_ == N) {
        return false;
      }
    }
    slots_[h & kMask] = value;
    // Release the slot publication to the consumer.
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns true on success and writes the popped value
  // into `out`; false iff the ring is empty. Caller spin/back-off on
  // false.
  inline bool try_pop(T& out) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    // First check against consumer-private cached head. If the cached
    // estimate already shows the ring as empty, refresh from the live
    // atomic (acquire to synchronize with the producer's slot release).
    if (t == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t == cached_head_) {
        return false;
      }
    }
    out = slots_[t & kMask];
    // Release the slot back to the producer.
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Snapshot helpers for diagnostics / tests. Not race-free against
  // concurrent push/pop; intended for steady-state inspection only.
  std::size_t size_approx() const noexcept {
    const std::size_t h = head_.load(std::memory_order_acquire);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    return h - t;
  }
  bool empty_approx() const noexcept { return size_approx() == 0; }
  bool full_approx() const noexcept { return size_approx() == N; }

 private:
  // Layout: every hot-path field is on its own cache line so the
  // producer and consumer never share a line, eliminating
  // false-sharing-induced cache-coherence traffic.
  //
  //   line 0: head_ (producer writes, consumer reads via acquire)
  //   line 1: cached_tail_ (producer-private)
  //   line 2: tail_ (consumer writes, producer reads via acquire)
  //   line 3: cached_head_ (consumer-private)
  //   line 4..: slots_ (shared, but each push/pop only touches the
  //            slot at its current index, not the index of the other
  //            side -- they only collide on the very last slot when
  //            the ring is near-full or near-empty).
  alignas(detail::kCacheLineBytes) std::atomic<std::size_t> head_{0};
  alignas(detail::kCacheLineBytes) std::size_t cached_tail_{0};
  alignas(detail::kCacheLineBytes) std::atomic<std::size_t> tail_{0};
  alignas(detail::kCacheLineBytes) std::size_t cached_head_{0};
  alignas(detail::kCacheLineBytes) std::array<T, N> slots_{};
};

}  // namespace jitse
