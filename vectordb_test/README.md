# Vector recall and performance benchmark

Run from the repository root:

```bash
bash vectordb_test/benchmark.sh
```

Requires Bash, Python 3.10+ (standard library only), CMake and the normal kvstore
build dependencies. There is **no dependency on `vectordb_service`**, Redis,
embeddings, gRPC, NumPy, or downloaded datasets.

The shell script builds two separate Release servers with `ENABLE_RAG=ON`:
`ENABLE_BRUTEFORCE=ON` for exact ground truth and `ENABLE_BRUTEFORCE=OFF` for HNSW.
Other build settings are identical. Builds are cached under
`out/vectordb-benchmark/`; only the server target is built, with two build jobs by
default. Set `BUILD_JOBS=4` or `BUILD_ROOT=/absolute/path` to override these.

## Workload and defaults

- **10,000 records**, each a random 768-dimensional Gaussian vector.
- **200 independent random queries**, not vectors sampled from the inserted
  corpus. Both engines receive exactly the same bytes in the same order.
- Fixed random seed `768`; the workload SHA256 is recorded for reproducibility.
- **TopK=10**, with HNSW `M=16`, `ef_construction=100`, `ef=100`. The construction
  breadth keeps this a short smoke benchmark rather than a long indexing test.
- **Threshold=4.00001**. The server normalizes vectors and applies a **squared L2
  distance** cutoff, not cosine similarity. Unit-vector squared L2 distances are
  in `[0, 4]`; the small margin avoids floating-point boundary effects. This
  default prevents threshold filtering from disguising missing neighbors.
- 20 unmeasured warmup queries per engine. Insertion and data generation are not
  included in search measurements.
- Each server runs sequentially in its own fresh temporary working directory,
  with persistence and replication disabled and an automatically selected port.
  The script only stops server processes it starts; no existing server or data
  directory is used.

This is a challenging high-dimensional random workload, not a claim about recall
on a particular real-world embedding distribution. Increasing `ef` usually
improves recall at the cost of search throughput.

## Metrics

At completion, an English ASCII table is printed directly to the terminal with
Engine, Search QPS, Mean latency, P95 and P99 columns. Latencies include an explicit
`ms` unit. A separate **HNSW-only insertion table** reports Insert QPS and
mean/P95/P99 insertion latency. P50/max latency, insertion sample count and
HNSW/brute-force search speedup are also printed. All metrics are saved in the
JSON reports.

Search uses one persistent client connection with **pipeline depth 1**. Latency is
client-observed round-trip time, including server work, transport and RESP reply
parsing. QPS is measured query count divided by elapsed time for that query loop;
it is **not a saturated multi-client maximum**.

HNSW insertion also uses one connection with **pipeline depth 1**. Every inserted
vector is measured from sending its `SET` to receiving its `OK` reply; these are
actual per-vector round-trip latencies, not batch latency divided by batch size.
Insert QPS is the number of successfully inserted vectors divided by the total
insertion-loop duration, including growth of the index. There is no insertion
warmup, and data generation, startup and queries are excluded.

Brute-force data is still loaded in batches of 64 to establish the same recall
ground truth, but its insertion performance is **not measured or printed**.
Its insertion fields in JSON are `null`. This does not change either engine's
search measurements.

Recall@K is the mean of
`|HNSW result IDs intersect brute-force result IDs| / |brute-force result IDs|`
over queries with nonempty exact results. With the default threshold, the
denominator is exactly K and both engines must return K IDs. Result order does
not affect recall. Exact result-set match rate and matched/expected neighbor
counts are also printed.

For a tighter threshold, the denominator is the actual number of exact matches,
which can be less than K. Empty ground-truth queries are counted and excluded
from recall, **not treated as 100% hits**. If all exact results are empty, the
script fails explicitly rather than reporting a misleading recall.

## Options and output

```bash
# A slightly longer measurement, or tune the recall/performance tradeoff:
bash vectordb_test/benchmark.sh --queries 500 --topk 20 --ef 200

# More data (the script enforces 10000 <= records <= 100000):
BUILD_JOBS=4 bash vectordb_test/benchmark.sh --records 20000

bash vectordb_test/benchmark.sh --help
```

Default runs are deliberately small; increasing records, query count, M or
construction breadth increases runtime. First-run compilation is additional;
later runs reuse incremental builds.

Each run keeps a unique directory under `out/vectordb-benchmark/results/`:

- `summary.json`: workload settings, recall and per-engine performance metrics.
- `bruteforce/results.json`, `hnsw/results.json`: ordered result IDs and individual
  query latencies, so the comparison can be inspected independently. The HNSW
  report additionally contains one insertion latency sample per inserted vector
  and its aggregate insertion statistics.
- Per-engine configuration and server logs.

Build failures print the end of the build log and its full path. Protocol errors,
invalid result IDs, missing unfiltered neighbors and server startup failures
abort the benchmark. Temporary server working directories are cleaned up after
the child processes stop, including on interruption.

Helper tests:

```bash
python3 -m unittest vectordb_test.test_benchmark
```
