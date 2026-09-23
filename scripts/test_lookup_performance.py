"""Checks for the simulator lookup performance gate (no firmware changes)."""
import contextlib
import io
import json
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

from run_simulator_smoke_test import check_lookup_performance


class LookupPerformanceTest(unittest.TestCase):
    def check_report(self, samples, baseline=None, extra=""):
        with tempfile.TemporaryDirectory() as folder:
            report = Path(folder) / "report.json"
            previous = Path(folder) / "before.json"
            if baseline is not None:
                previous.write_text(json.dumps(baseline))
            args = Namespace(env="simulator", performance_report=str(report),
                             performance_baseline=str(previous) if baseline is not None else None)
            output = "Dictionary first definition ready after 16 ms\n" + extra
            output += "\n".join(f"LOOKUP_PERF redraw_us={value}" for value in samples)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                result = check_lookup_performance(output, args)
            return result, json.loads(report.read_text()) if report.exists() else None

    def test_records_completed_redraw_samples(self):
        passed, report = self.check_report([1000] * 11 + [3000])
        self.assertTrue(passed)
        self.assertEqual(report["redraw_median_us"], 1000)
        self.assertEqual(report["redraw_max_us"], 3000)
        self.assertEqual(report["first_definition_max_ms"], 16)

    def test_rejects_missing_redraw_coverage(self):
        self.assertFalse(self.check_report([1000] * 11)[0])

    def test_rejects_slowdown_but_tolerates_small_host_jitter(self):
        baseline = {"env": "simulator", "redraw_median_us": 4000}
        self.assertTrue(self.check_report([5900] * 12, baseline)[0])
        self.assertFalse(self.check_report([7000] * 12, baseline)[0])

    def test_rejects_readiness_slowdown(self):
        baseline = {"env": "simulator", "redraw_median_us": 1000, "first_definition_max_ms": 1}
        self.assertFalse(self.check_report([1000] * 12, baseline, extra="Dictionary first definition ready after 80 ms\n")[0])

    def test_rejects_wrong_device_baseline(self):
        self.assertFalse(self.check_report([1000] * 12, {"env": "sticky-simulator", "redraw_median_us": 1000})[0])

    def test_rejects_readiness_deadline_miss(self):
        self.assertFalse(self.check_report([1000] * 12, extra="Dictionary first definition missed 1500 ms deadline")[0])


if __name__ == "__main__":
    unittest.main()
