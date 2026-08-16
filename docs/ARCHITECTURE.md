# Architecture

## Boundary

`glibc-runner` selects Termux's relocated glibc loader and libraries. It is not
a container and does not emulate syscalls. This project extends that native
glibc environment only where Android prevents the Linux behavior a real
application requires.

```text
glibc application
  -> ordinary libc/syscalls --------------------------> Android kernel
  -> SysV shm API -> existing Termux implementation --> memfd/ashmem + Unix IPC
  -> SysV sem API -> tgcompat client -----------------> same-UID broker
                                                        -> values/wait queues
                                                        -> replies/wakeups
```

The initial implementation target is a small client inside glibc's `sysvipc`
sources and a native `tgcompatd` broker. It is not a general syscall translator.

## Why not `LD_PRELOAD`

Preloading can intercept public dynamic symbols, but it cannot reliably replace
hidden libc calls or inline/raw syscall sites. Steam also launches helpers and
container tools that sanitize environments. Correctness therefore belongs at
the Termux glibc package boundary, with an optional preload build useful only
for early experiments.

## Semaphore broker

One broker runs per Termux Android UID. Its Unix-domain socket lives below a
mode-0700 runtime directory and accepts only peers whose `SO_PEERCRED` UID
matches the broker. The protocol is versioned, length-delimited, and has fixed
integer widths.

The broker owns:

- key lookup, `IPC_PRIVATE`, `IPC_CREAT`, and `IPC_EXCL`;
- semaphore-set IDs with generation counters;
- values, owner/mode metadata, and last-operation PIDs;
- atomic validation and application of multi-entry `semop` requests;
- FIFO blocking queues, `IPC_NOWAIT`, and monotonic timeouts;
- waiter counts and wakeups after every state change;
- `IPC_RMID` invalidation and wakeup behavior; and
- later, per-process `SEM_UNDO` accounting.

Blocking clients keep their connection open. Peer closure lets the broker
remove pending requests and later apply `SEM_UNDO`. This is intentionally
simpler to audit than a shared-memory lock whose owner can disappear without
kernel robust-list cleanup.

### Version 1 wire boundary

The checked-in protocol uses a fixed 24-byte, explicitly little-endian header:
magic, version, request/response kind, opcode, zeroed reserved field, request
ID, bounded payload length, and a signed result. Requests require a zero result;
responses carry either the state-core return value or a negative errno. The
maximum payload is 8192 bytes, enough for the bounded 512-entry `SETALL` and
`SEMOP` messages without unbounded allocation.

Version 1 exposes ping, `semget`, removal, `GETVAL`, `SETVAL`, `GETPID`,
`GETALL`, `SETALL`, and atomic `semop` evaluation. Integer encoding is manual
rather than a cast of C structures, so compiler padding and host alignment are
not protocol state. Every variable array includes a count and must match the
header length exactly; operation records include a reserved field that must be
zero. The dispatcher is independent of Unix sockets so malformed payloads are
covered by ordinary unit tests before transport or concurrency is involved.

## Robust lists

The current Termux glibc package removes NPTL robust-list registration and
marks it unavailable. Ordinary pthread mutexes still use futexes. We preserve
that known fallback and test pthread creation separately from the raw robust-
list syscall.

Robust process-shared mutex recovery is not claimed. If a target application
proves it needs that behavior, it becomes a separately measured requirement;
we will not report a fake registration as equivalent to kernel owner-death
handling.

## Filesystem and Pressure Vessel

Removing PRoot also removes its path rewriting and fake root filesystem.
The native Steam-client milestone will use explicit paths, environment, and
relocated glibc libraries. Steam Runtime/Pressure Vessel needs mount/user
namespace behavior that an unrooted Android app may not receive. That is a
later, separately tested layer; the semaphore broker must not grow into a
second all-purpose container.

## Success gates

1. All probes pass on conventional Linux.
2. The tablet baseline distinguishes unsupported features from semantic bugs.
3. The patched Termux glibc passes semaphore atomicity and wakeup probes.
4. Native ARM64 Steam reaches authenticated UI with no PRoot process.
5. The same workload is profiled A/B before any performance claim.
