import io
from contextlib import redirect_stdout
import struct
import unittest
from unittest.mock import Mock, call, patch

from vectordb_test import benchmark


class BenchmarkTests(unittest.TestCase):
    def test_resp_encoding_is_binary_safe(self):
        self.assertEqual(
            benchmark.encode_command(b"SET", b"id", b"\x00\r\n"),
            b"*3\r\n$3\r\nSET\r\n$2\r\nid\r\n$3\r\n\x00\r\n\r\n",
        )

    def test_resp_replies_and_errors(self):
        client = benchmark.RespClient(Mock(), io.BytesIO(b"+OK\r\n$3\r\na,b\r\n$-1\r\n"))
        self.assertEqual(client.read_reply(), b"OK")
        self.assertEqual(client.read_reply(), b"a,b")
        self.assertIsNone(client.read_reply())
        for reply in (b"-ERR invalid vector\r\n", b"$5\r\nab", b"+OK", b"$-2\r\n", b":1\r\n"):
            with self.subTest(reply=reply), self.assertRaises(RuntimeError):
                benchmark.RespClient(Mock(), io.BytesIO(reply)).read_reply()

    def test_workload_is_shared_deterministic_and_queries_are_independent(self):
        first = benchmark.make_workload(4, 2, 2, 4.00001, 768)
        second = benchmark.make_workload(4, 2, 2, 4.00001, 768)
        self.assertEqual(first, second)
        self.assertNotEqual(first.sha256, benchmark.make_workload(4, 2, 2, 4.00001, 769).sha256)
        self.assertEqual(len(first.keys), 4)
        header = struct.pack("<II", 1, 768)
        for query in first.queries:
            vector_start = query.index(header)
            blob = query[vector_start : vector_start + benchmark.VECTOR.size]
            self.assertTrue(all(blob not in insert for insert in first.inserts))
            self.assertEqual(len(benchmark.VECTOR.unpack(blob)), 770)

    def test_recall_uses_overlap_not_rank_order(self):
        recall = benchmark.compare_results([["a", "b"], ["c", "d"]], [["b", "a"], ["c", "x"]])
        self.assertEqual(recall["recall_at_k"], 0.75)
        self.assertEqual(recall["matched_neighbors"], 3)
        self.assertEqual(recall["ground_truth_neighbors"], 4)
        self.assertEqual(recall["exact_set_match_rate"], 0.5)

    def test_filtered_truth_uses_actual_count_and_does_not_inflate_empty_queries(self):
        recall = benchmark.compare_results([["a"], [], ["b", "c"]], [["a"], [], ["b"]])
        self.assertEqual(recall["recall_at_k"], 0.75)
        self.assertAlmostEqual(recall["micro_recall"], 2 / 3)
        self.assertEqual(recall["evaluated_queries"], 2)
        self.assertEqual(recall["empty_ground_truth_queries"], 1)
        with self.assertRaisesRegex(ValueError, "undefined"):
            benchmark.compare_results([[]], [[]])

    def test_invalid_recall_inputs_fail(self):
        for truth, candidate in (([], []), ([["a"]], []), ([["a", "a"]], [["a"]])):
            with self.subTest(truth=truth), self.assertRaises(ValueError):
                benchmark.compare_results(truth, candidate)

    def test_latency_percentiles_are_interpolated_in_milliseconds(self):
        result = benchmark.latency_summary([4.0, 1.0, 3.0, 2.0])
        self.assertEqual(result["mean"], 2.5)
        self.assertEqual(result["p50"], 2.5)
        self.assertAlmostEqual(result["p95"], 3.85)
        self.assertAlmostEqual(result["p99"], 3.97)
        self.assertEqual(result["max"], 4.0)
        self.assertEqual(benchmark.latency_summary([2.0])["p99"], 2.0)

    def test_search_results_are_validated(self):
        self.assertEqual(benchmark.search_ids(b"a,b", {"a", "b"}, 2), ["a", "b"])
        self.assertEqual(benchmark.search_ids(None, {"a"}, 1), [])
        for reply in (b"a,a", b"a,c", b"a,b", b""):
            with self.subTest(reply=reply), self.assertRaises(RuntimeError):
                benchmark.search_ids(reply, {"a", "b"}, 1)

    def test_hnsw_insertion_measures_each_acknowledged_vector(self):
        client = Mock(spec=benchmark.RespClient)
        client.execute.return_value = b"OK"
        with patch.object(
            benchmark.time, "perf_counter_ns",
            side_effect=[0, 1_000_000, 3_000_000, 4_000_000, 10_000_000, 11_000_000],
        ):
            result = benchmark.load_vectors(client, [b"first", b"second"], measure=True)
        self.assertEqual(result, (0.011, [2.0, 6.0]))
        self.assertEqual(client.execute.call_args_list, [call(b"first"), call(b"second")])
        client.read_reply.assert_not_called()
        self.assertIsNotNone(result)
        if result is not None:
            elapsed, samples = result
            self.assertAlmostEqual(len(samples) / elapsed, 181.818181818)
            self.assertEqual(benchmark.latency_summary(samples)["mean"], 4.0)
            self.assertAlmostEqual(benchmark.latency_summary(samples)["p95"], 5.8)

    def test_bruteforce_loading_is_batched_and_not_measured(self):
        client = Mock()
        client.read_reply.return_value = b"OK"
        commands = [bytes([index]) for index in range(65)]
        with patch.object(benchmark.time, "perf_counter_ns", side_effect=AssertionError("must not measure")):
            result = benchmark.load_vectors(client, commands, measure=False)
        self.assertIsNone(result)
        self.assertEqual(
            client.connection.sendall.call_args_list,
            [call(b"".join(commands[:64])), call(commands[64])],
        )
        self.assertEqual(client.read_reply.call_count, 65)
        client.execute.assert_not_called()

    def test_insertion_failures_abort_in_both_loading_modes(self):
        for measure in (True, False):
            client = Mock()
            client.execute.return_value = None
            client.read_reply.return_value = None
            with self.subTest(measure=measure), self.assertRaisesRegex(RuntimeError, "SET failed"):
                benchmark.load_vectors(client, [b"record"], measure=measure)

    def test_terminal_summary_is_an_english_ascii_table_with_measured_values(self):
        results = [
            benchmark.EngineResult(
                engine=name,
                insert_seconds=1.0 if name == "hnsw" else None,
                insert_qps=10000.0 if name == "hnsw" else None,
                query_seconds=1.0,
                query_qps=qps,
                latency_ms={"mean": mean, "p50": mean, "p95": p95, "p99": p99, "max": p99},
                samples_ms=[],
                results=[],
                insert_latency_ms={"mean": 0.1, "p50": 0.08, "p95": 0.2, "p99": 0.3, "max": 0.4}
                if name == "hnsw" else None,
                insert_samples_ms=[0.1] if name == "hnsw" else None,
            )
            for name, qps, mean, p95, p99 in (
                ("bruteforce", 272.10, 3.673, 3.808, 4.024),
                ("hnsw", 834.10, 1.198, 1.325, 1.413),
            )
        ]
        output = io.StringIO()
        with redirect_stdout(output):
            benchmark.print_performance_summary(results)
        expected_table = (
            "+-------------+------------+--------------+----------+----------+\n"
            "| Engine      | Search QPS | Mean latency |      P95 |      P99 |\n"
            "+-------------+------------+--------------+----------+----------+\n"
            "| Brute force |     272.10 |     3.673 ms | 3.808 ms | 4.024 ms |\n"
            "| HNSW        |     834.10 |     1.198 ms | 1.325 ms | 1.413 ms |\n"
            "+-------------+------------+--------------+----------+----------+\n"
        )
        self.assertTrue(output.getvalue().startswith(expected_table), output.getvalue())
        self.assertTrue(output.getvalue().isascii())
        self.assertIn("Brute force: P50=3.673 ms, Max=4.024 ms", output.getvalue())
        self.assertIn("HNSW: P50=1.198 ms, Max=1.413 ms", output.getvalue())
        self.assertIn("HNSW insertion (single-vector RTT, pipeline=1):", output.getvalue())
        insertion_output = output.getvalue().split("HNSW insertion", 1)[1]
        self.assertIn("Insert QPS", insertion_output)
        self.assertIn("| HNSW   |   10000.00 |     0.100 ms | 0.200 ms | 0.300 ms |", insertion_output)
        self.assertIn("Samples=1, P50=0.080 ms, Max=0.400 ms", insertion_output)
        self.assertNotIn("Brute force", insertion_output)
        results[0].query_qps = 123456789.12
        output = io.StringIO()
        with redirect_stdout(output):
            benchmark.print_performance_summary(results)
        table_lines = output.getvalue().splitlines()[:6]
        self.assertEqual(len({len(line) for line in table_lines}), 1)
        self.assertIn("123456789.12", table_lines[3])

    def test_minimum_records_and_threshold_are_enforced(self):
        binaries = ["--bruteforce-bin", "/tmp/brute", "--hnsw-bin", "/tmp/hnsw"]
        args = benchmark.parse_args(binaries)
        self.assertEqual(args.records, 10_000)
        self.assertEqual(args.topk, 10)
        self.assertEqual(args.threshold, benchmark.UNFILTERED_THRESHOLD)
        for options in (
            ["--records", "9999"],
            ["--records", "100001"],
            ["--queries", "0"],
            ["--threshold", "nan"],
            ["--threshold", "0"],
            ["--topk", "101"],
            ["--timeout", "-1"],
        ):
            with self.subTest(options=options), self.assertRaises(SystemExit), unittest.mock.patch("sys.stderr", io.StringIO()):
                benchmark.parse_args(binaries + options)


if __name__ == "__main__":
    unittest.main()
