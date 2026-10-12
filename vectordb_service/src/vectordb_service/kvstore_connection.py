import asyncio
import redis.asyncio as redis
import struct


def encode_vector(embedding):
    if len(embedding) != 768:
        raise ValueError("embedding must be 768 dimension")
    return struct.pack("<II768f", 1, 768, *embedding)


class VectorDbConnectionPool:

    def __init__(self, max_connection=20, host="127.0.0.1", port=8050, timeout=5):
        self.pool = redis.BlockingConnectionPool(
            host=host,
            port=port,
            max_connections=max_connection,
            timeout=timeout,
            decode_responses=True,
        )

    async def insert_vector(self, record_uuid, embedding):
        async with redis.Redis(connection_pool=self.pool) as db:
            return await db.set(str(record_uuid), encode_vector(embedding))

    async def search_uuids(self, embedding, k=5) -> list[str]:
        async with redis.Redis(connection_pool=self.pool) as db:
            result = await db.execute_command("VSEARCH", encode_vector(embedding), k)
            return result.split(",") if result else []

    async def aclose(self):
        await self.pool.aclose()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc_value, traceback):
        await self.aclose()


class KvstoreConnectionPool:

    def __init__(self, max_connection=20, host="127.0.0.1", port=8060, timeout=5):
        self.pool = redis.BlockingConnectionPool(
            host=host,
            port=port,
            max_connections=max_connection,
            timeout=timeout,
            decode_responses=True,
        )

    async def set(self, key: str, value: str):
        async with redis.Redis(connection_pool=self.pool) as db:
            return await db.set(str(key), str(value))

    async def get(self, key: str):
        async with redis.Redis(connection_pool=self.pool) as db:
            return await db.get(key)

    async def aclose(self):
        await self.pool.aclose()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc_value, traceback):
        await self.aclose()
