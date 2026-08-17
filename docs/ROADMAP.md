# Roadmap

## Milestone 0: executable baseline

- [x] Strict pthread, robust-list, SysV semaphore, and SysV shared-memory probes
- [x] Conventional Linux CI
- [x] Upstream Termux glibc source audit
- [x] Capture the same probes on the Tab S8+ with a glibc-linked toolchain

## Milestone 1: semaphore protocol

- [x] Versioned request/response definitions
- [x] Same-UID broker startup and peer validation
- [x] In-memory `semget`, key lookup, and generation-safe IDs
- [x] In-memory `GETVAL`, `GETALL`, `SETVAL`, and atomic `SETALL`
- [x] Atomic in-memory `semop` evaluation and blocking/`IPC_NOWAIT` outcomes
- [x] Expose the completed state-core operations through the broker dispatcher
- [x] Broker-side blocking queues and wakeups
- [x] `GETPID`, `GETNCNT`, and `GETZCNT`
- [x] Semaphore owner/mode/time metadata plus `IPC_INFO` and `SEM_INFO`
- [x] Linux `SEM_STAT` and `SEM_STAT_ANY` indexed lookup
- [x] `semtimedop` using monotonic deadlines
- [x] `IPC_RMID` and disconnected-waiter cleanup
- [x] `SEM_UNDO` process-exit behavior

## Milestone 2: Termux glibc integration

- [x] Build and validate the optimized native ARM64 broker on the Tab S8+
- [x] Patch glibc's SysV semaphore entry points to the client library
- [x] Link a host glibc 2.44 `libc.so` and pass the public black-box suite
- [x] Build through the official `termux/glibc-packages` recipe
- [x] Run this repository's black-box suite through the extracted package
- [x] Run glibc's `test-sysvsem`
- [x] Document packaging, rollback, and coexistence with stock glibc

## Milestone 3: native Steam client

- [x] Inventory the ARM64 bootstrap and CEF library/path requirements
- [ ] Launch updater/client directly through the patched glibc loader
- [ ] Preserve the existing authenticated state without copying secrets into
      logs or the repository
- [ ] Validate D-Bus, X11, audio, networking, and CEF independently
- [ ] Prove there is no PRoot tracer in the process tree

## Milestone 4: runtime and games

- [x] Isolate Pressure Vessel namespace/filesystem requirements
- [x] Document the required PRoot boundary for Pressure Vessel/Proton/FEX
- [ ] Re-run the Tomb Raider built-in benchmark with matched settings and
      thermal state
- [ ] Publish CPU, GPU, memory, frame-time, and correctness comparisons
