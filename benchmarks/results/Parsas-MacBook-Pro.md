# Parsas-MacBook-Pro

```
{
 "cpu": "Apple M3 Max",
 "cores": 16,
 "perf_cores": "12",
 "eff_cores": "4",
 "cache_line": "128",
 "l1d": "65536",
 "l2_perf": "16777216",
 "os": "macOS-26.6.2-arm64-arm-64bit"
}
```

Each cell: the loop's best time, its speedup over one thread, the
efficiency (speedup / threads), and how busy the threads were over the
timed loops (CPU time / (wall time * threads); a thread spinning after
a loop, KMP_BLOCKTIME, counts as busy).

| program | 1 thr | 2 thr | 4 thr | 8 thr | 12 thr | 16 thr |
|---|---|---|---|---|---|---|
| sum | 10.193 ms, 1.0x, eff 1.00, busy 1.00 | 5.175 ms, 2.0x, eff 0.98, busy 0.99 | 2.818 ms, 3.6x, eff 0.90, busy 0.97 | 1.441 ms, 7.1x, eff 0.88, busy 0.94 | 1.009 ms, 10.1x, eff 0.84, busy 0.89 | 0.872 ms, 11.7x, eff 0.73, busy 0.84 |
| dot | 11.134 ms, 1.0x, eff 1.00, busy 1.00 | 5.567 ms, 2.0x, eff 1.00, busy 0.99 | 3.021 ms, 3.7x, eff 0.92, busy 0.98 | 1.545 ms, 7.2x, eff 0.90, busy 0.93 | 1.081 ms, 10.3x, eff 0.86, busy 0.89 | 0.953 ms, 11.7x, eff 0.73, busy 0.83 |
| compute | 87.788 ms, 1.0x, eff 1.00, busy 1.00 | 45.182 ms, 1.9x, eff 0.97, busy 1.00 | 23.730 ms, 3.7x, eff 0.92, busy 0.99 | 12.279 ms, 7.1x, eff 0.89, busy 0.98 | 8.475 ms, 10.4x, eff 0.86, busy 0.96 | 6.985 ms, 12.6x, eff 0.79, busy 0.92 |
| saxpy | 0.007 ms, 1.0x, eff 1.00, busy 1.00 | 0.011 ms, 0.7x, eff 0.33, busy 0.73 | 0.014 ms, 0.5x, eff 0.13, busy 0.51 | 0.031 ms, 0.2x, eff 0.03, busy 0.37 | 0.044 ms, 0.2x, eff 0.01, busy 0.35 | 0.083 ms, 0.1x, eff 0.01, busy 0.38 |
| overhead | 0.002 ms, 1.0x, eff 1.00, busy 1.00 | 0.012 ms, 0.1x, eff 0.07, busy 0.63 | 0.020 ms, 0.1x, eff 0.02, busy 0.46 | 0.045 ms, 0.0x, eff 0.00, busy 0.33 | 0.070 ms, 0.0x, eff 0.00, busy 0.34 | 0.101 ms, 0.0x, eff 0.00, busy 0.37 |
| reduce_counter | 2.490 ms, 1.0x, eff 1.00, busy 1.00 | 1.146 ms, 2.2x, eff 1.09, busy 0.97 | 0.596 ms, 4.2x, eff 1.05, busy 0.91 | 0.334 ms, 7.5x, eff 0.93, busy 0.79 | 0.243 ms, 10.3x, eff 0.86, busy 0.69 | 0.270 ms, 9.2x, eff 0.58, busy 0.58 |
| atomic_counter | 7.861 ms, 1.0x, eff 1.00, busy 1.00 | 25.667 ms, 0.3x, eff 0.15, busy 1.00 | 46.553 ms, 0.2x, eff 0.04, busy 1.00 | 147.420 ms, 0.1x, eff 0.01, busy 1.00 | 121.406 ms, 0.1x, eff 0.01, busy 0.96 | 267.026 ms, 0.0x, eff 0.00, busy 0.97 |
| atomic_counter_16 | 2.226 ms, 1.0x, eff 1.00, busy 1.00 | 4.835 ms, 0.5x, eff 0.23, busy 0.99 | 6.110 ms, 0.4x, eff 0.09, busy 0.99 | 10.790 ms, 0.2x, eff 0.03, busy 0.99 | 10.314 ms, 0.2x, eff 0.02, busy 0.98 | 17.292 ms, 0.1x, eff 0.01, busy 0.95 |
| atomic_counter_256 | 2.352 ms, 1.0x, eff 1.00, busy 1.00 | 1.353 ms, 1.7x, eff 0.87, busy 0.97 | 0.748 ms, 3.1x, eff 0.79, busy 0.92 | 0.846 ms, 2.8x, eff 0.35, busy 0.90 | 1.006 ms, 2.3x, eff 0.19, busy 0.90 | 1.227 ms, 1.9x, eff 0.12, busy 0.87 |
