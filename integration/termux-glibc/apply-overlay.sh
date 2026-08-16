#!/usr/bin/env bash
set -euo pipefail

readonly EXPECTED_COMMIT=954c6b200aa001088fcc420550b9304dd81229b8

usage() {
    printf 'Usage: %s /absolute/path/to/glibc-packages\n' "$0" >&2
}

if (( $# != 1 )) || [[ "$1" != /* ]]; then
    usage
    exit 2
fi

readonly checkout=$1
readonly package_dir="$checkout/gpkg/glibc"
readonly script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
readonly project_root=$(cd -- "$script_dir/../.." && pwd -P)

if [[ ! -d "$checkout/.git" || ! -f "$package_dir/build.sh" ]]; then
    printf 'Not a glibc-packages checkout: %s\n' "$checkout" >&2
    exit 1
fi

readonly actual_commit=$(git -C "$checkout" rev-parse HEAD)
if [[ "$actual_commit" != "$EXPECTED_COMMIT" ]]; then
    printf 'Unsupported glibc-packages revision: %s (expected %s)\n' \
        "$actual_commit" "$EXPECTED_COMMIT" >&2
    exit 1
fi

if rg -q 'tgcompat-client' "$package_dir/build.sh"; then
    printf 'tgcompat overlay is already present in %s\n' "$checkout"
    exit 0
fi

if [[ -n $(git -C "$checkout" status --short) ]]; then
    printf 'Refusing to overlap an existing dirty glibc-packages tree.\n' >&2
    exit 1
fi

readonly source_files=(
    "$project_root/include/tgcompat/client.h"
    "$project_root/include/tgcompat/protocol.h"
    "$project_root/include/tgcompat/sem_store.h"
    "$project_root/include/tgcompat/transport.h"
    "$project_root/src/client.c"
    "$project_root/src/protocol.c"
    "$project_root/src/transport.c"
    "$script_dir/overlay/tgcompat-glibc.h"
    "$script_dir/overlay/tgcompat-glibc.c"
    "$script_dir/overlay/zz-tgcompat-semaphores.patch"
)
for source_file in "${source_files[@]}"; do
    if [[ ! -f "$source_file" ]]; then
        printf 'Missing overlay input: %s\n' "$source_file" >&2
        exit 1
    fi
done

patch -d "$checkout" -p1 --dry-run < "$script_dir/glibc-packages.patch"
patch -d "$checkout" -p1 < "$script_dir/glibc-packages.patch"

install -m 0644 "$project_root/include/tgcompat/client.h" \
    "$package_dir/tgcompat-client.h"
install -m 0644 "$project_root/include/tgcompat/protocol.h" \
    "$package_dir/tgcompat-protocol.h"
install -m 0644 "$project_root/include/tgcompat/sem_store.h" \
    "$package_dir/tgcompat-sem_store.h"
install -m 0644 "$project_root/include/tgcompat/transport.h" \
    "$package_dir/tgcompat-transport.h"
install -m 0644 "$project_root/src/client.c" \
    "$package_dir/tgcompat-client.c"
install -m 0644 "$project_root/src/protocol.c" \
    "$package_dir/tgcompat-protocol.c"
install -m 0644 "$project_root/src/transport.c" \
    "$package_dir/tgcompat-transport.c"
install -m 0644 "$script_dir/overlay/tgcompat-glibc.h" \
    "$package_dir/tgcompat-glibc.h"
install -m 0644 "$script_dir/overlay/tgcompat-glibc.c" \
    "$package_dir/tgcompat-glibc.c"
install -m 0644 "$script_dir/overlay/zz-tgcompat-semaphores.patch" \
    "$package_dir/zz-tgcompat-semaphores.patch"

git -C "$checkout" diff --check
printf 'Applied tgcompat overlay to glibc-packages %s\n' "$actual_commit"
printf 'Review with: git -C %q diff -- gpkg/glibc\n' "$checkout"
