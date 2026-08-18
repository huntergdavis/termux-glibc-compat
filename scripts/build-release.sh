#!/usr/bin/env bash

set -euo pipefail

CDPATH=''
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
profile=native
run_checks=0
jobs=${TGCOMPAT_BUILD_JOBS:-}

usage() {
    cat <<'EOF'
Usage: scripts/build-release.sh [--native|--portable] [--check] [--jobs N]

Builds the broker, glibc execution, robust-list, mprotect and Android-root
shims, and static client with LTO. Native mode tunes for the current device;
portable mode is redistributable.
EOF
}

while (($# > 0)); do
    case $1 in
        --native)
            profile=native
            ;;
        --portable)
            profile=portable
            ;;
        --check)
            run_checks=1
            ;;
        --jobs)
            shift
            (($# > 0)) || { printf '%s\n' 'missing value for --jobs' >&2; exit 2; }
            jobs=$1
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            printf 'unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [[ -z $jobs ]]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')
fi
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { printf '%s\n' 'jobs must be positive' >&2; exit 2; }

compiler=${CC:-cc}
archiver=${AR:-ar}
stripper=${STRIP:-strip}
compiler_kind=gcc
lto_flag=-flto

if "$compiler" --version 2>/dev/null | head -n 1 | grep -qi clang; then
    compiler_kind=clang
    lto_flag=-flto=thin
    command -v llvm-ar >/dev/null 2>&1 && archiver=llvm-ar
    command -v llvm-strip >/dev/null 2>&1 && stripper=llvm-strip
fi

cpu_flag=
if [[ $profile == native ]]; then
    target=$($compiler -dumpmachine 2>/dev/null || true)
    case $target in
        aarch64*)
            # Clang 21 maps -mcpu=native to the largest core on heterogeneous
            # Android systems. On the Tab S8+ that incorrectly enabled SVE/SVE2
            # even though the kernel did not expose either feature, producing
            # a binary that linked successfully and then raised SIGILL. Build
            # only from features Linux reports as process-wide instead.
            arm_march=armv8-a
            cpu_features=" $(sed -n 's/^Features[[:space:]]*:[[:space:]]*/ /p' \
                /proc/cpuinfo 2>/dev/null | head -n 1) "
            [[ $cpu_features == *' crc32 '* ]] && arm_march+=+crc
            if [[ $cpu_features == *' aes '* &&
                    $cpu_features == *' pmull '* &&
                    $cpu_features == *' sha1 '* &&
                    $cpu_features == *' sha2 '* ]]; then
                arm_march+=+crypto
            fi
            [[ $cpu_features == *' atomics '* ]] && arm_march+=+lse
            cpu_flag="-march=$arm_march"
            ;;
        arm*) cpu_flag=-mcpu=native ;;
        *) cpu_flag=-march=native ;;
    esac
fi

release_cflags="-O3 -DNDEBUG $lto_flag -fno-plt -fno-semantic-interposition -fomit-frame-pointer -ffunction-sections -fdata-sections"
release_ldflags="$lto_flag -Wl,-O2,--as-needed,--gc-sections -Wl,-z,relro,-z,now"

printf 'tgcompat release: compiler=%s kind=%s profile=%s jobs=%s cpu=%s\n' \
    "$compiler" "$compiler_kind" "$profile" "$jobs" "${cpu_flag:-portable}"

make_args=(
    -C "$repo_dir"
    -j"$jobs"
    "CC=$compiler"
    "AR=$archiver"
    "STRIP=$stripper"
    "RELEASE_CFLAGS=$release_cflags"
    "RELEASE_CPU_FLAGS=$cpu_flag"
    "RELEASE_LDFLAGS=$release_ldflags"
)

make "${make_args[@]}" release

# This exercises optimized code in both the daemon and client instead of
# accepting link success as proof that device-specific instructions execute.
(
    cd "$repo_dir"
    ./build/benchmark-broker-roundtrip 100 >/dev/null
)

if ((run_checks != 0)); then
    make -C "$repo_dir" -j"$jobs" clean
    make -C "$repo_dir" -j"$jobs" \
        "CC=$compiler" "AR=$archiver" \
        "CFLAGS=$release_cflags $cpu_flag" \
        "LDFLAGS=$release_ldflags" all
    # Keep execution outside a parallel GNU make jobserver. Termux make 4.4.1
    # under Android 16/Scudo corrupted its own heap after two SIGSYS-denied
    # probe children, even though every recipe had completed successfully.
    "$repo_dir/scripts/run-probes.sh" --no-build
    env -u MAKEFLAGS make -C "$repo_dir" -j1 \
        check-broker check-exec-shim check-android-root-shim \
        check-robust-shim check-mprotect-shim
    "$stripper" --strip-unneeded \
        "$repo_dir/build/tgcompatd" \
        "$repo_dir/build/libtgcompat-exec.so" \
        "$repo_dir/build/libtgcompat-android-root.so" \
        "$repo_dir/build/libtgcompat-robust.so" \
        "$repo_dir/build/libtgcompat-mprotect.so"
fi

printf 'built %s\n' "$repo_dir/build/tgcompatd"
