# SPSC Live-Ingest Pipeline Latency

Signal program: `../examples/filtered_momentum.sig`  
Events: warmup=`200000`, measured=`2000000`  
Rate: `2e+06` Hz (0 = unpaced, closed-loop)  
Pinning: producer cpu=`2`, consumer cpu=`4`  
Ring capacity: `1024` slots (lock-free SPSC, cache-line padded)  

## Pipeline latency (enqueue -> signal output)

## pipeline_ns

Total samples: 2000000  
Min: 133 ns  
Max: 7504265 ns

| Percentile | Latency (ns) |
|---|---:|
| p50 | 280 |
| p75 | 312 |
| p90 | 30208 |
| p99 | 671744 |
| p99.9 | 1736704 |
| p99.99 | 3866624 |
| p99.999 | 3866624 |
| max | 7504265 |


## Methodology

The producer thread stamps `enqueue_ns = clock_gettime(CLOCK_MONOTONIC)` immediately before calling `ring.try_push(...)`. The consumer thread pops the event, applies it to `MarketState`, calls the JIT-compiled signal, and stamps `out_ns` immediately after. The recorded latency is `out_ns - enqueue_ns`, which includes:

  * SPSC ring enqueue (cache-line-padded `head_.store(release)`)
  * Inter-core hand-off cost (producer and consumer pinned to distinct physical cores)
  * SPSC ring dequeue (`tail_.load(acquire)` plus a slot copy)
  * Event apply to MarketState (two indexed stores)
  * One whole JIT signal call (`5` output(s))

The producer paces at `2e+06` Hz using open-loop Coordinated-Omission–aware scheduling: each event's `enqueue_ns` is stamped at its TARGET time, not its observed wall-clock time. If the consumer stalls and the producer catches up later, the stalled event's recorded latency includes the queueing delay -- the standard wrk2 / HdrHistogram CO correction.

## Reproduction

```
cd build-wsl
JITSE_BENCH_PRODUCER_CPU=2 JITSE_BENCH_CONSUMER_CPU=4 \
  ./spsc_jit_pipeline_bench ../examples/filtered_momentum.sig --events=2000000 --warmup=200000 --rate-hz=2e+06
```
