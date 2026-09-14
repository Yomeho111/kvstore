#!/usr/bin/env bash

# kvstore vs redis-server performance comparison.
#
# Usage:
#   benchmark_compare.sh [persistence|save|pipeline|all] [--no-build] [--quick]
#
#   persistence  redis-benchmark -c 1 -P 1 -n <requests> (PING hello, then
#                -t set,get) against kvstore and redis-server, each run twice:
#                with incremental persistence on (kvstore mode = aof,
#                redis appendonly yes) and off (mode = none, appendonly no).
#                Reports qps and latency percentiles.
#   save         kvstore full-snapshot cost, no redis counterpart: for every
#                <interval>:<writes> pair in SAVE_PLAN the server is started
#                fresh with mode = rdb and the client (testcase mode 12) writes
#                <writes> keys, sending a SAVE every <interval> of them.
#                Reports the command count, elapsed time and qps. A SAVE dumps
#                the whole dataset, so the writes per pair shrink as the
#                interval does, keeping every run to a sane amount of I/O.
#   pipeline     redis-benchmark -c 1 -P <depth> -n <requests> -t set,get
#                against both servers for depth 10, 20, 40, 80, 160.
#                Reports qps and latency percentiles.
#   all          all of the above (default)
#
#   --no-build   reuse the binaries already in BUILD_DIR instead of compiling
#   --quick      small request counts, for checking the script itself
#
# Each server is started by this script in its own directory under
# .benchmark-compare/, which is removed afterwards, so no database or AOF is
# carried between runs.
#
# Overrides: BUILD_DIR, HOST, KV_PORT, REDIS_PORT, REQUESTS, SAVE_PLAN,
#            PIPELINE_SWEEP, PIPELINE_PERSIST (none|aof), REDIS_APPENDFSYNC,
#            KEEP_LOGS=1

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)

BUILD_DIR=$(readlink -m -- "${BUILD_DIR:-$REPO/build}")
RUNTIME_ROOT="$REPO/.benchmark-compare"
HOST=${HOST:-127.0.0.1}
KV_PORT=${KV_PORT:-8050}
REDIS_PORT=${REDIS_PORT:-6379}
REQUESTS=${REQUESTS:-1000000}

# <SAVE interval>:<writes> pairs for the snapshot phase.
SAVE_PLAN=${SAVE_PLAN:-"1000000:2000000 100000:2000000 10000:2000000 1000:2000000"}

PIPELINE_SWEEP=${PIPELINE_SWEEP:-"10 20 40 80 160"}
PIPELINE_PERSIST=${PIPELINE_PERSIST:-none}

# kvstore's AOF path writes through io_uring without an fsync, so `no` is the
# closest redis equivalent; everysec is redis' own default and the more common
# production setting.
REDIS_APPENDFSYNC=${REDIS_APPENDFSYNC:-everysec}

KVSTORE="$BUILD_DIR/kvstore"
TESTCASE="$BUILD_DIR/kvstore_client_testcase"

SERVER_PID=
CURRENT_RUNTIME=
NO_BUILD=0
HAVE_REDIS=1
ROWS=()

log()
{
    printf '%s\n' "$*"
}

die()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
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

