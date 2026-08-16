#!/usr/bin/env bash

set -euo pipefail
umask 077

CDPATH=''
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
package=${1:-}
broker_repo=${2:-$repo_dir}
temp_parent=${TMPDIR:-/tmp}
stage=
broker_started=0

die() {
    printf 'test-extracted-package: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    if ((broker_started != 0)); then
        "$broker_repo/scripts/tgcompat-session.sh" stop >/dev/null 2>&1 || true
    fi
    if [[ -n $stage && -d $stage && ! -L $stage &&
            $stage == "$temp_parent"/tgcompat-glibc-test.* ]]; then
        rm -rf -- "$stage"
    fi
}
trap cleanup EXIT

[[ -n $package ]] || die 'usage: test-extracted-package.sh ABSOLUTE_DEB [BROKER_REPO]'
[[ $package == /* && -f $package && ! -L $package ]] ||
    die "package must be an absolute regular file: $package"
[[ $temp_parent == /* && -d $temp_parent && ! -L $temp_parent ]] ||
    die "TMPDIR must be an absolute real directory: $temp_parent"
[[ -x $broker_repo/scripts/tgcompat-session.sh &&
        -x $broker_repo/build/tgcompatd ]] ||
    die "optimized broker is unavailable in: $broker_repo"
command -v dpkg-deb >/dev/null 2>&1 || die 'dpkg-deb is required'

stage=$(mktemp -d "$temp_parent/tgcompat-glibc-test.XXXXXX")
dpkg-deb -x "$package" "$stage/root"

candidate_prefix=$stage/root${PREFIX:?Termux PREFIX is required}/glibc
candidate_lib=$candidate_prefix/lib
loader=$candidate_lib/ld-linux-aarch64.so.1
[[ -x $loader && -f $candidate_lib/libc.so.6 ]] ||
    die 'package does not contain the expected AArch64 Termux glibc layout'

probe=$stage/sysv-semaphore
compiler=${GLIBC_CC:-$PREFIX/glibc/bin/gcc}
[[ -x $compiler ]] || die "glibc compiler is unavailable: $compiler"
"$compiler" -O2 -g -Wall -Wextra -Werror -Wpedantic -pthread \
    "$repo_dir/probes/sysv-semaphore.c" -o "$probe"

if ! "$broker_repo/scripts/tgcompat-session.sh" status >/dev/null 2>&1; then
    "$broker_repo/scripts/tgcompat-session.sh" start
    broker_started=1
fi
if [[ -n ${TGCOMPAT_SOCKET:-} ]]; then
    socket_path=$TGCOMPAT_SOCKET
else
    socket_path=$temp_parent/tgcompat-$(id -u)/broker.sock
fi
[[ -S $socket_path ]] || die "broker socket is unavailable: $socket_path"

export TGCOMPAT_SOCKET=$socket_path
unset LD_PRELOAD LD_LIBRARY_PATH GLIBC_LD_LIBRARY_PATH

printf 'package_sha256='; sha256sum "$package" | awk '{print $1}'
printf 'candidate_loader=%s\n' "$loader"
"$loader" --inhibit-cache --library-path "$candidate_lib" --verify "$probe"
"$loader" --inhibit-cache --library-path "$candidate_lib" --list "$probe"
"$loader" --inhibit-cache --library-path "$candidate_lib" "$probe"
printf '%s\n' 'extracted patched-glibc package: PASS'
