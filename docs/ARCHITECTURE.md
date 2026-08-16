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
- per-process `SEM_UNDO` accounting and process-exit restoration.

Blocking clients keep their connection open. Peer closure lets the broker
remove pending requests. Undo state is keyed by the authenticated process ID,
shared by all that process's client threads, and survives an individual thread
or connection closing. A small monitor uses `pidfd_open` where available and a
PID-plus-`/proc`-inode fallback otherwise; once the process exits it applies
and clears all pending adjustments under the broker state lock, then wakes
waiters. This follows Linux process semantics without polling every active
client socket or putting a tracer in the application hot path.

### Version 1 wire boundary

The checked-in protocol uses a fixed 24-byte, explicitly little-endian header:
magic, version, request/response kind, opcode, zeroed reserved field, request
ID, bounded payload length, and a signed result. Requests require a zero result;
responses carry either the state-core return value or a negative errno. The
maximum payload is 8192 bytes, enough for the bounded 512-entry `SETALL` and
`SEMOP` messages without unbounded allocation.

Version 1 exposes ping, `semget`, removal, `GETVAL`, `SETVAL`, `GETPID`,
`GETNCNT`, `GETZCNT`, `GETALL`, `SETALL`, metadata get/set, `IPC_INFO`,
`SEM_INFO`, atomic `semop`, and monotonic `semtimedop`. Integer encoding is
manual rather than a cast of C structures, so compiler padding and host
alignment are not protocol state. Every variable array includes a count and
must match the header length exactly; operation records include a reserved
field that must be zero. The dispatcher is independent of Unix sockets so
malformed payloads are covered by ordinary unit tests before transport or
concurrency is involved.

The broker derives the operation PID from Linux `SO_PEERCRED`; mutating
requests do not carry a caller-controlled PID. The daemon refuses relative or
overlong socket paths, requires the containing directory to be owned by its
effective UID with exact mode 0700, creates the socket with mode 0600, and
never unlinks a pre-existing path during startup. A malformed or truncated
frame closes only that client connection.

Successful `SEM_UNDO` operations atomically update a bounded per-process undo
table. Accumulated adjustments use Linux's `-32768..32767` bounds. `SETVAL`
clears undo state for that semaphore; `SETALL` and `IPC_RMID` clear it for the
whole set. Process-exit restoration clamps values to the Linux semaphore value
range, updates `GETPID`/operation time, and broadcasts the resulting change.

Each authenticated connection has a bounded worker slot. State access stays
behind one broker mutex, while blocked `semop` requests enter a FIFO queue per
semaphore set and wait on a monotonic condition variable. `SETVAL`, `SETALL`,
successful operations, and removal broadcast a state change. Only the first
blocked request for a set retries, preserving queue order; unrelated sets can
make progress concurrently. Waiter counts are updated under the same state
mutex. Relative timeouts become one monotonic deadline when enqueued, and a
timed peer check removes abandoned waiters when a client disconnects.

The native client is a caller-owned structure rather than a heap object. One
instance belongs to one calling thread: after its lazy first connection, the
steady-state path has no allocation, client mutex, process creation, or socket
reconnect. Every call validates its correlated response. A cached owner PID
detects `fork`, closes the inherited descriptor in the child, and reconnects so
the broker's `SO_PEERCRED` PID remains authoritative for `GETPID` semantics.

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
