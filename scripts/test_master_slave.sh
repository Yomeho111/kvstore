#!/usr/bin/env bash

# End-to-end replication test.
#
# Usage:
#   test_master_slave.sh            run the test
#   test_master_slave.sh --clean    delete everything the test generates, then exit
#
# Generated artifacts (nothing here is source):
#   <repo>/build-replication-test/  the single build tree (the replica reuses it)
#   <repo>/.replication-test/       server logs and AOF data
#   $SLAVE_REPO                     replica directory: copied binaries plus its own data
#
# Requirements:
#   - An RDMA device reachable through MASTER_IP (port 20000 is fixed by rdma.h).
#   - Permission to load/attach the eBPF program used for delta replication.
#   - Enough locked memory for the 128 MiB RDMA buffers.
#
# Useful overrides:
#   MASTER_IP=192.0.2.10        Address on the master's RDMA-capable interface.
#   SLAVE_REPO=$HOME/kvstore-slave
#   RECREATE_SLAVE_REPO=1       Replace a pre-existing, unmarked destination.
#   BUILD_JOBS=8
#   SYNC_TIMEOUT=180

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
MASTER_REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)
SLAVE_REPO=$(readlink -m -- "${SLAVE_REPO:-$HOME/kvstore-slave}")

MASTER_IP=${MASTER_IP:-}
CLIENT_IP=${CLIENT_IP:-127.0.0.1}
CLIENT_PORT=8050
RDMA_PORT=20000
SIW_LINK=${SIW_LINK:-siw0}
BUILD_JOBS=${BUILD_JOBS:-$(nproc)}
SYNC_TIMEOUT=${SYNC_TIMEOUT:-180}
SYNC_QUIET_TENTHS=${SYNC_QUIET_TENTHS:-20}
LOG_LEVEL=${LOG_LEVEL:-info}
BUILD_DIR_NAME=build-replication-test
COPY_MARKER=.kvstore-replication-test-copy

MASTER_BUILD="$MASTER_REPO/$BUILD_DIR_NAME"
SLAVE_BIN="$SLAVE_REPO/bin"
MASTER_RUNTIME="$MASTER_REPO/.replication-test/master"
SLAVE_RUNTIME="$SLAVE_REPO/.replication-test/slave"

MASTER_ARTIFACTS="$MASTER_REPO/.replication-test"
SLAVE_ARTIFACTS="$SLAVE_REPO/.replication-test"

MASTER_PID=
SLAVE_PID=
PROMOTED_PID=
SERVERS_STARTED=0

log()
{
    printf '\n==> %s\n' "$*"
}

die()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

