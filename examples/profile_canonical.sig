# Canonical signal for the P3 runtime-call profile artifact.
# Exercises every stateful op P0 lowered (sma, ema, lag) plus one that P0
# did NOT lower (rolling_std) so the artifact's per-op breakdown table has
# a column showing the lowered ops dropping to 0% and a control column
# showing rolling_std persisting unchanged across configurations.
signal short_ma = ema(mid(AAPL), 10)
signal long_ma = sma(mid(AAPL), 60)
signal lagged = lag(mid(AAPL), 5)
signal vol = rolling_std(mid(AAPL), 30)
signal score = short_ma - long_ma + (mid(AAPL) - lagged) * 0.25
signal out = if vol > 0.0 then score / vol else 0.0
