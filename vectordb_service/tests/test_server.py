import asyncio
import signal
import sys
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock
from uuid import UUID

import grpc
import pytest
import pytest_asyncio
from redis.exceptions import ConnectionError, RedisError, TimeoutError

from vectordb_service import run, server, vectordb_pb2, vectordb_pb2_grpc

SETTINGS = {
    "vectordb_host": "127.0.0.1",
    "vectordb_port": 8060,
    "vectordb_max_connections": 20,
    "vectordb_timeout": 5,
    "kvstore_host": "127.0.0.1",
    "kvstore_port": 8050,
    "kvstore_max_connections": 20,
    "kvstore_timeout": 5,
}


@pytest.fixture
def rag(monkeypatch):
    vectors = SimpleNamespace(
        insert_vector=AsyncMock(return_value=True),
        search_uuids=AsyncMock(return_value=[]),
        aclose=AsyncMock(),
    )
    texts = SimpleNamespace(
        set=AsyncMock(return_value=True),
        get=AsyncMock(return_value="stored text"),
        aclose=AsyncMock(),
    )
    monkeypatch.setattr(server, "VectorDbConnectionPool", Mock(return_value=vectors))
    monkeypatch.setattr(server, "KvstoreConnectionPool", Mock(return_value=texts))
    embedding = Mock()
    embedding.encode.return_value = SimpleNamespace(tolist=lambda: [1.0] * 768)
    return server.Rag(**SETTINGS, embedding_layer=embedding)


@pytest_asyncio.fixture
async def rpc(rag):
    transport = grpc.aio.server()
    vectordb_pb2_grpc.add_RagServicer_to_server(rag, transport)
    port = transport.add_insecure_port("127.0.0.1:0")
    await transport.start()
    try:
        async with grpc.aio.insecure_channel(f"127.0.0.1:{port}") as channel:
            yield vectordb_pb2_grpc.RagStub(channel)
    finally:
        await transport.stop(grace=0)
        await rag.aclose()


async def test_vset_returns_uuid_for_both_backends(rpc, rag):
    response = await rpc.Vset(vectordb_pb2.VsetRequest(text="hello"), timeout=3)
    assert str(UUID(response.uuid)) == response.uuid
    rag.vectordb.insert_vector.assert_awaited_once_with(response.uuid, [1.0] * 768)
    rag.kvstore.set.assert_awaited_once_with(response.uuid, "hello")
    rag.embedding_layer.encode.assert_called_once_with("hello")


async def test_vsearch_filters_missing_text_but_preserves_empty_text(rpc, rag):
    rag.vectordb.search_uuids.return_value = ["first", "missing", "empty"]
    values = {"first": "text", "missing": None, "empty": ""}
    rag.kvstore.get.side_effect = lambda key: values[key]
    response = await rpc.Vsearch(
        vectordb_pb2.VsearchRequest(text="query", topk=3), timeout=3
    )
    assert list(response.texts) == ["text", ""]
    rag.vectordb.search_uuids.assert_awaited_once_with([1.0] * 768, 3)


async def test_empty_vector_search_does_not_read_text(rpc, rag):
    response = await rpc.Vsearch(
        vectordb_pb2.VsearchRequest(text="query", topk=5), timeout=3
    )
    assert list(response.texts) == []
    rag.kvstore.get.assert_not_awaited()


@pytest.mark.parametrize("top_k", [0, -1, 65536])
async def test_invalid_top_k(rpc, rag, top_k):
    with pytest.raises(grpc.aio.AioRpcError) as failure:
        await rpc.Vsearch(
            vectordb_pb2.VsearchRequest(text="query", topk=top_k), timeout=3
        )
    assert failure.value.code() == grpc.StatusCode.INVALID_ARGUMENT
    rag.vectordb.search_uuids.assert_not_awaited()


