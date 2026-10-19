# Qdrant recall, QPS and latency benchmark

## Run

From the kvstore repository, run:

```bash
bash qdrant_test/benchmark.sh
bash qdrant_test/benchmark.sh --m 16 --ef-construction 200 --ef 100
```

The script starts a uniquely named temporary Qdrant container using
`sudo docker`, waits for it to become ready, runs the benchmark, then removes
only that container, including on error or interruption. It does not use a
volume: Qdrant data is ephemeral; JSON benchmark reports remain under
`out/qdrant-benchmark/results/`. The image is pinned to `qdrant/qdrant:v1.19.2`
by default; set `QDRANT_IMAGE` to override it. Docker and `curl` must be
installed, and the caller must be able to run `sudo docker`.

The shell script also creates an isolated virtual environment under
`out/qdrant-benchmark/venv` and installs the client dependency if needed.
Override the environment with `QDRANT_VENV_DIR`; `PYTHON` selects the Python
3.10+ interpreter used to create it.

HTTP and gRPC ports are automatically assigned on `127.0.0.1`, avoiding
conflicts with existing Qdrant instances. The script checks both endpoints
before creating a collection. The container port mappings can be inspected
during execution with `sudo docker ps`. Use `--help` to show benchmark options;
`--host`, `--http-port` and `--grpc-port` are managed by the shell script.

Running `qdrant_test/benchmark.py` directly is still possible when connecting
to an already running Qdrant service; that entry point accepts `--host`,
`--http-port` and `--grpc-port` and does not manage Docker.

## Same workload and output as kvstore

The benchmark reuses the random-vector generator, argument validation, recall
calculation, latency percentiles and English ASCII tables from
[vectordb_test](../vectordb_test/README.md). It has no dependency on
`vectordb_service`, embeddings or downloaded datasets.

Defaults match the kvstore test:

- 10,000 random Gaussian vectors, dimension 768, seed 768.
- 200 independent random queries, 20 unmeasured warmup queries per search mode.
- TopK=10, squared-L2 threshold=4.00001.
- HNSW M=16, ef_construction=100, ef=100.
- One synchronous client, pipeline depth 1.

Use the same `--records`, `--queries`, `--warmup`, `--seed`, `--topk`,
`--threshold`, `--m`, `--ef-construction` and `--ef` options for both benchmarks.
The supported corpus range is 10,000..100,000 records. For example:

```bash
bash qdrant_test/benchmark.sh --queries 500 --topk 20 --ef 200
```

The raw float32 vectors, UUIDs, query order and workload SHA256 match kvstore for
the same arguments. Unlike kvstore, Qdrant's EUCLID metric does not normalize
vectors automatically: the script normalizes both records and queries before
sending them. Normalization uses float32 output and can differ in the last bits
from kvstore's C++ float accumulation.

**Threshold semantics matter:** kvstore uses squared L2, but Qdrant EUCLID scores
are L2 distances. The script sends `score_threshold=sqrt(threshold)` to Qdrant.
The default admits the entire unit-vector distance range, so both modes must
return exactly K IDs. Tighter thresholds use the actual number of exact matches
as the recall denominator; empty exact queries are excluded, not counted as
100% recall. All-empty exact results fail explicitly.

## Test procedure

1. Create a uniquely named temporary collection, with EUCLID vectors, one shard,
   no quantization, and the requested `m` and `ef_construct`.
2. Insert every vector with its UUID using a separate `upsert(wait=True)`. Measure
   each call's round-trip latency and total insertion QPS; no batch averaging.
3. Wait for a healthy collection with **all records indexed**, checking the exact
   point count as well. `indexing_threshold=1` KiB triggers indexing for this
   small benchmark; `full_scan_threshold=10` KiB (the server minimum) keeps the
   roughly 30 MiB corpus above the planner's full-scan cutoff. A timeout
   (`--index-timeout`, default 120 seconds) is an error, not an
   excuse to report an unindexed scan as HNSW.
4. Warm up and measure exact searches with `SearchParams(exact=True)`.
5. Warm up and measure HNSW searches on the **same collection** with
   `SearchParams(exact=False, hnsw_ef=ef)`.
6. Compare result IDs to compute Recall@K, print search and HNSW-only insertion
   tables, and delete only the collection created by this run.

Qdrant does not need two builds or two data copies: per-request `exact=True`
provides the brute-force ground truth. This tests a real remote server over
gRPC, never the client's local/in-memory mode (which only does exact search).
Existing collections, including one named `benchmark`, are not touched. The
generated collection name is printed so failed cleanup can be investigated.

## Interpreting the metrics

The terminal prints the same Search QPS / Mean latency / P95 / P99 table and the
same HNSW Insert QPS / Mean latency / P95 / P99 table as kvstore. Recall, matching
neighbor counts, exact-set match rate, P50/max latency and speedup are retained.
These are client-observed synchronous SDK round trips, not multi-client peak QPS.
Insertion throughput also includes per-request point-model preparation.

**Insertion acknowledgement is not index completion:** Qdrant builds HNSW
asynchronously, while kvstore builds it during `SET`. `wait=True` means the
update has been applied, not that the HNSW graph is complete. The additional
index-readiness wait is measured and printed separately, outside search timing.
Qdrant also normally uses a WAL, whereas the kvstore benchmark disables
persistence. gRPC/SDK overhead and server parallelism differ from RESP/kvstore.
Do not interpret the resulting difference as an algorithm-only comparison.

Results are saved to a unique directory under `out/qdrant-benchmark/results/`:

- `summary.json`: versions, parameters, raw-workload hash, collection/index
  state, indexing wait, recall and aggregate performance.
- `bruteforce/results.json`, `hnsw/results.json`: result UUIDs and per-query
  latencies; the HNSW report also includes every insertion latency sample.

To run the focused tests after dependency installation:

```bash
out/qdrant-benchmark/venv/bin/python -m unittest \
  vectordb_test.test_benchmark qdrant_test.test_benchmark
```
