#!/usr/bin/env python3
"""Benchmark a running Qdrant server using the kvstore vector workload and tables."""

from __future__ import annotations

import argparse
from array import array
from dataclasses import asdict, dataclass
import hashlib
from importlib.metadata import version
import json
import math
from pathlib import Path
import random
import socket
import sys
import tempfile
import time
import uuid

from grpc import RpcError
from qdrant_client import QdrantClient
from qdrant_client.http import models
from qdrant_client.http.exceptions import ResponseHandlingException, UnexpectedResponse

from vectordb_test.benchmark import (
    DIMENSION,
    UNFILTERED_THRESHOLD,
    VECTOR,
    EngineResult,
    add_workload_arguments,
    compare_results,
    encode_command,
    latency_summary,
    print_performance_summary,
    random_vector_blob,
    validate_workload_arguments,
)


@dataclass
class Workload:
    keys: list[str]
    vectors: list[array[float]]
    queries: list[list[float]]
    sha256: str


def normalized_vector(blob: bytes) -> array[float]:
    components = VECTOR.unpack(blob)[2:]
    norm = math.sqrt(math.fsum(value * value for value in components))
    if not math.isfinite(norm) or norm <= 0:
        raise ValueError("Random workload contains an invalid vector")
    return array("f", (value / norm for value in components))


def make_workload(args: argparse.Namespace) -> Workload:
    generator = random.Random(args.seed)
    digest = hashlib.sha256()
    keys = []
    vectors = []
    for index in range(args.records):
        key = f"1e9a6754-d8b0-4c91-8e27-{index + 1:012x}"
        blob = random_vector_blob(generator)
        digest.update(encode_command(b"SET", key.encode("ascii"), blob))
        keys.append(key)
        vectors.append(normalized_vector(blob))
    queries = []
    for _ in range(args.queries):
        blob = random_vector_blob(generator)
        digest.update(encode_command(
            b"VSEARCH", blob, str(args.topk).encode("ascii"), str(args.threshold).encode("ascii")
        ))
        queries.append(normalized_vector(blob).tolist())
    return Workload(keys, vectors, queries, digest.hexdigest())


def insert_vectors(
    client: QdrantClient, collection: str, workload: Workload
) -> tuple[float, list[float]]:
    samples_ms = []
    begin = time.perf_counter()
    for index, (key, vector) in enumerate(zip(workload.keys, workload.vectors)):
        point = models.PointStruct(id=key, vector=vector.tolist())
        insert_begin = time.perf_counter_ns()
        reply = client.upsert(collection_name=collection, points=[point], wait=True)
        elapsed_ms = (time.perf_counter_ns() - insert_begin) / 1_000_000
        if reply.status != models.UpdateStatus.COMPLETED:
            raise RuntimeError(f"Qdrant upsert was not completed at record {index}: {reply.status}")
        samples_ms.append(elapsed_ms)
    return time.perf_counter() - begin, samples_ms


def wait_for_index(
    client: QdrantClient, collection: str, records: int, timeout: float
) -> tuple[float, models.CollectionInfo]:
    begin = time.perf_counter()
    while True:
        info = client.get_collection(collection)
        if info.optimizer_status != "ok" or info.status == models.CollectionStatus.RED:
            raise RuntimeError(f"Qdrant optimizer failed: {info.optimizer_status}; status={info.status}")
        if (
            info.status == models.CollectionStatus.GREEN
            and info.points_count == records
            and info.indexed_vectors_count == records
        ):
            actual = client.count(collection, exact=True).count
            if actual != records:
                raise RuntimeError(f"Qdrant contains {actual} points, expected {records}")
            return time.perf_counter() - begin, info
        if time.perf_counter() - begin >= timeout:
            raise RuntimeError(
                f"HNSW indexing timed out after {timeout}s: points={info.points_count}, "
                f"indexed={info.indexed_vectors_count}, expected={records}, status={info.status}. "
                "Refusing to report a plain scan as HNSW."
            )
        time.sleep(0.25)


def result_ids(reply: models.QueryResponse, keys: set[str], topk: int) -> list[str]:
    ids = [str(point.id) for point in reply.points]
    if len(ids) > topk or len(set(ids)) != len(ids) or any(key not in keys for key in ids):
        raise RuntimeError(f"Invalid Qdrant search IDs: {ids}")
    if any(not math.isfinite(point.score) or point.score < 0 for point in reply.points):
        raise RuntimeError("Qdrant returned an invalid Euclidean distance")
    return ids


