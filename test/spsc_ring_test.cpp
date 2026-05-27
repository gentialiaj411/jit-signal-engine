// spsc_ring_test.cpp
//
// Unit + stress test for `src/spsc_ring.h`. Three things are gated:
//
//   1. Single-thread shape: try_push / try_pop behave like a FIFO,
//      full and empty boundaries are reported correctly, the ring
//      wraps cleanly past its capacity.
//   2. Concurrent stress: one producer thread pushes a known
//      sequence of N values; one consumer thread pops them. The
//      consumer must see EXACTLY the producer's sequence in order
//      (FIFO), with no drops, no duplicates, no reordering. This
//      gates the acquire/release pairing.
//   3. Alloc discipline: after construction, no hot-path call
//      (try_push, try_pop, size_approx) may allocate. The
//      hot_path_allocation_test pattern (global new override) is
//      reused.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#include "spsc_ring.h"

namespace {

// Local allocation tracker -- same shape as hot_path_allocation_test
// but kept self-contained so this test can run independently.
std::atomic<std::uint64_t> g_allocations{0};
thread_local bool g_count_allocations = false;

struct AllocCountScope {
  explicit AllocCountScope(bool enabled) : prev(g_count_allocations) { g_count_allocations = enabled; }
  ~AllocCountScope() { g_count_allocations = prev; }
  bool prev;
};

#define JITSE_CHECK(cond)                                                                                              \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "JITSE_CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
      std::abort();                                                                                                    \
    }                                                                                                                  \
  } while (0)

void TestSingleThreadFifo() {
  jitse::SpscRing<int, 8> ring;
  JITSE_CHECK(ring.empty_approx());
  JITSE_CHECK(!ring.full_approx());

  for (int i = 0; i < 8; ++i) JITSE_CHECK(ring.try_push(i));
  JITSE_CHECK(ring.full_approx());
  JITSE_CHECK(!ring.try_push(99));

  int out = -1;
  for (int i = 0; i < 8; ++i) {
    JITSE_CHECK(ring.try_pop(out));
    JITSE_CHECK(out == i);
  }
  JITSE_CHECK(ring.empty_approx());
  JITSE_CHECK(!ring.try_pop(out));

  // Wrap test: push/pop 100 elements with capacity 8.
  for (int i = 0; i < 100; ++i) {
    JITSE_CHECK(ring.try_push(i * 7));
    int popped = -1;
    JITSE_CHECK(ring.try_pop(popped));
    JITSE_CHECK(popped == i * 7);
  }
  std::printf("  single_thread_fifo: pass\n");
}

void TestConcurrentStress() {
  // Capacity intentionally small (16) relative to message count (1M)
  // so the ring fills and drains many times in the run. Any ordering
  // bug or dropped slot would manifest as a sequence mismatch on the
  // consumer side.
  constexpr std::size_t kN = 16;
  constexpr std::uint64_t kMessages = 1'000'000;
  jitse::SpscRing<std::uint64_t, kN> ring;

  std::atomic<bool> consumer_failed{false};
  std::atomic<std::uint64_t> consumer_last_seen{0};

  std::thread consumer([&] {
    std::uint64_t expected = 0;
    std::uint64_t v = 0;
    while (expected < kMessages) {
      if (ring.try_pop(v)) {
        if (v != expected) {
          consumer_failed.store(true);
          std::fprintf(stderr, "consumer got %llu, expected %llu\n",
                       static_cast<unsigned long long>(v),
                       static_cast<unsigned long long>(expected));
          return;
        }
        ++expected;
        consumer_last_seen.store(v, std::memory_order_relaxed);
      }
    }
  });

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      while (!ring.try_push(i)) {
        // pause to give consumer time to drain
      }
    }
  });

  producer.join();
  consumer.join();

  JITSE_CHECK(!consumer_failed.load());
  JITSE_CHECK(consumer_last_seen.load() == kMessages - 1);
  std::printf("  concurrent_stress: pass (1M messages, ring capacity %zu)\n", kN);
}

void TestNoAllocOnHotPath() {
  // Construct the ring OUTSIDE the alloc-counted scope. Inside the
  // scope, only push/pop calls happen; if any hot-path call
  // allocates we fail.
  jitse::SpscRing<std::uint64_t, 64> ring;
  // Drain into existence so the slots array is touched before
  // counting starts (initial first-touch wouldn't allocate either,
  // but this rules out any first-use lazy work).
  std::uint64_t out = 0;
  ring.try_push(1);
  ring.try_pop(out);

  g_allocations.store(0, std::memory_order_relaxed);
  {
    AllocCountScope scope(true);
    // 100k push/pop pairs interleaved.
    for (std::uint64_t i = 0; i < 100'000; ++i) {
      JITSE_CHECK(ring.try_push(i));
      JITSE_CHECK(ring.try_pop(out));
      JITSE_CHECK(out == i);
      (void)ring.size_approx();
      (void)ring.empty_approx();
      (void)ring.full_approx();
    }
  }
  const std::uint64_t allocs = g_allocations.load();
  if (allocs != 0) {
    std::fprintf(stderr, "no_alloc_on_hot_path: %llu allocations on hot path\n",
                 static_cast<unsigned long long>(allocs));
    std::abort();
  }
  std::printf("  no_alloc_on_hot_path: pass (0 allocations across 100000 push/pop pairs)\n");
}

}  // namespace

void* operator new(std::size_t sz) {
  if (g_count_allocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(sz)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t sz) {
  if (g_count_allocations) g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(sz)) return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }

int main() {
  std::printf("spsc_ring_test:\n");
  TestSingleThreadFifo();
  TestConcurrentStress();
  TestNoAllocOnHotPath();
  std::printf("spsc_ring_test: PASSED\n");
  return 0;
}