@pytest.mark.parametrize("backend", ["vectordb", "kvstore"])
async def test_unsuccessful_write_reports_internal(rpc, rag, backend):
    write = rag.vectordb.insert_vector if backend == "vectordb" else rag.kvstore.set
    write.return_value = False
    with pytest.raises(grpc.aio.AioRpcError) as failure:
        await rpc.Vset(vectordb_pb2.VsetRequest(text="hello"), timeout=3)
    assert failure.value.code() == grpc.StatusCode.INTERNAL


@pytest.mark.parametrize(
    "error,status",
    [
        (ConnectionError("offline"), grpc.StatusCode.UNAVAILABLE),
        (TimeoutError("busy"), grpc.StatusCode.UNAVAILABLE),
        (RedisError("failed"), grpc.StatusCode.INTERNAL),
    ],
)
async def test_write_errors_are_rpc_statuses(rpc, rag, error, status):
    rag.vectordb.insert_vector.side_effect = error
    with pytest.raises(grpc.aio.AioRpcError) as failure:
        await rpc.Vset(vectordb_pb2.VsetRequest(text="hello"), timeout=3)
    assert failure.value.code() == status


@pytest.mark.parametrize(
    "error,status",
    [
        (ConnectionError("offline"), grpc.StatusCode.UNAVAILABLE),
        (TimeoutError("busy"), grpc.StatusCode.UNAVAILABLE),
        (RedisError("failed"), grpc.StatusCode.INTERNAL),
    ],
)
async def test_get_errors_are_not_silently_filtered(rpc, rag, error, status):
    rag.vectordb.search_uuids.return_value = ["first"]
    rag.kvstore.get.side_effect = error
    with pytest.raises(grpc.aio.AioRpcError) as failure:
        await rpc.Vsearch(vectordb_pb2.VsearchRequest(text="query", topk=1), timeout=3)
    assert failure.value.code() == status


@pytest.mark.parametrize(
    "failure", [None, RuntimeError("server failed"), asyncio.CancelledError()]
)
async def test_serve_closes_grpc_and_backend_pools(monkeypatch, rag, failure):
    transport = SimpleNamespace(
        add_insecure_port=Mock(return_value=50051),
        start=AsyncMock(),
        wait_for_termination=AsyncMock(side_effect=failure),
        stop=AsyncMock(),
    )
    constructor = Mock(return_value=rag)
    monkeypatch.setattr(run, "load_config", Mock(return_value=SETTINGS))
    monkeypatch.setattr(run, "Rag", constructor)
    monkeypatch.setattr(run.grpc.aio, "server", Mock(return_value=transport))
    monkeypatch.setattr(run.vectordb_pb2_grpc, "add_RagServicer_to_server", Mock())
    if failure is None:
        await run.serve(listen="127.0.0.1:50051", model_name="local-model")
    else:
        with pytest.raises(type(failure)):
            await run.serve(model_name="local-model")
    constructor.assert_called_once_with(**SETTINGS, model_name="local-model")
    transport.stop.assert_awaited_once_with(grace=5)
    rag.vectordb.aclose.assert_awaited_once()
    rag.kvstore.aclose.assert_awaited_once()


def test_missing_embedding_extra_has_actionable_error(monkeypatch):
    monkeypatch.setitem(sys.modules, "sentence_transformers", None)
    with pytest.raises(RuntimeError, match="uv sync --extra embedding"):
        server.EmbeddingLayer()


async def test_signal_wrapper_removes_handler_after_shutdown(monkeypatch):
    loop = asyncio.get_running_loop()
    add_handler = Mock()
    remove_handler = Mock()
    monkeypatch.setattr(loop, "add_signal_handler", add_handler)
    monkeypatch.setattr(loop, "remove_signal_handler", remove_handler)
    monkeypatch.setattr(run, "serve", AsyncMock(side_effect=asyncio.CancelledError()))
    await run._run_with_signals()
    add_handler.assert_called_once_with(signal.SIGTERM, asyncio.current_task().cancel)
    remove_handler.assert_called_once_with(signal.SIGTERM)