def run_search(
    client: QdrantClient, collection: str, args: argparse.Namespace, workload: Workload, exact: bool
) -> EngineResult:
    name = "bruteforce" if exact else "hnsw"
    params = models.SearchParams(exact=exact, hnsw_ef=args.ef)
    keys = set(workload.keys)
    # Qdrant EUCLID returns L2, while kvstore's threshold is squared L2.
    score_threshold = math.sqrt(args.threshold)

    def query(vector: list[float]) -> models.QueryResponse:
        return client.query_points(
            collection_name=collection,
            query=vector,
            search_params=params,
            limit=args.topk,
            score_threshold=score_threshold,
            with_payload=False,
            with_vectors=False,
        )

    print(f"{name}: warming up {args.warmup} queries...", flush=True)
    for index in range(args.warmup):
        result_ids(query(workload.queries[index % len(workload.queries)]), keys, args.topk)
    samples_ms = []
    replies = []
    begin = time.perf_counter()
    for vector in workload.queries:
        query_begin = time.perf_counter_ns()
        reply = query(vector)
        samples_ms.append((time.perf_counter_ns() - query_begin) / 1_000_000)
        replies.append(reply)
    elapsed = time.perf_counter() - begin
    results = [result_ids(reply, keys, args.topk) for reply in replies]
    if args.threshold >= UNFILTERED_THRESHOLD and any(len(ids) != args.topk for ids in results):
        raise RuntimeError(f"{name}: unfiltered queries did not all return TopK={args.topk} results")
    return EngineResult(
        engine=name,
        insert_seconds=None,
        insert_qps=None,
        query_seconds=elapsed,
        query_qps=len(workload.queries) / elapsed,
        latency_ms=latency_summary(samples_ms),
        samples_ms=samples_ms,
        results=results,
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark Qdrant over gRPC with the same random workload and tables as kvstore.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    add_workload_arguments(parser)
    parser.add_argument("--host", default="127.0.0.1", help="running Qdrant host")
    parser.add_argument("--http-port", type=int, default=6333, help="Qdrant HTTP port")
    parser.add_argument("--grpc-port", type=int, default=6334, help="Qdrant gRPC port")
    parser.add_argument("--timeout", type=int, default=10, help="request timeout in seconds")
    parser.add_argument("--index-timeout", type=float, default=120, help="maximum indexing wait in seconds")
    parser.add_argument(
        "--output-dir", type=Path,
        default=Path(__file__).resolve().parents[1] / "out/qdrant-benchmark/results",
        help="parent directory for unique run directories",
    )
    parser.add_argument("--check-args", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    validate_workload_arguments(parser, args)
    if not 1 <= args.http_port <= 65535 or not 1 <= args.grpc_port <= 65535:
        parser.error("HTTP and gRPC ports must be between 1 and 65535")
    if not math.isfinite(args.index_timeout) or args.index_timeout <= 0:
        parser.error("--index-timeout must be finite and positive")
    return args


def check_tcp_endpoint(host: str, port: int, protocol: str, timeout: int) -> None:
    try:
        with socket.create_connection((host, port), timeout=min(timeout, 3)):
            pass
    except OSError as error:
        protocol_name = "gRPC" if protocol.lower() == "grpc" else protocol.upper()
        hint = (
            f"Start Qdrant with both Docker ports published, e.g. "
            f"`-p 127.0.0.1:6333:6333 -p 127.0.0.1:6334:6334`. "
            f"Check `sudo docker ps` and `sudo docker port qdrant {port}/tcp`; "
            f"if the host port differs, pass `--{protocol}-port <host-port>`."
        )
        raise RuntimeError(
            f"Cannot connect to Qdrant {protocol_name} endpoint {host}:{port}: {error}. {hint}"
        ) from error


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.check_args:
        return 0
    print(
        f"Random Gaussian vectors: records={args.records:,}, dimension={DIMENSION}, "
        f"queries={args.queries}, TopK={args.topk}, threshold={args.threshold}, seed={args.seed}\n"
        f"HNSW: M={args.m}, ef_construction={args.ef_construction}, ef={args.ef}; "
        f"gRPC={args.host}:{args.grpc_port}, single client, pipeline=1",
        flush=True,
    )
    begin = time.perf_counter()
    client = QdrantClient(
        host=args.host, port=args.http_port, grpc_port=args.grpc_port, prefer_grpc=True, timeout=args.timeout
    )
    created = False
    try:
        check_tcp_endpoint(args.host, args.http_port, "http", args.timeout)
        check_tcp_endpoint(args.host, args.grpc_port, "grpc", args.timeout)
        server_info = client.info()
        args.output_dir.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output_dir.resolve()))
        collection = f"kvstore_bench_{uuid.uuid4().hex}"
        print(f"Results: {output}\nTemporary collection: {collection}", flush=True)
        if not client.create_collection(
            collection_name=collection,
            vectors_config=models.VectorParams(size=DIMENSION, distance=models.Distance.EUCLID, on_disk=False),
            hnsw_config=models.HnswConfigDiff(
                m=args.m, ef_construct=args.ef_construction, full_scan_threshold=10, on_disk=False
            ),
            optimizers_config=models.OptimizersConfigDiff(indexing_threshold=1, default_segment_number=1),
            shard_number=1,
            replication_factor=1,
            on_disk_payload=False,
        ):
            raise RuntimeError(f"Failed to create benchmark collection {collection}")
        created = True
        generation_begin = time.perf_counter()
        workload = make_workload(args)
        print(
            f"Shared workload generated in {time.perf_counter() - generation_begin:.3f}s "
            f"(SHA256={workload.sha256})",
            flush=True,
        )
        print(f"hnsw: inserting {args.records:,} normalized vectors (one upsert per request, wait=True)...", flush=True)
        insert_seconds, insert_samples_ms = insert_vectors(client, collection, workload)
        print(f"hnsw: inserted in {insert_seconds:.3f}s; waiting for HNSW indexing...", flush=True)
        index_seconds, collection_info = wait_for_index(client, collection, args.records, args.index_timeout)
        print(f"HNSW index ready: {collection_info.indexed_vectors_count}/{args.records}; waited {index_seconds:.3f}s", flush=True)
        brute = run_search(client, collection, args, workload, exact=True)
        hnsw = run_search(client, collection, args, workload, exact=False)
        hnsw.insert_seconds = insert_seconds
        hnsw.insert_qps = args.records / insert_seconds
        hnsw.insert_latency_ms = latency_summary(insert_samples_ms)
        hnsw.insert_samples_ms = insert_samples_ms
        recall = compare_results(brute.results, hnsw.results)
        for result in (brute, hnsw):
            directory = output / result.engine
            directory.mkdir()
            (directory / "results.json").write_text(json.dumps(asdict(result), indent=2) + "\n", encoding="utf-8")
    finally:
        try:
            if created and not client.delete_collection(collection):
                raise RuntimeError(f"Failed to delete owned benchmark collection {collection}")
        finally:
            client.close()
    summary = {
        "backend": "qdrant",
        "config": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "client_version": version("qdrant-client"),
        "server": server_info.model_dump(mode="json"),
        "dimension": DIMENSION,
        "workload_sha256": workload.sha256,
        "measurement": "single-client sequential gRPC SDK RTT; excludes data generation, loading, indexing and warmup",
        "insertion_measurement": "single-point upsert(wait=True) RTT; total QPS includes SDK point preparation; asynchronous index readiness timed separately",
        "distance": "normalized float32 EUCLID; Qdrant score_threshold=sqrt(kvstore squared-L2 threshold)",
        "collection": collection,
        "collection_deleted": True,
        "index_wait_seconds": index_seconds,
        "collection_info_before_queries": collection_info.model_dump(mode="json"),
        "recall": recall,
        "engines": {
            result.engine: {
                key: value for key, value in asdict(result).items()
                if key not in ("results", "samples_ms", "insert_samples_ms")
            }
            for result in (brute, hnsw)
        },
        "benchmark_seconds": time.perf_counter() - begin,
    }
    report = output / "summary.json"
    report.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("\n=== Qdrant vector benchmark summary ===")
    print(f"HNSW Recall@{args.topk}: {recall['recall_at_k']:.2%} (brute-force ground truth)")
    print(
        f"Matched neighbors: {recall['matched_neighbors']}/{recall['ground_truth_neighbors']}; "
        f"evaluated queries: {recall['evaluated_queries']}/{args.queries}; "
        f"empty ground truth: {recall['empty_ground_truth_queries']}"
    )
    print(f"Exact result-set match: {recall['exact_set_match_rate']:.2%}")
    print_performance_summary([brute, hnsw])
    print("Insertion timing is upsert acknowledgement, not synchronous HNSW construction.")
    print(f"HNSW index ready: {args.records}/{args.records}; additional wait: {index_seconds:.3f}s")
    print(f"Search QPS speedup (HNSW / brute-force): {hnsw.query_qps / brute.query_qps:.2f}x")
    print(f"Benchmark time: {summary['benchmark_seconds']:.3f}s")
    print(f"Temporary collection deleted: {collection}")
    print(f"Full report: {report}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, RpcError, UnexpectedResponse, ResponseHandlingException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("ERROR: benchmark interrupted; owned collection cleanup was attempted", file=sys.stderr)
        sys.exit(130)
