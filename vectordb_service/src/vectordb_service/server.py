from . import vectordb_pb2, vectordb_pb2_grpc

from .embedding import DEFAULT_MODEL, EmbeddingLayer
from .kvstore_connection import (
    VectorDbConnectionPool,
    KvstoreConnectionPool,
)

from redis.exceptions import ConnectionError, RedisError, TimeoutError

import uuid
import grpc
import asyncio
import math


class Rag(vectordb_pb2_grpc.RagServicer):

    def __init__(
        self,
        vectordb_host: str,
        vectordb_port: int,
        vectordb_max_connections: int,
        vectordb_timeout: int,
        kvstore_host: str,
        kvstore_port: int,
        kvstore_max_connections: int,
        kvstore_timeout: int,
        *,
        embedding_layer=None,
        model_name=DEFAULT_MODEL,
    ):
        super().__init__()

        self.embedding_layer = (
            embedding_layer
            if embedding_layer is not None
            else EmbeddingLayer(model_name)
        )

        self.vectordb = VectorDbConnectionPool(
            host=vectordb_host,
            port=vectordb_port,
            max_connection=vectordb_max_connections,
            timeout=vectordb_timeout,
        )

        self.kvstore = KvstoreConnectionPool(
            host=kvstore_host,
            port=kvstore_port,
            max_connection=kvstore_max_connections,
            timeout=kvstore_timeout,
        )

    async def aclose(self):
        await asyncio.gather(self.vectordb.aclose(), self.kvstore.aclose())

    async def Vset(self, request, context) -> vectordb_pb2.VsetResponse:
        record_uuid = str(uuid.uuid4())
        embedding = self.embedding_layer.encode(request.text)

        try:
            vector_written = await self.vectordb.insert_vector(
                record_uuid, embedding.tolist()
            )
            if not vector_written:
                await context.abort(grpc.StatusCode.INTERNAL, "Vector write failed")

        except (ConnectionError, TimeoutError):
            await context.abort(grpc.StatusCode.UNAVAILABLE, "Storage unavailable")
        except RedisError:
            await context.abort(grpc.StatusCode.INTERNAL, "Storage write failed")

        return vectordb_pb2.VsetResponse(uuid=record_uuid)

    async def Vsearch(self, request, context) -> vectordb_pb2.VsearchResponse:
        if not 1 <= request.topk <= 65535:
            await context.abort(
                grpc.StatusCode.INVALID_ARGUMENT, "topk must be between 1 and 65535"
            )

        if not math.isfinite(request.threshold):
            await context.abort(
                grpc.StatusCode.INVALID_ARGUMENT, "threshold must be a finite float"
            )

        embedding = self.embedding_layer.encode(request.text)
        try:
            uuids = await self.vectordb.search_uuids(
                embedding.tolist(), threshold=request.threshold, k=request.topk
            )

            if not uuids:
                return vectordb_pb2.VsearchResponse(uuids=[])

            return vectordb_pb2.VsearchResponse(uuids=uuids)
        except (ConnectionError, TimeoutError):
            await context.abort(grpc.StatusCode.UNAVAILABLE, "Storage unavailable")
        except RedisError:
            await context.abort(grpc.StatusCode.INTERNAL, "Storage read failed")

    async def Set(self, request, context) -> vectordb_pb2.SetResponse:
        try:
            text_written = await self.kvstore.set(request.uuid, request.text)
            if not text_written:
                await context.abort(grpc.StatusCode.INTERNAL, "Text write failed")
        except (ConnectionError, TimeoutError):
            await context.abort(grpc.StatusCode.UNAVAILABLE, "Storage unavailable")
        except RedisError:
            await context.abort(grpc.StatusCode.INTERNAL, "Storage write failed")

        return vectordb_pb2.SetResponse(ok=True)

    async def Get(self, request, context) -> vectordb_pb2.GetResponse:
        try:
            text = await self.kvstore.get(request.uuid)
            if text is None:
                await context.abort(grpc.StatusCode.NOT_FOUND, "Text not found")
        except (ConnectionError, TimeoutError):
            await context.abort(grpc.StatusCode.UNAVAILABLE, "Storage unavailable")
        except RedisError:
            await context.abort(grpc.StatusCode.INTERNAL, "Storage read failed")

        return vectordb_pb2.GetResponse(text=text)
