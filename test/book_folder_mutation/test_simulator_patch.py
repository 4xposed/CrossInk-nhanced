"""Disposable patch tests; HEAD mocked, no Git repository or commits created."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("storage_patch", PROJECT / "scripts/patch_simulator_storage.py")
HOOK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HOOK)
MANIFEST = json.loads((PROJECT / "tools/simulator-patches/checked-storage.json").read_text())
BASELINE = Path("/private/tmp/crossink-manga-transport10c-before/.pio/libdeps/simulator/simulator")


class PatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="crossink-storage-patch-")
        self.package = Path(self.temp.name) / "custom-libdeps" / "profile" / "simulator"
        # Reconstruct pristine files from the postimages and the checked reverse
        # patch; tests do not depend on the developer's private baseline folder.
        for name in MANIFEST["files"]:
            target = self.package / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((PROJECT / ".pio/libdeps/simulator/simulator" / name).read_bytes())
        subprocess.run(["git", "-C", str(self.package), "apply", "--reverse", str(PROJECT / "tools/simulator-patches/checked-storage.patch")], check=True)
        self.head = patch.object(HOOK.subprocess, "check_output", return_value=MANIFEST["revision"] + "\n")
        self.head.start()

    def tearDown(self):
        self.head.stop()
        self.temp.cleanup()

    def test_first_apply_and_idempotence(self):
        self.assertEqual(HOOK.apply_storage_patch(self.package, PROJECT), "applied")
        self.assertEqual(HOOK.apply_storage_patch(self.package, PROJECT), "already applied")

    def test_unknown_revision(self):
        with patch.object(HOOK.subprocess, "check_output", return_value="unknown\n"):
            with self.assertRaises(RuntimeError): HOOK.apply_storage_patch(self.package, PROJECT)

    def test_unknown_content_changes_nothing(self):
        target = self.package / "src/HalStorage.h"
        target.write_text(target.read_text() + "// user edit\n")
        other = (self.package / "src/HalStorage.cpp").read_bytes()
        with self.assertRaises(RuntimeError): HOOK.apply_storage_patch(self.package, PROJECT)
        self.assertEqual((self.package / "src/HalStorage.cpp").read_bytes(), other)

    def test_mixed_known_images_refused(self):
        (self.package / "src/HalStorage.h").write_bytes((PROJECT / ".pio/libdeps/simulator/simulator/src/HalStorage.h").read_bytes())
        with self.assertRaises(RuntimeError): HOOK.apply_storage_patch(self.package, PROJECT)


if __name__ == "__main__": unittest.main()
