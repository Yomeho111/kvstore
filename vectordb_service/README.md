# VectorDB Service

Async gRPC service exposing `Rag.Vset` and `Rag.Vsearch` over the existing
kvstore backends. Text and normalized 768-dimensional vectors share a UUID key.

## Layout

```text
vectordb_service/
  pyproject.toml
  uv.lock
  server_config.json
  proto/vectordb.proto
  src/vectordb_service/
    __init__.py
    __main__.py
    run.py
    server.py
    embedding.py
    kvstore_connection.py
    generate.py
    default_config.json
    vectordb_pb2.py
    vectordb_pb2_grpc.py
  tests/
```

## Deploy From Source

Use Python 3.11 or newer and install `uv`. Start the ordinary KV backend and
the vector backend (`ENABLE_RAG=ON`) before serving requests.

```bash
cd vectordb_service
uv sync --locked --extra embedding --no-dev
uv run --no-sync vectordb-server --config-dir ../config --server-config server_config.json
```

The first startup downloads the model
`sentence-transformers/paraphrase-multilingual-mpnet-base-v2` and needs network
access and enough disk space for its weights. Linux model dependencies use CPU
PyTorch wheels, avoiding CUDA downloads. A predownloaded 768-dimensional model
can be supplied with `--model /path/to/model`.

The listener defaults to `127.0.0.1:50051`. Change it with
`--listen 0.0.0.0:50051` only on a trusted network: this listener has no TLS or
authentication. `Ctrl+C` and `SIGTERM` stop gRPC and close the backend pools.

## Configuration

`--config-dir` reads `[server] ip` and `port` from `kvstore.ini` and
`vectordb.ini`. An omitted `ip` defaults to `127.0.0.1`.
`--server-config` reads `max_connection` and `timeout` from the JSON objects
`kvstore` and `vectordb`. The timeout is the pool acquisition timeout.

In a source checkout the service finds the existing `../config/` directory and
`server_config.json` automatically, independent of the current working directory.
An installed wheel works without this repository: it uses packaged defaults
(KV port 8050, vector port 8060) or explicit configuration paths. Without INI
files, JSON objects may also specify `host` and `port`.

## Development And Tests

```bash
uv sync --locked
uv run --no-sync pytest
uv run --no-sync vectordb-generate-proto
```

Tests use fake embeddings and storage, and exercise real local gRPC calls.
They do not download a model or require running databases. The generator uses
a virtual protobuf package path so imports work inside `vectordb_service`;
do not manually edit the generated modules. Regenerate after changing the proto.

## Build A Wheel

```bash
uv build
```

Install the wheel with its `embedding` extra, or install the package first and
add the model dependencies separately. The wheel includes generated protobuf
modules and default configuration; `proto/` and tests are included in the sdist.
`python -m vectordb_service` provides the same entry point as `vectordb-server`.

The CPU PyTorch index is a uv project setting, not wheel metadata. A plain pip
installation does not inherit it; preinstall CPU PyTorch from its official wheel
index when deploying the wheel to a CPU-only environment.

For VS Code, select `vectordb_service/.venv/bin/python` as the interpreter after
`uv sync` so the editor uses the package dependencies and editable installation.

The two database writes are not transactional: a text-write failure after the
vector write can leave an orphan UUID. Missing texts are skipped during search.