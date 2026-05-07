#include <cassert>

#include "market_sim.h"

int main() {
  jitse::MarketSimulator sim(12345, 8);
  std::uint64_t last_ts = 0;
  for (int i = 0; i < 1000; ++i) {
    jitse::MarketEvent ev = sim.NextEvent(1000);
    assert(ev.instrument_id < 8);
    assert(ev.bid < ev.ask);
    assert(ev.timestamp_ns > last_ts);
    last_ts = ev.timestamp_ns;
  }
  return 0;
}

