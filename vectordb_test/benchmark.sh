#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)
PYTHON=${PYTHON:-python3}
BUILD_ROOT=${BUILD_ROOT:-"$REPO/out/vectordb-benchmark"}
BUILD_JOBS=${BUILD_JOBS:-2}

for arg in "$@"; do
    if [[ "$arg" == --help || "$arg" == -h ]]; then
        exec "$PYTHON" "$SCRIPT_DIR/benchmark.py" --help
    fi
done

[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || {
    printf 'ERROR: BUILD_JOBS must be a positive integer\n' >&2
    exit 2
}
mkdir -p -- "$BUILD_ROOT"
BUILD_ROOT=$(cd -- "$BUILD_ROOT" && pwd)
COMMON_ARGS=(
    --bruteforce-bin "$BUILD_ROOT/bruteforce/kvstore"
    --hnsw-bin "$BUILD_ROOT/hnsw/kvstore"
    --output-dir "$BUILD_ROOT/results"
)
"$PYTHON" "$SCRIPT_DIR/benchmark.py" "${COMMON_ARGS[@]}" "$@" --check-args

for engine in bruteforce hnsw; do
    brute_flag=OFF
    [[ "$engine" != bruteforce ]] || brute_flag=ON
    build_log="$BUILD_ROOT/$engine-build.log"
    printf 'Building %s (ENABLE_RAG=ON, ENABLE_BRUTEFORCE=%s)...\n' "$engine" "$brute_flag"
    if ! {
        cmake -S "$REPO" -B "$BUILD_ROOT/$engine" \
            -DCMAKE_BUILD_TYPE=Release \
            -DENGINE=RBTREE_ENGINE \
            -DKVSTORE_PORT_NUM=1 \
            -DENABLE_RAG=ON \
            -DENABLE_BRUTEFORCE="$brute_flag" \
            -DENABLE_MEMORY_POOL=OFF \
            -DENABLE_TCMALLOC=OFF \
            -DENABLE_JEMALLOC=OFF \
            -DENABLE_CPP_EXCEPTIONS=OFF \
            -DKVSTORE_ENABLE_TIMER=OFF \
            -DBUILD_EBPF_TRANSFER=OFF &&
            cmake --build "$BUILD_ROOT/$engine" --target kvstore --parallel "$BUILD_JOBS"
    } >"$build_log" 2>&1; then
        tail -n 30 "$build_log" >&2
        printf 'ERROR: %s build failed; full log: %s\n' "$engine" "$build_log" >&2
        exit 1
    fi
done

printf 'Both builds finished in %s seconds (cached on subsequent runs).\n' "$SECONDS"
exec "$PYTHON" "$SCRIPT_DIR/benchmark.py" "${COMMON_ARGS[@]}" "$@"
