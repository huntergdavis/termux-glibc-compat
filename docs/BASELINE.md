# Measured baseline

## Device and workload

- Samsung Galaxy Tab S8+ (SM-X808U)
- Snapdragon 8 Gen 1 / Adreno 730
- Android app UID, no root
- Termux glibc 2.42 and `glibc-runner` 2.0-3 on the measured tablet
- Native ARM64 Steam client, official ARM64 Proton, FEX, DXVK, and Turnip

During a live Tomb Raider (2013) menu profile, `TombRaider.exe` used 215–233%
CPU, the outer PRoot tracer used 60–65%, wineserver used 31–33%, and Steam/CEF
used roughly another core. KGSL reported only 12–16% GPU busy. This does not
predict the eventual native-host speedup, but it proves that the tracer is a
material CPU consumer in the current stack.

## Compatibility matrix before this project

| Capability | Patched PRoot control | Termux glibc upstream | Native target |
|---|---:|---:|---:|
| Ordinary pthread creation | Pass | **Pass on device** | Pass |
| Kernel robust-list registration | Emulated registration/query | **`ENOSYS` on device** | Graceful fallback |
| SysV shared memory | Pass | **Pass on device** | Pass |
| SysV semaphore create/control | Pass after local patch | **`ENOSYS` on device** | Pass |
| Blocking `semop` wakeup | Pass after local patch | Blocked by `semget` `ENOSYS` | Pass |
| `GETPID`/`GETNCNT`/`GETZCNT` | Pass after local patch | Blocked by `semget` `ENOSYS` | Pass |
| Pressure Vessel namespaces | PRoot-specific workarounds | Not supplied | Later phase |

The device run used `gcc-glibc` 14.2.1-1 to produce glibc-linked AArch64
binaries and `glibc-runner` to execute them. It reported two passes, two
unsupported facilities, and zero failures. The complete stdout is retained in
[`docs/results/2026-08-16-tab-s8plus-glibc-2.42.txt`](results/2026-08-16-tab-s8plus-glibc-2.42.txt).

## Upstream source audit

The official Termux glibc mirror was inspected at commit
`954c6b200aa001088fcc420550b9304dd81229b8` on 2026-08-16. Its build recipe:

- removes NPTL calls to `set_robust_list` and marks the facility unavailable;
- substitutes Android System V shared-memory sources derived from
  `libandroid-shmem`; and
- lists `set_robust_list`, `get_robust_list`, all four SysV semaphore calls,
  and SysV message queues in the `ENOSYS` fallback set.

Relevant primary sources:

- [glibc build recipe](https://github.com/termux/glibc-packages/blob/954c6b200aa001088fcc420550b9304dd81229b8/gpkg/glibc/build.sh)
- [NPTL Android patch](https://github.com/termux/glibc-packages/blob/954c6b200aa001088fcc420550b9304dd81229b8/gpkg/glibc/set-nptl-syscalls.patch)
- [unsupported syscall table](https://github.com/termux/glibc-packages/blob/954c6b200aa001088fcc420550b9304dd81229b8/gpkg/glibc/fakesyscall.json)
- [Android shared-memory implementation](https://github.com/termux/glibc-packages/blob/954c6b200aa001088fcc420550b9304dd81229b8/gpkg/glibc/shmem-android.c)

## Reused local evidence

The companion project established the required semaphore behavior by repairing
PRoot's partial SysV implementation. The retained patch implements `GETPID`,
`SETALL`, `GETNCNT`, `GETZCNT`, per-semaphore last-operation PIDs, and wakeups
after value changes. Those behaviors become black-box requirements here; the
ptrace implementation itself is not the new architecture.

The required cross-session recall query returned no matches. The reusable
evidence is therefore the companion repository's logged measurements, probe,
and exact PRoot patch, not an unrecorded earlier implementation.
