"""Regenerate fixtures with the unmodified pinned reference, never the ported writer."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[3]
REFERENCE = ROOT.parent / 'matcha-reader'
SHA = '61ca61ba86e3c5709a24d1b9c4f3cf2d41488012'
SOURCE = 'tools/manga_convert/convert_manga.py'
assert subprocess.check_output(['git', '-C', str(REFERENCE), 'rev-parse', 'HEAD'], text=True).strip() == SHA
original = subprocess.check_output(['git', '-C', str(REFERENCE), 'show', f'{SHA}:{SOURCE}'])
assert (REFERENCE / SOURCE).read_bytes() == original
spec = importlib.util.spec_from_file_location('original_manga_converter', REFERENCE / SOURCE)
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)
output = Path(__file__).parent
panels = [
    {'box': (600, 20, 790, 470), 'translation': 'Hello!\nWorld.', 'text_blocks': [
        {'box': (12, 30, 70, 200), 'text': 'こんにちは世界'},
        {'box': (0, 0, 0, 0), 'text': '猫\x00犬'},
    ]},
    {'box': (0, 0, 0, 0), 'translation': '', 'text_blocks': []},
]
page = converter.encode_page(panels)
empty = converter.encode_page([])
converter._write_panel_index(str(output), [(0, len(page), 800, 480), (len(page), len(empty), 65535, 0), (len(page) + len(empty), 0, 0, 65535)], [page, empty])
converter.write_meta(str(output), '漫画', '作者', 'ja')
(output / 'meta-language.bin').write_bytes((output / 'meta.bin').read_bytes())
converter.write_meta(str(output), '漫画', '作者')
(output / 'meta-legacy.bin').write_bytes((output / 'meta.bin').read_bytes())
(output / 'meta.bin').unlink()
converter.write_toc(str(output), [(2, '第二章'), (1, '第一章')])
lines = [f'Reference commit: {SHA}', f'Original converter SHA256: {hashlib.sha256(original).hexdigest()}', 'Inputs: generate.py (literal rectangles, strings and index records).', 'Regenerate: python3 test/manga_format/fixtures/generate.py', 'No optional image/OCR packages or network calls are used.', '']
for path in sorted(output.iterdir()):
    if path.suffix in ('.bin', '.idx', '.dat'):
        lines.append(f'{path.name}: {len(path.read_bytes())} bytes, SHA256 {hashlib.sha256(path.read_bytes()).hexdigest()}')
(output / 'README.txt').write_text('\n'.join(lines) + '\n')
