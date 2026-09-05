"""Byte-level compatibility checks for CrossInk's future dictionary converter."""

from __future__ import annotations

import contextlib
import io
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock
from pathlib import Path


RECORD = struct.Struct("<32sIHBB")
ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
GOLDEN = Path(__file__).resolve().parent / "golden"


class JapaneseDictionaryConverterTest(unittest.TestCase):
    """Keep the CrossInk converter byte-compatible with the pinned Matcha tools."""

    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.output_dir = Path(self.temp_dir.name) / "output"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def _require_crossink_converter(self):
        try:
            from tools.dict_convert.convert_jmdict import (
                convert_jmdict,
                convert_yomitan,
                write_binary,
            )
            from scripts.gen_dict_spx import gen_one
        except ModuleNotFoundError as error:
            self.fail(
                "CrossInk Japanese dictionary converter is absent; expected "
                "tools/dict_convert/convert_jmdict.py and scripts/gen_dict_spx.py "
                f"({error})"
            )
        return convert_jmdict, convert_yomitan, write_binary, gen_one

    @staticmethod
    def _read_output(output_dir: Path, name: str = "vocab") -> dict[str, bytes]:
        return {
            f"{name}.{suffix}": (output_dir / f"{name}.{suffix}").read_bytes()
            for suffix in ("idx", "dat", "spx")
        }

    @staticmethod
    def _read_golden(fixture_name: str) -> dict[str, bytes]:
        return {
            filename: (GOLDEN / fixture_name / filename).read_bytes()
            for filename in ("vocab.idx", "vocab.dat", "vocab.spx")
        }

    def _assert_packed_golden(self, fixture_name: str) -> None:
        self.golden = self._read_golden(fixture_name)
        self.outputs = self._read_output(self.output_dir)

        self.assertEqual(RECORD.size, 40)
        self.assertEqual(self.outputs["vocab.idx"], self.golden["vocab.idx"])
        self.assertEqual(self.outputs["vocab.dat"], self.golden["vocab.dat"])
        self.assertEqual(self.outputs["vocab.spx"], self.golden["vocab.spx"])
        headword, offset, length, priority, pos_flags = RECORD.unpack_from(
            self.outputs["vocab.idx"], 0
        )
        self.assertIn(b"\0", headword)
        self.assertLessEqual(offset + length, len(self.outputs["vocab.dat"]))
        self.assertGreater(priority, 0)
        self.assertNotEqual(pos_flags, 0)

    def test_record_layout_and_matcha_golden(self) -> None:
        convert_jmdict, _, _, gen_one = self._require_crossink_converter()
        convert_jmdict(str(FIXTURES / "mini_jmdict.json"), str(self.output_dir), "vocab")
        gen_one(self.output_dir / "vocab.idx", self.output_dir / "vocab.spx")
        self._assert_packed_golden("mini_jmdict")
        self.assertIn("超長語彙項目候補文字x".encode("utf-8") + b"\0", self.outputs["vocab.idx"])

    def test_structured_yomitan_matches_matcha_golden(self) -> None:
        _, convert_yomitan, _, gen_one = self._require_crossink_converter()
        archive_path = Path(self.temp_dir.name) / "mini_yomitan.zip"
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for source in sorted((FIXTURES / "mini_yomitan").iterdir()):
                archive.write(source, source.name)
        convert_yomitan(str(archive_path), str(self.output_dir), "vocab")
        gen_one(self.output_dir / "vocab.idx", self.output_dir / "vocab.spx")
        self._assert_packed_golden("mini_yomitan")

    def test_yomitan_directory_matches_zip_golden(self) -> None:
        _, convert_yomitan, _, _ = self._require_crossink_converter()
        convert_yomitan(str(FIXTURES / "mini_yomitan"), str(self.output_dir), "vocab")
        self._assert_packed_golden("mini_yomitan")

    def test_31_byte_headword_is_accepted(self) -> None:
        headword = "超長語彙項目候補文字x"
        self.assertEqual(len(headword.encode("utf-8")), 31)

    def test_all_dictionary_basenames_publish_three_files(self) -> None:
        convert_jmdict, _, _, _ = self._require_crossink_converter()
        expected = self._read_golden("mini_jmdict")

        for name in ("vocab", "names", "grammar"):
            output_dir = self.output_dir / name
            convert_jmdict(str(FIXTURES / "mini_jmdict.json"), str(output_dir), name)
            self.assertEqual(self._read_output(output_dir, name), {
                f"{name}.{suffix}": expected[f"vocab.{suffix}"] for suffix in ("idx", "dat", "spx")
            })

    def test_tgz_input_matches_jmdict_golden(self) -> None:
        convert_jmdict, _, _, _ = self._require_crossink_converter()
        archive_path = Path(self.temp_dir.name) / "mini_jmdict.json.tgz"
        with tarfile.open(archive_path, "w:gz") as archive:
            source = FIXTURES / "mini_jmdict.json"
            archive.add(source, arcname="nested/mini_jmdict.json")

        convert_jmdict(str(archive_path), str(self.output_dir), "vocab")

        self._assert_packed_golden("mini_jmdict")

    def test_supplied_input_cli_never_calls_downloader(self) -> None:
        from tools.dict_convert import convert_jmdict as converter

        argv = [
            "convert_jmdict.py",
            "--input", str(FIXTURES / "mini_jmdict.json"),
            "--output-dir", str(self.output_dir),
            "--name", "vocab",
        ]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(
            converter, "download_jmdict", side_effect=AssertionError("network must stay unused")
        ):
            converter.main()

        self._assert_packed_golden("mini_jmdict")

    def test_corrupt_source_creates_no_output_set(self) -> None:
        convert_jmdict, _, _, _ = self._require_crossink_converter()
        corrupt = Path(self.temp_dir.name) / "corrupt.json"
        corrupt.write_text("{ invalid json", encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "Invalid JMdict JSON"):
            convert_jmdict(str(corrupt), str(self.output_dir), "vocab")

        self.assertFalse(self.output_dir.exists())

    def test_definition_larger_than_uint16_is_truncated(self) -> None:
        _, _, write_binary, _ = self._require_crossink_converter()
        definition = b"a" * (0xFFFF + 1)

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            write_binary([(b"short", definition, 100, 0x20)], self.output_dir, "vocab")

        _, offset, length, _, _ = RECORD.unpack((self.output_dir / "vocab.idx").read_bytes())
        self.assertEqual(offset, 0)
        self.assertEqual(length, 0xFFFF)
        self.assertEqual((self.output_dir / "vocab.dat").read_bytes(), definition[:0xFFFF])
        self.assertIn("Truncated 1 definition > 65535 bytes", output.getvalue())

    def test_adjacent_duplicate_definitions_reuse_data_slice(self) -> None:
        _, _, write_binary, _ = self._require_crossink_converter()

        write_binary(
            [(b"a", b"same", 100, 0x20), (b"b", b"same", 100, 0x20)],
            self.output_dir,
            "vocab",
        )

        first = RECORD.unpack_from((self.output_dir / "vocab.idx").read_bytes(), 0)
        second = RECORD.unpack_from((self.output_dir / "vocab.idx").read_bytes(), RECORD.size)
        self.assertEqual(first[1:3], second[1:3])
        self.assertEqual((self.output_dir / "vocab.dat").read_bytes(), b"same")

    def test_32_byte_headword_is_rejected(self) -> None:
        convert_jmdict, _, _, _ = self._require_crossink_converter()
        source = Path(self.temp_dir.name) / "too-long.json"
        source.write_text(
            '{"words":[{"kana":[{"text":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],'
            '"sense":[{"partOfSpeech":["n"],"gloss":[{"text":"ignored"}]}]}]}',
            encoding="utf-8",
        )

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            convert_jmdict(str(source), str(self.output_dir), "vocab")

        self.assertEqual((self.output_dir / "vocab.idx").read_bytes(), b"")
        self.assertEqual((self.output_dir / "vocab.dat").read_bytes(), b"")
        self.assertIn("Skipped 1 headword >= 32 bytes", output.getvalue())

    def test_mdict_missing_dependency_has_one_actionable_error(self) -> None:
        from tools.dict_convert.convert_jmdict import convert_mdict

        with mock.patch.dict(sys.modules, {"readmdict": None}):
            with self.assertRaisesRegex(RuntimeError, "pip install readmdict"):
                convert_mdict("fixture.mdx", str(self.output_dir), "vocab")

    def test_yomitan_reports_oversized_headword_skip(self) -> None:
        _, convert_yomitan, _, _ = self._require_crossink_converter()
        source_dir = Path(self.temp_dir.name) / "oversized-yomitan"
        source_dir.mkdir()
        (source_dir / "term_bank_1.json").write_text(
            '[["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "", "", "n", 0, ["ignored"]]]',
            encoding="utf-8",
        )

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            convert_yomitan(str(source_dir), str(self.output_dir), "vocab")

        self.assertEqual((self.output_dir / "vocab.idx").read_bytes(), b"")
        self.assertIn("Skipped 1 headword >= 32 bytes", output.getvalue())

    def test_mdict_reports_oversized_headword_skip_with_lazy_mock(self) -> None:
        from tools.dict_convert.convert_jmdict import convert_mdict

        class FakeMdx:
            def __init__(self, _path) -> None:
                pass

            def items(self):
                return [(b"a" * 32, b"ignored")]

        fake_module = type("FakeReadMdict", (), {"MDX": FakeMdx})
        output = io.StringIO()
        with mock.patch.dict(sys.modules, {"readmdict": fake_module}), contextlib.redirect_stdout(output):
            convert_mdict("fixture.mdx", str(self.output_dir), "vocab")

        self.assertEqual((self.output_dir / "vocab.idx").read_bytes(), b"")
        self.assertIn("Skipped 1 headword >= 32 bytes", output.getvalue())

    def test_stale_temp_siblings_are_preserved_when_publication_is_rejected(self) -> None:
        _, _, write_binary, _ = self._require_crossink_converter()
        self.output_dir.mkdir()
        stale = self.output_dir / "vocab.idx.tmp"
        stale.write_bytes(b"previous interrupted invocation")

        with self.assertRaisesRegex(FileExistsError, "stale temporary output"):
            write_binary([(b"a", b"definition", 100, 0x20)], self.output_dir, "vocab")

        self.assertEqual(stale.read_bytes(), b"previous interrupted invocation")
        self.assertFalse((self.output_dir / "vocab.idx").exists())
        self.assertFalse((self.output_dir / "vocab.dat").exists())
        self.assertFalse((self.output_dir / "vocab.spx").exists())

    def test_failed_publication_restores_previous_complete_set(self) -> None:
        _, _, write_binary, _ = self._require_crossink_converter()
        write_binary([(b"a", b"old", 100, 0x20)], self.output_dir, "vocab")
        previous = {
            suffix: (self.output_dir / f"vocab.{suffix}").read_bytes()
            for suffix in ("idx", "dat", "spx")
        }

        from tools.dict_convert import convert_jmdict as converter

        real_replace = converter.os.replace

        def fail_spx_replace(source, destination):
            if str(destination).endswith("vocab.spx") and str(source).endswith(".tmp"):
                raise OSError("injected spx publication failure")
            return real_replace(source, destination)

        with mock.patch.object(converter.os, "replace", side_effect=fail_spx_replace):
            with self.assertRaisesRegex(OSError, "injected spx publication failure"):
                write_binary([(b"a", b"new", 100, 0x20)], self.output_dir, "vocab")

        self.assertEqual(
            {
                suffix: (self.output_dir / f"vocab.{suffix}").read_bytes()
                for suffix in ("idx", "dat", "spx")
            },
            previous,
        )
        self.assertFalse(any(self.output_dir.glob("vocab.*.tmp")))

    def test_temp_write_failure_removes_owned_temp_and_never_publishes(self) -> None:
        _, _, write_binary, _ = self._require_crossink_converter()
        real_open = Path.open

        class FailingWriter:
            def __init__(self, file_handle) -> None:
                self.file_handle = file_handle

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.file_handle.close()

            def write(self, _payload):
                raise OSError("injected dat temp write failure")

        def fail_dat_temp_write(path, *args, **kwargs):
            opened = real_open(path, *args, **kwargs)
            if path.name == "vocab.dat.tmp" and args[0] == "xb":
                return FailingWriter(opened)
            return opened

        with mock.patch.object(Path, "open", new=fail_dat_temp_write):
            with self.assertRaisesRegex(OSError, "injected dat temp write failure"):
                write_binary([(b"a", b"definition", 100, 0x20)], self.output_dir, "vocab")

        self.assertFalse(any(self.output_dir.glob("vocab.*.tmp")))
        self.assertFalse(any((self.output_dir / f"vocab.{suffix}").exists() for suffix in ("idx", "dat", "spx")))

    def test_temp_creation_race_never_deletes_foreign_temp(self) -> None:
        _, _, write_binary, _ = self._require_crossink_converter()
        real_open = Path.open

        def foreign_process_wins(path, *args, **kwargs):
            if path.name == "vocab.dat.tmp" and args[0] == "xb":
                with real_open(path, "xb") as foreign_file:
                    foreign_file.write(b"foreign invocation")
                raise FileExistsError("injected concurrent temp creation")
            return real_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", new=foreign_process_wins):
            with self.assertRaisesRegex(FileExistsError, "injected concurrent temp creation"):
                write_binary([(b"a", b"definition", 100, 0x20)], self.output_dir, "vocab")

        foreign_temp = self.output_dir / "vocab.dat.tmp"
        self.assertEqual(foreign_temp.read_bytes(), b"foreign invocation")
        self.assertFalse((self.output_dir / "vocab.idx.tmp").exists())
        self.assertFalse(any((self.output_dir / f"vocab.{suffix}").exists() for suffix in ("idx", "dat", "spx")))

    def test_backup_cleanup_failure_keeps_published_complete_set(self) -> None:
        _, _, write_binary, _ = self._require_crossink_converter()
        write_binary([(b"a", b"old", 100, 0x20)], self.output_dir, "vocab")
        real_unlink = Path.unlink

        def fail_last_backup_cleanup(path, *args, **kwargs):
            if path.name == "vocab.spx.bak":
                raise OSError("injected backup cleanup failure")
            return real_unlink(path, *args, **kwargs)

        with mock.patch.object(Path, "unlink", new=fail_last_backup_cleanup):
            write_binary([(b"a", b"new", 100, 0x20)], self.output_dir, "vocab")

        self.assertEqual((self.output_dir / "vocab.dat").read_bytes(), b"new")
        self.assertTrue((self.output_dir / "vocab.spx.bak").exists())

    def test_idx_dat_validator_rejects_corruption(self) -> None:
        from tools.dict_convert.convert_jmdict import _validate_idx_dat

        valid = RECORD.pack(b"a" + b"\0" * 31, 0, 1, 100, 0x20)
        unsorted = (
            RECORD.pack(b"b" + b"\0" * 31, 0, 1, 100, 0x20)
            + RECORD.pack(b"a" + b"\0" * 31, 0, 1, 100, 0x20)
        )
        missing_nul = RECORD.pack(b"a" * 32, 0, 1, 100, 0x20)
        nonzero_padding = RECORD.pack(b"a\0x" + b"\0" * 29, 0, 1, 100, 0x20)
        out_of_bounds = RECORD.pack(b"a" + b"\0" * 31, 1, 1, 100, 0x20)
        cases = (
            (b"x", b"", "divisible"),
            (unsorted, b"x", "out of order"),
            (missing_nul, b"x", "NUL padding"),
            (nonzero_padding, b"x", "NUL padding"),
            (out_of_bounds, b"x", "exceeds dat"),
        )
        for idx, dat, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                _validate_idx_dat(idx, dat)
        _validate_idx_dat(valid, b"x")

    def test_spx_validator_rejects_every_header_and_checkpoint_corruption(self) -> None:
        from scripts.gen_dict_spx import build_spx_bytes, validate_spx

        idx = RECORD.pack(b"a" + b"\0" * 31, 0, 0, 100, 0x20)
        valid = build_spx_bytes(idx)
        corruptions = []
        corruptions.append((b"", "shorter"))
        magic = bytearray(valid); magic[0] ^= 1; corruptions.append((bytes(magic), "magic"))
        version = bytearray(valid); struct.pack_into("<I", version, 8, 2); corruptions.append((bytes(version), "header"))
        stride = bytearray(valid); struct.pack_into("<I", stride, 12, 1); corruptions.append((bytes(stride), "header"))
        count = bytearray(valid); struct.pack_into("<I", count, 16, 2); corruptions.append((bytes(count), "header"))
        corruptions.append((valid[:-1], "size"))
        key = bytearray(valid); key[32] = ord("z"); corruptions.append((bytes(key), "checkpoint"))
        for spx, message in corruptions:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                validate_spx(idx, spx)

    def test_spx_uses_every_48th_key_across_three_checkpoints(self) -> None:
        from scripts.gen_dict_spx import build_spx_bytes, validate_spx

        headwords = [f"{index:03d}".encode("ascii") for index in range(97)]
        idx = b"".join(
            RECORD.pack(headword + b"\0" * (32 - len(headword)), 0, 0, 100, 0x20)
            for headword in headwords
        )

        spx = build_spx_bytes(idx)
        version, stride, count, fine_count, reserved = struct.unpack_from("<IIIII", spx, 8)
        self.assertEqual((version, stride, count, fine_count, reserved), (1, 48, 97, 3, 0))
        for checkpoint, record in enumerate((0, 48, 96)):
            offset = 32 + checkpoint * 32
            self.assertEqual(spx[offset : offset + 32], idx[record * 40 : record * 40 + 32])
        validate_spx(idx, spx)

        corrupted = bytearray(spx)
        corrupted[32 + 32] = ord("z")
        with self.assertRaisesRegex(ValueError, "checkpoint 1"):
            validate_spx(idx, bytes(corrupted))

    def test_gen_spx_cli_help_and_positional_contract(self) -> None:
        script = ROOT / "scripts" / "gen_dict_spx.py"
        for flag in ("-h", "--help"):
            with self.subTest(flag=flag):
                result = subprocess.run(
                    [sys.executable, str(script), flag],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0)
                self.assertIn("usage: gen_dict_spx.py", result.stdout)
                self.assertNotIn("skip vocab", result.stdout)

        self.output_dir.mkdir()
        positional = subprocess.run(
            [sys.executable, str(script), str(self.output_dir)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(positional.returncode, 0)
        self.assertIn(f"skip vocab: no {self.output_dir / 'vocab.idx'}", positional.stdout)

        missing = subprocess.run(
            [sys.executable, str(script)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("usage: gen_dict_spx.py", missing.stderr)


if __name__ == "__main__":
    unittest.main()
