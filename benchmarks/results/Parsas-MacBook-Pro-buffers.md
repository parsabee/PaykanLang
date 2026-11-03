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
| sum_buf | 465.169 ms, 1.0x, eff 1.00, busy 1.00 | 236.791 ms, 2.0x, eff 0.98, busy 1.00 | 120.649 ms, 3.9x, eff 0.96, busy 1.00 | 60.851 ms, 7.6x, eff 0.96, busy 0.99 | 42.009 ms, 11.1x, eff 0.92, busy 0.96 | 36.019 ms, 12.9x, eff 0.81, busy 0.94 |
| sum_buf_1m | 7.243 ms, 1.0x, eff 1.00, busy 1.00 | 3.693 ms, 2.0x, eff 0.98, busy 0.99 | 1.899 ms, 3.8x, eff 0.95, busy 0.97 | 0.977 ms, 7.4x, eff 0.93, busy 0.90 | 0.698 ms, 10.4x, eff 0.86, busy 0.81 | 0.620 ms, 11.7x, eff 0.73, busy 0.79 |
| dot_buf | 475.915 ms, 1.0x, eff 1.00, busy 1.00 | 241.420 ms, 2.0x, eff 0.99, busy 1.00 | 122.907 ms, 3.9x, eff 0.97, busy 0.99 | 62.078 ms, 7.7x, eff 0.96, busy 0.99 | 42.837 ms, 11.1x, eff 0.93, busy 0.98 | 36.664 ms, 13.0x, eff 0.81, busy 0.95 |
