#!/usr/bin/env bash

# Automated kvstore test suite.
#
# Usage:
#   test_suite.sh [aof|rdb|timer|pressure|all] [--no-build] [extra pressure_test.sh args...]
#
#   aof        insert 100k keys, crash the server, restart, verify the AOF replay
#   rdb        insert 100k keys, snapshot via SIGUSR1, crash, restart, verify the snapshot
#   timer      set batches with a 1s TTL over several steps, verify each batch expires
#   pressure   run scripts/pressure_test.sh and print a throughput summary
#   all        all of the above (default)
#
#   --no-build reuse the binaries already in BUILD_DIR instead of compiling
#
# The project is compiled once per run, into ./build by default. Each test runs
# in its own directory under .test-suite/ which is deleted afterwards, so no
# persistent database is carried between tests. The build tree is never removed.
#
# Overrides: BUILD_DIR, PORT, HOST, PRESSURE_PERSIST (aof|rdb), KEEP_LOGS=1

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)

BUILD_DIR=$(readlink -m -- "${BUILD_DIR:-$REPO/build}")
RUNTIME_ROOT="$REPO/.test-suite"
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8050}
PRESSURE_PERSIST=${PRESSURE_PERSIST:-aof}
KEY_COUNT=100000

KVSTORE="$BUILD_DIR/kvstore"
TESTCASE="$BUILD_DIR/kvstore_client_testcase"

SERVER_PID=
CURRENT_RUNTIME=
FAILURES=0
NO_BUILD=0

log()
{
    printf '%s\n' "$*"
}

die()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

pass()
{
    printf 'PASSED  %s\n' "$*"
}

fail()
{
    printf 'FAILED  %s\n' "$*" >&2
    FAILURES=$((FAILURES + 1))
}

show_log_tail()
{
    local path=$1

    if [[ -f "$path" ]]; then
        printf '\n--- %s (last 20 lines) ---\n' "${path##*/}" >&2
        tail -n 20 "$path" >&2
        printf -- '---\n' >&2
    fi
}

kill_server()
{
    local signal=${1:-KILL}

    [[ -n "$SERVER_PID" ]] || return 0

    if kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "-$signal" "$SERVER_PID" 2>/dev/null || true
        for ((i = 0; i < 100; ++i)); do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
    fi

    SERVER_PID=
}

