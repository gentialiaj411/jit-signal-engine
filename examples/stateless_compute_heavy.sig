# Compute-heavy stateless signal designed to expose the AVX2 win in
# cross-symbol vectorization. Same shape as stateless_heavy.sig (pure
# arithmetic over market loads, no stateful ops, no conditionals) but
# with a much higher arithmetic-intensity-per-load ratio:
#
#   * 4 market loads (mid/ask/bid on two symbols) feed
#   * ~40 floating-point ops including 6 sqrt and 4 abs nonlinearities
#
# This shifts the bottleneck from MarketState gather to FP throughput,
# which is exactly where AVX2 lane-parallel <4 x double> vectorization
# beats scalar `vmulsd`/`vaddsd`. The cross_symbol_benchmark on this
# program is the canonical "AVX2 wins" artifact -- the speedup over
# scalar JIT (NOT over the interpreter) is the headline number.
signal a = mid(AAPL)
signal b = mid(MSFT)
signal sa = ask(AAPL) - bid(AAPL)
signal sb = ask(MSFT) - bid(MSFT)

# Power-of-two depth-6 polynomial-style mixing -- gives the FP scheduler
# many independent chains across the K=4 lanes so AVX2 throughput-bound
# execution kicks in.
signal p1 = a * a + b * b + sa * sa + sb * sb
signal p2 = a * b - sa * sb + (a - b) * (sa - sb)
signal p3 = sqrt(p1 + 1.0) - sqrt(p2 * p2 + 1.0)
signal p4 = (a + b) * (a + b) - (a - b) * (a - b)
signal p5 = sqrt(p4 + 1.0) + sqrt(abs(p2) + 1.0)

signal q1 = p1 * p3 - p2 * p5 + p4
signal q2 = p3 * p5 + p1 * p4 - p2
signal q3 = sqrt(abs(q1) + 1.0) + sqrt(abs(q2) + 1.0)
signal q4 = q1 * q2 - q3 * q3

signal r1 = sqrt(abs(q4) + 1.0) * 0.5 + abs(q3 - q1) * 0.25
signal r2 = sqrt(abs(q1 * q2) + 1.0) - abs(q4) * 0.125
signal out = (r1 - r2) * 0.5 + (q1 + q2) * 0.125