# kvstore ignores SIGTERM (main.cpp sets every signal to SIG_IGN), and a
# snapshot child outlives its parent, so kill the whole family outright.
stop_server()
{
    [[ -n "$SERVER_PID" ]] || return 0

    pkill -KILL -P "$SERVER_PID" 2>/dev/null || true
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

cleanup()
{
    local status=$?
    trap - EXIT
    set +e
    stop_server
    [[ -n "$CURRENT_RUNTIME" && "${KEEP_LOGS:-0}" != 1 ]] && rm -rf -- "$CURRENT_RUNTIME"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

port_in_use()
{
    command -v ss >/dev/null 2>&1 || return 1
    ss -H -ltn "sport = :$1" 2>/dev/null | grep -q .
}

require_free_port()
{
    port_in_use "$1" && die "TCP port $1 is already in use; stop the existing listener first"
    return 0
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
        rm -rf -- "$CURRENT_RUNTIME/data" "$CURRENT_RUNTIME/rdb_data" \
            "$CURRENT_RUNTIME/appendonlydir" "$CURRENT_RUNTIME/dump.rdb"
    else
        rm -rf -- "$CURRENT_RUNTIME"
    fi
    CURRENT_RUNTIME=
}

wait_for_server()
{
    local port=$1 runtime=$2 log_name=$3
    local deadline=$((SECONDS + 30))

    while ((SECONDS < deadline)); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            show_log_tail "$runtime/$log_name"
            return 1
        fi
        if (exec 3<>"/dev/tcp/$HOST/$port") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        sleep 0.1
    done

    show_log_tail "$runtime/$log_name"
    return 1
}

start_kvstore()
{
    local runtime=$1 mode=$2

    cat >"$runtime/kvstore.ini" <<EOF
[server]
port = $KV_PORT
log_level = error

[persistence]
mode = $mode

[replication]
role = standalone
EOF

    (
        cd "$runtime"
        exec "$KVSTORE" kvstore.ini
    ) >"$runtime/kvstore.log" 2>&1 &
    SERVER_PID=$!

    # The servers are stopped on purpose; disown keeps bash from printing job
    # notices over the result tables.
    disown "$SERVER_PID" 2>/dev/null || true

    wait_for_server "$KV_PORT" "$runtime" kvstore.log
}

# appendonly is redis' incremental persistence; RDB snapshots stay off in both
# configurations so only the AOF is compared.
start_redis()
{
    local runtime=$1 appendonly=$2

    (
        cd "$runtime"
        exec redis-server \
            --port "$REDIS_PORT" \
            --dir "$runtime" \
            --save '' \
            --appendonly "$appendonly" \
            --appendfsync "$REDIS_APPENDFSYNC" \
            --protected-mode no \
            --loglevel warning
    ) >"$runtime/redis.log" 2>&1 &
    SERVER_PID=$!

    disown "$SERVER_PID" 2>/dev/null || true

    wait_for_server "$REDIS_PORT" "$runtime" redis.log
}

# One "<name>|<qps>|<avg>|<p50>|<p95>|<p99>" line per benchmarked command.
#
# redis-benchmark redraws its progress counter with carriage returns, so a
# summary line can share a physical line with a counter update; split on CR
# before parsing or the fields are garbage.
parse_benchmark()
{
    tr '\r' '\n' <"$1" | awk '
        /^====== / {
            name = $0
            sub(/^====== /, "", name)
            sub(/ ======[[:space:]]*$/, "", name)
            next
        }
        /throughput summary:/ { thr = $3; next }
        /latency summary/ {
            getline   # column header
            getline   # avg min p50 p95 p99 max
            printf "%s|%.0f|%s|%s|%s|%s\n", name, thr, $1, $3, $4, $5
        }
    '
}

add_rows()
{
    local server=$1 persist=$2 pipeline=$3 log=$4
    local op qps avg p50 p95 p99

    while IFS='|' read -r op qps avg p50 p95 p99; do
        ROWS+=("$server|$persist|$pipeline|$op|$qps|$avg|$p50|$p95|$p99")
    done < <(parse_benchmark "$log")
}

print_rows()
{
    local title=$1
    local row server persist pipeline op qps avg p50 p95 p99

    printf '\n%s\n' "$title"
    printf '  %-8s %-8s %-4s %-12s %12s %10s %10s %10s %10s\n' \
        SERVER PERSIST P COMMAND "QPS" "avg ms" "p50 ms" "p95 ms" "p99 ms"
    printf '  %-8s %-8s %-4s %-12s %12s %10s %10s %10s %10s\n' \
        -------- -------- ---- ------------ ------------ ---------- ---------- ---------- ----------

    if ((${#ROWS[@]} == 0)); then
        printf '  (no results)\n'
        return
    fi

    for row in "${ROWS[@]}"; do
        IFS='|' read -r server persist pipeline op qps avg p50 p95 p99 <<<"$row"
        printf '  %-8s %-8s %-4s %-12s %12s %10s %10s %10s %10s\n' \
            "$server" "$persist" "$pipeline" "$op" "$qps" "$avg" "$p50" "$p95" "$p99"
    done
}

run_benchmark()
{
    local log=$1
    shift

    if ! redis-benchmark "$@" >"$log" 2>&1; then
        show_log_tail "$log"
        die "redis-benchmark failed: redis-benchmark $*"
    fi
}

# ---------------------------------------------------------------------------
# 1. Incremental persistence on vs off, single client, no pipelining.
# ---------------------------------------------------------------------------
bench_persistence_target()
{
    local server=$1 persist=$2
    local runtime port appendonly

    new_runtime "persistence-$server-$persist"
    runtime=$CURRENT_RUNTIME

    if [[ "$server" == kvstore ]]; then
        port=$KV_PORT
        start_kvstore "$runtime" "$persist" || die "kvstore (mode = $persist) did not start"
    else
        port=$REDIS_PORT
        appendonly=no
        if [[ "$persist" == aof ]]; then
            appendonly=yes
        fi
        start_redis "$runtime" "$appendonly" || die "redis-server (appendonly $appendonly) did not start"
    fi

    log "  $server / persistence=$persist: PING hello, then SET+GET (-c 1 -P 1 -n $REQUESTS)"

    run_benchmark "$runtime/ping.log" \
        -h "$HOST" -p "$port" -c 1 -P 1 -n "$REQUESTS" PING hello
    add_rows "$server" "$persist" 1 "$runtime/ping.log"

    run_benchmark "$runtime/setget.log" \
        -h "$HOST" -p "$port" -c 1 -P 1 -n "$REQUESTS" -t set,get
    add_rows "$server" "$persist" 1 "$runtime/setget.log"

    stop_server
    drop_runtime
}

bench_persistence()
{
    log
    log "=== 1. incremental persistence: kvstore vs redis-server (-c 1 -P 1 -n $REQUESTS) ==="

    ROWS=()
    bench_persistence_target kvstore none
    bench_persistence_target kvstore aof

    if ((HAVE_REDIS == 1)); then
        bench_persistence_target redis none
        bench_persistence_target redis aof
    fi

    print_rows "Result: incremental persistence (redis appendfsync $REDIS_APPENDFSYNC)"
}

# ---------------------------------------------------------------------------
# 2. kvstore full snapshots: <SAVE interval>:<writes> pairs.
# ---------------------------------------------------------------------------
bench_save()
{
    local runtime pair interval writes snapshots commands ms qps

    log
    log "=== 2. kvstore full persistence: SAVE plan (interval:writes) $SAVE_PLAN ==="

    printf '\nResult: full persistence (SAVE, kvstore only)\n'
    printf '  %-12s %10s %10s %12s %10s %12s\n' "SAVE EVERY" WRITES SNAPSHOTS COMMANDS "TIME ms" QPS
    printf '  %-12s %10s %10s %12s %10s %12s\n' \
        ------------ ---------- ---------- ------------ ---------- ------------

    for pair in $SAVE_PLAN; do
        interval=${pair%%:*}
        writes=${pair##*:}

        new_runtime "save-$interval"
        runtime=$CURRENT_RUNTIME

        # SAVE is only accepted in rdb mode; every other mode answers an error.
        start_kvstore "$runtime" rdb || die "kvstore (mode = rdb) did not start"

        if ! "$TESTCASE" "$HOST" "$KV_PORT" 12 "$writes" "$interval" >"$runtime/save.log" 2>&1; then
            show_log_tail "$runtime/save.log"
            stop_server
            drop_runtime
            die "the SAVE testcase failed (mode 12, interval $interval, writes $writes)"
        fi

        snapshots=$((writes / interval))

        read -r commands ms qps < <(tr -d ',' <"$runtime/save.log" | awk '
            /resp-save-interval/ {
                for (i = 1; i <= NF; i++) {
                    if ($i == "commands:") commands = $(i + 1)
                    if ($i == "time_used:") ms = $(i + 1)
                    if ($i == "qps:") qps = $(i + 1)
                }
                print commands, ms, qps
            }')

        printf '  %-12s %10s %10s %12s %10s %12s\n' \
            "$interval" "$writes" "$snapshots" "$commands" "$ms" "$qps"

        sleep 60
        stop_server
        sleep 5
        drop_runtime
    done
}

# ---------------------------------------------------------------------------
# 3. Pipeline depth sweep, single client.
# ---------------------------------------------------------------------------
bench_pipeline_target()
{
    local server=$1
    local runtime port depth appendonly

    new_runtime "pipeline-$server"
    runtime=$CURRENT_RUNTIME

    if [[ "$server" == kvstore ]]; then
        port=$KV_PORT
        start_kvstore "$runtime" "$PIPELINE_PERSIST" || die "kvstore did not start"
    else
        port=$REDIS_PORT
        appendonly=no
        if [[ "$PIPELINE_PERSIST" == aof ]]; then
            appendonly=yes
        fi
        start_redis "$runtime" "$appendonly" || die "redis-server did not start"
    fi

    for depth in $PIPELINE_SWEEP; do
        log "  $server: SET+GET at -P $depth (-c 1 -n $REQUESTS)"
        run_benchmark "$runtime/pipeline-$depth.log" \
            -h "$HOST" -p "$port" -c 1 -P "$depth" -n "$REQUESTS" -t set,get
        add_rows "$server" "$PIPELINE_PERSIST" "$depth" "$runtime/pipeline-$depth.log"
    done

    stop_server
    drop_runtime
}

bench_pipeline()
{
    log
    log "=== 3. pipelining: kvstore vs redis-server (-c 1 -n $REQUESTS, P = $PIPELINE_SWEEP) ==="

    ROWS=()
    bench_pipeline_target kvstore
    if ((HAVE_REDIS == 1)); then
        bench_pipeline_target redis
    fi

    print_rows "Result: pipeline depth sweep (persistence = $PIPELINE_PERSIST)"
}

usage()
{
    awk 'NR == 1 { next }
         /^#/ { sub(/^# ?/, ""); print; seen = 1; next }
         seen { exit }' "$0"
}

SELECTED=all
SELECTED_SET=0

while (($# > 0)); do
    case "$1" in
        persistence | save | pipeline | all)
            ((SELECTED_SET == 1)) && die "more than one benchmark selected: '$SELECTED' and '$1'"
            SELECTED=$1
            SELECTED_SET=1
            shift
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        --quick)
            REQUESTS=50000
            SAVE_PLAN="10000:50000 1000:20000"
            PIPELINE_SWEEP="10 160"
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown argument '$1'"
            ;;
    esac
done

command -v redis-benchmark >/dev/null 2>&1 ||
    die "redis-benchmark not found (apt install redis-tools)"

if ! command -v redis-server >/dev/null 2>&1; then
    HAVE_REDIS=0
    log "WARNING: redis-server not found, only kvstore will be measured"
fi

require_free_port "$KV_PORT"
if ((HAVE_REDIS == 1)); then
    require_free_port "$REDIS_PORT"
fi

mkdir -p -- "$RUNTIME_ROOT"
build_once

REDIS_BIN="not installed"
if ((HAVE_REDIS == 1)); then
    REDIS_BIN=$(command -v redis-server)
fi

log
log "kvstore     : $KVSTORE ($HOST:$KV_PORT)"
log "redis-server: $REDIS_BIN ($HOST:$REDIS_PORT)"
log "requests    : $REQUESTS per benchmark"
log "save plan   : $SAVE_PLAN (SAVE interval:writes)"

case "$SELECTED" in
    persistence) bench_persistence ;;
    save) bench_save ;;
    pipeline) bench_pipeline ;;
    all)
        bench_persistence
        bench_save
        bench_pipeline
        ;;
esac

printf '\n'
rmdir -- "$RUNTIME_ROOT" 2>/dev/null || true
