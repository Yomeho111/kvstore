import struct
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, Mock

import pytest

from vectordb_service import kvstore_connection as connections


@pytest.fixture
def redis_client(monkeypatch):
    client = SimpleNamespace(
        set=AsyncMock(return_value=True),
        get=AsyncMock(return_value="stored text"),
        execute_command=AsyncMock(return_value="uuid-one,uuid-two"),
    )
    context = MagicMock()
    context.__aenter__ = AsyncMock(return_value=client)
    context.__aexit__ = AsyncMock(return_value=False)
    monkeypatch.setattr(connections.redis, "Redis", Mock(return_value=context))
    return client


def test_binary_vector_format():
    blob = connections.encode_vector([1.0] * 768)
    assert len(blob) == 3080
    assert struct.unpack("<II", blob[:8]) == (1, 768)
    assert struct.unpack("<768f", blob[8:]) == (1.0,) * 768


def test_invalid_vector_dimension():
    with pytest.raises(ValueError, match="768"):
        connections.encode_vector([1.0] * 767)


async def test_insert_vector(redis_client):
    async with connections.VectorDbConnectionPool() as database:
        assert await database.insert_vector("uuid-one", [1.0] * 768) is True
    redis_client.set.assert_awaited_once_with("uuid-one", connections.encode_vector([1.0] * 768))


@pytest.mark.parametrize("reply,expected", [("uuid-one,uuid-two", ["uuid-one", "uuid-two"]), (None, [])])
async def test_search_result_strings(redis_client, reply, expected):
    redis_client.execute_command.return_value = reply
    async with connections.VectorDbConnectionPool() as database:
        result = await database.search_uuids([1.0] * 768, 5)
    assert result == expected
    redis_client.execute_command.assert_awaited_once_with(
        "VSEARCH", connections.encode_vector([1.0] * 768), 5
    )


@pytest.mark.parametrize("value", ["stored text", None, ""])
async def test_plain_kv_returns_text_or_none(redis_client, value):
    redis_client.get.return_value = value
    async with connections.KvstoreConnectionPool() as database:
        assert await database.set("uuid-one", "text") is True
        assert await database.get("uuid-one") == value
    redis_client.get.assert_awaited_once_with("uuid-one")


@pytest.mark.parametrize("connection_class", [connections.VectorDbConnectionPool, connections.KvstoreConnectionPool])
@pytest.mark.parametrize("fail", [False, True])
async def test_pool_closes_on_context_exit(connection_class, fail):
    database = connection_class()
    database.pool.aclose = AsyncMock()
    if fail:
        with pytest.raises(RuntimeError, match="request failed"):
            async with database:
                raise RuntimeError("request failed")
    else:
        async with database as entered:
            assert entered is database
    database.pool.aclose.assert_awaited_once()