param scale_mid = 0.2
param scale_spread = 0.4
param bias = -0.25

signal centered_mid = mid(AAPL) - 100.0
signal pred = scale_mid * centered_mid + scale_spread * spread(AAPL) + bias
