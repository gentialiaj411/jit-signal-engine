# P7: a small two-instrument signal using the new rolling_corr and
# rolling_beta operators.
#
# The signal goes long the spread when AAPL and MSFT decorrelate
# (|corr| drops below 0.3) AND a recent regression slope is above 1.0.
signal corr  = rolling_corr(mid(AAPL), mid(MSFT), 60)
signal beta  = rolling_beta(mid(AAPL), mid(MSFT), 60)
signal vol   = rolling_std(mid(AAPL), 30)
signal raw   = mid(AAPL) - beta * mid(MSFT)
signal out   = if vol > 0.0 && corr < 0.3 then raw / vol else 0.0
