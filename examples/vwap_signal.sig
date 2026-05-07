# Rolling VWAP deviation normalized by rolling volatility
signal dev = (mid(AAPL) - vwap(AAPL, 30)) / rolling_std(mid(AAPL), 30)

