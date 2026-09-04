#!/usr/bin/env bash

# Memory profile of the kvstore server across allocator configurations.
#
# Usage:
#   scripts/mem_profile_allocators.sh [config...]
#
#   nopool     glibc malloc (no pool, no external allocator)   [default: all]
#   pool       -DENABLE_MEMORY_POOL=ON
#   tcmalloc   -DENABLE_TCMALLOC=ON
#   jemalloc   -DENABLE_JEMALLOC=ON
#
# For every configuration the script rebuilds the project, starts the server in
# a fresh runtime directory with an empty data/, and samples VmSize / VmRSS from
# /proc/<pid>/status at three points:
#
#   start   just after the server binds its port
#   peak    after testcase mode 5 has SET the whole working set
#   end     SETTLE seconds after testcase mode 6 has DEL'd every key
#
# The working-set size is the N constant in kvstore_client/testcase.cpp.
# A markdown table of every sample is printed at the end.
#
# Overrides: BUILD_DIR, HOST, PORT, SETTLE, PHASE_TIMEOUT, BUILD_TYPE,
#            NETWORK, ENGINE, KEEP_LOGS=1

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)

BUILD_DIR=$(readlink -m -- "${BUILD_DIR:-$REPO/build}")
RUNTIME_ROOT="$REPO/.mem-profile"
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8050}
SETTLE=${SETTLE:-3}
PHASE_TIMEOUT=${PHASE_TIMEOUT:-1800}
BUILD_TYPE=${BUILD_TYPE:-Release}
NETWORK=${NETWORK:-REACTOR}
ENGINE=${ENGINE:-RBTREE_ENGINE}

KVSTORE="$BUILD_DIR/kvstore"
TESTCASE="$BUILD_DIR/kvstore_client_testcase"

ALL_CONFIGS=(nopool pool tcmalloc jemalloc)

declare -A CONFIG_FLAGS=(
    [nopool]="-DENABLE_MEMORY_POOL=OFF -DENABLE_TCMALLOC=OFF -DENABLE_JEMALLOC=OFF"
    [pool]="-DENABLE_MEMORY_POOL=ON  -DENABLE_TCMALLOC=OFF -DENABLE_JEMALLOC=OFF"
    [tcmalloc]="-DENABLE_MEMORY_POOL=OFF -DENABLE_TCMALLOC=ON  -DENABLE_JEMALLOC=OFF"
    [jemalloc]="-DENABLE_MEMORY_POOL=OFF -DENABLE_TCMALLOC=OFF -DENABLE_JEMALLOC=ON"
)

declare -A CONFIG_LABEL=(
    [nopool]='No pool (glibc `malloc`)'
    [pool]='Custom pool (`-DENABLE_MEMORY_POOL=ON`)'
    [tcmalloc]='tcmalloc (`-DENABLE_TCMALLOC=ON`)'
    [jemalloc]='jemalloc (`-DENABLE_JEMALLOC=ON`)'
)

# "<config>:<phase>:<VmSize|VmRSS>" -> value in MB
declare -A SAMPLE
# "<config>:<set|del>" -> seconds, or "timeout"
declare -A PHASE_TIME

SERVER_PID=
CURRENT_RUNTIME=

log()
{
    printf '%s\n' "$*"
}

die()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

