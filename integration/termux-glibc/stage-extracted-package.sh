#!/usr/bin/env bash

set -euo pipefail
umask 077

CDPATH=''
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_dir=$(cd -- "$script_dir/../.." && pwd -P)
package=${1:-}
deploy_root=${2:-$HOME/.local/share/tgcompat/glibc}
stage=
selector=

die() {
    printf 'stage-extracted-package: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    if [[ -n $selector && -L $selector &&
            $selector == "$deploy_root"/.current.* ]]; then
        rm -- "$selector"
    fi
    if [[ -n $stage && -d $stage && ! -L $stage &&
            $stage == "$deploy_root"/.stage.* ]]; then
        rm -rf -- "$stage"
    fi
}
trap cleanup EXIT

[[ -n $package ]] || die 'usage: stage-extracted-package.sh ABSOLUTE_DEB [DESTINATION]'
[[ $package == /* && -f $package && ! -L $package ]] ||
    die "package must be an absolute regular file: $package"
[[ $deploy_root == /* && $deploy_root != / && $deploy_root != "$HOME" ]] ||
    die "destination must be an absolute directory below HOME: $deploy_root"
command -v dpkg-deb >/dev/null 2>&1 || die 'dpkg-deb is required'
command -v realpath >/dev/null 2>&1 || die 'realpath is required'

home_real=$(realpath -e "$HOME")
deploy_root=$(realpath -m "$deploy_root")
[[ $deploy_root == "$home_real"/* ]] ||
    die "destination resolves outside HOME: $deploy_root"
install -d -m 0700 -- "$deploy_root"
[[ ! -L $deploy_root ]] || die "destination cannot be a symbolic link: $deploy_root"
deploy_root=$(realpath -e "$deploy_root")
[[ $deploy_root == "$home_real"/* ]] ||
    die "destination resolves outside HOME: $deploy_root"
[[ $(stat -c %u "$deploy_root") == "$(id -u)" ]] ||
    die "destination is not owned by the current UID: $deploy_root"
chmod 0700 -- "$deploy_root"

package_name=$(dpkg-deb -f "$package" Package)
package_arch=$(dpkg-deb -f "$package" Architecture)
[[ $package_name == glibc && $package_arch == aarch64 ]] ||
    die "expected glibc/aarch64 package, got $package_name/$package_arch"
package_sha=$(sha256sum "$package" | awk '{print $1}')
[[ $package_sha =~ ^[0-9a-f]{64}$ ]] || die 'invalid package SHA-256'

# Prove the package through its own loader and public semaphore API before it
# becomes a selectable staged runtime. This never writes the active glibc tree.
"$script_dir/test-extracted-package.sh" "$package" "$repo_dir"

final=$deploy_root/$package_sha
marker=$final/.tgcompat-package-sha256
if [[ -e $final || -L $final ]]; then
    [[ -d $final && ! -L $final && -f $marker && ! -L $marker ]] ||
        die "existing version is not a validated directory: $final"
    read -r existing_sha < "$marker"
    [[ $existing_sha == "$package_sha" ]] ||
        die "existing version has the wrong marker: $final"
else
    stage=$(mktemp -d "$deploy_root/.stage.XXXXXX")
    dpkg-deb -x "$package" "$stage/root"
    candidate=$stage/root${PREFIX:?Termux PREFIX is required}/glibc
    [[ -x $candidate/lib/ld-linux-aarch64.so.1 &&
            -f $candidate/lib/libc.so.6 ]] ||
        die 'package does not contain the expected AArch64 Termux glibc layout'
    printf '%s\n' "$package_sha" > "$candidate/.tgcompat-package-sha256"
    mv -- "$candidate" "$final"
fi

[[ -x $final/lib/ld-linux-aarch64.so.1 &&
        ! -L $final/lib/ld-linux-aarch64.so.1 &&
        -f $final/lib/libc.so.6 && ! -L $final/lib/libc.so.6 ]] ||
    die "staged version has an invalid loader layout: $final"

[[ ! -e $deploy_root/current || -L $deploy_root/current ]] ||
    die "current selector is not a symbolic link: $deploy_root/current"
selector=$deploy_root/.current.$$
[[ ! -e $selector && ! -L $selector ]] ||
    die "temporary selector already exists: $selector"
ln -s -- "$package_sha" "$selector"
mv -Tf -- "$selector" "$deploy_root/current"
selector=

loader=$final/lib/ld-linux-aarch64.so.1
printf 'package_sha256=%s\n' "$package_sha"
printf 'staged_glibc=%s\n' "$final"
printf 'candidate_loader=%s\n' "$loader"
printf '%s\n' 'active Termux glibc: unchanged'
