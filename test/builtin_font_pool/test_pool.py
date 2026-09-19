"""Host checks: python3 -m unittest discover -s test/builtin_font_pool -v."""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("font_pool", ROOT / "scripts/pool_builtin_fonts.py")
pool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pool)


class PoolTests(unittest.TestCase):
    def test_hook_priority_survives_later_library_include_prepend(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("A native C++ compiler is required for include ordering")
        with tempfile.TemporaryDirectory(prefix="font pool ") as temp:
            root = Path(temp)
            generated = root / "build/pooled-fonts"
            original = root / "library"
            for directory, marker in ((generated, 1), (original, 0)):
                (directory / "builtinFonts").mkdir(parents=True)
                (directory / "builtinFonts/all.h").write_text("#include <builtinFonts/child.h>\n")
                (directory / "builtinFonts/child.h").write_text(f"#define POOLED_FONT_MARKER {marker}\n")

            class Environment(dict):
                def subst(self, text):
                    return {"$PROJECT_DIR": str(ROOT), "$BUILD_DIR": str(root / "build")}[text]

                def Prepend(self, **values):
                    for key, value in values.items():
                        self[key] = value + self.get(key, [])

            env = Environment()
            hook = ROOT / "scripts/pool_builtin_fonts_pio.py"
            with patch("runpy.run_path", return_value={"generate": lambda *_: None}):
                exec(compile(hook.read_text(), str(hook), "exec"), {"env": env, "Import": lambda *_: None})
            # PlatformIO's library builder prepends library directories AFTER
            # pre-scripts. SCons emits CCFLAGS before its $_CPPINCFLAGS expansion.
            env.Prepend(CPPPATH=[str(original)])
            cpp = root / "check.cpp"
            cpp.write_text("#include <builtinFonts/all.h>\nstatic_assert(POOLED_FONT_MARKER == 1, \"Original fonts selected\");\n")
            command = [compiler, "-std=c++17", "-fsyntax-only"] + env.get("CCFLAGS", [])
            for include in env.get("CPPPATH", []):
                command.extend(["-I", include])
            subprocess.run(command + [str(cpp)], check=True, capture_output=True)

    def test_source_directory_cannot_be_overwritten(self):
        source = ROOT / "lib/EpdFont/builtinFonts"
        for destination in (source, source.parent):
            with self.subTest(destination=destination), self.assertRaises(ValueError):
                pool.generate(source, destination)

    def test_compiled_array_bytes_and_font_references(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("A native C++ compiler is required for compiled equivalence")
        source = ROOT / "lib/EpdFont/builtinFonts"
        fields = ("bitmap glyph intervals intervalCount advanceY ascender descender is2Bit "
                  "groups groupCount glyphToGroup kernLeftClasses kernRightClasses "
                  "kernLeftCodepoints kernLeftClassIds kernRightCodepoints kernRightClassIds "
                  "kernMatrix kernRowOffsets kernSparseCols kernSparseValues "
                  "kernLeftEntryCount kernRightEntryCount kernLeftClassCount kernRightClassCount "
                  "ligaturePairs ligaturePairCount").split()
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "generated"
            pool.generate(source, output)
            for noemoji in (False, True):
                body = ["#include <cstdio>", "#include <EpdFontData.h>",
                        "#include <builtinFonts/all.h>", "int main() {"]
                for relative in pool.font_paths(source):
                    if (source / "noemoji" / relative.name).exists() and ("noemoji" in relative.parts) != noemoji:
                        continue
                    original = (source / relative).read_text()
                    for array in pool.parse_arrays(original):
                        name = array["name"]
                        body.append(f"static_assert(sizeof({name}) == {array['bytes']}, \"Array size changed\");")
                        body.append(f"if (std::fwrite({name}, 1, sizeof({name}), stdout) != sizeof({name})) return 2;")
                    # Verify each descriptor's exact scalar value and named table
                    # reference with the compiler, independently of pool parsing.
                    font, initializer = re.search(r"static constexpr EpdFontData (\w+) = \{(.*?)\};", original, re.S).groups()
                    initializer = re.sub(r"//[^\n]*", "", initializer)
                    values = [value.strip() for value in initializer.split(",") if value.strip()]
                    self.assertEqual(len(fields), len(values))
                    for field, value in zip(fields, values):
                        body.append(f"static_assert({font}.{field} == {value}, \"Font descriptor changed\");")
                    for field in ("glyphMissHandler", "glyphMissCtx", "coverageHandler"):
                        body.append(f"static_assert({font}.{field} == nullptr, \"Builtin callback changed\");")
                body.append("return 0; }")
                cpp = Path(temp) / "equivalence.cpp"
                cpp.write_text("\n".join(body))
                results = []
                for label, includes in (("original", [source.parent]), ("pooled", [output, source.parent])):
                    executable = Path(temp) / label
                    command = [compiler, "-std=c++17", "-O0", str(cpp), "-o", str(executable)]
                    if noemoji:
                        command.append("-DOMIT_EMOJI_FONTS")
                    for include in includes:
                        command.extend(["-I", str(include)])
                    subprocess.run(command, check=True, capture_output=True)
                    results.append(subprocess.run([str(executable)], check=True, capture_output=True).stdout)
                self.assertEqual(results[0], results[1], f"Compiled tables differ: noemoji={noemoji}")

    def test_types_and_values_are_part_of_identity(self):
        arrays = pool.parse_arrays("""
static const uint8_t a[] = {1, 2};
static const int8_t b[] = {1, 2};
static const uint8_t c[2] = {0x1, 2};
static const uint8_t d[] = {1, 3};
""")
        self.assertNotEqual(arrays[0]["key"], arrays[1]["key"])
        self.assertEqual(arrays[0]["key"], arrays[2]["key"])
        self.assertNotEqual(arrays[0]["key"], arrays[3]["key"])

    def test_unsupported_initializers_fail_closed(self):
        for source in (
            "static const uint8_t a[] = {SOME_MACRO};",
            "static const uint8_t a[] = {1 << 2};",
            "static const uint8_t a[3] = {1, 2};",
            "static const uint8_t a[] PROGMEM = {1, 2};",
            "static const uint8_t a[] = {256};",
            "static const EpdUnicodeInterval a[] = {{1, 2}};",
        ):
            with self.subTest(source=source), self.assertRaises(ValueError):
                pool.parse_arrays(source)

    def test_real_headers_equivalent_and_reproducible(self):
        source = ROOT / "lib/EpdFont/builtinFonts"
        with tempfile.TemporaryDirectory() as temp:
            first, second = Path(temp) / "first", Path(temp) / "second"
            report = pool.generate(source, first)
            self.assertEqual(report, pool.generate(source, second))
            for path in first.rglob("*"):
                if path.is_file():
                    self.assertEqual(path.read_bytes(), (second / path.relative_to(first)).read_bytes())
            self.assertEqual((first / "builtinFonts/all.h").read_bytes(), (source / "all.h").read_bytes())
            for relative in pool.font_paths(source):
                original = (source / relative).read_text()
                transformed = (first / "builtinFonts" / relative).read_text()
                # Expand each generated include+alias back to the exact original
                # declaration. Everything else (including every font reference,
                # bitmap, metric, and preprocessor branch) must remain identical.
                for array in pool.parse_arrays(original):
                    symbol = pool.pool_symbol(array["key"])
                    replacement = pool.alias_text(array["name"], symbol)
                    if replacement in transformed:
                        shared = (first / "builtinFonts/pool" / (symbol + ".h")).read_text()
                        shared_array = pool.parse_arrays(shared)[0]
                        self.assertEqual(array["key"], shared_array["key"])
                        transformed = transformed.replace(replacement, array["declaration"])
                self.assertEqual(original, transformed, str(relative))
            self.assertGreater(report["default"]["candidate_bytes_saved"], 100000)
            self.assertGreater(report["noemoji"]["candidate_bytes_saved"], 0)


if __name__ == "__main__":
    unittest.main()
