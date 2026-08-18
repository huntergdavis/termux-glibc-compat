#!/usr/bin/env bash

set -euo pipefail

CDPATH=''
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
shim=$repo_dir/build/libtgcompat-exec.so
driver=$repo_dir/build/test-exec-shim-driver
target=$repo_dir/build/test-exec-shim-target
final_target=$repo_dir/build/test-exec-shim-final-target

die() {
    printf 'test-exec-shim: %s\n' "$*" >&2
    exit 1
}

for path in "$shim" "$driver" "$target" "$final_target"; do
    [[ -f $path && ! -L $path ]] || die "missing build artifact: $path"
done
command -v readelf >/dev/null 2>&1 || die 'readelf is required'

loader=$(
    LC_ALL=C readelf -l "$driver" |
        sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'
)
[[ $loader == /* && -x $loader ]] || die "invalid host loader: $loader"
target_real=$(realpath -e "$target") || die "unable to resolve target: $target"
final_target_real=$(realpath -e "$final_target") ||
    die "unable to resolve final target: $final_target"
final_prefix=${final_target_real%/*}/

guest_loader=/no/tgcompat-direct-loader.so
for mode in execve-loader posix_spawn-loader; do
    output=$(
        env \
            LD_PRELOAD="$shim" \
            TGCOMPAT_LD_SO="$loader" \
            TGCOMPAT_EXEC_MATCH_INTERPRETER="$guest_loader" \
            TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
            TGCOMPAT_PROC_SELF_EXE="$target_real" \
            TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
            "$driver" "$mode" "$guest_loader" "$target"
    )
    [[ $output == 'exec shim target: PASS' ]] ||
        die "$mode direct loader redirect produced unexpected output: $output"
done

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

for mode in execve execv execvp execvpe execl posix_spawn posix_spawnp; do
    output=$(
        env \
            LD_PRELOAD="$shim" \
            TGCOMPAT_LD_SO="$loader" \
            TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
            TGCOMPAT_EXEC_SHELL="$target" \
            TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
            TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
            "$driver" "$mode" /bin/sh
    )
    [[ $output == 'exec shim target: PASS' ]] ||
        die "$mode shell redirect produced unexpected output: $output"
done

output=$(
    env \
        LD_PRELOAD="$shim" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_SHELL="$target" \
        TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
        TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
        "$driver" execve /usr/bin/sh
)
[[ $output == 'exec shim target: PASS' ]] ||
    die "/usr/bin/sh redirect produced unexpected output: $output"

redirect_source=/no/tgcompat-runtime-entry
for mode in execve execv execvp execvpe execl posix_spawn posix_spawnp; do
    output=$(
        env \
            LD_PRELOAD="$shim" \
            TGCOMPAT_LD_SO="$loader" \
            TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
            TGCOMPAT_EXEC_PATH_FROM="$redirect_source" \
            TGCOMPAT_EXEC_PATH_TO="$target" \
            TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
            TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
            "$driver" "$mode" "$redirect_source"
    )
    [[ $output == 'exec shim target: PASS' ]] ||
        die "$mode exact path redirect produced unexpected output: $output"
done

output=$(
    env \
        LD_PRELOAD="$shim" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_PATH_FROM="$redirect_source" \
        TGCOMPAT_EXEC_PATH_TO="$target" \
        TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
        TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
        "$driver" execve-filter-path-control "$redirect_source"
)
[[ $output == 'exec shim target: PASS' ]] ||
    die "process-level path policy produced unexpected output: $output"

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

output=$(
    env \
        LD_PRELOAD="$shim:/deliberately/wrong-preload.so" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_LD_PRELOAD="$shim" \
        TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
        TGCOMPAT_EXPECT_PROC_SELF_EXE="$target_real" \
        "$driver" execve-filter-control "$target" 2>/dev/null
)
[[ $output == 'exec shim target: PASS' ]] ||
    die "process-level LD_PRELOAD policy produced unexpected output: $output"

if env \
        LD_PRELOAD="$shim" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_DISABLE=1 \
        "$driver" execve "$target" >/dev/null 2>&1; then
    die 'TGCOMPAT_EXEC_DISABLE did not bypass the shim'
fi

for mode in execve posix_spawn; do
    output=$(
        env \
            LD_PRELOAD="$shim" \
            TGCOMPAT_LD_SO="$loader" \
            TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
            TGCOMPAT_EXEC_FINAL_PATH_PREFIX="$final_prefix" \
            TGCOMPAT_EXEC_FINAL_LD_PRELOAD= \
            TGCOMPAT_EXEC_FINAL_PROC_SELF_EXE= \
            TGCOMPAT_EXPECT_LD_PRELOAD= \
            TGCOMPAT_PROC_SELF_EXE=/deliberately/wrong \
            TGCOMPAT_EXPECT_PROC_SELF_EXE= \
            "$driver" "$mode" "$final_target"
    )
    [[ $output == 'exec shim target: PASS' ]] ||
        die "$mode final preload boundary produced unexpected output: $output"
done

output=$(
    env \
        LD_PRELOAD="$shim" \
        TGCOMPAT_LD_SO="$loader" \
        TGCOMPAT_EXEC_MATCH_INTERPRETER=/no/tgcompat-ld.so \
        TGCOMPAT_EXEC_FINAL_PATH_PREFIX=/no/tgcompat-final/ \
        TGCOMPAT_EXEC_FINAL_LD_PRELOAD= \
        TGCOMPAT_EXPECT_LD_PRELOAD="$shim" \
        TGCOMPAT_PROC_SELF_EXE="$final_target_real" \
        TGCOMPAT_EXPECT_PROC_SELF_EXE="$final_target_real" \
        "$driver" execve "$final_target"
)
[[ $output == 'exec shim target: PASS' ]] ||
    die "non-matching final preload boundary changed the environment: $output"

printf '%s\n' 'exec shim tests: PASS'
