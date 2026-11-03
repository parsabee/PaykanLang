# Parsas-MacBook-Pro-blocktime200

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
| sum | 10.149 ms, 1.0x, eff 1.00, busy 1.00 | 5.177 ms, 2.0x, eff 0.98, busy 0.89 | 2.721 ms, 3.7x, eff 0.93, busy 0.77 | 1.420 ms, 7.1x, eff 0.89, busy 0.79 | 0.983 ms, 10.3x, eff 0.86, busy 0.20 | 0.825 ms, 12.3x, eff 0.77, busy 0.61 |
| dot | 11.755 ms, 1.0x, eff 1.00, busy 1.00 | 6.054 ms, 1.9x, eff 0.97, busy 0.95 | 2.930 ms, 4.0x, eff 1.00, busy 0.79 | 1.527 ms, 7.7x, eff 0.96, busy 0.40 | 1.051 ms, 11.2x, eff 0.93, busy 0.15 | 0.881 ms, 13.3x, eff 0.83, busy 0.34 |
| compute | 87.147 ms, 1.0x, eff 1.00, busy 1.00 | 45.140 ms, 1.9x, eff 0.97, busy 0.98 | 24.444 ms, 3.6x, eff 0.89, busy 0.97 | 12.267 ms, 7.1x, eff 0.89, busy 0.93 | 8.445 ms, 10.3x, eff 0.86, busy 0.95 | 7.081 ms, 12.3x, eff 0.77, busy 0.93 |
| saxpy | 0.006 ms, 1.0x, eff 1.00, busy 1.00 | 0.005 ms, 1.3x, eff 0.63, busy 0.84 | 0.003 ms, 1.9x, eff 0.48, busy 0.46 | 0.005 ms, 1.2x, eff 0.16, busy 0.33 | 0.007 ms, 0.9x, eff 0.07, busy 0.88 | 0.011 ms, 0.5x, eff 0.03, busy 0.83 |
| overhead | 0.001 ms, 1.0x, eff 1.00, busy 1.00 | 0.003 ms, 0.4x, eff 0.19, busy 0.54 | 0.003 ms, 0.4x, eff 0.09, busy 0.48 | 0.008 ms, 0.1x, eff 0.02, busy 0.79 | 0.011 ms, 0.1x, eff 0.01, busy 0.87 | 0.015 ms, 0.1x, eff 0.00, busy 0.81 |
| reduce_counter | 2.224 ms, 1.0x, eff 1.00, busy 1.00 | 1.136 ms, 2.0x, eff 0.98, busy 0.88 | 0.582 ms, 3.8x, eff 0.96, busy 0.52 | 0.296 ms, 7.5x, eff 0.94, busy 0.20 | 0.207 ms, 10.7x, eff 0.89, busy 0.17 | 0.195 ms, 11.4x, eff 0.71, busy 0.07 |
| atomic_counter | 7.805 ms, 1.0x, eff 1.00, busy 1.00 | 24.465 ms, 0.3x, eff 0.16, busy 0.99 | 45.854 ms, 0.2x, eff 0.04, busy 1.00 | 145.632 ms, 0.1x, eff 0.01, busy 0.99 | 125.514 ms, 0.1x, eff 0.01, busy 0.99 | 268.191 ms, 0.0x, eff 0.00, busy 0.98 |
| atomic_counter_16 | 2.224 ms, 1.0x, eff 1.00, busy 1.00 | 4.837 ms, 0.5x, eff 0.23, busy 0.99 | 6.027 ms, 0.4x, eff 0.09, busy 0.98 | 10.578 ms, 0.2x, eff 0.03, busy 0.94 | 10.007 ms, 0.2x, eff 0.02, busy 0.94 | 17.304 ms, 0.1x, eff 0.01, busy 0.95 |
| atomic_counter_256 | 2.364 ms, 1.0x, eff 1.00, busy 1.00 | 1.408 ms, 1.7x, eff 0.84, busy 0.88 | 0.728 ms, 3.2x, eff 0.81, busy 0.53 | 0.835 ms, 2.8x, eff 0.35, busy 0.73 | 0.973 ms, 2.4x, eff 0.20, busy 0.22 | 1.248 ms, 1.9x, eff 0.12, busy 0.47 |