stop_server()
{
    [[ -n "$SERVER_PID" ]] || return 0
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    # exit() runs the static destructors, so give the process time to unwind
    for _ in $(seq 1 60); do
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.5
    done
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

cleanup()
{
    local rc=$?
    stop_server
    if [[ $rc -eq 0 && "${KEEP_LOGS:-0}" != 1 ]]; then
        rm -rf -- "$RUNTIME_ROOT"
    elif [[ -d "$RUNTIME_ROOT" ]]; then
        printf 'runtime logs kept in %s\n' "$RUNTIME_ROOT" >&2
    fi
}
trap cleanup EXIT

build_config()
{
    local cfg=$1
    log "[$cfg] configuring and building (${BUILD_TYPE}, ${NETWORK}, ${ENGINE})"

    # shellcheck disable=SC2086
    cmake -S "$REPO" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DNETWORK="$NETWORK" -DENGINE="$ENGINE" \
        ${CONFIG_FLAGS[$cfg]} > "$RUNTIME_ROOT/cmake-$cfg.log" 2>&1 ||
        die "cmake configure failed for $cfg, see $RUNTIME_ROOT/cmake-$cfg.log"

    cmake --build "$BUILD_DIR" -j"$(nproc)" > "$RUNTIME_ROOT/build-$cfg.log" 2>&1 ||
        die "build failed for $cfg, see $RUNTIME_ROOT/build-$cfg.log"
}

wait_for_port()
{
    for _ in $(seq 1 600); do
        if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
            exec 3>&-
            return 0
        fi
        kill -0 "$SERVER_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

sample()
{
    local cfg=$1 phase=$2
    local status="/proc/$SERVER_PID/status"
    [[ -r "$status" ]] || die "cannot read $status while sampling '$phase'"

    SAMPLE["$cfg:$phase:VmSize"]=$(awk '/^VmSize:/ {printf "%.2f", $2 / 1024}' "$status")
    SAMPLE["$cfg:$phase:VmRSS"]=$(awk '/^VmRSS:/  {printf "%.2f", $2 / 1024}' "$status")

    log "[$cfg] $phase: VmSize=${SAMPLE["$cfg:$phase:VmSize"]} MB  VmRSS=${SAMPLE["$cfg:$phase:VmRSS"]} MB"
}

run_phase()
{
    local cfg=$1 name=$2 mode=$3
    local started=$SECONDS

    if timeout "$PHASE_TIMEOUT" "$TESTCASE" "$HOST" "$PORT" "$mode" \
        > "$CURRENT_RUNTIME/$name.log" 2>&1; then
        PHASE_TIME["$cfg:$name"]=$((SECONDS - started))
        log "[$cfg] mode $mode ($name) finished in ${PHASE_TIME["$cfg:$name"]}s"
    else
        local rc=$?
        PHASE_TIME["$cfg:$name"]=timeout
        if [[ $rc -eq 124 ]]; then
            log "[$cfg] mode $mode ($name) TIMED OUT after ${PHASE_TIMEOUT}s -- sampling anyway"
        else
            log "[$cfg] mode $mode ($name) FAILED with rc=$rc -- sampling anyway"
        fi
    fi
}

profile_config()
{
    local cfg=$1

    build_config "$cfg"

    CURRENT_RUNTIME="$RUNTIME_ROOT/$cfg"
    rm -rf -- "$CURRENT_RUNTIME"
    mkdir -p -- "$CURRENT_RUNTIME"
    cp -- "$REPO/kvstore.ini" "$CURRENT_RUNTIME/"

    (
        cd -- "$CURRENT_RUNTIME"
        exec "$KVSTORE" ./kvstore.ini
    ) > "$CURRENT_RUNTIME/server.log" 2>&1 &
    SERVER_PID=$!

    wait_for_port || {
        cat "$CURRENT_RUNTIME/server.log" >&2
        die "server did not bind $HOST:$PORT for config $cfg"
    }
    sleep 1
    sample "$cfg" start

    run_phase "$cfg" set 5
    sleep 1
    sample "$cfg" peak

    run_phase "$cfg" del 6
    sleep "$SETTLE"
    sample "$cfg" end

    stop_server
    CURRENT_RUNTIME=
}

cell()
{
    local key=$1
    printf '%s' "${SAMPLE[$key]:-n/a}"
}

print_table()
{
    local cfg
    echo
    echo "| Allocator | Metric | Start (MB) | Peak / full set (MB) | End / after DEL (MB) |"
    echo "| --- | --- | --- | --- | --- |"
    for cfg in "${CONFIGS[@]}"; do
        printf '| %s | Virtual (`VmSize`) | %s | %s | %s |\n' \
            "${CONFIG_LABEL[$cfg]}" \
            "$(cell "$cfg:start:VmSize")" \
            "$(cell "$cfg:peak:VmSize")" \
            "$(cell "$cfg:end:VmSize")"
        printf '| %s | Physical (`VmRSS`) | %s | %s | %s |\n' \
            "${CONFIG_LABEL[$cfg]}" \
            "$(cell "$cfg:start:VmRSS")" \
            "$(cell "$cfg:peak:VmRSS")" \
            "$(cell "$cfg:end:VmRSS")"
    done
    echo

    for cfg in "${CONFIGS[@]}"; do
        if [[ "${PHASE_TIME[$cfg:set]:-}" == timeout || "${PHASE_TIME[$cfg:del]:-}" == timeout ]]; then
            printf '> NOTE: %s did not complete within PHASE_TIMEOUT=%ss (set=%s del=%s); its numbers are partial.\n' \
                "${CONFIG_LABEL[$cfg]}" "$PHASE_TIMEOUT" \
                "${PHASE_TIME[$cfg:set]:-n/a}" "${PHASE_TIME[$cfg:del]:-n/a}"
        fi
    done
}

main()
{
    if [[ $# -gt 0 ]]; then
        CONFIGS=("$@")
    else
        CONFIGS=("${ALL_CONFIGS[@]}")
    fi

    local cfg
    for cfg in "${CONFIGS[@]}"; do
        [[ -n "${CONFIG_FLAGS[$cfg]:-}" ]] || die "unknown config '$cfg' (use: ${ALL_CONFIGS[*]})"
    done

    command -v cmake  >/dev/null || die "cmake not found"
    command -v timeout >/dev/null || die "timeout(1) not found"

    rm -rf -- "$RUNTIME_ROOT"
    mkdir -p -- "$RUNTIME_ROOT"

    for cfg in "${CONFIGS[@]}"; do
        profile_config "$cfg"
    done

    print_table
}

main "$@"
