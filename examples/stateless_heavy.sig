# Stateless signal stack used to benchmark P2 cross-symbol vectorization.
# Every signal is pure arithmetic over market loads -- no stateful ops,
# no conditionals. Several layers of arithmetic widen the kernel enough
# that the per-call function-dispatch overhead doesn't dominate scalar.
signal s1 = mid(AAPL) - mid(MSFT)
signal s2 = mid(AAPL) + mid(MSFT)
signal s3 = (ask(AAPL) - bid(AAPL)) * 0.5 + (ask(MSFT) - bid(MSFT)) * 0.5
signal s4 = s1 * s2 - s3
signal s5 = (s4 + s1) * (s4 - s2)
signal s6 = abs(s4) - abs(s5)
signal s7 = s5 * 0.25 + s6 * 0.75
signal s8 = (s7 + s1) - (s7 + s2)
signal out = s8 * 0.5 + (s1 - s2) * 0.5
