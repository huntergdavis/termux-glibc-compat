# Performance method

The compatibility broker is not a general syscall translator. Only System V
semaphore operations cross its socket; file I/O, graphics, futexes, networking,
and process execution continue directly against the Android kernel. Removing
PRoot from those ordinary paths is the primary performance result this project
is designed to test.

`make benchmark` measures sequential broker round trips over one persistent,
already-authenticated Unix socket. Daemon startup, socket connection, and
semaphore creation are excluded. The two cases bracket protocol overhead:

- `PING`: an empty request and empty response;
- `GETVAL`: an eight-byte request and empty response plus a state lookup.

## 2026-08-16 workstation baseline

These numbers are development evidence, not tablet claims. The host reported
Linux 7.0 and glibc 2.43. A 100,000-iteration `-O2` run before request framing
was combined measured:

| Case | ns/operation | operations/second |
|---|---:|---:|
| `PING` | 44,154.5 | 22,648 |
| `GETVAL` | 51,674.7 | 19,352 |

Combining the fixed header and payload into one bounded send reduced the median
`GETVAL` result across three 50,000-iteration runs to 45,534.8 ns and 21,961
operations/second: 11.9% less latency and 13.5% more throughput. `PING`, which
never had a payload, remained within noise at 44,289.8 ns.

An `-O3`, LTO, no-PLT release build produced median results of 44,336.7 ns for
`PING` and 44,957.3 ns for `GETVAL`. That is not a defensible compiler-driven
speedup on this host; scheduler and socket context-switch cost dominate the C
work. The release target keeps the flags for size and target builds, while the
tablet benchmark remains the decision gate.

## Rules for performance claims

1. Reuse a persistent connection; reconnect cost is not a hot-path design.
2. Run correctness tests before and after an optimization.
3. Compare medians from repeated runs at the same CPU/thermal state.
4. Report native tablet results separately from workstation results.
5. Do not attribute whole-game gains to this microbenchmark. The meaningful
   A/B is native Steam/game launch against the matched PRoot baseline.
