#!/usr/bin/env python3
"""Compare two kvstore vector engines directly over RESP using only the standard library."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import asdict, dataclass
import hashlib
import json
import math
from pathlib import Path
import random
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from typing import BinaryIO, Iterator


DIMENSION = 768
MAX_RECORDS = 100_000
UNFILTERED_THRESHOLD = 4.00001
VECTOR = struct.Struct(f"<II{DIMENSION}f")


def encode_command(*arguments: bytes) -> bytes:
    parts = [f"*{len(arguments)}\r\n".encode("ascii")]
    for argument in arguments:
        parts.extend((f"${len(argument)}\r\n".encode("ascii"), argument, b"\r\n"))
    return b"".join(parts)


@dataclass
class RespClient:
    connection: socket.socket
    stream: BinaryIO

    def read_reply(self) -> bytes | None:
        line = self.stream.readline()
        if not line.endswith(b"\r\n"):
            raise RuntimeError("Truncated RESP reply or closed server connection")
        prefix, payload = line[:1], line[1:-2]
        if prefix == b"+":
            return payload
        if prefix == b"-":
            raise RuntimeError(f"Server error: {payload.decode('utf-8', errors='replace')}")
        if prefix != b"$":
            raise RuntimeError(f"Unexpected RESP reply: {line!r}")
        length = int(payload)
        if length == -1:
            return None
        if length < 0:
            raise RuntimeError(f"Invalid RESP bulk length: {length}")
        body = self.stream.read(length + 2)
        if len(body) != length + 2 or not body.endswith(b"\r\n"):
            raise RuntimeError("Truncated RESP bulk reply")
        return body[:-2]

    def execute(self, command: bytes) -> bytes | None:
        self.connection.sendall(command)
        return self.read_reply()


@contextmanager
def connect(port: int, timeout: float) -> Iterator[RespClient]:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as connection:
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with connection.makefile("rb") as stream:
            yield RespClient(connection, stream)


@dataclass
class Workload:
    inserts: list[bytes]
    queries: list[bytes]
    keys: set[str]
    sha256: str


def random_vector_blob(generator: random.Random) -> bytes:
    return VECTOR.pack(1, DIMENSION, *(generator.gauss(0.0, 1.0) for _ in range(DIMENSION)))


def make_workload(records: int, queries: int, topk: int, threshold: float, seed: int) -> Workload:
    generator = random.Random(seed)
    digest = hashlib.sha256()

    inserts = []
    keys = set()
    for index in range(records):
        key = f"1e9a6754-d8b0-4c91-8e27-{index + 1:012x}"
        keys.add(key)
        command = encode_command(b"SET", key.encode("ascii"), random_vector_blob(generator))
        inserts.append(command)
        digest.update(command)
    query_commands = []
    for _ in range(queries):
        command = encode_command(
            b"VSEARCH", random_vector_blob(generator), str(topk).encode("ascii"), str(threshold).encode("ascii")
        )
        query_commands.append(command)
        digest.update(command)
    return Workload(inserts, query_commands, keys, digest.hexdigest())


def search_ids(reply: bytes | None, keys: set[str], topk: int) -> list[str]:
    if reply is None:
        return []
    ids = reply.decode("ascii").split(",")
    if len(ids) > topk or len(set(ids)) != len(ids) or any(key not in keys for key in ids):
        raise RuntimeError(f"Invalid search result (unknown/duplicate UUID or too many results): {ids}")
    return ids


def latency_summary(samples_ms: list[float]) -> dict[str, float]:
    if not samples_ms:
        raise ValueError("Latency statistics require at least one sample")
    ordered = sorted(samples_ms)

    def percentile(percent: float) -> float:
        position = (len(ordered) - 1) * percent / 100
        lower = math.floor(position)
        upper = math.ceil(position)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)

    return {
        "mean": statistics.fmean(ordered),
        "p50": percentile(50),
        "p95": percentile(95),
        "p99": percentile(99),
        "max": ordered[-1],
    }


def compare_results(expected: list[list[str]], actual: list[list[str]]) -> dict[str, float | int]:
    if not expected or len(expected) != len(actual):
        raise ValueError("Both engines must have the same nonzero query count")
    recalls = []
    overlaps = total_expected = empty = exact_sets = 0
    for truth, candidate in zip(expected, actual):
        if len(set(truth)) != len(truth) or len(set(candidate)) != len(candidate):
            raise ValueError("Recall inputs must contain unique IDs per query")
        exact_sets += set(truth) == set(candidate)
        if not truth:
            empty += 1
            continue
        overlap = len(set(truth) & set(candidate))
        overlaps += overlap
        total_expected += len(truth)
        recalls.append(overlap / len(truth))
    if not recalls:
        raise ValueError("All brute-force results are empty; increase --threshold (recall is undefined)")
    return {
        "recall_at_k": statistics.fmean(recalls),
        "minimum_query_recall": min(recalls),
        "micro_recall": overlaps / total_expected,
        "matched_neighbors": overlaps,
        "ground_truth_neighbors": total_expected,
        "evaluated_queries": len(recalls),
        "empty_ground_truth_queries": empty,
        "exact_set_match_rate": exact_sets / len(expected),
    }


def wait_for_server(process: subprocess.Popen[bytes], port: int, log_path: Path) -> None:
    deadline = time.monotonic() + 15
    last_error = ""
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Server exited with code {process.returncode}; see {log_path}")
        try:
            with connect(port, 0.5) as client:
                if client.execute(encode_command(b"PING")) != b"PONG":
                    raise RuntimeError(f"Unexpected server readiness reply; see {log_path}")
            return
        except OSError as error:
            last_error = str(error)
            time.sleep(0.05)
    raise RuntimeError(f"Server readiness timed out: {last_error}; see {log_path}")


@contextmanager
def server(binary: Path, args: argparse.Namespace, directory: Path) -> Iterator[int]:
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    config = directory / "kvstore.ini"
    config.write_text(
        f"[server]\nport = {port}\nlog_level = error\n"
        "[persistence]\nmode = none\n[replication]\nrole = standalone\n"
        f"[vectordb]\nm = {args.m}\nef_construction = {args.ef_construction}\nef = {args.ef}\n",
        encoding="ascii",
    )
    log_path = directory / "server.log"
    with tempfile.TemporaryDirectory(prefix="kvstore-vectordb-") as runtime, log_path.open("wb") as log:
        process = subprocess.Popen(
            [str(binary), "--config", str(config)], cwd=runtime, stdout=log, stderr=subprocess.STDOUT
        )
        try:
            wait_for_server(process, port, log_path)
            yield port
            if process.poll() is not None:
                raise RuntimeError(f"Server exited unexpectedly; see {log_path}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    print(f"WARNING: forcing server PID {process.pid} to stop; see {log_path}", file=sys.stderr)
                    process.kill()
                    process.wait(timeout=5)
            if process.returncode not in (0, -9):
                raise RuntimeError(f"Server exited with code {process.returncode}; see {log_path}")


@dataclass
class EngineResult:
    engine: str
    insert_seconds: float | None
    insert_qps: float | None
    query_seconds: float
    query_qps: float
    latency_ms: dict[str, float]
    samples_ms: list[float]
    results: list[list[str]]
    insert_latency_ms: dict[str, float] | None = None
    insert_samples_ms: list[float] | None = None


def print_table(rows: list[tuple[str, ...]]) -> None:
    widths = [max(len(row[column]) for row in rows) for column in range(len(rows[0]))]
    border = "+" + "+".join("-" * (width + 2) for width in widths) + "+"
    print(border)
    for index, row in enumerate(rows):
        cells = [f"{row[0]:<{widths[0]}}"]
        cells.extend(f"{value:>{width}}" for value, width in zip(row[1:], widths[1:]))
        print("| " + " | ".join(cells) + " |")
        if index == 0:
            print(border)
    print(border)


def print_performance_summary(results: list[EngineResult]) -> None:
    labels = {"bruteforce": "Brute force", "hnsw": "HNSW"}
    rows: list[tuple[str, ...]] = [("Engine", "Search QPS", "Mean latency", "P95", "P99")]
    for result in results:
        rows.append((
            labels[result.engine],
            f"{result.query_qps:.2f}",
            f"{result.latency_ms['mean']:.3f} ms",
            f"{result.latency_ms['p95']:.3f} ms",
            f"{result.latency_ms['p99']:.3f} ms",
        ))
    print_table(rows)
    print("Additional search metrics:")
    for result in results:
        print(
            f"  {labels[result.engine]}: P50={result.latency_ms['p50']:.3f} ms, "
            f"Max={result.latency_ms['max']:.3f} ms"
        )
    for result in results:
        if result.engine != "hnsw":
            continue
        if result.insert_qps is None or result.insert_latency_ms is None or result.insert_samples_ms is None:
            raise ValueError("HNSW insertion measurements are missing")
        latency = result.insert_latency_ms
        print("\nHNSW insertion (single-vector RTT, pipeline=1):")
        print_table([
            ("Engine", "Insert QPS", "Mean latency", "P95", "P99"),
            ("HNSW", f"{result.insert_qps:.2f}", f"{latency['mean']:.3f} ms",
             f"{latency['p95']:.3f} ms", f"{latency['p99']:.3f} ms"),
        ])
        print(
            f"  Samples={len(result.insert_samples_ms)}, P50={latency['p50']:.3f} ms, "
            f"Max={latency['max']:.3f} ms"
        )


def load_vectors(
    client: RespClient, commands: list[bytes], measure: bool
) -> tuple[float, list[float]] | None:
    if measure:
        samples_ms = []
        begin = time.perf_counter_ns()
        for index, command in enumerate(commands):
            insert_begin = time.perf_counter_ns()
            reply = client.execute(command)
            elapsed_ms = (time.perf_counter_ns() - insert_begin) / 1_000_000
            if reply != b"OK":
                raise RuntimeError(f"SET failed at record {index}")
            samples_ms.append(elapsed_ms)
        return (time.perf_counter_ns() - begin) / 1_000_000_000, samples_ms
    for offset in range(0, len(commands), 64):
        batch = commands[offset : offset + 64]
        client.connection.sendall(b"".join(batch))
        for index in range(len(batch)):
            if client.read_reply() != b"OK":
                raise RuntimeError(f"SET failed at record {offset + index}")
    return None


def run_engine(
    name: str, binary: Path, args: argparse.Namespace, workload: Workload, output: Path
) -> EngineResult:
    directory = output / name
    directory.mkdir()
    print(f"{name}: loading {len(workload.inserts):,} random vectors...", flush=True)
    insert_seconds = None
    insert_samples_ms = None
    with server(binary, args, directory) as port, connect(port, args.timeout) as client:
        insertion = load_vectors(client, workload.inserts, measure=name == "hnsw")
        if insertion is not None:
            insert_seconds, insert_samples_ms = insertion
            print(f"{name}: inserted {len(workload.inserts):,} vectors in {insert_seconds:.3f}s", flush=True)
        else:
            print(f"{name}: data loaded (insertion not measured)", flush=True)
        print(f"{name}: warming up {args.warmup} queries...", flush=True)
        for index in range(args.warmup):
            search_ids(client.execute(workload.queries[index % len(workload.queries)]), workload.keys, args.topk)
        samples_ms = []
        replies = []
        begin = time.perf_counter()
        for command in workload.queries:
            query_begin = time.perf_counter_ns()
            reply = client.execute(command)
            samples_ms.append((time.perf_counter_ns() - query_begin) / 1_000_000)
            replies.append(reply)
        query_seconds = time.perf_counter() - begin
    results = [search_ids(reply, workload.keys, args.topk) for reply in replies]
    if args.threshold >= UNFILTERED_THRESHOLD and any(len(ids) != args.topk for ids in results):
        raise RuntimeError(f"{name}: unfiltered queries did not all return TopK={args.topk} results")
    result = EngineResult(
        name,
        insert_seconds,
        len(workload.inserts) / insert_seconds if insert_seconds is not None else None,
        query_seconds,
        len(workload.queries) / query_seconds,
        latency_summary(samples_ms),
        samples_ms,
        results,
        insert_latency_ms=latency_summary(insert_samples_ms) if insert_samples_ms is not None else None,
        insert_samples_ms=insert_samples_ms,
    )
    (directory / "results.json").write_text(json.dumps(asdict(result), indent=2) + "\n", encoding="utf-8")
    return result


def add_workload_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--records", type=int, default=10_000, help="random vectors (10000..100000)")
    parser.add_argument("--queries", type=int, default=200, help="independent random queries per engine")
    parser.add_argument("--warmup", type=int, default=20, help="unmeasured warmup queries per engine")
    parser.add_argument("--topk", type=int, default=10, help="neighbors requested per query")
    parser.add_argument("--threshold", type=float, default=UNFILTERED_THRESHOLD, help="normalized squared-L2 cutoff")
    parser.add_argument("--seed", type=int, default=768, help="shared corpus/query random seed")
    parser.add_argument("--m", type=int, default=16, help="HNSW graph connectivity")
    parser.add_argument("--ef-construction", type=int, default=100, help="HNSW construction breadth")
    parser.add_argument("--ef", type=int, default=100, help="HNSW query breadth (at least TopK)")


def validate_workload_arguments(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if not 10_000 <= args.records <= MAX_RECORDS:
        parser.error("--records must be between 10000 and the server capacity of 100000")
    if args.queries < 1 or args.warmup < 0:
        parser.error("--queries must be positive and --warmup must be nonnegative")
    if not 1 <= args.topk <= min(args.records, 65535):
        parser.error("--topk must be between 1 and min(records, 65535)")
    if not math.isfinite(args.threshold) or args.threshold <= 1e-8:
        parser.error("--threshold must be finite and greater than 1e-8")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if not 2 <= args.m <= 10_000 or args.ef_construction < args.m or args.ef < args.topk:
        parser.error("require 2 <= M <= 10000, ef_construction >= M and ef >= TopK")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark kvstore directly with random 768-D vectors; no vectordb_service or Python packages.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--bruteforce-bin", type=Path, required=True, help="built ENABLE_BRUTEFORCE=ON server")
    parser.add_argument("--hnsw-bin", type=Path, required=True, help="built ENABLE_BRUTEFORCE=OFF server")
    add_workload_arguments(parser)
    parser.add_argument("--timeout", type=float, default=10, help="socket timeout in seconds")
    parser.add_argument(
        "--output-dir", type=Path,
        default=Path(__file__).resolve().parents[1] / "out/vectordb-benchmark/results",
        help="parent directory for unique run directories",
    )
    parser.add_argument("--check-args", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    validate_workload_arguments(parser, args)
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.check_args:
        return 0
    for binary in (args.bruteforce_bin, args.hnsw_bin):
        if not binary.is_file():
            raise ValueError(f"Missing server binary: {binary}; run benchmark.sh to build both engines")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output_dir.resolve()))
    print(f"Results and server logs: {output}", flush=True)
    print(
        f"Random Gaussian vectors: records={args.records:,}, dimension={DIMENSION}, "
        f"queries={args.queries}, TopK={args.topk}, threshold={args.threshold}, seed={args.seed}\n"
        f"HNSW: M={args.m}, ef_construction={args.ef_construction}, ef={args.ef}; "
        "single client, query pipeline=1, persistence=none",
        flush=True,
    )
    begin = time.perf_counter()
    workload = make_workload(args.records, args.queries, args.topk, args.threshold, args.seed)
    print(f"Shared workload generated in {time.perf_counter() - begin:.3f}s (SHA256={workload.sha256})", flush=True)
    brute = run_engine("bruteforce", args.bruteforce_bin.resolve(), args, workload, output)
    hnsw = run_engine("hnsw", args.hnsw_bin.resolve(), args, workload, output)
    recall = compare_results(brute.results, hnsw.results)
    config = {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}
    summary = {
        "config": config,
        "dimension": DIMENSION,
        "workload_sha256": workload.sha256,
        "measurement": "single-client sequential RESP RTT; search excludes generation, loading, warmup and shutdown",
        "insertion_measurement": "HNSW only, sequential SET-to-OK RTT, pipeline=1; all inserted vectors measured",
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
    print("\n=== Vector benchmark summary ===")
    print(f"HNSW Recall@{args.topk}: {recall['recall_at_k']:.2%} (brute-force ground truth)")
    print(
        f"Matched neighbors: {recall['matched_neighbors']}/{recall['ground_truth_neighbors']}; "
        f"evaluated queries: {recall['evaluated_queries']}/{args.queries}; "
        f"empty ground truth: {recall['empty_ground_truth_queries']}"
    )
    print(f"Exact result-set match: {recall['exact_set_match_rate']:.2%}")
    print_performance_summary([brute, hnsw])
    print(f"Search QPS speedup (HNSW / brute-force): {hnsw.query_qps / brute.query_qps:.2f}x")
    print(f"Benchmark time (excluding builds): {summary['benchmark_seconds']:.3f}s")
    print(f"Full report: {report}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("ERROR: benchmark interrupted; owned server processes have been stopped", file=sys.stderr)
        sys.exit(130)
