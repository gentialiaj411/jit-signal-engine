// sampling_profiler.cpp -- see sampling_profiler.h for design notes.

#include "sampling_profiler.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <signal.h>
#include <sys/time.h>
#include <unordered_map>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#if defined(__linux__)
#include <ucontext.h>
#endif

#ifdef __GNUG__
#include <cxxabi.h>
#endif

namespace jitse {

namespace {

// Global aggregator. We use a global because the SIGPROF handler runs in
// async-signal context and cannot capture `this`. We assume only one
// SamplingProfiler is active at a time; Start() enforces this.
struct GlobalProfilerState {
  std::atomic<bool> active{false};
  std::atomic<std::uint64_t> total_samples{0};
  // Lock-free spinlock guarding the map. The handler must NOT call malloc
  // or take a normal mutex (deadlock + AS-safety violation), so we use a
  // pre-sized map plus a sentinel that the handler increments without
  // touching std::string. The trick: the handler resolves IP -> address of
  // a stable C string (`Dl_info::dli_sname`), and we key the map by that
  // pointer (interned). dli_sname is owned by the dynamic linker for the
  // lifetime of the process for normally-loaded symbols.
  //
  // For JIT/unknown addresses we use two fixed sentinel keys.
  static constexpr const char* kJitBucket     = "[JIT]";
  static constexpr const char* kUnknownBucket = "[unknown]";
  // Pre-allocated bucket map. Keyed by `const char*` (interned).
  // 256 distinct symbols is more than enough for our hot path (typical
  // counts: ~6-10 named jit_rt_* helpers + runtime stdlib symbols).
  static constexpr std::size_t kMaxSymbols = 256;
  struct Bucket {
    const char* name;
    std::atomic<std::uint64_t> samples;
  };
  Bucket buckets[kMaxSymbols];
  std::atomic<std::size_t> bucket_count{0};

  GlobalProfilerState() {
    for (auto& b : buckets) {
      b.name = nullptr;
      b.samples.store(0, std::memory_order_relaxed);
    }
  }

  // Returns index of bucket for `name`. Allocates a new bucket if needed.
  // `name` MUST be a stable C string (interned by the loader or one of the
  // sentinel constants above). Called from signal handler -- must be AS-safe.
  std::size_t BucketFor(const char* name) {
    const std::size_t cur = bucket_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < cur; ++i) {
      if (buckets[i].name == name) return i;
    }
    // Try to claim a new slot. Multiple SIGPROF signals racing here are
    // possible only across threads; we use a single-thread design so this
    // is a fast-path slot bump.
    std::size_t idx = bucket_count.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= kMaxSymbols) {
      // Overflow: bucket cap exceeded. Fold into [unknown].
      bucket_count.store(kMaxSymbols, std::memory_order_release);
      return BucketFor(kUnknownBucket);
    }
    buckets[idx].name = name;
    return idx;
  }
};

GlobalProfilerState& State() {
  static GlobalProfilerState g;
  return g;
}

#if defined(__linux__)
void SigprofHandler(int /*sig*/, siginfo_t* /*info*/, void* uctx) {
  auto& s = State();
  if (!s.active.load(std::memory_order_acquire)) return;

  void* pc = nullptr;
  auto* ucontext = static_cast<ucontext_t*>(uctx);
#if defined(__x86_64__)
  pc = reinterpret_cast<void*>(ucontext->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
  pc = reinterpret_cast<void*>(ucontext->uc_mcontext.pc);
#else
  // Unsupported arch; bail.
  return;
#endif

  const char* sym_name = GlobalProfilerState::kUnknownBucket;
  Dl_info info;
  if (dladdr(pc, &info) != 0) {
    if (info.dli_sname != nullptr) {
      // Stable C string owned by the dynamic linker -- safe to use as map
      // key for the lifetime of the process.
      sym_name = info.dli_sname;
    } else if (info.dli_fbase == nullptr) {
      // No object info -- probably JIT-allocated executable page.
      sym_name = GlobalProfilerState::kJitBucket;
    } else {
      // Inside a known shared object but anonymous symbol (e.g. local
      // helper). Bucket as [unknown].
    }
  } else {
    // dladdr failed entirely -- JIT pages typically do this on Linux.
    sym_name = GlobalProfilerState::kJitBucket;
  }

  const std::size_t idx = s.BucketFor(sym_name);
  if (idx < GlobalProfilerState::kMaxSymbols) {
    s.buckets[idx].samples.fetch_add(1, std::memory_order_relaxed);
  }
  s.total_samples.fetch_add(1, std::memory_order_relaxed);
}
#else
void SigprofHandler(int, siginfo_t*, void*) {}
#endif

std::string Demangle(const char* name) {
  if (name == nullptr) return "";
#ifdef __GNUG__
  int status = 0;
  char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
  if (status == 0 && demangled != nullptr) {
    std::string out(demangled);
    std::free(demangled);
    return out;
  }
#endif
  return std::string(name);
}

}  // namespace

