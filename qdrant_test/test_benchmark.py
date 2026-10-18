from contextlib import redirect_stdout
import io
import math
import socket
import tempfile
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

from qdrant_client import QdrantClient
from qdrant_client.http import models

from qdrant_test import benchmark
from vectordb_test import benchmark as kvstore


class QdrantBenchmarkTests(unittest.TestCase):
    def workload(self):
        args = benchmark.parse_args([])
        args.records, args.queries, args.topk = 4, 2, 2
        return benchmark.make_workload(args)

    def point(self, key, score=0.5):
        return models.ScoredPoint(id=key, version=0, score=score)

    def collection_info(self, points=10000, indexed=10000, status=models.CollectionStatus.GREEN):
        return models.CollectionInfo.model_construct(
            points_count=points, indexed_vectors_count=indexed, status=status, optimizer_status="ok"
        )

    def test_workload_matches_kvstore_and_is_normalized(self):
        workload = self.workload()
        reference = kvstore.make_workload(4, 2, 2, kvstore.UNFILTERED_THRESHOLD, 768)
        self.assertEqual(workload.sha256, reference.sha256)
        self.assertEqual(set(workload.keys), reference.keys)
        for vector in workload.vectors + workload.queries:
            self.assertEqual(len(vector), 768)
            self.assertAlmostEqual(math.fsum(value * value for value in vector), 1.0, places=6)
        self.assertTrue(all(query not in [vector.tolist() for vector in workload.vectors] for query in workload.queries))

    def test_insertion_is_single_point_wait_true_and_individually_timed(self):
        workload = self.workload()
        workload.keys = workload.keys[:2]
        workload.vectors = workload.vectors[:2]
        client = Mock(spec=QdrantClient)
        client.upsert.return_value = models.UpdateResult(operation_id=1, status=models.UpdateStatus.COMPLETED)
        with (
            patch.object(benchmark.time, "perf_counter", side_effect=[1.0, 1.25]),
            patch.object(benchmark.time, "perf_counter_ns", side_effect=[1_000_000, 3_000_000, 4_000_000, 10_000_000]),
        ):
            elapsed, samples = benchmark.insert_vectors(client, "owned", workload)
        self.assertEqual((elapsed, samples), (0.25, [2.0, 6.0]))
        self.assertEqual(client.upsert.call_count, 2)
        for index, call in enumerate(client.upsert.call_args_list):
            self.assertTrue(call.kwargs["wait"])
            self.assertEqual(call.kwargs["collection_name"], "owned")
            self.assertEqual(len(call.kwargs["points"]), 1)
            self.assertEqual(call.kwargs["points"][0].id, workload.keys[index])
        client.upsert.return_value = models.UpdateResult(operation_id=1, status=models.UpdateStatus.ACKNOWLEDGED)
        with self.assertRaisesRegex(RuntimeError, "not completed"):
            benchmark.insert_vectors(client, "owned", workload)

    def test_wait_requires_all_vectors_indexed_and_checks_exact_point_count(self):
        client = Mock(spec=QdrantClient)
        client.get_collection.side_effect = [self.collection_info(indexed=9999), self.collection_info()]
        client.count.return_value = models.CountResult(count=10000)
        with patch.object(benchmark.time, "sleep") as sleep:
            _, info = benchmark.wait_for_index(client, "owned", 10000, 10)
        self.assertEqual(info.indexed_vectors_count, 10000)
        sleep.assert_called_once_with(0.25)
        client.count.assert_called_once_with("owned", exact=True)

    def test_unindexed_timeout_and_optimizer_errors_do_not_fall_back(self):
        client = Mock(spec=QdrantClient)
        client.get_collection.return_value = self.collection_info(indexed=0)
        with patch.object(benchmark.time, "perf_counter", side_effect=[0.0, 1.0]):
            with self.assertRaisesRegex(RuntimeError, "Refusing to report a plain scan"):
                benchmark.wait_for_index(client, "owned", 10000, 0.5)
        client.get_collection.return_value = self.collection_info(status=models.CollectionStatus.RED)
        with self.assertRaisesRegex(RuntimeError, "optimizer failed"):
            benchmark.wait_for_index(client, "owned", 10000, 1)
        client.get_collection.return_value = self.collection_info()
        client.count.return_value = models.CountResult(count=9999)
        with self.assertRaisesRegex(RuntimeError, "9999 points"):
            benchmark.wait_for_index(client, "owned", 10000, 1)

    def test_exact_and_hnsw_use_same_queries_and_sqrt_threshold(self):
        args = benchmark.parse_args(["--threshold", "1.21", "--ef", "64", "--warmup", "1"])
        args.topk = 2
        workload = self.workload()
        for exact in (True, False):
            client = Mock(spec=QdrantClient)
            client.query_points.return_value = models.QueryResponse(points=[self.point(workload.keys[0])])
            with self.subTest(exact=exact), redirect_stdout(io.StringIO()):
                result = benchmark.run_search(client, "owned", args, workload, exact=exact)
            self.assertEqual(result.engine, "bruteforce" if exact else "hnsw")
            self.assertEqual(len(result.samples_ms), 2)
            self.assertEqual(result.results, [[workload.keys[0]], [workload.keys[0]]])
            self.assertIsNone(result.insert_qps)
            self.assertEqual(client.query_points.call_count, 3)
            for call in client.query_points.call_args_list:
                self.assertEqual(call.kwargs["search_params"].exact, exact)
                self.assertEqual(call.kwargs["search_params"].hnsw_ef, 64)
                self.assertAlmostEqual(call.kwargs["score_threshold"], 1.1)
                self.assertFalse(call.kwargs["with_payload"])
                self.assertFalse(call.kwargs["with_vectors"])
            self.assertEqual(
                [call.kwargs["query"] for call in client.query_points.call_args_list[1:]],
                workload.queries,
            )

    def test_invalid_ids_scores_and_missing_unfiltered_neighbors_fail(self):
        workload = self.workload()
        keys = set(workload.keys)
        for points in (
            [self.point("1e9a6754-d8b0-4c91-8e27-00000000ffff")],
            [self.point(workload.keys[0]), self.point(workload.keys[0])],
            [self.point(workload.keys[0], float("nan"))],
            [self.point(workload.keys[0], -1.0)],
        ):
            with self.subTest(points=points), self.assertRaises(RuntimeError):
                benchmark.result_ids(models.QueryResponse(points=points), keys, 2)
        client = Mock(spec=QdrantClient)
        client.query_points.return_value = models.QueryResponse(points=[])
        args = benchmark.parse_args(["--warmup", "0"])
        with self.assertRaisesRegex(RuntimeError, "unfiltered"), redirect_stdout(io.StringIO()):
            benchmark.run_search(client, "owned", args, workload, exact=False)

    def test_failure_deletes_only_the_new_collection_and_closes_client(self):
        for created in (True, False):
            client = Mock(spec=QdrantClient)
            client.create_collection.return_value = created
            client.delete_collection.return_value = True
            with (
                self.subTest(created=created),
                tempfile.TemporaryDirectory() as directory,
                patch.object(benchmark, "QdrantClient", return_value=client),
                patch.object(benchmark, "make_workload", side_effect=RuntimeError("generation failed")),
                patch.object(benchmark, "check_tcp_endpoint"),
                redirect_stdout(io.StringIO()),
                self.assertRaises(RuntimeError),
            ):
                benchmark.main(["--output-dir", directory])
            collection = client.create_collection.call_args.kwargs["collection_name"]
            self.assertTrue(collection.startswith("kvstore_bench_"))
            self.assertNotEqual(collection, "benchmark")
            if created:
                client.delete_collection.assert_called_once_with(collection)
            else:
                client.delete_collection.assert_not_called()
            client.close.assert_called_once()
            config = client.create_collection.call_args.kwargs
            self.assertEqual(config["hnsw_config"].m, 16)
            self.assertEqual(config["hnsw_config"].ef_construct, 100)
            self.assertEqual(config["hnsw_config"].full_scan_threshold, 10)
            self.assertEqual(config["optimizers_config"].indexing_threshold, 1)
            self.assertEqual(config["shard_number"], 1)

    def test_unreachable_grpc_endpoint_has_docker_port_guidance(self):
        with patch.object(benchmark.socket, "create_connection", side_effect=ConnectionRefusedError("refused")):
            with self.assertRaisesRegex(
                RuntimeError,
                r"gRPC endpoint 127\.0\.0\.1:6334.*6334:6334.*--grpc-port",
            ):
                benchmark.check_tcp_endpoint("127.0.0.1", 6334, "grpc", 10)

    def test_unreachable_grpc_fails_before_creating_report_or_collection(self):
        client = Mock(spec=QdrantClient)
        client.info.return_value = models.VersionInfo(title="qdrant", version="1.19.2", commit="test")
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(benchmark, "QdrantClient", return_value=client),
            patch.object(benchmark, "check_tcp_endpoint", side_effect=[None, RuntimeError("gRPC unavailable")]) as check,
            redirect_stdout(io.StringIO()),
            self.assertRaisesRegex(RuntimeError, "gRPC unavailable"),
        ):
            benchmark.main(["--output-dir", directory])
        self.assertEqual([call.args[2] for call in check.call_args_list], ["http", "grpc"])
        client.create_collection.assert_not_called()
        client.delete_collection.assert_not_called()
        client.close.assert_called_once()
        self.assertFalse(Path(directory).exists())

    def test_arguments_share_kvstore_defaults_and_reject_invalid_values(self):
        args = benchmark.parse_args(["--m", "32", "--ef-construction", "200", "--ef", "300"])
        self.assertEqual((args.m, args.ef_construction, args.ef), (32, 200, 300))
        self.assertEqual((args.records, args.queries, args.topk), (10000, 200, 10))
        for options in (
            ["--records", "9999"],
            ["--ef", "1"],
            ["--threshold", "nan"],
            ["--index-timeout", "0"],
            ["--grpc-port", "0"],
            ["--http-port", "65536"],
        ):
            with self.subTest(options=options), self.assertRaises(SystemExit), patch("sys.stderr", io.StringIO()):
                benchmark.parse_args(options)


if __name__ == "__main__":
    unittest.main()
