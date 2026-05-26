signal short = ema(mid(AAPL), 10)
signal long = ema(mid(AAPL), 60)
signal vol = rolling_std(mid(AAPL), 30)
signal out = if vol > 0.0 then (short - long) / vol else 0.0
