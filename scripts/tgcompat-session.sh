#!/usr/bin/env bash

set -euo pipefail
umask 077

CDPATH=''
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
daemon_binary=${TGCOMPATD:-"$repo_dir/build/tgcompatd"}

if [[ -n ${TGCOMPAT_SOCKET:-} ]]; then
    socket_path=$TGCOMPAT_SOCKET
    runtime_dir=$(dirname -- "$socket_path")
else
    runtime_parent=${TMPDIR:-/tmp}
    runtime_dir=$runtime_parent/tgcompat-$(id -u)
    socket_path=$runtime_dir/broker.sock
fi
pid_file=$runtime_dir/tgcompatd.pid
log_file=$runtime_dir/tgcompatd.log

die() {
    printf 'tgcompat: %s\n' "$*" >&2
    exit 1
}

ensure_runtime_dir() {
    [[ $runtime_dir == /* ]] || die "runtime directory must be absolute: $runtime_dir"
    [[ ! -L $runtime_dir ]] || die "runtime directory cannot be a symlink: $runtime_dir"
    if [[ ! -d $runtime_dir ]]; then
        install -d -m 0700 -- "$runtime_dir"
    fi
    local owner mode
    owner=$(stat -c %u -- "$runtime_dir")
    mode=$(stat -c %a -- "$runtime_dir")
    [[ $owner == "$(id -u)" ]] || die "runtime directory has the wrong owner"
    [[ $mode == 700 ]] || die "runtime directory mode must be exactly 0700"
}

read_managed_pid() {
    managed_pid=
    [[ -f $pid_file && ! -L $pid_file ]] || return 1
    IFS= read -r managed_pid < "$pid_file" || return 1
    [[ $managed_pid =~ ^[1-9][0-9]*$ ]] || return 1
}

is_managed_daemon() {
    read_managed_pid || return 1
    [[ -e /proc/$managed_pid/exe ]] || return 1
    local expected_exe running_exe
    expected_exe=$(readlink -f -- "$daemon_binary") || return 1
    running_exe=$(readlink -- "/proc/$managed_pid/exe") || return 1
    running_exe=${running_exe% (deleted)}
    [[ $running_exe == "$expected_exe" ]] || return 1
    [[ $(stat -c %u -- "/proc/$managed_pid") == "$(id -u)" ]] || return 1
    kill -0 "$managed_pid" 2>/dev/null
}

start_daemon() {
    ensure_runtime_dir
    [[ $socket_path == "$runtime_dir/"* ]] || die "socket must be inside its runtime directory"
    [[ -x $daemon_binary ]] || die "broker is not built: run scripts/build-release.sh"
    if is_managed_daemon; then
        printf 'tgcompat: already running (pid %s)\n' "$managed_pid"
        return
    fi

    if read_managed_pid && ! kill -0 "$managed_pid" 2>/dev/null; then
        rm -f -- "$pid_file"
        [[ ! -S $socket_path ]] || rm -f -- "$socket_path"
    elif [[ -e $pid_file ]]; then
        die "refusing unverified pid file: $pid_file"
    fi
    [[ ! -e $socket_path ]] || die "refusing existing socket path: $socket_path"

    "$daemon_binary" --socket "$socket_path" >>"$log_file" 2>&1 &
    managed_pid=$!
    printf '%s\n' "$managed_pid" >"$pid_file"

    for ((attempt = 0; attempt < 200; ++attempt)); do
        if [[ -S $socket_path ]] && is_managed_daemon; then
            printf 'tgcompat: ready (pid %s, socket %s)\n' \
                "$managed_pid" "$socket_path"
            return
        fi
        if ! kill -0 "$managed_pid" 2>/dev/null; then
            tail -n 20 -- "$log_file" >&2 || true
            die 'broker exited during startup'
        fi
        sleep 0.01
    done
    die 'broker startup timed out'
}

stop_daemon() {
    ensure_runtime_dir
    if ! is_managed_daemon; then
        printf '%s\n' 'tgcompat: not running'
        return
    fi
    local stopping_pid=$managed_pid
    kill -TERM "$stopping_pid"
    for ((attempt = 0; attempt < 100; ++attempt)); do
        if ! kill -0 "$stopping_pid" 2>/dev/null; then
            rm -f -- "$pid_file"
            printf 'tgcompat: stopped (pid %s)\n' "$stopping_pid"
            return
        fi
        sleep 0.01
    done
    die "broker did not stop after SIGTERM (pid $stopping_pid)"
}

status_daemon() {
    ensure_runtime_dir
    if is_managed_daemon; then
        printf 'tgcompat: running (pid %s, socket %s)\n' \
            "$managed_pid" "$socket_path"
        return 0
    fi
    printf '%s\n' 'tgcompat: stopped'
    return 1
}

command_name=${1:-status}
case $command_name in
    start)
        start_daemon
        ;;
    stop)
        stop_daemon
        ;;
    restart)
        stop_daemon
        start_daemon
        ;;
    status)
        status_daemon
        ;;
    env)
        ensure_runtime_dir
        printf 'export TGCOMPAT_SOCKET=%q\n' "$socket_path"
        ;;
    run)
        shift
        (($# > 0)) || die 'run requires a command'
        start_daemon
        export TGCOMPAT_SOCKET=$socket_path
        exec "$@"
        ;;
    --help|-h|help)
        printf '%s\n' \
            'Usage: scripts/tgcompat-session.sh {start|stop|restart|status|env|run COMMAND...}'
        ;;
    *)
        die "unknown command: $command_name"
        ;;
esac