stop_process()
{
    local pid=$1
    local name=$2

    if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
        return
    fi

    log "Stopping $name (pid $pid)"
    kill -TERM "$pid" 2>/dev/null || true

    for ((attempt = 0; attempt < 50; ++attempt)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return
        fi
        sleep 0.1
    done

    printf 'WARN: %s did not stop after SIGTERM; sending SIGKILL\n' "$name" >&2
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

show_log_tail()
{
    local path=$1
    local name=$2

    if [[ -f "$path" ]]; then
        printf '\n--- %s (last 40 lines) ---\n' "$name" >&2
        tail -n 40 "$path" >&2
    fi
}

cleanup()
{
    local status=$?
    trap - EXIT
    set +e

    stop_process "$PROMOTED_PID" "promoted replica"
    stop_process "$SLAVE_PID" "replica"
    stop_process "$MASTER_PID" "master"

    if ((status != 0 && SERVERS_STARTED == 1)); then
        show_log_tail "$MASTER_RUNTIME/master.log" "master.log"
        show_log_tail "$SLAVE_RUNTIME/slave.log" "slave.log"
        show_log_tail "$SLAVE_RUNTIME/promoted.log" "promoted.log"
    fi

    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

require_free_tcp_port()
{
    local port=$1

    if command -v ss >/dev/null 2>&1 &&
        ss -H -ltn "sport = :$port" 2>/dev/null | grep -q .; then
        die "TCP port $port is already in use; stop the existing listener before running this test"
    fi
}

# Echoes "<netdev> <ipv4>" for the first RDMA link whose netdev carries a global
# IPv4 address. rdma_resolve_addr() only accepts addresses owned by an RDMA
# netdev, so a link without one (e.g. an unconfigured SR-IOV VF) is useless here,
# and 127.0.0.1 never resolves.
usable_rdma_netdev()
{
    local netdev address

    command -v rdma >/dev/null 2>&1 || return 0
    command -v ip >/dev/null 2>&1 || return 0

    while read -r netdev; do
        [[ -n "$netdev" ]] || continue
        address=$(ip -o -4 addr show dev "$netdev" scope global 2>/dev/null |
            awk 'NR == 1 { split($4, parts, "/"); print parts[1] }')
        if [[ -n "$address" ]]; then
            printf '%s %s\n' "$netdev" "$address"
            return 0
        fi
    done < <(rdma link show 2>/dev/null |
        awk '{ for (i = 1; i <= NF; ++i) if ($i == "netdev") print $(i + 1) }')
}

ensure_rdma_link()
{
    if [[ -n "$(usable_rdma_netdev)" ]]; then
        return
    fi

    local netdev
    netdev=$(ip -o -4 route show default 2>/dev/null | awk 'NR == 1 { print $5 }')
    [[ -n "$netdev" ]] ||
        die "no RDMA link has an IPv4 address and the default-route interface could not be determined"

    log "No usable RDMA link; creating soft-iWARP link $SIW_LINK on $netdev"
    sudo -n modprobe siw ||
        die "failed to load the siw module; install linux-modules-extra-$(uname -r) or set MASTER_IP to a real RDMA interface"
    sudo -n rdma link add "$SIW_LINK" type siw netdev "$netdev" ||
        die "failed to create the siw link $SIW_LINK on $netdev"
}

resolve_master_ip()
{
    if [[ -n "$MASTER_IP" ]]; then
        return
    fi

    ensure_rdma_link

    local netdev
    read -r netdev MASTER_IP < <(usable_rdma_netdev)

    if [[ -z "$MASTER_IP" ]]; then
        die "no IPv4 address found on any RDMA netdev; assign one and rerun with MASTER_IP=<rdma-interface-ip>"
    fi

    log "Using RDMA netdev $netdev ($MASTER_IP)"
}

prepare_slave_dir()
{
    case "$SLAVE_REPO" in
        ""|/|"$HOME"|"$MASTER_REPO")
            die "unsafe SLAVE_REPO: $SLAVE_REPO"
            ;;
    esac

    if [[ -e "$SLAVE_REPO" ]]; then
        if [[ ! -f "$SLAVE_REPO/$COPY_MARKER" && "${RECREATE_SLAVE_REPO:-0}" != 1 ]]; then
            die "$SLAVE_REPO already exists and was not created by this test; set RECREATE_SLAVE_REPO=1 to replace it"
        fi
        rm -rf -- "$SLAVE_REPO"
    fi

    log "Creating replica directory $SLAVE_REPO"
    mkdir -p -- "$SLAVE_REPO"
    touch "$SLAVE_REPO/$COPY_MARKER"
}

build_kvstore()
{
    log "Configuring and building kvstore"
    cmake -S "$MASTER_REPO" -B "$MASTER_BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNETWORK=REACTOR \
        -DKVSTORE_PORT_NUM=1 \
        -DENABLE_MEMORY_POOL=ON
    cmake --build "$MASTER_BUILD" -j "$BUILD_JOBS"
    grant_capabilities "$MASTER_BUILD/kvstore"
}

# The replica runs the very same build as the master, so only the executables are
# copied. cp/install do not carry file capabilities across, hence the re-grant.
install_slave_binaries()
{
    log "Installing binaries into $SLAVE_BIN"

    mkdir -p -- "$SLAVE_BIN"
    install -m 0755 \
        "$MASTER_BUILD/kvstore" \
        "$MASTER_BUILD/kvstore_client_testcase" \
        "$SLAVE_BIN/"

    grant_capabilities "$SLAVE_BIN/kvstore"
}

# The delta tracer loads an eBPF program and attaches a uprobe to itself, which
# needs CAP_BPF/CAP_PERFMON while kernel.unprivileged_bpf_disabled is non-zero,
# and CAP_SYS_ADMIN to create the uprobe perf event. CAP_IPC_LOCK covers the
# 128 MiB RDMA memory registrations.
grant_capabilities()
{
    local binary=$1

    sudo -n setcap cap_bpf,cap_perfmon,cap_ipc_lock,cap_sys_admin+ep "$binary" ||
        die "failed to grant CAP_BPF/CAP_PERFMON/CAP_SYS_ADMIN to $binary; rerun the test as root instead"
}

process_is_alive()
{
    local pid=$1
    local name=$2
    local log_path=$3

    if ! kill -0 "$pid" 2>/dev/null; then
        show_log_tail "$log_path" "$name log"
        die "$name exited unexpectedly"
    fi
}

wait_for_tcp()
{
    local host=$1
    local port=$2
    local pid=$3
    local name=$4
    local log_path=$5
    local deadline=$((SECONDS + 30))

    while ((SECONDS < deadline)); do
        process_is_alive "$pid" "$name" "$log_path"
        if (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null; then
            exec 3>&-
            exec 3<&-
            return
        fi
        sleep 0.1
    done

    show_log_tail "$log_path" "$name log"
    die "$name did not listen on $host:$port within 30 seconds"
}

aof_bytes()
{
    local data_dir=$1

    if [[ ! -d "$data_dir" ]]; then
        printf '0\n'
        return
    fi

    find "$data_dir" -maxdepth 1 -type f -name 'kv_*.dt' -printf '%s\n' |
        awk '{ total += $1 } END { print total + 0 }'
}

wait_for_replica_bytes()
{
    local expected=$1
    local phase=$2
    local deadline=$((SECONDS + SYNC_TIMEOUT))
    local actual=0
    local previous=-1
    local quiet_tenths=0

    while ((SECONDS < deadline)); do
        process_is_alive "$MASTER_PID" "master" "$MASTER_RUNTIME/master.log"
        process_is_alive "$SLAVE_PID" "replica" "$SLAVE_RUNTIME/slave.log"
        actual=$(aof_bytes "$SLAVE_RUNTIME/data")
        if ((actual >= expected && actual == previous)); then
            ((quiet_tenths += 1))
        else
            quiet_tenths=0
        fi
        if ((quiet_tenths >= SYNC_QUIET_TENTHS)); then
            log "$phase replicated ($actual AOF bytes)"
            return
        fi
        previous=$actual
        sleep 0.1
    done

    die "$phase timed out or did not quiesce: replica has $actual of at least $expected expected AOF bytes after ${SYNC_TIMEOUT}s"
}

run_client_mode()
{
    local client=$1
    local mode=$2
    local description=$3

    log "$description"
    "$client" "$CLIENT_IP" "$CLIENT_PORT" "$mode"
}

# The server is configured entirely through an .ini file; there are no flags.
write_config()
{
    local path=$1
    local role=$2

    {
        printf '[server]\n'
        printf 'port = %s\n' "$CLIENT_PORT"
        printf 'log_level = %s\n' "$LOG_LEVEL"
        printf '\n[persistence]\n'
        printf 'mode = aof\n'
        printf '\n[replication]\n'
        printf 'role = %s\n' "$role"
        if [[ $role == slave ]]; then
            printf 'master_ip = %s\n' "$MASTER_IP"
            printf 'master_port = %s\n' "$RDMA_PORT"
        fi
    } >"$path"
}

clean_artifacts()
{
    log "Removing generated test artifacts"

    local removed=0
    local path

    remove_path()
    {
        [[ -e "$1" ]] || return 0
        rm -rf -- "$1"
        printf '    removed %s\n' "$1"
        removed=1
    }

    remove_path "$MASTER_BUILD"
    remove_path "$MASTER_ARTIFACTS"

    if [[ -f "$SLAVE_REPO/$COPY_MARKER" ]]; then
        remove_path "$SLAVE_REPO"
    elif [[ -d "$SLAVE_REPO" ]]; then
        # Not a directory this test made, so drop only what the test would have added.
        remove_path "$SLAVE_BIN"
        remove_path "$SLAVE_ARTIFACTS"
        printf 'WARN: %s was not created by this test and was kept\n' "$SLAVE_REPO" >&2
    fi

    ((removed == 1)) || printf '    nothing to remove\n'
}

usage()
{
    printf 'Usage: %s [--clean]\n\n' "${BASH_SOURCE[0]##*/}"
    printf '  (no arguments)  run the end-to-end replication test\n'
    printf '  --clean         delete the build trees, runtime data and replica copy, then exit\n'
}

case "${1:-}" in
    --clean)
        clean_artifacts
        exit 0
        ;;
    "") ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        die "unknown argument: $1"
        ;;
esac

require_free_tcp_port "$CLIENT_PORT"
require_free_tcp_port "$RDMA_PORT"
resolve_master_ip
prepare_slave_dir
build_kvstore
install_slave_binaries

rm -rf -- "$MASTER_RUNTIME" "$SLAVE_RUNTIME"
mkdir -p -- "$MASTER_RUNTIME" "$SLAVE_RUNTIME"

if ! ulimit -l unlimited 2>/dev/null; then
    printf 'WARN: locked-memory limit is %s KiB; RDMA registration may require at least 131072 KiB\n' \
        "$(ulimit -l)" >&2
fi

log "Starting master from $MASTER_REPO"
SERVERS_STARTED=1
write_config "$MASTER_RUNTIME/kvstore.ini" master
(
    cd "$MASTER_RUNTIME"
    exec "$MASTER_BUILD/kvstore" kvstore.ini
) >"$MASTER_RUNTIME/master.log" 2>&1 &
MASTER_PID=$!
wait_for_tcp "$CLIENT_IP" "$CLIENT_PORT" "$MASTER_PID" "master" "$MASTER_RUNTIME/master.log"

run_client_mode "$MASTER_BUILD/kvstore_client_testcase" 14 \
    "Writing unique keys 1-50000 to the master"
FIRST_PHASE_BYTES=$(aof_bytes "$MASTER_RUNTIME/data")
((FIRST_PHASE_BYTES > 0)) || die "master AOF is empty after the first write phase"

log "Starting replica from $SLAVE_REPO (RDMA $MASTER_IP:$RDMA_PORT)"
write_config "$SLAVE_RUNTIME/kvstore.ini" slave
(
    cd "$SLAVE_RUNTIME"
    exec "$SLAVE_BIN/kvstore" kvstore.ini
) >"$SLAVE_RUNTIME/slave.log" 2>&1 &
SLAVE_PID=$!

# No barrier here on purpose: the replica's initial full sync has to overlap the
# second write phase, so the master keeps taking writes while the replica is
# still catching up. That is what exercises the delta path (and its
# ring-overflow fallback) rather than a quiet full sync.
run_client_mode "$MASTER_BUILD/kvstore_client_testcase" 15 \
    "Writing unique keys 50001-100000 while the replica performs its initial sync"
SECOND_PHASE_BYTES=$(aof_bytes "$MASTER_RUNTIME/data")
((SECOND_PHASE_BYTES > FIRST_PHASE_BYTES)) || die "master AOF did not grow during the second write phase"

wait_for_replica_bytes "$SECOND_PHASE_BYTES" "Concurrent full sync + 50000-key delta sync"

stop_process "$SLAVE_PID" "replica"
SLAVE_PID=
stop_process "$MASTER_PID" "master"
MASTER_PID=

log "Promoting and restarting the replica as a standalone server"
write_config "$SLAVE_RUNTIME/kvstore.ini" standalone
(
    cd "$SLAVE_RUNTIME"
    exec "$SLAVE_BIN/kvstore" kvstore.ini
) >"$SLAVE_RUNTIME/promoted.log" 2>&1 &
PROMOTED_PID=$!
wait_for_tcp "$CLIENT_IP" "$CLIENT_PORT" "$PROMOTED_PID" "promoted replica" "$SLAVE_RUNTIME/promoted.log"

run_client_mode "$SLAVE_BIN/kvstore_client_testcase" 8 \
    "Reading and verifying all 100000 unique key/value pairs from the promoted replica"

log "PASS: the promoted replica returned all 100000 expected values"