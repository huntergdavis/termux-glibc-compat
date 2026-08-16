# termux-glibc-compat

Focused Linux compatibility for running glibc applications natively inside an
unrooted Termux app sandbox—without putting every syscall through PRoot's
`ptrace` tracer.

This is an early research and implementation project. The native glibc patch
is host-validated, but it does **not** yet run Steam or Proton without PRoot on
the tablet.

## Why this exists

The companion
[`steamclienttermux`](https://github.com/huntergdavis/steamclienttermux)
project runs Valve's native ARM64 Steam client and Windows games on a Samsung
Galaxy Tab S8+. A live Tomb Raider (2013) profile measured the outer PRoot
tracer using 60–65% of one CPU core while GPU utilization remained only
12–16%. That makes removing PRoot from the hot path the largest structural
performance opportunity.

The goal here is deliberately narrower than “rewrite glibc”:

1. use the existing Termux glibc packages and loader;
2. measure the kernel/API differences that block real applications;
3. implement only the missing compatibility behavior at stable libc
   boundaries; and
4. keep ordinary files, sockets, futexes, graphics, and process execution on
   their native kernel paths.

No order-of-magnitude speedup is promised. The first success criterion is a
correct native Steam client launch with no PRoot tracer; game benchmarks come
after correctness.

## What upstream already provides

The official Termux glibc package is more than a relocated stock glibc. At
upstream commit
[`954c6b2`](https://github.com/termux/glibc-packages/commit/954c6b200aa001088fcc420550b9304dd81229b8),
it already:

- suppresses NPTL robust-list registration on Android;
- provides Android-backed System V shared-memory functions; and
- routes unsupported syscalls through explicit fallback handling.

The remaining measured gap is System V semaphores. Termux glibc currently maps
`semget`, `semctl`, `semop`, and `semtimedop` to `ENOSYS`. Steam required
working semaphore metadata plus wakeups after `SETVAL`, `SETALL`, and successful
operations in the PRoot implementation.

## Repository status

The first milestone is checked in:

- strict C probes for pthread creation, robust-list behavior, SysV semaphore
  wakeups, and cross-process SysV shared memory;
- a tested in-memory semaphore-set core with generation-safe IDs, Linux-like
  key lookup, atomic `SETALL` validation, and all-or-nothing multi-operation
  evaluation;
- a version-1, explicitly little-endian and bounded request/response protocol,
  plus a strict dispatcher exposing every completed state-core operation;
- a runnable `tgcompatd` Unix-socket broker with mode-0700 runtime-directory
  validation, a mode-0600 socket, and same-UID `SO_PEERCRED` authentication;
- concurrent client workers and broker-side FIFO `semop` wait/wakeup handling;
- ownership/mode/timestamp metadata, `GETNCNT`/`GETZCNT`, `IPC_INFO`,
  `SEM_INFO`, indexed `SEM_STAT`/`SEM_STAT_ANY`, and monotonic `semtimedop`
  deadlines;
- Linux-compatible per-process `SEM_UNDO` accounting, including process-exit
  restoration observed with `pidfd_open` and a `/proc` fallback;
- a lazy, persistent, per-thread native client API that reconnects safely after
  `fork`, closes its connection at thread exit, and never allocates or
  reconnects in steady-state calls;
- a pinned Termux glibc 2.44 package overlay that preserves public symbol and
  time ABIs while replacing only the semaphore syscall boundary;
- a successful real `libc.so` link and public-API probe through that built
  loader against `tgcompatd`;
- an evidence-backed compatibility matrix;
- a no-ptrace architecture for a per-Termux-UID semaphore broker; and
- a staged path from probes to a patched Termux glibc package and native Steam
  experiment.

The first native Tab S8+ run is also captured: pthreads and cross-process SysV
shared memory pass; robust-list and SysV semaphore calls return `ENOSYS`. This
confirms that semaphore compatibility is the first implementation target. See
the [raw result](docs/results/2026-08-16-tab-s8plus-glibc-2.42.txt).

A safe isolated tablet package test and the first native Steam client launch
are the next implementation milestones. The integration overlay and its exact
upstream pin are documented in
[`integration/termux-glibc/README.md`](integration/termux-glibc/README.md).
See also [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Run the broker

The socket directory is part of the security boundary. It must belong to the
current UID and have exact mode 0700:

```sh
install -d -m 0700 "$HOME/.cache/tgcompat-run"
./build/tgcompatd --socket "$HOME/.cache/tgcompat-run/broker.sock"
```

`TGCOMPAT_SOCKET` can provide the same absolute path. Startup refuses to
replace an existing path; a normally stopped broker removes its own socket.

## Run the probes

On conventional Linux:

```sh
make check
make benchmark
```

On Bionic/Android, `make check-broker` runs the complete broker, protocol,
transport, client, fork, timeout, and `SEM_UNDO` suite without conflating those
results with the separate host-libc capability probes. `make check` also works
when Termux's `libandroid-shmem` and `libandroid-sysv-semaphore` development
packages are installed; the build selects those Bionic link shims
automatically. Android seccomp `SIGSYS` is reported as `UNSUPPORTED`, not as a
false semantic pass. Glibc behavior must still be tested through
`glibc-runner`.

The runner reports `PASS`, `UNSUPPORTED`, or `FAIL`. `UNSUPPORTED` is a useful
Android baseline, not a false pass. A broken implementation that advertises a
feature but violates its semantics is `FAIL`.

The benchmark excludes daemon startup and reuses one authenticated connection,
matching the intended native-client hot path. It reports `PING` and `GETVAL`
round-trip latency and throughput; tablet results, not workstation numbers,
are the performance gate. Method and current measurements are in
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

`SEM_UNDO` behavior follows the upstream Linux implementation, including its
`-32768..32767` accumulated adjustment range and clearing adjustments after
`SETVAL`, `SETALL`, or removal. The broker tracks one authenticated process,
not one client thread, so all connections from the same process share the undo
list just as Linux threads using `CLONE_SYSVSEM` do.

For a stripped LTO build:

```sh
make release
```

The production helper selects ThinLTO under Clang, parallelizes the build, and
tunes for kernel-reported CPU features by default:

```sh
scripts/build-release.sh --native --check
```

The helper parallelizes compilation but runs Android capability probes and the
broker gate outside the parallel GNU make jobserver. This avoids a reproduced
Termux make 4.4.1/Scudo self-crash after `SIGSYS` probe children; it does not
weaken any test.

Use `--portable` for a redistributable binary. Published binaries deliberately
do not assume one ARM core design. On heterogeneous AArch64 Android devices the
helper deliberately avoids `-mcpu=native`: Clang can select the largest core
and emit SVE instructions that Android does not expose uniformly. A post-link
broker/client smoke benchmark rejects such unusable builds immediately.

The session helper owns a private runtime directory and performs PID/executable
validation before signaling anything:

```sh
scripts/tgcompat-session.sh start
eval "$(scripts/tgcompat-session.sh env)"
# launch any patched-glibc application here
scripts/tgcompat-session.sh stop
```

`scripts/tgcompat-session.sh run COMMAND...` combines startup and environment
setup. It leaves the broker alive so child processes and subsequent Steam games
keep the same compatibility service; `stop` shuts it down explicitly.

`build/libtgcompat-client.a` contains the caller-owned persistent client. Its
public API is [`include/tgcompat/client.h`](include/tgcompat/client.h); negative
errno results are preserved for the thin glibc boundary to translate.

On Termux, compile the probes with the same glibc toolchain used by the target
runtime. Do not use the ordinary Bionic-linked `gcc` alias to infer glibc
behavior:

```sh
pkg install glibc-repo glibc-runner gcc-glibc
glibc-runner -s 'make clean && make CC=gcc'
glibc-runner -s 'bash scripts/run-probes.sh --no-build'
```

## Design constraints

- Unrooted Android and the normal Termux application UID.
- No `ptrace`, root service, custom kernel, or global Android modification.
- No `LD_PRELOAD` claim for glibc-internal raw syscalls.
- Same-UID local IPC only; peer credentials and filesystem permissions are
  part of the protocol boundary.
- Linux-compatible blocking, atomic multi-operation, removal, and waiter
  wakeup behavior before performance work.
- Honest separation between client-host compatibility and the later Steam
  Runtime/Pressure Vessel namespace problem.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design and
[`docs/BASELINE.md`](docs/BASELINE.md) for the measured starting point.

## Provenance

This project starts from measurements, probes, and the 244-line Android PRoot
IPC patch in `steamclienttermux`. The required recall query
`deja "Steam ARM64 Termux replace PRoot native glibc runner robust list SysV IPC"`
returned no indexed prior implementation, so no undocumented session solution
was reused.

Upstream references:

- [Termux glibc packages](https://github.com/termux/glibc-packages)
- [Termux PRoot-Distro: PRoot limitations](https://github.com/termux/proot-distro#the-proot-utility)
- [Linux robust-futex ABI](https://github.com/torvalds/linux/blob/master/Documentation/locking/robust-futex-ABI.rst)
- [Linux SysV semaphore implementation](https://github.com/torvalds/linux/blob/master/ipc/sem.c)

## License

MIT. Contributions intended for inclusion in glibc must also remain compatible
with glibc's LGPL licensing.
