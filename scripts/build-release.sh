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

Builds the broker and static glibc client with LTO. Native mode tunes for the
current device; portable mode is suitable for redistribution.
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
        aarch64*|arm*) cpu_flag=-mcpu=native ;;
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

if ((run_checks != 0)); then
    make -C "$repo_dir" -j"$jobs" clean
    make -C "$repo_dir" -j"$jobs" \
        "CC=$compiler" "AR=$archiver" \
        "CFLAGS=$release_cflags $cpu_flag" \
        "LDFLAGS=$release_ldflags" check
    "$stripper" --strip-unneeded "$repo_dir/build/tgcompatd"
fi

printf 'built %s\n' "$repo_dir/build/tgcompatd"
