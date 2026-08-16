# termux-glibc-compat

Focused Linux compatibility for running glibc applications natively inside an
unrooted Termux app sandbox—without putting every syscall through PRoot's
`ptrace` tracer.

This is an early research and implementation project. It does **not** yet run
Steam or Proton without PRoot.

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
- an evidence-backed compatibility matrix;
- a no-ptrace architecture for a per-Termux-UID semaphore broker; and
- a staged path from probes to a patched Termux glibc package and native Steam
  experiment.

The first native Tab S8+ run is also captured: pthreads and cross-process SysV
shared memory pass; robust-list and SysV semaphore calls return `ENOSYS`. This
confirms that semaphore compatibility is the first implementation target. See
the [raw result](docs/results/2026-08-16-tab-s8plus-glibc-2.42.txt).

The same-UID Unix socket transport, blocking `semop`, and glibc integration are
the next implementation milestone. See
[`docs/ROADMAP.md`](docs/ROADMAP.md).

## Run the probes

On conventional Linux:

```sh
make check
```

The runner reports `PASS`, `UNSUPPORTED`, or `FAIL`. `UNSUPPORTED` is a useful
Android baseline, not a false pass. A broken implementation that advertises a
feature but violates its semantics is `FAIL`.

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
