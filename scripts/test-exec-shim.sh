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
target_real=$(realpath -e "$target") || die "unable to resolve target: $target"

for mode in execve execv execvp execvpe execl posix_spawn posix_spawnp; do
    mode_target=$target
    mode_path=$PATH
    case $mode in
        execvp|execvpe|posix_spawnp)
            mode_target=${target##*/}
            mode_path=${target%/*}:$PATH
            ;;
    esac
    if env PATH="$mode_path" "$driver" "$mode" "$mode_target" \
            >/dev/null 2>&1; then
        die "$mode broken-interpreter fixture unexpectedly ran without the shim"
    fi

    output=$(
        env \
            PATH="$mode_path" \
            LD_PRELOAD="$shim" \
            TGCOMPAT_LD_SO="$loader" \
            TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
            TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
            TGCOMPAT_PROC_SELF_EXE=/deliberately/wrong \
            TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
            "$driver" "$mode" "$mode_target"
    )
    [[ $output == 'exec shim target: PASS' ]] ||
        die "$mode produced unexpected wrapped output: $output"
done

output=$(
    env \
        LD_PRELOAD="$shim:/deliberately/wrong-preload.so" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_LD_PRELOAD="$shim" \
        TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
        TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
        "$driver" execve "$target" 2>/dev/null
)
[[ $output == 'exec shim target: PASS' ]] ||
    die "LD_PRELOAD override produced unexpected output: $output"

if env \
        LD_PRELOAD="$shim" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_DISABLE=1 \
        "$driver" execve "$target" >/dev/null 2>&1; then
    die 'TGCOMPAT_EXEC_DISABLE did not bypass the shim'
fi

printf '%s\n' 'exec shim tests: PASS'
