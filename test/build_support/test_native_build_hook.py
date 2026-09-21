import contextlib
import io
from pathlib import Path
import runpy
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
with contextlib.redirect_stdout(io.StringIO()):
    HOOK = runpy.run_path(str(ROOT / "scripts/git_branch.py"))


class Environment:
    def __init__(self, profile):
        self.values = {
            "$PROJECT_DIR": str(ROOT), "$PIOENV": profile,
            "$BUILD_DIR": str(ROOT / ".pio/build" / profile),
            "$PROJECT_LIBDEPS_DIR": str(ROOT / ".pio/libdeps"),
        }
        self.prepended = {}

    def subst(self, value):
        return self.values[value]

    def Prepend(self, **kwargs):
        self.prepended.update(kwargs)


class NativeBuildHookTest(unittest.TestCase):
    def test_firmware_pools_fonts_and_keeps_include_priority(self):
        self.assertIn("configure_native_tools", HOOK)
        env = Environment("default")
        with patch("subprocess.run") as run:
            HOOK["configure_native_tools"](env, [])
        self.assertEqual(run.call_count, 1)
        command = run.call_args.args[0]
        self.assertIn("pool-builtin-fonts", command)
        self.assertIn("--locked", command)
        self.assertTrue(run.call_args.kwargs["check"])
        generated = str(ROOT / ".pio/build/default/pooled-fonts")
        self.assertEqual(env.prepended["CCFLAGS"], ["-I", generated])
        self.assertEqual(env.prepended["CPPPATH"], [generated])

    def test_simulator_checks_storage_before_font_generation(self):
        self.assertIn("configure_native_tools", HOOK)
        with patch("subprocess.run") as run:
            HOOK["configure_native_tools"](Environment("x4-pro-simulator"), [])
        self.assertEqual(run.call_count, 2)
        command = run.call_args_list[0].args[0]
        self.assertIn("patch-storage", command)
        self.assertEqual(command[-1], str(ROOT / ".pio/libdeps/x4-pro-simulator/simulator"))

    def test_clean_does_not_require_cargo_or_mutate_dependencies(self):
        self.assertIn("configure_native_tools", HOOK)
        with patch("subprocess.run") as run:
            HOOK["configure_native_tools"](Environment("simulator"), ["clean"])
        run.assert_not_called()
