# Adaptive momentum signal with volatility filter
# Long when short EMA > long EMA AND vol is low
signal short_ma = ema(mid(AAPL), 10)
signal long_ma = ema(mid(AAPL), 60)
signal vol = rolling_std(mid(AAPL), 30)
signal raw = short_ma - long_ma
signal filtered = if short_ma > long_ma && vol > 0.0 then raw / vol else 0.0
