# VectorDB Service

Async gRPC service exposing `Rag.Vset`, `Rag.Vsearch`, `Rag.Set`, and `Rag.Get`
over the existing kvstore backends. Text and normalized 768-dimensional vectors
share a UUID key, but vector and text operations are independent.

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
`Alibaba-NLP/gte-multilingual-base` and needs network
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

## RPC Operations

- `Vset(VsetRequest(text=...))` embeds the text, writes only the vector, and
  returns a generated UUID. It does not save text to the KV backend.
- `Vsearch(VsearchRequest(text=..., topk=..., threshold=...))` searches only the
  vector backend and returns `VsearchResponse.uuids`. It does not fetch text.
- `Set(SetRequest(uuid=..., text=...))` writes text directly to the KV backend
  under the supplied UUID and returns `SetResponse(ok=True)` on success.
- `Get(GetRequest(uuid=...))` reads text from the KV backend and returns
  `GetResponse(text=...)`. A missing UUID returns gRPC `NOT_FOUND`; a stored
  empty string is returned successfully.

Write failures return `INTERNAL`. Backend connection errors and timeouts return
`UNAVAILABLE`; other Redis errors return `INTERNAL`.

Clients explicitly save text after inserting its vector, and fetch text for
the UUIDs returned by a search:

```python
from vectordb_service import vectordb_pb2


async def save_and_search(stub):
    created = await stub.Vset(vectordb_pb2.VsetRequest(text="document"))
    await stub.Set(vectordb_pb2.SetRequest(uuid=created.uuid, text="document"))
    matches = await stub.Vsearch(
        vectordb_pb2.VsearchRequest(text="query", topk=5, threshold=0.5)
    )
    for record_uuid in matches.uuids:
        response = await stub.Get(vectordb_pb2.GetRequest(uuid=record_uuid))
        print(response.text)
```

## Search Threshold

Pass the distance threshold explicitly when constructing a search request:

```python
from vectordb_service import vectordb_pb2

request = vectordb_pb2.VsearchRequest(text="query", topk=5, threshold=0.5)
```

The service forwards it using `search_uuids(embedding, threshold=..., k=...)`.
The RESP command is `VSEARCH <vector> <topk> <threshold>`. The backend uses
cosine distance (`1 - cosine_similarity`) and keeps candidates whose distance
is strictly less than the threshold, so it may return fewer than `topk` UUIDs.
NaN and infinity are rejected. An omitted proto3 float has the value `0.0`;
it is not automatically replaced by a permissive default.

The current request uses field numbers `topk=1`, `threshold=2`, `text=3`.
This changes the previous `text=2` wire contract: regenerate client protobuf
bindings and update clients together with the server.
`VsearchResponse` field 1 now contains `uuids` instead of `texts`; existing
clients must use the regenerated bindings and call `Get` to retrieve text.

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

The separate `Vset` and `Set` calls are not transactional: omitting `Set` or a
text-write failure after the vector write can leave a UUID without stored text.
`Vsearch` still returns that UUID, and `Get` reports `NOT_FOUND` until text is
stored.