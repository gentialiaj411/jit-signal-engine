#include <cassert>
#include <cmath>
#include <limits>

#include "runtime.h"

int main() {
  jitse::RingStatsState st;

  jitse::RingStatsPush(st, 3, 1.0);
  assert(!jitse::RingStatsFull(st));
  assert(std::isnan(jitse::RingStatsStddevSample(st)));

  jitse::RingStatsPush(st, 3, 2.0);
  assert(!jitse::RingStatsFull(st));
  assert(std::fabs(jitse::RingStatsStddevSample(st) - std::sqrt(0.5)) < 1e-12);

  jitse::RingStatsPush(st, 3, 3.0);
  assert(jitse::RingStatsFull(st));
  assert(std::fabs(jitse::RingStatsMean(st) - 2.0) < 1e-12);
  assert(std::fabs(jitse::RingStatsStddevSample(st) - 1.0) < 1e-12);

  // Window slides from [1,2,3] to [2,3,4].
  jitse::RingStatsPush(st, 3, 4.0);
  assert(std::fabs(jitse::RingStatsMean(st) - 3.0) < 1e-12);
  assert(std::fabs(jitse::RingStatsStddevSample(st) - 1.0) < 1e-12);

  return 0;
}
