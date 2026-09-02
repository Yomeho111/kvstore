#!/usr/bin/env bash

# Configure, build and verify kvstore.
#
# Every CMake option in CMakeLists.txt is exposed as a flag. The project is
# compiled once, then scripts/test_suite.sh and scripts/test_master_slave.sh are
# run against that build with --no-build, so nothing is compiled twice.
#
# Usage:
#   install.sh [build options] [run options]
#
# Build options (defaults in brackets):
#   -b, --build-dir DIR        output directory                        [build]
#   -t, --build-type TYPE      Debug | Release                         [Release]
#   -n, --network BACKEND      REACTOR | PROACTOR | COROUTINE          [REACTOR]
#   -e, --engine ENGINE        RBTREE_ENGINE | HASH_ENGINE |
#                              SKIPLIST_ENGINE | ARRAY_ENGINE          [RBTREE_ENGINE]
#   -p, --port-num N           consecutive ports to listen on          [1]
#       --enable-timer BOOL    connection timing logs                  [OFF]
#       --memory-pool BOOL     built-in memory pool allocator          [ON]
#       --tcmalloc BOOL        link against tcmalloc                   [OFF]
#       --jemalloc BOOL        link against jemalloc                   [OFF]
#       --exceptions BOOL      compile with C++ exceptions             [OFF]
#   -j, --jobs N               parallel build jobs                     [nproc]
#       --clean                remove the build directory first
#
# Run options:
#       --tests LIST           comma-separated: suite, replication, none  [suite,replication]
#       --skip-tests           same as --tests none
#       --pressure-args "..."  extra arguments for pressure_test.sh (e.g. "--quick")
#
# BOOL is ON or OFF.
#
# Examples:
#   install.sh
#   install.sh --network PROACTOR --engine HASH_ENGINE --tests suite
#   install.sh --build-type Debug --memory-pool OFF --pressure-args "--quick"
#   install.sh --skip-tests

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)

BUILD_DIR="$REPO/build"
BUILD_TYPE=Release
NETWORK=REACTOR
ENGINE=RBTREE_ENGINE
PORT_NUM=1
ENABLE_TIMER=OFF
MEMORY_POOL=ON
TCMALLOC=OFF
JEMALLOC=OFF
EXCEPTIONS=OFF
JOBS=$(nproc)
CLEAN_FIRST=0
TESTS=suite,replication
PRESSURE_ARGS_RAW=

usage()
{
    awk 'NR == 1 { next }
         /^#/ { sub(/^# ?/, ""); print; seen = 1; next }
         seen { exit }' "$0"
}

die()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

need_value()
{
    [[ -n "${2:-}" ]] || die "$1 requires a value"
}

# Rejects anything CMake would reject anyway, but with a clearer message.
check_choice()
{
    local name=$1 value=$2
    shift 2

    local choice
    for choice in "$@"; do
        [[ "$value" == "$choice" ]] && return 0
    done

    die "invalid $name '$value' (expected one of: $*)"
}

while (($# > 0)); do
    case "$1" in
        -b | --build-dir)
            need_value "$1" "${2:-}"
            BUILD_DIR=$(readlink -m -- "$2")
            shift 2
            ;;
        -t | --build-type)
            need_value "$1" "${2:-}"
            BUILD_TYPE=$2
            shift 2
            ;;
        -n | --network)
            need_value "$1" "${2:-}"
            NETWORK=${2^^}
            shift 2
            ;;
        -e | --engine)
            need_value "$1" "${2:-}"
            ENGINE=${2^^}
            shift 2
            ;;
        -p | --port-num)
            need_value "$1" "${2:-}"
            PORT_NUM=$2
            shift 2
            ;;
        --enable-timer)
            need_value "$1" "${2:-}"
            ENABLE_TIMER=${2^^}
            shift 2
            ;;
        --memory-pool)
            need_value "$1" "${2:-}"
            MEMORY_POOL=${2^^}
            shift 2
            ;;
        --tcmalloc)
            need_value "$1" "${2:-}"
            TCMALLOC=${2^^}
            shift 2
            ;;
        --jemalloc)
            need_value "$1" "${2:-}"
            JEMALLOC=${2^^}
            shift 2
            ;;
        --exceptions)
            need_value "$1" "${2:-}"
            EXCEPTIONS=${2^^}
            shift 2
            ;;
        -j | --jobs)
            need_value "$1" "${2:-}"
            JOBS=$2
            shift 2
            ;;
        --clean)
            CLEAN_FIRST=1
            shift
            ;;
        --tests)
            need_value "$1" "${2:-}"
            TESTS=$2
            shift 2
            ;;
        --skip-tests)
            TESTS=none
            shift
            ;;
        --pressure-args)
            need_value "$1" "${2:-}"
            PRESSURE_ARGS_RAW=$2
            shift 2
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown option '$1'"
            ;;
    esac
