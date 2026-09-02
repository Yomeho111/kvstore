#!/usr/bin/env bash
#
# pressure_test.sh — comprehensive redis-benchmark pressure test for kvstore.
#
# kvstore speaks the Redis RESP protocol (multi-bulk) and implements a subset of
# commands: PING, SET, GET, EXISTS, DEL (+ SET EX/PX, SETEX, EXPIRE). This script
# drives that subset with redis-benchmark across several dimensions:
#
#   1. connectivity + PING latency        (single client, no pipeline)
#   2. single-op SET/GET latency          (single client, no pipeline)
#   3. headline throughput                (your heavy set,get run)
#   4. pipeline-depth sweep               (-P 1..64)
#   5. payload-size sweep                 (-d 16..4096)
#   6. concurrency sweep                  (-c 16..256)
#   7. EXISTS throughput                  (random keys)
#   8. DEL throughput                     (random keys, destructive — runs last)
#
# Every knob is overridable via environment variables or flags (see --help).
# All output is echoed and, unless --no-save is given, tee'd to a timestamped
# log under $OUTDIR.
#
# Requirements: bash, redis-benchmark (redis-tools), and a running kvstore.
#
# Examples:
#   ./pressure_test.sh                         # full suite, defaults
#   ./pressure_test.sh -h 127.0.0.1 -p 8050    # explicit host/port
#   ./pressure_test.sh --quick                 # fast smoke run
#   REQUESTS=2000000 CLIENTS=128 ./pressure_test.sh
#
set -uo pipefail

# --------------------------------------------------------------------------- #
# Configuration (env-overridable; flags below take precedence)
# --------------------------------------------------------------------------- #
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8050}
REQUESTS=${REQUESTS:-1000000}      # -n for the headline run
SWEEP_REQUESTS=${SWEEP_REQUESTS:-200000}  # -n for each sweep point
LATENCY_REQUESTS=${LATENCY_REQUESTS:-50000}  # -n for the single-client latency runs
CLIENTS=${CLIENTS:-64}             # -c
PIPELINE=${PIPELINE:-16}           # -P
DATASIZE=${DATASIZE:-128}          # -d (SET payload bytes)
KEYSPACE=${KEYSPACE:-100000}       # -r (random keyspace size)
KEEPALIVE=${KEEPALIVE:-1}          # -k

# Sweeps (space-separated lists)
PIPELINE_SWEEP=${PIPELINE_SWEEP:-"1 8 16 32 64"}
DATASIZE_SWEEP=${DATASIZE_SWEEP:-"16 128 512 4096"}
CLIENTS_SWEEP=${CLIENTS_SWEEP:-"16 64 128 256"}

CSV=${CSV:-0}                      # 1 => redis-benchmark --csv
SAVE=${SAVE:-1}                    # 1 => tee output to a log file
OUTDIR=${OUTDIR:-./benchmark_results}

# --------------------------------------------------------------------------- #
# Argument parsing
# --------------------------------------------------------------------------- #
usage() {
    # Print the leading comment header (skip the shebang, stop at first code line).
    awk 'NR==1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
    cat <<EOF

Flags:
  -h, --host <ip>        target host           (default: $HOST)
  -p, --port <port>      target port           (default: $PORT)
  -n, --requests <n>     headline request count(default: $REQUESTS)
  -c, --clients <n>      parallel connections  (default: $CLIENTS)
  -P, --pipeline <n>     pipeline depth        (default: $PIPELINE)
  -d, --datasize <n>     SET payload bytes     (default: $DATASIZE)
  -r, --keyspace <n>     random keyspace size  (default: $KEYSPACE)
      --csv              emit CSV from redis-benchmark
      --no-save          do not write a log file
      --quick            fast smoke run (small counts)
      --help             show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--host)     HOST="$2"; shift 2 ;;
        -p|--port)     PORT="$2"; shift 2 ;;
        -n|--requests) REQUESTS="$2"; shift 2 ;;
        -c|--clients)  CLIENTS="$2"; shift 2 ;;
        -P|--pipeline) PIPELINE="$2"; shift 2 ;;
        -d|--datasize) DATASIZE="$2"; shift 2 ;;
        -r|--keyspace) KEYSPACE="$2"; shift 2 ;;
        --csv)         CSV=1; shift ;;
        --no-save)     SAVE=0; shift ;;
        --quick)
            REQUESTS=50000; SWEEP_REQUESTS=20000
            PIPELINE_SWEEP="1 16"; DATASIZE_SWEEP="16 512"; CLIENTS_SWEEP="16 64"
            shift ;;
        --help)        usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

# --------------------------------------------------------------------------- #
# Preflight
# --------------------------------------------------------------------------- #
if ! command -v redis-benchmark >/dev/null 2>&1; then
    echo "ERROR: redis-benchmark not found. Install it (e.g. 'sudo apt install redis-tools')." >&2
    exit 1
fi

