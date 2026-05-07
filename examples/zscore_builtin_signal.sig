# Built-in zscore form of a momentum spread
signal z = zscore(ema(mid(AAPL), 10) - ema(mid(AAPL), 30), 30)

