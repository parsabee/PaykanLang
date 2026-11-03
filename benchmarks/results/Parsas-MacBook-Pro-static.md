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
efficiency (speedup / threads), and how busy the threads were (CPU
time / (wall time * threads), the whole process; spinning at barriers
counts as busy unless --passive).

| program | 1 thr | 2 thr | 4 thr | 8 thr | 12 thr | 16 thr |
|---|---|---|---|---|---|---|
| sum | 10.122 ms, 1.0x, eff 1.00, busy 0.98 | 5.161 ms, 2.0x, eff 0.98, busy 0.91 | 2.822 ms, 3.6x, eff 0.90, busy 0.80 | 1.441 ms, 7.0x, eff 0.88, busy 0.65 | 1.014 ms, 10.0x, eff 0.83, busy 0.51 | 1.107 ms, 9.1x, eff 0.57, busy 0.41 |
| dot | 10.898 ms, 1.0x, eff 1.00, busy 0.98 | 5.576 ms, 2.0x, eff 0.98, busy 0.91 | 3.025 ms, 3.6x, eff 0.90, busy 0.79 | 1.546 ms, 7.1x, eff 0.88, busy 0.68 | 1.086 ms, 10.0x, eff 0.84, busy 0.55 | 1.153 ms, 9.5x, eff 0.59, busy 0.43 |
| compute | 88.076 ms, 1.0x, eff 1.00, busy 1.00 | 45.100 ms, 2.0x, eff 0.98, busy 0.98 | 23.699 ms, 3.7x, eff 0.93, busy 0.96 | 12.280 ms, 7.2x, eff 0.90, busy 0.92 | 8.518 ms, 10.3x, eff 0.86, busy 0.86 | 8.236 ms, 10.7x, eff 0.67, busy 0.67 |
| saxpy | 0.005 ms, 1.0x, eff 1.00, busy 0.91 | 0.010 ms, 0.5x, eff 0.26, busy 0.57 | 0.015 ms, 0.4x, eff 0.09, busy 0.38 | 0.028 ms, 0.2x, eff 0.02, busy 0.31 | 0.046 ms, 0.1x, eff 0.01, busy 0.30 | 0.077 ms, 0.1x, eff 0.00, busy 0.34 |
| overhead | 0.001 ms, 1.0x, eff 1.00, busy 0.84 | 0.011 ms, 0.1x, eff 0.05, busy 0.53 | 0.019 ms, 0.1x, eff 0.01, busy 0.34 | 0.041 ms, 0.0x, eff 0.00, busy 0.31 | 0.067 ms, 0.0x, eff 0.00, busy 0.29 | 0.099 ms, 0.0x, eff 0.00, busy 0.34 |
| reduce_counter | 2.069 ms, 1.0x, eff 1.00, busy 0.94 | 1.069 ms, 1.9x, eff 0.97, busy 0.73 | 0.593 ms, 3.5x, eff 0.87, busy 0.52 | 0.325 ms, 6.4x, eff 0.79, busy 0.32 | 0.247 ms, 8.4x, eff 0.70, busy 0.24 | 0.344 ms, 6.0x, eff 0.38, busy 0.19 |
| atomic_counter | 7.805 ms, 1.0x, eff 1.00, busy 0.98 | 24.623 ms, 0.3x, eff 0.16, busy 0.98 | 48.095 ms, 0.2x, eff 0.04, busy 0.97 | 139.508 ms, 0.1x, eff 0.01, busy 0.98 | 123.592 ms, 0.1x, eff 0.01, busy 0.97 | 250.980 ms, 0.0x, eff 0.00, busy 0.95 |
| atomic_counter_16 | 2.072 ms, 1.0x, eff 1.00, busy 0.93 | 4.638 ms, 0.4x, eff 0.22, busy 0.88 | 6.173 ms, 0.3x, eff 0.08, busy 0.86 | 9.676 ms, 0.2x, eff 0.03, busy 0.82 | 10.114 ms, 0.2x, eff 0.02, busy 0.88 | 16.434 ms, 0.1x, eff 0.01, busy 0.88 |
| atomic_counter_256 | 2.232 ms, 1.0x, eff 1.00, busy 0.93 | 1.319 ms, 1.7x, eff 0.85, busy 0.77 | 0.722 ms, 3.1x, eff 0.77, busy 0.59 | 0.808 ms, 2.8x, eff 0.35, busy 0.52 | 1.022 ms, 2.2x, eff 0.18, busy 0.55 | 1.223 ms, 1.8x, eff 0.11, busy 0.56 |