# TCP reachability check (bash /dev/tcp, no extra deps).
if ! (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
    echo "ERROR: cannot connect to $HOST:$PORT — is kvstore running?" >&2
    echo "       start it with:  ./build/kvstore" >&2
    exit 1
fi
exec 3>&- 2>/dev/null || true

# Optional log file.
if [[ "$SAVE" == "1" ]]; then
    mkdir -p "$OUTDIR"
    STAMP=$(date +%Y%m%d_%H%M%S)
    LOG="$OUTDIR/pressure_${STAMP}.log"
    exec > >(tee -a "$LOG") 2>&1
fi

# --------------------------------------------------------------------------- #
# Common redis-benchmark arguments
# --------------------------------------------------------------------------- #
BASE=(-h "$HOST" -p "$PORT" -k "$KEEPALIVE")
[[ "$CSV" == "1" ]] && BASE+=(--csv)

FAILS=0
START_TS=$(date +%s)

run() {
    local label="$1"; shift
    echo
    echo "==================================================================="
    echo ">> $label"
    echo "   redis-benchmark ${*}"
    echo "==================================================================="
    if ! redis-benchmark "$@"; then
        echo "!! WARNING: '$label' exited non-zero"
        FAILS=$((FAILS + 1))
    fi
}

trap 'echo; echo "Interrupted."; exit 130' INT

# --------------------------------------------------------------------------- #
# Banner
# --------------------------------------------------------------------------- #
echo "############################################################"
echo "# kvstore pressure test — $(date)"
echo "# target      : $HOST:$PORT"
echo "# headline    : -n $REQUESTS -c $CLIENTS -P $PIPELINE -d $DATASIZE -r $KEYSPACE"
echo "# sweep -n    : $SWEEP_REQUESTS"
echo "# pipeline    : $PIPELINE_SWEEP"
echo "# datasize    : $DATASIZE_SWEEP"
echo "# clients     : $CLIENTS_SWEEP"
[[ "$SAVE" == "1" ]] && echo "# log         : $LOG"
echo "############################################################"

# --------------------------------------------------------------------------- #
# 0. Warm-up: populate the keyspace so GET/EXISTS/DEL hit real keys.
# --------------------------------------------------------------------------- #
run "Warm-up: populate ${KEYSPACE} keys" \
    "${BASE[@]}" -t set -r "$KEYSPACE" -n "$((KEYSPACE * 2))" -c "$CLIENTS" -P 32 -d "$DATASIZE" -q

# --------------------------------------------------------------------------- #
# 1. Connectivity + PING latency (single client, no pipeline).
#    Use an explicit PING command (multi-bulk) — kvstore does not support the
#    inline PING that "-t ping" also emits.
# --------------------------------------------------------------------------- #
run "PING latency (c=1, P=1)" \
    "${BASE[@]}" -n "$LATENCY_REQUESTS" -c 1 -P 1 PING

# --------------------------------------------------------------------------- #
# 2. Single-op SET/GET latency (single client, no pipeline).
# --------------------------------------------------------------------------- #
run "SET/GET latency baseline (c=1, P=1)" \
    "${BASE[@]}" -n "$LATENCY_REQUESTS" -c 1 -P 1 -d "$DATASIZE" -r "$KEYSPACE" -t set,get -q

# --------------------------------------------------------------------------- #
# 3. Headline throughput (your heavy run).
# --------------------------------------------------------------------------- #
run "Headline throughput (SET/GET)" \
    "${BASE[@]}" -n "$REQUESTS" -c "$CLIENTS" -P "$PIPELINE" -d "$DATASIZE" -r "$KEYSPACE" -t set,get

# --------------------------------------------------------------------------- #
# 4. Pipeline-depth sweep.
# --------------------------------------------------------------------------- #
for p in $PIPELINE_SWEEP; do
    run "Pipeline sweep: P=$p" \
        "${BASE[@]}" -n "$SWEEP_REQUESTS" -c "$CLIENTS" -P "$p" -d "$DATASIZE" -r "$KEYSPACE" -t set,get -q
done

# --------------------------------------------------------------------------- #
# 5. Payload-size sweep.
# --------------------------------------------------------------------------- #
for d in $DATASIZE_SWEEP; do
    run "Payload sweep: d=${d}B" \
        "${BASE[@]}" -n "$SWEEP_REQUESTS" -c "$CLIENTS" -P "$PIPELINE" -d "$d" -r "$KEYSPACE" -t set,get -q
done

# --------------------------------------------------------------------------- #
# 6. Concurrency sweep.
# --------------------------------------------------------------------------- #
for c in $CLIENTS_SWEEP; do
    run "Concurrency sweep: c=$c" \
        "${BASE[@]}" -n "$SWEEP_REQUESTS" -c "$c" -P "$PIPELINE" -d "$DATASIZE" -r "$KEYSPACE" -t set,get -q
done

# --------------------------------------------------------------------------- #
# 7. EXISTS throughput (random keys against the warmed keyspace).
# --------------------------------------------------------------------------- #
run "EXISTS throughput (random keys)" \
    "${BASE[@]}" -n "$SWEEP_REQUESTS" -c "$CLIENTS" -P "$PIPELINE" -r "$KEYSPACE" EXISTS "key:__rand_int__"

# --------------------------------------------------------------------------- #
# 8. DEL throughput (destructive — empties the keyspace, so it runs last).
# --------------------------------------------------------------------------- #
run "DEL throughput (random keys)" \
    "${BASE[@]}" -n "$SWEEP_REQUESTS" -c "$CLIENTS" -P "$PIPELINE" -r "$KEYSPACE" DEL "key:__rand_int__"

# --------------------------------------------------------------------------- #
# Summary
# --------------------------------------------------------------------------- #
END_TS=$(date +%s)
echo
echo "############################################################"
echo "# Done in $((END_TS - START_TS))s. Benchmarks with warnings: $FAILS"
[[ "$SAVE" == "1" ]] && echo "# Results saved to: $LOG"
echo "############################################################"

exit 0
