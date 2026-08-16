#!/usr/bin/env bash
set -u

repo_root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

if [[ ${1-} != --no-build ]]; then
    make -C "$repo_root" all || exit 1
fi

probes=(
    pthread-basic
    robust-list
    sysv-shm
    sysv-semaphore
)

printf 'kernel=%s\n' "$(uname -srmo 2>/dev/null || uname -a)"
if command -v getconf >/dev/null 2>&1; then
    printf 'libc=%s\n' "$(getconf GNU_LIBC_VERSION 2>/dev/null || printf unknown)"
fi

passed=0
unsupported=0
failed=0
android_sigsys_status=
if [[ $(uname -o 2>/dev/null || true) == Android ]]; then
    sigsys_number=$(kill -l SIGSYS 2>/dev/null || true)
    if [[ $sigsys_number =~ ^[1-9][0-9]*$ ]]; then
        android_sigsys_status=$((128 + sigsys_number))
    fi
fi

for name in "${probes[@]}"; do
    printf '\n[%s]\n' "$name"
    "$repo_root/build/$name"
    status=$?
    case $status in
        0)
            printf 'RESULT %s PASS\n' "$name"
            ((passed += 1))
            ;;
        77|"$android_sigsys_status")
            printf 'RESULT %s UNSUPPORTED\n' "$name"
            ((unsupported += 1))
            ;;
        *)
            printf 'RESULT %s FAIL exit=%d\n' "$name" "$status"
            ((failed += 1))
            ;;
    esac
done

printf '\nSUMMARY pass=%d unsupported=%d fail=%d\n' \
    "$passed" "$unsupported" "$failed"

((failed == 0))
