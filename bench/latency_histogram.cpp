// latency_histogram.cpp -- see header for design notes.

#include "latency_histogram.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace jitse {

std::uint64_t LatencyHistogram::Percentile(double p) const {
  if (total_ == 0) return 0;
  if (p <= 0.0) return Min();
  if (p >= 1.0) return Max();
  const std::uint64_t target =
      static_cast<std::uint64_t>(p * static_cast<double>(total_) + 0.5);
  std::uint64_t acc = 0;
  for (int i = 0; i < kBucketCount; ++i) {
    acc += buckets_[i];
    if (acc >= target) {
      const auto bv = At(i);
      // Return the midpoint of the bucket range. For the very last
      // observed bucket clamp to max_observed_ since the bucket's hi_ns
      // may overshoot the true tail.
      const std::uint64_t mid = bv.lo_ns + (bv.hi_ns - bv.lo_ns) / 2;
      return mid;
    }
  }
  return Max();
}

LatencyHistogram::BucketView LatencyHistogram::At(int idx) const {
  const int magnitude = idx / kSubBuckets;
  const int sub = idx % kSubBuckets;
  std::uint64_t lo;
  std::uint64_t hi;
  if (magnitude == 0) {
    // First magnitude covers [0, 2). Two sub-buckets {0} {1} are populated;
    // the rest are technically unreachable. Treat width = 1 ns.
    lo = static_cast<std::uint64_t>(sub);
    hi = lo + 1;
  } else {
    const std::uint64_t base = 1ULL << magnitude;
    const int shift = magnitude - kPrecisionBits;
    const std::uint64_t step = shift >= 0 ? (1ULL << shift) : 1;
    lo = base + static_cast<std::uint64_t>(sub) * step;
    hi = lo + step;
  }
  return BucketView{lo, hi, buckets_[idx]};
}

void LatencyHistogram::WriteCsv(std::ostream& out, const std::string& label_col) const {
  if (!label_col.empty()) out << "label,";
  out << "bucket_index,lo_ns,hi_ns,count,cum_count,cum_fraction\n";
  std::uint64_t cum = 0;
  for (int i = 0; i < kBucketCount; ++i) {
    const auto bv = At(i);
    if (bv.count == 0) continue;
    cum += bv.count;
    const double frac =
        total_ == 0 ? 0.0 : static_cast<double>(cum) / static_cast<double>(total_);
    if (!label_col.empty()) out << label_col << ',';
    out << i << ',' << bv.lo_ns << ',' << bv.hi_ns << ',' << bv.count << ','
        << cum << ',' << frac << '\n';
  }
}

void LatencyHistogram::WriteMarkdownSummary(std::ostream& out, const std::string& label) const {
  out << "## " << label << "\n\n";
  out << "Total samples: " << total_ << "  \n";
  out << "Min: " << Min() << " ns  \n";
  out << "Max: " << Max() << " ns";
  if (overflow_ > 0) {
    out << "  \nOverflow (>" << (1ULL << kMaxMagnitude) << " ns): "
        << overflow_ << " sample(s)";
  }
  out << "\n\n";
  out << "| Percentile | Latency (ns) |\n|---|---:|\n";
  static const struct { const char* name; double p; } kRows[] = {
      {"p50", 0.50},  {"p75", 0.75},   {"p90", 0.90},     {"p99", 0.99},
      {"p99.9", 0.999}, {"p99.99", 0.9999}, {"p99.999", 0.99999},
      {"max", 1.0},
  };
  for (const auto& r : kRows) {
    out << "| " << r.name << " | " << Percentile(r.p) << " |\n";
  }
  out << "\n";
}

namespace {

constexpr const char* kColors[] = {
    "#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e", "#17becf",
};

inline double SafeLog10(double v) { return v <= 0.0 ? 0.0 : std::log10(v); }

}  // namespace