cleanup()
{
    local status=$?
    trap - EXIT
    set +e
    kill_server KILL
    [[ -n "$CURRENT_RUNTIME" && "${KEEP_LOGS:-0}" != 1 ]] && rm -rf -- "$CURRENT_RUNTIME"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

require_free_port()
{
    if command -v ss >/dev/null 2>&1 &&
        ss -H -ltn "sport = :$PORT" 2>/dev/null | grep -q .; then
        die "TCP port $PORT is already in use; stop the existing listener first"
    fi
}

build_once()
{
    local build_log="$RUNTIME_ROOT/build.log"

    if ((NO_BUILD == 1)); then
        [[ -x "$KVSTORE" && -x "$TESTCASE" ]] ||
            die "--no-build was given but $BUILD_DIR does not contain kvstore and kvstore_client_testcase"
        log "Reusing the existing build in $BUILD_DIR"
        return
    fi

    log "Building kvstore into $BUILD_DIR"
    mkdir -p -- "$RUNTIME_ROOT"

    if ! {
        cmake -S "$REPO" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release \
            -DNETWORK=REACTOR \
            -DKVSTORE_PORT_NUM=1 \
            -DENABLE_MEMORY_POOL=ON &&
            cmake --build "$BUILD_DIR" -j "$(nproc)"
    } >"$build_log" 2>&1; then
        tail -n 30 "$build_log" >&2
        die "build failed (full log: $build_log)"
    fi

    rm -f -- "$build_log"

    [[ -x "$KVSTORE" ]] || die "missing $KVSTORE after the build"
    [[ -x "$TESTCASE" ]] || die "missing $TESTCASE after the build"
}

# These tests are standalone, so the eBPF/RDMA replication path never starts and
# no elevated capabilities are needed.
write_config()
{
    local runtime=$1
    local mode=$2

    cat >"$runtime/kvstore.ini" <<EOF
[server]
port = $PORT
log_level = info

[persistence]
mode = $mode

[replication]
role = standalone
EOF
}

start_server()
{
    local runtime=$1
    local log_name=$2

    (
        cd "$runtime"
        exec "$KVSTORE" kvstore.ini
    ) >"$runtime/$log_name" 2>&1 &
    SERVER_PID=$!

    # These servers are killed on purpose; disown keeps bash from printing
    # "Killed" job notices over the test output.
    disown "$SERVER_PID" 2>/dev/null || true

    local deadline=$((SECONDS + 30))
    while ((SECONDS < deadline)); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            show_log_tail "$runtime/$log_name"
            return 1
        fi
        if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        sleep 0.1
    done

    show_log_tail "$runtime/$log_name"
    return 1
}

# Waits for the SIGUSR1 snapshot child to finish: the dump lands on a temporary
# file that is atomically renamed, so the size must also stop changing.
wait_for_snapshot()
{
    local dir=$1
    local deadline=$((SECONDS + 180))
    local size previous=-1 stable=0

    while ((SECONDS < deadline)); do
        if [[ -f "$dir/kv_0.rdt" && ! -e "$dir/kv_0.rdt.tmp" ]]; then
            size=$(stat -c%s "$dir/kv_0.rdt")
            if ((size > 0 && size == previous)); then
                stable=$((stable + 1))
                ((stable >= 5)) && return 0
            else
                stable=0
            fi
            previous=$size
        fi
        sleep 0.1
    done

    return 1
}

# Sets CURRENT_RUNTIME. Not echoed through a command substitution on purpose:
# that runs in a subshell and the assignment would be lost, leaving the database
# behind after the test.
new_runtime()
{
    CURRENT_RUNTIME="$RUNTIME_ROOT/$1"
    rm -rf -- "$CURRENT_RUNTIME"
    mkdir -p -- "$CURRENT_RUNTIME"
}

drop_runtime()
{
    [[ -n "$CURRENT_RUNTIME" ]] || return 0
    if [[ "${KEEP_LOGS:-0}" == 1 ]]; then
        rm -rf -- "$CURRENT_RUNTIME/data" "$CURRENT_RUNTIME/rdb_data"
    else
        rm -rf -- "$CURRENT_RUNTIME"
    fi
    CURRENT_RUNTIME=
}

test_aof()
{
    local name="aof persistence ($KEY_COUNT keys)"
    local runtime
    new_runtime aof
    runtime=$CURRENT_RUNTIME
    write_config "$runtime" aof

    if ! start_server "$runtime" first.log; then
        fail "$name (server did not start)"
        drop_runtime
        return
    fi

    if ! "$TESTCASE" "$HOST" "$PORT" 7 >"$runtime/insert.log" 2>&1; then
        show_log_tail "$runtime/insert.log"
        fail "$name (insert failed)"
        kill_server KILL
        drop_runtime
        return
    fi

    if [[ ! -s "$runtime/data/kv_0.dt" ]]; then
        fail "$name (no incremental AOF file was written)"
        kill_server KILL
        drop_runtime
        return
    fi

    # SIGKILL: the server must recover from its log alone, with no clean shutdown.
    kill_server KILL

    if ! start_server "$runtime" second.log; then
        fail "$name (server did not restart)"
        drop_runtime
        return
    fi

    if ! "$TESTCASE" "$HOST" "$PORT" 8 >"$runtime/verify.log" 2>&1; then
        show_log_tail "$runtime/verify.log"
        fail "$name (data mismatch after restart)"
        kill_server KILL
        drop_runtime
        return
    fi

    kill_server KILL
    drop_runtime
    pass "$name"
}

test_rdb()
{
    local name="rdb persistence ($KEY_COUNT keys)"
    local runtime
    new_runtime rdb
    runtime=$CURRENT_RUNTIME
    write_config "$runtime" rdb

    if ! start_server "$runtime" first.log; then
        fail "$name (server did not start)"
        drop_runtime
        return
    fi

    if ! "$TESTCASE" "$HOST" "$PORT" 7 >"$runtime/insert.log" 2>&1; then
        show_log_tail "$runtime/insert.log"
        fail "$name (insert failed)"
        kill_server KILL
        drop_runtime
        return
    fi

    kill -USR1 "$SERVER_PID"

    if ! wait_for_snapshot "$runtime/rdb_data"; then
        show_log_tail "$runtime/first.log"
        fail "$name (snapshot was not written after SIGUSR1)"
        kill_server KILL
        drop_runtime
        return
    fi

    if [[ -d "$runtime/data" ]]; then
        fail "$name (rdb mode should not write an AOF directory)"
        kill_server KILL
        drop_runtime
        return
    fi

    kill_server KILL

    if ! start_server "$runtime" second.log; then
        fail "$name (server did not restart)"
        drop_runtime
        return
    fi

    if ! "$TESTCASE" "$HOST" "$PORT" 8 >"$runtime/verify.log" 2>&1; then
        show_log_tail "$runtime/verify.log"
        fail "$name (data mismatch after restart)"
        kill_server KILL
        drop_runtime
        return
    fi

    kill_server KILL
    drop_runtime
    pass "$name"
}

# Batches are written with a 1s TTL and read back one step later, so the server
# has to expire them on its own while it keeps taking new writes.
test_timer()
{
    local name="timer expiration (multi-step 1s TTL)"
    local runtime
    new_runtime timer
    runtime=$CURRENT_RUNTIME
    write_config "$runtime" aof

    if ! start_server "$runtime" server.log; then
        fail "$name (server did not start)"
        drop_runtime
        return
    fi

    if ! "$TESTCASE" "$HOST" "$PORT" 9 >"$runtime/timer.log" 2>&1; then
        show_log_tail "$runtime/timer.log"
        fail "$name (a batch did not expire as expected)"
        kill_server KILL
        drop_runtime
        return
    fi

    kill_server KILL
    drop_runtime
    pass "$name"
}

print_pressure_metrics()
{
    local log=$1

    printf '\n  %-42s %-8s %13s\n' "BENCHMARK" "OP" "THROUGHPUT"
    printf '  %-42s %-8s %13s\n' \
        "------------------------------------------" "--------" "-------------"

    # redis-benchmark redraws progress with carriage returns, so a summary can
    # share a physical line with counter updates; split on CR before parsing.
    tr '\r' '\n' <"$log" | awk '
        /^>> / { label = substr($0, 4); op = ""; next }
        /^====== / { op = $2; next }
        /requests per second/ {
            if ($1 == "throughput" && $2 == "summary:")
                printf "  %-42s %-8s %8.0f ops/s\n", label, op, $3
            else {
                cmd = $1
                sub(/:$/, "", cmd)
                printf "  %-42s %-8s %8.0f ops/s\n", label, cmd, $2
            }
        }
    '
}

test_pressure()
{
    local name="pressure test (redis-benchmark)"
    local runtime
    new_runtime pressure
    runtime=$CURRENT_RUNTIME
    write_config "$runtime" "$PRESSURE_PERSIST"

    if ! command -v redis-benchmark >/dev/null 2>&1; then
        fail "$name (redis-benchmark not installed: apt install redis-tools)"
        drop_runtime
        return
    fi

    if ! start_server "$runtime" server.log; then
        fail "$name (server did not start)"
        drop_runtime
        return
    fi

    log "Running pressure test (persistence=$PRESSURE_PERSIST, output muted)..."

    local status=0
    "$SCRIPT_DIR/pressure_test.sh" -h "$HOST" -p "$PORT" --no-save "${PRESSURE_ARGS[@]}" \
        >"$runtime/pressure.log" 2>&1 || status=$?

    if ((status != 0)); then
        show_log_tail "$runtime/pressure.log"
        fail "$name (pressure_test.sh exited $status)"
        kill_server KILL
        drop_runtime
        return
    fi

    print_pressure_metrics "$runtime/pressure.log"

    local warnings
    warnings=$(awk '/Benchmarks with warnings:/ { print $NF }' "$runtime/pressure.log")
    if [[ -n "$warnings" && "$warnings" != 0 ]]; then
        show_log_tail "$runtime/pressure.log"
        fail "$name ($warnings benchmark(s) reported warnings)"
        kill_server KILL
        drop_runtime
        return
    fi

    kill_server KILL
    drop_runtime
    printf '\n'
    pass "$name"
}

usage()
{
    awk 'NR == 1 { next }
         /^#/ { sub(/^# ?/, ""); print; seen = 1; next }
         seen { exit }' "$0"
}

SELECTED=all
SELECTED_SET=0
PRESSURE_ARGS=()

# The first unrecognised flag ends our own option parsing: everything from there
# on is handed to pressure_test.sh verbatim, so its value-taking flags such as
# `-n 50000` survive. `--` forces the switch explicitly.
while (($# > 0)); do
    case "$1" in
        aof | rdb | timer | pressure | all)
            if ((SELECTED_SET == 1)); then
                usage >&2
                die "more than one test selected: '$SELECTED' and '$1'"
            fi
            SELECTED=$1
            SELECTED_SET=1
            shift
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        --)
            shift
            PRESSURE_ARGS+=("$@")
            break
            ;;
        -*)
            PRESSURE_ARGS+=("$@")
            break
            ;;
        *)
            usage >&2
            die "unknown test '$1'"
            ;;
    esac
done

require_free_port
mkdir -p -- "$RUNTIME_ROOT"
build_once
printf '\n'

case "$SELECTED" in
    aof) test_aof ;;
    rdb) test_rdb ;;
    timer) test_timer ;;
    pressure) test_pressure ;;
    all)
        test_aof
        test_rdb
        test_timer
        test_pressure
        ;;
esac

rmdir -- "$RUNTIME_ROOT" 2>/dev/null || true

if ((FAILURES > 0)); then
    printf '\n%d test(s) failed\n' "$FAILURES" >&2
    exit 1
fi