done

check_choice "--build-type" "$BUILD_TYPE" Debug Release RelWithDebInfo MinSizeRel
check_choice "--network" "$NETWORK" REACTOR PROACTOR COROUTINE
check_choice "--engine" "$ENGINE" RBTREE_ENGINE HASH_ENGINE SKIPLIST_ENGINE ARRAY_ENGINE
check_choice "--enable-timer" "$ENABLE_TIMER" ON OFF
check_choice "--memory-pool" "$MEMORY_POOL" ON OFF
check_choice "--tcmalloc" "$TCMALLOC" ON OFF
check_choice "--jemalloc" "$JEMALLOC" ON OFF
check_choice "--exceptions" "$EXCEPTIONS" ON OFF

[[ "$PORT_NUM" =~ ^[1-9][0-9]*$ ]] || die "--port-num must be a positive integer, got '$PORT_NUM'"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer, got '$JOBS'"

if [[ "$TCMALLOC" == ON && "$JEMALLOC" == ON ]]; then
    die "--tcmalloc and --jemalloc are mutually exclusive"
fi

RUN_SUITE=0
RUN_REPLICATION=0
if [[ "$TESTS" != none ]]; then
    IFS=',' read -ra requested <<<"$TESTS"
    for name in "${requested[@]}"; do
        case "${name// /}" in
            suite) RUN_SUITE=1 ;;
            replication) RUN_REPLICATION=1 ;;
            "") ;;
            *) die "unknown test '$name' in --tests (expected: suite, replication, none)" ;;
        esac
    done
fi

printf '==> Configuration\n'
printf '    build dir    : %s\n' "$BUILD_DIR"
printf '    build type   : %s\n' "$BUILD_TYPE"
printf '    network      : %s\n' "$NETWORK"
printf '    engine       : %s\n' "$ENGINE"
printf '    port num     : %s\n' "$PORT_NUM"
printf '    timer logs   : %s\n' "$ENABLE_TIMER"
printf '    memory pool  : %s\n' "$MEMORY_POOL"
printf '    tcmalloc     : %s\n' "$TCMALLOC"
printf '    jemalloc     : %s\n' "$JEMALLOC"
printf '    exceptions   : %s\n' "$EXCEPTIONS"
printf '    jobs         : %s\n' "$JOBS"
printf '    tests        : %s\n' "$TESTS"

if ((CLEAN_FIRST == 1)); then
    printf '\n==> Removing %s\n' "$BUILD_DIR"
    rm -rf -- "$BUILD_DIR"
fi

printf '\n==> Building\n'
BUILD_LOG=$(mktemp)
if ! {
    cmake -S "$REPO" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DNETWORK="$NETWORK" \
        -DENGINE="$ENGINE" \
        -DKVSTORE_PORT_NUM="$PORT_NUM" \
        -DKVSTORE_ENABLE_TIMER="$ENABLE_TIMER" \
        -DENABLE_MEMORY_POOL="$MEMORY_POOL" \
        -DENABLE_TCMALLOC="$TCMALLOC" \
        -DENABLE_JEMALLOC="$JEMALLOC" \
        -DENABLE_CPP_EXCEPTIONS="$EXCEPTIONS" &&
        cmake --build "$BUILD_DIR" -j "$JOBS"
} >"$BUILD_LOG" 2>&1; then
    tail -n 40 "$BUILD_LOG" >&2
    die "build failed (full log: $BUILD_LOG)"
fi
rm -f -- "$BUILD_LOG"

for binary in kvstore kvstore_client kvstore_client_testcase; do
    [[ -x "$BUILD_DIR/$binary" ]] || die "missing $BUILD_DIR/$binary after the build"
done
printf '    built kvstore, kvstore_client, kvstore_client_testcase\n'

FAILURES=0

if ((RUN_SUITE == 1)); then
    printf '\n==> test_suite.sh\n'
    read -ra pressure_args <<<"$PRESSURE_ARGS_RAW"
    BUILD_DIR="$BUILD_DIR" "$SCRIPT_DIR/test_suite.sh" --no-build "${pressure_args[@]}" ||
        FAILURES=$((FAILURES + 1))
fi

if ((RUN_REPLICATION == 1)); then
    printf '\n==> test_master_slave.sh\n'
    BUILD_DIR="$BUILD_DIR" RECREATE_SLAVE_REPO=1 "$SCRIPT_DIR/test_master_slave.sh" --no-build ||
        FAILURES=$((FAILURES + 1))
fi

if ((FAILURES > 0)); then
    printf '\n==> %d test script(s) failed\n' "$FAILURES" >&2
    exit 1
fi

printf '\n==> Done. Binaries are in %s\n' "$BUILD_DIR"
