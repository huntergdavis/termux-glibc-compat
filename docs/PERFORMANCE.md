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

Removing whole-packet zeroing then reduced three-run `-O2` medians to 37,218.8
ns for raw `PING` and 42,394.4 ns for raw `GETVAL`. Relative to the initial
baseline, `GETVAL` latency is down 18.0% and throughput is up 21.9%. The exact
high-level persistent client measured median 42,823.6 ns `PING` and 46,793.3 ns
`GETVAL`, or roughly 21,400 complete validated `GETVAL` calls per second. It
adds fork detection and full response correlation without reconnecting.

The first Tab S8+ ThinLTO build also established that Clang 21's
`-mcpu=native` is unsafe on this heterogeneous Android CPU: it selected
Cortex-X2 and enabled SVE/SVE2, while `/proc/cpuinfo` did not expose SVE to the
process, and the optimized benchmark raised `SIGILL`. Native release builds now
derive only conservative, process-wide AArch64 extensions from the kernel's
`Features` line and execute a broker/client smoke benchmark before reporting
success.

## 2026-08-16 Tab S8+ native broker

The corrected Bionic build used Clang 21 ThinLTO and the kernel-reported common
`armv8-a+crc+crypto+lse` feature set. One 20,000-operation pass after the
built-in warmup measured:

| Case | ns/operation | operations/second |
|---|---:|---:|
| `PING` | 103,581.8 | 9,654 |
| `GETVAL` | 111,666.6 | 8,955 |
| persistent client `PING` | 120,025.8 | 8,332 |
| persistent client `GETVAL` | 108,384.3 | 9,226 |

This establishes that the native service and optimized client execute on the
target. The official patched glibc package now also passes its extracted public
API suite. The authenticated Steam/CEF host now runs through its
content-addressed loader without a Steam-host PRoot tracer.

## 2026-08-17 native Steam and controlled game measurements

The controlled native-host Tomb Raider launch reached the first 2800x1752 game
window 58.256 seconds after the Runtime request, versus 407.236 seconds in the
earlier all-PRoot observation. That interval is 85.7% shorter, or 6.99x as
fast. This is a startup result, not an FPS result.

The completed 119.92 Hz control at 2800x1752 Low, V-Sync off reported:

| Pass | Minimum | Maximum | Average |
|---|---:|---:|---:|
| Recorded 1 | 14.6 | 32.4 | 24.8 |
| Recorded 2 | 16.3 | 33.6 | 22.7 |
| Recorded 3 | 16.4 | 31.7 | 22.7 |
| Mean | **15.767** | **32.567** | **23.400** |

The earlier PRoot-host three-run mean was 22.2 FPS average, so this native-host
control is 5.4% higher. Each pass began with full CPU/GPU policy after automatic
cooldown.

Changing only the X presentation rate from 119.92 to a verified 59.97 Hz then
reported:

| Pass | Minimum | Maximum | Average |
|---|---:|---:|---:|
| Recorded 1 | 17.3 | 33.8 | 25.3 |
| Recorded 2 | 13.4 | 35.5 | 24.9 |
| Recorded 3 | 17.9 | 34.2 | 25.3 |
| Mean | **16.200** | **34.500** | **25.167** |

Average FPS improved 7.6%, maximum 5.9%, minimum 2.7%, and average-FPS median
11.5%. This is a controlled display-rate result, not a pure glibc claim. Every
pass still ended with the GPU at 492 MHz/thermal level six; 60 Hz did not remove
the tablet's thermal ceiling.

Process evidence explains why startup and FPS moved differently. Steam itself
is native glibc now, while the game still enters Runtime/Proton/FEX through one
explicit PRoot filesystem process because unrooted Android rejects the needed
Bubblewrap namespaces. Removing that remaining game-boundary tracer is the next
structural performance experiment.

The exact game series live in the companion repository:
[`119.92 Hz control`](https://github.com/huntergdavis/steamclienttermux/blob/main/docs/benchmark-series/tombraider-native-glibc-safe-119hz-20260817.json)
and
[`59.97 Hz A/B`](https://github.com/huntergdavis/steamclienttermux/blob/main/docs/benchmark-series/tombraider-native-glibc-safe-60hz-20260817.json).

A fixed-40 C bundled-Proton profile repeat at 59.97 Hz then reported
5.1/34.7/22.8, 14.4/30.8/22.7, and 18.0/33.4/25.2 FPS. Its aggregate was
12.500/32.967/23.567 FPS, 6.4% below `safe` in average FPS; average-FPS median
was 22.8 versus 25.3. Every pass began at 37.0-37.6 C with full CPU/GPU policy,
so `safe` remains the accepted native-resolution profile. The 5.1 FPS minimum
belonged to a pass ending at 83.8 C and thermal level six, illustrating why
profile selection uses the three-run mean and median. The
[`matched Proton series`](https://github.com/huntergdavis/steamclienttermux/blob/main/docs/benchmark-series/tombraider-native-glibc-proton-60hz-40c-20260817.json)
preserves the exact evidence.

The matched `fast` profile then reported 17.6/33.9/25.5,
16.3/33.9/23.0, and 15.2/29.1/22.9 FPS. Its aggregate was
16.367/32.300/23.800 FPS, 5.4% below `safe` in average FPS and only 1.0% above
bundled Proton. Average-FPS median was 23.0 versus `safe` at 25.3. Every
recorded pass began at exactly 37.0 C with full CPU/GPU policy and GPU thermal
level zero; end temperatures were 63.3, 59.5, and 60.5 C. Available RAM after
the final pass was 3,175,508 KiB, so the result was not memory-pressure or OOM
limited. The correctness-risking TSO-off profile remains opt-in, and `safe`
remains selected. The
[`matched fast series`](https://github.com/huntergdavis/steamclienttermux/blob/main/docs/benchmark-series/tombraider-native-glibc-fast-60hz-40c-20260817.json)
preserves the exact evidence.

This completes the bounded FEX-profile phase without closing the performance
gap. The next structural experiment remains removal or reduction of the
explicit game-boundary PRoot tracer.

## Rules for performance claims

1. Reuse a persistent connection; reconnect cost is not a hot-path design.
2. Run correctness tests before and after an optimization.
3. Compare medians from repeated runs at the same CPU/thermal state.
4. Report native tablet results separately from workstation results.
5. Do not attribute whole-game gains to this microbenchmark. The meaningful
   A/B is native Steam/game launch against the matched PRoot baseline.
