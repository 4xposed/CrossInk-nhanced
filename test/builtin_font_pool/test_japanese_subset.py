import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FONT_DIR = ROOT / 'lib/EpdFont/builtinFonts'

class JapaneseSubsetTest(unittest.TestCase):
    def test_generated_sizes_cover_the_recorded_subset(self):
        requested = {int(line, 16) for line in (FONT_DIR / 'source/NotoSansJP/codepoints.txt').read_text().splitlines()
                     if line and not line.startswith('#')}
        for size in (8, 12):
            with self.subTest(size=size):
                path = FONT_DIR / f'notosansjp_joyo_{size}_regular.h'
                self.assertTrue(path.exists(), 'Japanese subset has not been generated')
                text = path.read_text().split('regularIntervals[] = {', 1)[1].split('};', 1)[0]
                generated = set()
                for a,b in re.findall(r'\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),', text):
                    generated.update(range(int(a,16), int(b,16)+1))
                self.assertEqual(generated, requested)
                self.assertTrue(set(map(ord, '猫あア。１')).issubset(generated))

    def test_japanese_interface_keys_placeholders_and_glyphs(self):
        import runpy
        parse = runpy.run_path(str(ROOT / 'scripts/gen_i18n.py'))['parse_yaml_file']
        english = parse(str(ROOT / 'lib/I18n/translations/english.yaml'))
        japanese = parse(str(ROOT / 'lib/I18n/translations/japanese.yaml'))
        keys = {k for k in english if not k.startswith('_')}
        self.assertEqual(keys, {k for k in japanese if not k.startswith('_')})
        points = {int(line,16) for line in (FONT_DIR / 'source/NotoSansJP/codepoints.txt').read_text().splitlines()
                  if line and not line.startswith('#')}
        placeholders = re.compile(r'(?<!\d)%(?:[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z)?[diuoxXfFeEgGcsp]|%)')
        missing = set()
        for key in keys:
            with self.subTest(key=key):
                self.assertEqual(placeholders.findall(english[key]), placeholders.findall(japanese[key]))
            missing.update(ord(ch) for ch in japanese[key] if ord(ch) >= 32 and ord(ch) not in points)
        self.assertEqual(missing, set(), f'Missing glyphs: {sorted(missing)}')
