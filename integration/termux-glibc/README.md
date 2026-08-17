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
patch after the existing fake-syscall patch. The overlay also restores
Termux's configured make job count to the glibc recipe's overridden build step,
so clean package rebuilds compile in parallel without changing release flags.

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

The repository includes that extraction-only gate. It never invokes `dpkg -i`
or writes below the active glibc prefix: it extracts the `.deb` under `TMPDIR`,
compiles the public probe with the installed compiler, and invokes the
candidate loader with an explicit candidate-only library path:

```sh
integration/termux-glibc/test-extracted-package.sh \
  /absolute/path/to/glibc_2.44-*.deb
```

The script starts the native broker only when needed, verifies the package and
loader layout, prints the package hash and dependency resolution, runs the
complete public semaphore probe, then removes only its validated temporary
directory. Existing Steam client/config files are outside its scope.

This gate passed on the Tab S8+ for the official-recipe glibc 2.44 package with
SHA-256
`bd490b547660f7857e26a02fff168d7818e1b6d49adab37f0cc7d7566c9aed7c`.
`semget`, control operations, cross-process blocking, and wakeup completed
through the extracted candidate loader and `tgcompatd`.

After that test passes, stage the exact package in a content-addressed,
user-owned directory:

```sh
integration/termux-glibc/stage-extracted-package.sh \
  /absolute/path/to/glibc_2.44_aarch64.deb
```

This repeats the black-box test, extracts the package beneath
`~/.local/share/tgcompat/glibc/<sha256>`, and atomically points this project's
`current` selector at it. The selector is not the active Termux glibc prefix;
the installed package and Steam tree remain untouched. The printed
`candidate_loader` path is the explicit loader input for native experiments.