void LatencyHistogram::WriteSvgCdf(std::ostream& out,
                                   const std::vector<const LatencyHistogram*>& series,
                                   const std::vector<std::string>& series_labels,
                                   int width_px, int height_px) {
  const int margin_l = 70;
  const int margin_r = 20;
  const int margin_t = 30;
  const int margin_b = 50;
  const int plot_w = width_px - margin_l - margin_r;
  const int plot_h = height_px - margin_t - margin_b;

  // X axis: log10(ns). Find global min/max across series.
  double log_min_x = 0.0;
  double log_max_x = 0.0;
  bool first = true;
  for (const auto* h : series) {
    if (h == nullptr || h->Total() == 0) continue;
    const double lo = SafeLog10(static_cast<double>(std::max<std::uint64_t>(h->Min(), 1)));
    const double hi = SafeLog10(static_cast<double>(std::max<std::uint64_t>(h->Max(), 1)));
    if (first || lo < log_min_x) log_min_x = lo;
    if (first || hi > log_max_x) log_max_x = hi;
    first = false;
  }
  if (first || log_max_x <= log_min_x) {
    log_min_x = 0.0;
    log_max_x = 6.0;
  } else {
    // Pad ends to nearest integer log10 so gridlines look clean.
    log_min_x = std::floor(log_min_x);
    log_max_x = std::ceil(log_max_x);
    if (log_max_x - log_min_x < 1.0) log_max_x = log_min_x + 1.0;
  }
  const double log_range = log_max_x - log_min_x;

  auto x_for_ns = [&](double ns) -> double {
    const double lx = SafeLog10(std::max(ns, 1.0));
    const double t = (lx - log_min_x) / log_range;
    return margin_l + t * plot_w;
  };
  auto y_for_frac = [&](double frac) -> double {
    return margin_t + (1.0 - frac) * plot_h;
  };

  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width_px
      << "\" height=\"" << height_px
      << "\" viewBox=\"0 0 " << width_px << ' ' << height_px
      << "\" font-family=\"sans-serif\" font-size=\"12\">\n";
  out << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";
  // Plot border.
  out << "<rect x=\"" << margin_l << "\" y=\"" << margin_t
      << "\" width=\"" << plot_w << "\" height=\"" << plot_h
      << "\" fill=\"none\" stroke=\"#888\" stroke-width=\"1\"/>\n";
  // X gridlines at each decade.
  for (double lx = log_min_x; lx <= log_max_x + 1e-9; lx += 1.0) {
    const double xpix = margin_l + ((lx - log_min_x) / log_range) * plot_w;
    out << "<line x1=\"" << xpix << "\" y1=\"" << margin_t
        << "\" x2=\"" << xpix << "\" y2=\"" << (margin_t + plot_h)
        << "\" stroke=\"#ddd\" stroke-width=\"1\"/>\n";
    const double ns_at = std::pow(10.0, lx);
    out << "<text x=\"" << xpix << "\" y=\"" << (margin_t + plot_h + 16)
        << "\" text-anchor=\"middle\">";
    if (ns_at >= 1e9) out << (ns_at / 1e9) << " s";
    else if (ns_at >= 1e6) out << (ns_at / 1e6) << " ms";
    else if (ns_at >= 1e3) out << (ns_at / 1e3) << " us";
    else out << ns_at << " ns";
    out << "</text>\n";
  }
  // Y gridlines at fractions of interest.
  static const double kYTicks[] = {0.0, 0.5, 0.9, 0.99, 0.999, 0.9999, 1.0};
  for (double f : kYTicks) {
    const double ypix = margin_t + (1.0 - f) * plot_h;
    out << "<line x1=\"" << margin_l << "\" y1=\"" << ypix
        << "\" x2=\"" << (margin_l + plot_w) << "\" y2=\"" << ypix
        << "\" stroke=\"#eee\" stroke-width=\"1\"/>\n";
    out << "<text x=\"" << (margin_l - 8) << "\" y=\"" << (ypix + 4)
        << "\" text-anchor=\"end\">" << f << "</text>\n";
  }
  // Axis labels.
  out << "<text x=\"" << (margin_l + plot_w / 2) << "\" y=\"" << (height_px - 12)
      << "\" text-anchor=\"middle\">latency (log scale)</text>\n";
  out << "<text x=\"" << 16 << "\" y=\"" << (margin_t + plot_h / 2)
      << "\" text-anchor=\"middle\" transform=\"rotate(-90 16 "
      << (margin_t + plot_h / 2)
      << ")\">cumulative fraction (linear)</text>\n";

  // Plot each CDF.
  for (std::size_t s = 0; s < series.size(); ++s) {
    const auto* h = series[s];
    if (h == nullptr || h->Total() == 0) continue;
    const char* color = kColors[s % (sizeof(kColors) / sizeof(kColors[0]))];
    out << "<polyline fill=\"none\" stroke=\"" << color
        << "\" stroke-width=\"1.6\" points=\"";
    std::uint64_t cum = 0;
    bool emitted_first = false;
    for (int i = 0; i < kBucketCount; ++i) {
      const auto bv = h->At(i);
      if (bv.count == 0) continue;
      cum += bv.count;
      const double frac =
          static_cast<double>(cum) / static_cast<double>(h->Total());
      const double x = x_for_ns(static_cast<double>(bv.hi_ns));
      const double y = y_for_frac(frac);
      if (!emitted_first) {
        // Anchor the CDF at (lo, 0) so the line doesn't dangle.
        const double x0 = x_for_ns(static_cast<double>(bv.lo_ns));
        const double y0 = y_for_frac(0.0);
        out << x0 << ',' << y0 << ' ';
        emitted_first = true;
      }
      out << x << ',' << y << ' ';
    }
    out << "\"/>\n";
    // Legend entry.
    const int lx = margin_l + 10;
    const int ly = margin_t + 16 + static_cast<int>(s) * 16;
    out << "<rect x=\"" << lx << "\" y=\"" << (ly - 10)
        << "\" width=\"12\" height=\"4\" fill=\"" << color << "\"/>\n";
    out << "<text x=\"" << (lx + 18) << "\" y=\"" << (ly - 2)
        << "\">"
        << (s < series_labels.size() ? series_labels[s] : std::string("series"))
        << "</text>\n";
  }
  out << "</svg>\n";
}

}  // namespace jitse
