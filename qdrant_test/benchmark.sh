#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)
PYTHON=${PYTHON:-python3}
VENV_DIR=${QDRANT_VENV_DIR:-"$REPO/out/qdrant-benchmark/venv"}
QDRANT_IMAGE=${QDRANT_IMAGE:-qdrant/qdrant:v1.19.2}
STARTUP_TIMEOUT=${QDRANT_STARTUP_TIMEOUT:-60}
CONTAINER_NAME="kvstore-qdrant-bench-$$-$RANDOM"
CONTAINER_ID=

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
    "$PYTHON" -m venv "$VENV_DIR"
fi
VENV_DIR=$(cd -- "$VENV_DIR" && pwd)
if ! "$VENV_DIR/bin/python" -c '
from importlib.metadata import version
import qdrant_client
installed = tuple(int(part) for part in version("qdrant-client").split(".")[:2])
if not (1, 19) <= installed < (2, 0):
    raise SystemExit("qdrant-client>=1.19,<2 is required")
'; then
    "$VENV_DIR/bin/python" -m pip install -r "$SCRIPT_DIR/requirements.txt"
fi

for arg in "$@"; do
    case "$arg" in
        --host|--host=*|--http-port|--http-port=*|--grpc-port|--grpc-port=*)
            printf 'ERROR: Docker manages the host and ports for this run; do not pass %s.\n' "$arg" >&2
            exit 2
            ;;
        --help|-h)
            exec "$VENV_DIR/bin/python" -m qdrant_test.benchmark "$@"
            ;;
    esac
done

"$VENV_DIR/bin/python" -m qdrant_test.benchmark "$@" --check-args

[[ "$STARTUP_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || {
    printf 'ERROR: QDRANT_STARTUP_TIMEOUT must be a positive integer\n' >&2
    exit 2
}
command -v curl >/dev/null 2>&1 || {
    printf 'ERROR: curl is required for the Qdrant Docker readiness check\n' >&2
    exit 1
}

cleanup()
{
    local status=$?
    trap - EXIT
    set +e
    if [[ -n "$CONTAINER_ID" ]] && sudo docker container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
        if ((status != 0)); then
            printf 'Qdrant container logs (last 30 lines):\n' >&2
            sudo docker logs --tail 30 "$CONTAINER_NAME" >&2
        fi
        printf 'Removing benchmark Qdrant container %s...\n' "$CONTAINER_NAME"
        if ! sudo docker rm --force "$CONTAINER_NAME" >/dev/null; then
            printf 'ERROR: failed to remove benchmark container %s\n' "$CONTAINER_NAME" >&2
            ((status != 0)) || status=1
        fi
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'Starting temporary Qdrant container %s from %s...\n' "$CONTAINER_NAME" "$QDRANT_IMAGE"
CONTAINER_ID=$(sudo docker run --detach --rm \
    --name "$CONTAINER_NAME" \
    --publish 127.0.0.1::6333 \
    --publish 127.0.0.1::6334 \
    --env QDRANT__TELEMETRY_DISABLED=true \
    "$QDRANT_IMAGE")
[[ -n "$CONTAINER_ID" ]] || {
    printf 'ERROR: Docker did not return a container ID\n' >&2
    exit 1
}

get_host_port()
{
    local binding
    binding=$(sudo docker port "$CONTAINER_NAME" "$1/tcp" | head -n 1)
    local port=${binding##*:}
    [[ "$port" =~ ^[1-9][0-9]*$ ]] || {
        printf 'ERROR: could not determine the mapped host port for container port %s: %s\n' "$1" "$binding" >&2
        return 1
    }
    printf '%s\n' "$port"
}

HTTP_PORT=$(get_host_port 6333)
GRPC_PORT=$(get_host_port 6334)
printf 'Waiting for Qdrant HTTP on 127.0.0.1:%s and gRPC on 127.0.0.1:%s...\n' \
    "$HTTP_PORT" "$GRPC_PORT"
deadline=$((SECONDS + STARTUP_TIMEOUT))
until curl --connect-timeout 1 --max-time 2 --fail --silent \
    "http://127.0.0.1:$HTTP_PORT/" >/dev/null 2>&1; do
    if ((SECONDS >= deadline)); then
        printf 'ERROR: Qdrant did not become ready within %s seconds\n' "$STARTUP_TIMEOUT" >&2
        exit 1
    fi
    sleep 0.25
done
printf 'Qdrant is ready.\n'

cd -- "$REPO"
"$VENV_DIR/bin/python" -m qdrant_test.benchmark \
    --host 127.0.0.1 \
    --http-port "$HTTP_PORT" \
    --grpc-port "$GRPC_PORT" \
    "$@"
