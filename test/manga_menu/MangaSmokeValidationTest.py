"""Exercise the real smoke harness output gate without starting a simulator."""
import argparse
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import run_manga_simulator_smoke_test as smoke


class MangaSmokeValidationTest(unittest.TestCase):
    def run_mode(self, mode, *, error=False, success=True):
        markers = ("\n".join(f"Verified active manga automatic event cancellation mode={i}" for i in range(6))
                   if mode == "ACTIVE_EVENTS" else
                   "Verified touch-only manga paging in both directions and fresh popup hitboxes")
        output = markers + ("\nSimulator smoke test passed\n" if success else "\n")
        if error:
            output += "[ERR] [MANGA] injected recoverable reader error\n"
        result = subprocess.CompletedProcess([], 0, stdout=output)
        args = argparse.Namespace(build=False, env="sticky-simulator", headless=True, timeout=1)
        with patch.dict(os.environ, {f"CROSSINK_SIMULATOR_MANGA_{mode}": "1"}, clear=True), \
                patch.object(smoke, "program_path", return_value=Path(__file__)), \
                patch.object(smoke, "prepare_fs"), patch.object(smoke, "prepare_manga_fixture"), \
                patch.object(smoke.subprocess, "run", return_value=result), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return smoke.run_smoke(args)

    def test_success_markers_pass(self):
        for mode in ("ACTIVE_EVENTS", "TOUCH_PAGING"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_mode(mode), 0)

    def test_recoverable_reader_error_fails_even_with_all_markers(self):
        for mode in ("ACTIVE_EVENTS", "TOUCH_PAGING"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_mode(mode, error=True), 2)

    def test_common_success_marker_required(self):
        for mode in ("ACTIVE_EVENTS", "TOUCH_PAGING"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_mode(mode, success=False), 2)


class MangaPrefetchStressValidationTest(unittest.TestCase):
    HOLD = "Verified prefetch holds source before foreground intent"
    CANCELLED = "Prefetch result=4 page=0 panel=0"

    def transcript(self):
        lines = [
            "Verified coalesced input page=0 panel=-1 with menu closed",
            "Verified coalesced input page=1 panel=0 with menu closed",
            "Rendering Manga Confirm on menu drain selected chapter",
            "Rendering Manga duplicate menu request consumed",
        ]
        for phase, activity in (
            ("child push", "ReaderOptions"), ("manual refresh", None),
            ("replace", "ReaderOptions"), ("reader pop", "Home"), ("main sleep", "Sleep"),
        ):
            lines.append(self.HOLD)
            if phase == "main sleep":
                lines.append("Verified main sleep defers before persistence while prefetch drains")
            lines.append(self.CANCELLED)
            if phase in ("replace", "reader pop", "main sleep"):
                lines.append("Exiting activity: MangaReader")
            if activity:
                lines.append(f"Entering activity: {activity}")
            if phase == "main sleep":
                lines.append("Verified main sleep preparation completed after worker quiescence")
            lines.append(f"Rendering Prefetch {phase} drained")
            if phase == "child push":
                lines.append("Rendering Prefetch child pop resumed manga")
        return "\n".join(lines)

    def test_ordered_worker_drain_passes_without_obsolete_log(self):
        smoke.validate_prefetch_stress(self.transcript())

    def test_each_phase_requires_its_own_hold_and_cancelled_completion(self):
        for marker in (self.HOLD, self.CANCELLED):
            for occurrence in range(5):
                with self.subTest(marker=marker, occurrence=occurrence):
                    parts = self.transcript().split(marker)
                    parts[occurrence] += parts.pop(occurrence + 1)
                    with self.assertRaisesRegex(RuntimeError, "ordered worker drain"):
                        smoke.validate_prefetch_stress(marker.join(parts))

    def test_successful_worker_result_does_not_prove_cancellation(self):
        with self.assertRaisesRegex(RuntimeError, "ordered worker drain"):
            smoke.validate_prefetch_stress(self.transcript().replace("Prefetch result=4 ", "Prefetch result=0 "))

    def test_cancellation_after_activity_entry_fails(self):
        before = self.CANCELLED + "\nEntering activity: ReaderOptions"
        after = "Entering activity: ReaderOptions\n" + self.CANCELLED
        with self.assertRaisesRegex(RuntimeError, "ordered worker drain"):
            smoke.validate_prefetch_stress(self.transcript().replace(before, after, 1))

    def test_reader_exit_before_worker_completion_fails(self):
        before = self.CANCELLED + "\nExiting activity: MangaReader"
        after = "Exiting activity: MangaReader\n" + self.CANCELLED
        with self.assertRaisesRegex(RuntimeError, "exited manga before worker drain"):
            smoke.validate_prefetch_stress(self.transcript().replace(before, after, 1))

    def test_all_remaining_lifecycle_markers_are_required(self):
        for marker in (
            "Verified coalesced input page=0 panel=-1 with menu closed",
            "Verified coalesced input page=1 panel=0 with menu closed",
            "Rendering Manga Confirm on menu drain selected chapter",
            "Rendering Manga duplicate menu request consumed",
            "Rendering Prefetch child pop resumed manga",
            "Verified main sleep defers before persistence while prefetch drains",
            "Verified main sleep preparation completed after worker quiescence",
        ):
            with self.subTest(marker=marker), self.assertRaises(RuntimeError):
                smoke.validate_prefetch_stress(self.transcript().replace(marker, ""))


if __name__ == "__main__":
    unittest.main()
