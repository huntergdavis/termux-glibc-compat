# Termux glibc 2.44 integration

This overlay replaces Termux glibc's intentional `ENOSYS` System V semaphore
stubs with the tested persistent tgcompat client. It targets upstream
`termux/glibc-packages` commit
`954c6b200aa001088fcc420550b9304dd81229b8` and refuses any other revision.

The patch keeps glibc's public symbol versions, time32/time64 conversion, and
`semctl` varargs handling. Only the final syscall boundary changes. The
following operations route to the same-UID broker:

- `semget`, `semop`, and `semtimedop`;
- `IPC_RMID`, `IPC_STAT`, `IPC_SET`, `IPC_INFO`, and `SEM_INFO`;
- Linux `SEM_STAT` and `SEM_STAT_ANY` index lookups;
- `GETVAL`, `SETVAL`, `GETPID`, `GETNCNT`, and `GETZCNT`; and
- `GETALL` and `SETALL`.

`SEM_UNDO` is complete, including atomic per-process adjustment accounting,
Linux-compatible bounds and clearing rules, and process-exit restoration.
Blocking/timed waits use one monotonic deadline in the broker.

## Apply

Use a clean, pinned checkout:

```sh
git clone https://github.com/termux/glibc-packages.git
git -C glibc-packages checkout 954c6b200aa001088fcc420550b9304dd81229b8
./integration/termux-glibc/apply-overlay.sh "$PWD/glibc-packages"
```

The installer dry-runs both patches, validates every source input, refuses a
dirty target tree, and then copies the current tested protocol/transport/client
sources into `gpkg/glibc`. The package recipe applies the semaphore entry-point
patch after the existing fake-syscall patch.

At runtime, export an absolute socket path before starting a glibc process:

```sh
export TGCOMPAT_SOCKET="$HOME/.cache/tgcompat-run/broker.sock"
```

If the variable is absent, the patched semaphore functions fail with `ENOSYS`;
there is no insecure implicit socket location.

## Validation

The overlay has been applied after the official fake-syscall patch and built
against glibc 2.44 as both static and shared sysvipc objects. A complete
`libc.so` link then succeeded. The repository's public Linux semaphore probe
was run through that new loader against `tgcompatd`; it passed `IPC_STAT`,
`IPC_SET`, `IPC_INFO`, `SEM_STAT_ANY`, `GETALL`, `SETALL`, `GETPID`, `GETNCNT`,
a timed `EAGAIN`, cross-process blocking/wakeup behavior, and `SEM_UNDO`
restoration after a child exits.

The persistent connection is one socket per calling thread so a blocking
`semop` cannot deadlock unrelated semaphore calls. A pthread-key destructor
closes it when the thread exits, preventing descriptor growth in games that
churn worker threads.

## Rollback

This overlay does not install or replace the tablet's libc. Before a package is
installed, rollback is simply discarding the dedicated glibc-packages checkout.
For the first tablet install, keep the downloaded stock package and test through
an isolated package root before changing the active glibc prefix.
