#!/usr/bin/env bash

set -euo pipefail

CDPATH=''
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
shim=$repo_dir/build/libtgcompat-exec.so
driver=$repo_dir/build/test-exec-shim-driver
target=$repo_dir/build/test-exec-shim-target

die() {
    printf 'test-exec-shim: %s\n' "$*" >&2
    exit 1
}

for path in "$shim" "$driver" "$target"; do
    [[ -f $path && ! -L $path ]] || die "missing build artifact: $path"
done
command -v readelf >/dev/null 2>&1 || die 'readelf is required'

loader=$(
    LC_ALL=C readelf -l "$driver" |
        sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'
)
[[ $loader == /* && -x $loader ]] || die "invalid host loader: $loader"

if "$driver" "$target" >/dev/null 2>&1; then
    die 'broken-interpreter fixture unexpectedly ran without the shim'
fi

output=$(
    env \
        LD_PRELOAD="$shim" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        "$driver" "$target"
)
[[ $output == 'exec shim target: PASS' ]] ||
    die "unexpected wrapped output: $output"

if env \
        LD_PRELOAD="$shim" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_DISABLE=1 \
        "$driver" "$target" >/dev/null 2>&1; then
    die 'TGCOMPAT_EXEC_DISABLE did not bypass the shim'
fi

printf '%s\n' 'exec shim tests: PASS'
