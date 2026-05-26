# P7: scalar Kalman-smoothed mid-price.
#
# q = 0.01 (small process noise -> trust the model)
# r = 1.00 (larger measurement noise -> distrust each tick)
#
# The signal's value is the smoothed price minus the raw mid, i.e. the
# instantaneous filter residual.
signal smooth   = kalman1d(mid(AAPL), 0.01, 1.0)
signal residual = mid(AAPL) - smooth