SamplingProfiler::SamplingProfiler(unsigned sample_period_us)
    : sample_period_us_(sample_period_us) {}

SamplingProfiler::~SamplingProfiler() { Stop(); }

bool SamplingProfiler::Start() {
  if (running_) return false;
  auto& s = State();
  if (s.active.exchange(true)) return false;  // already active elsewhere
  s.total_samples.store(0, std::memory_order_relaxed);
  s.bucket_count.store(0, std::memory_order_relaxed);
  for (auto& b : s.buckets) {
    b.name = nullptr;
    b.samples.store(0, std::memory_order_relaxed);
  }

#if defined(__linux__)
  struct sigaction sa{};
  sa.sa_sigaction = SigprofHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, nullptr) != 0) {
    s.active.store(false);
    return false;
  }
  struct itimerval it{};
  it.it_interval.tv_sec = 0;
  it.it_interval.tv_usec = static_cast<suseconds_t>(sample_period_us_);
  it.it_value = it.it_interval;
  if (setitimer(ITIMER_PROF, &it, nullptr) != 0) {
    s.active.store(false);
    return false;
  }
#endif
  running_ = true;
  return true;
}

void SamplingProfiler::Stop() {
  if (!running_) return;
#if defined(__linux__)
  struct itimerval it{};  // zero -> disarm
  setitimer(ITIMER_PROF, &it, nullptr);
  struct sigaction sa{};
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);
#endif
  // Snapshot global state into per-instance storage so a subsequent
  // Start() on another profiler doesn't clobber our results.
  auto& s = State();
  const std::size_t bc = s.bucket_count.load(std::memory_order_acquire);
  snapshot_total_ = s.total_samples.load(std::memory_order_acquire);
  snapshot_.clear();
  snapshot_.reserve(bc);
  for (std::size_t i = 0; i < bc; ++i) {
    const std::uint64_t v = s.buckets[i].samples.load(std::memory_order_acquire);
    if (v == 0 || s.buckets[i].name == nullptr) continue;
    snapshot_.push_back({Demangle(s.buckets[i].name), v});
  }
  s.active.store(false);
  running_ = false;
}

std::uint64_t SamplingProfiler::TotalSamples() const { return snapshot_total_; }

std::vector<SamplingProfiler::SymbolSamples> SamplingProfiler::Top(std::size_t n) const {
  std::vector<SymbolSamples> rows;
  rows.reserve(snapshot_.size());
  for (const auto& e : snapshot_) {
    rows.push_back({e.name, e.samples,
                    snapshot_total_ == 0
                        ? 0.0
                        : 100.0 * static_cast<double>(e.samples) /
                              static_cast<double>(snapshot_total_)});
  }
  std::sort(rows.begin(), rows.end(),
            [](const SymbolSamples& a, const SymbolSamples& b) { return a.samples > b.samples; });
  if (rows.size() > n) rows.resize(n);
  return rows;
}

void SamplingProfiler::WriteReport(std::ostream& out, std::size_t top_n,
                                   const std::string& header_label) const {
  out << header_label << "\n";
  out << "samples_total=" << TotalSamples() << "\n";
  out << "  Overhead   Samples  Symbol\n";
  out << "  --------   -------  -------------------------------------------------------------\n";
  for (const auto& row : Top(top_n)) {
    char overhead[32];
    std::snprintf(overhead, sizeof(overhead), "%7.3f%%", row.percent);
    out << "  " << overhead << "  ";
    out << std::setw(8) << row.samples << "  ";
    out << row.name << "\n";
  }
  out << "\n";
}

void SamplingProfiler::WriteMarkdownTable(std::ostream& out, std::size_t top_n) const {
  out << "| Overhead | Samples | Symbol |\n";
  out << "|---------:|--------:|--------|\n";
  for (const auto& row : Top(top_n)) {
    char overhead[32];
    std::snprintf(overhead, sizeof(overhead), "%.3f%%", row.percent);
    out << "| " << overhead << " | " << row.samples << " | `" << row.name << "` |\n";
  }
}

double SamplingProfiler::PercentForPrefix(const std::string& name_prefix) const {
  if (snapshot_total_ == 0) return 0.0;
  std::uint64_t hit = 0;
  for (const auto& e : snapshot_) {
    if (e.name.rfind(name_prefix, 0) == 0) hit += e.samples;
  }
  return 100.0 * static_cast<double>(hit) / static_cast<double>(snapshot_total_);
}

}  // namespace jitse
