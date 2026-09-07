"""Offline compatibility tests; expected bytes do not use converter packing constants."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = None

SCRIPT = Path(__file__).resolve().parents[2] / 'tools/manga_convert/convert_manga.py'


class ConverterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SCRIPT.is_file():
            spec = importlib.util.spec_from_file_location('manga_converter', SCRIPT)
            cls.converter = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.converter)

    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), 'The host converter has not been ported')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.work = self.root / 'work'
        self.work.mkdir()
        self.out = self.root / 'out'
        self.out.mkdir()
        for patch in [mock.patch('socket.socket.connect', side_effect=AssertionError('Network forbidden')),
                      mock.patch('subprocess.run', side_effect=AssertionError('Subprocess forbidden')),
                      mock.patch.dict('os.environ', {}, clear=True)]:
            patch.start()
            self.addCleanup(patch.stop)
        # Own only these optional-import keys. Restoring the whole module table
        # unloads unrelated native modules imported during a PDF test (PyMuPDF).
        missing = object()
        for name in ('huggingface_hub', 'ultralytics'):
            previous = sys.modules.get(name, missing)
            sys.modules[name] = None
            def restore(name=name, previous=previous):
                if previous is missing:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = previous
            self.addCleanup(restore)
        self.c = self.converter
        for stream in ['stdout', 'stderr']:
            patch = mock.patch.object(sys, stream, io.StringIO())
            patch.start()
            self.addCleanup(patch.stop)

    def test_version_two_utf8_literal_bytes(self):
        page = self.c.encode_page([{'box': [1, 2, 31, 42], 'translation': 'Hi',
                                   'text_blocks': [{'box': [3, 4, 5, 6], 'text': '猫'}]}])
        expected = bytes.fromhex('0100 010002001e002800 0100 0200 4869 0300040005000600 0300 e78cab')
        self.assertEqual(page, expected)
        self.c._write_panel_index(str(self.out), [(0, 29, 480, 800), (29, 2, 10, 20)], [page, b'\0\0'])
        self.assertEqual((self.out / 'panels.idx').read_bytes(), bytes.fromhex(
            '02000000 02000000 00000000 1d000000 e0012003 1d000000 02000000 0a001400'))
        self.assertEqual((self.out / 'panels.dat').read_bytes(), expected + b'\0\0')

    def test_metadata_legacy_and_language_trailer(self):
        self.c.write_meta(str(self.out), '猫', 'A')
        self.assertEqual((self.out / 'meta.bin').read_bytes(), bytes.fromhex('01000000 03000100 e78cab41'))
        self.c.write_meta(str(self.out), '猫', 'A', 'JP')
        self.assertEqual((self.out / 'meta.bin').read_bytes(), bytes.fromhex('01000000 03000100 e78cab41 0200 6a61'))

    def test_toc_sorts_and_inserts_cover(self):
        self.c.write_toc(str(self.out), [(3, '猫'), (1, 'A')])
        self.assertEqual((self.out / 'toc.idx').read_bytes(), bytes.fromhex(
            '01000000 03000000 00000000 0500 436f766572 01000000 0100 41 03000000 0300 e78cab'))

    def test_recursive_natural_and_explicit_order(self):
        source = self.root / 'source'
        for name in ['ch10/1.jpg', 'ch2/10.jpg', 'ch2/2.jpg', '999cover.jpg', '999copyright.png']:
            p = source / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.touch()
        pages = self.c.collect_pages(str(source), str(self.work), None)
        self.assertEqual([Path(p).relative_to(source).as_posix() for p in pages],
                         ['999cover.jpg', '999copyright.png', 'ch2/2.jpg', 'ch2/10.jpg', 'ch10/1.jpg'])
        order = self.root / 'order.txt'
        order.write_text('ch10/1.jpg\nch2/2.jpg\n', encoding='utf-8')
        pages = self.c.collect_pages(str(source), str(self.work), str(order))
        self.assertEqual([Path(p).relative_to(source).as_posix() for p in pages], ['ch10/1.jpg', 'ch2/2.jpg'])

    def test_cbz_retains_duplicate_basenames_and_rejects_traversal(self):
        archive = self.root / 'book.cbz'
        with zipfile.ZipFile(archive, 'w') as z:
            for name, data in [('ch2/1.jpg', b'two'), ('ch1/1.jpg', b'one'), ('../escape.jpg', b'bad')]:
                z.writestr(name, data)
            z.writestr('ComicInfo.xml', '<ComicInfo><Title>A &amp; B</Title><Writer>W</Writer><LanguageISO>ja</LanguageISO></ComicInfo>')
        pages = self.c.collect_pages(str(archive), str(self.work), None)
        self.assertEqual([Path(p).read_bytes() for p in pages], [b'one', b'two'])
        self.assertFalse((self.work / 'escape.jpg').exists())
        self.assertEqual(self.c.extract_metadata(str(archive), str(self.work)), ('A & B', 'W', 'ja'))

    def test_epub_spine_metadata_and_native_toc(self):
        book = self.root / 'book.epub'
        with zipfile.ZipFile(book, 'w') as z:
            z.writestr('META-INF/container.xml', '<rootfile full-path="OEBPS/book.opf"/>')
            z.writestr('OEBPS/book.opf', '<package><metadata><dc:title>A &amp; B</dc:title><dc:creator>猫</dc:creator><dc:language>ja</dc:language></metadata><manifest><item id="b" href="b.xhtml"/><item href="a.xhtml" id="a"/><item id="nav" properties="nav" href="nav.xhtml"/></manifest><spine><itemref idref="b"/><itemref idref="a"/></spine></package>')
            z.writestr('OEBPS/b.xhtml', '<script src="kobo.js"/><img src="img/10.png#x"/>')
            z.writestr('OEBPS/a.xhtml', '<img src="img/2.png"/>')
            z.writestr('OEBPS/img/10.png', b'first')
            z.writestr('OEBPS/img/2.png', b'second')
            z.writestr('OEBPS/nav.xhtml', '<nav epub:type="toc"><a href="a.xhtml#section">猫 &amp; B</a></nav>')
        pages = self.c.collect_pages(str(book), str(self.work), None)
        self.assertEqual([Path(p).read_bytes() for p in pages], [b'first', b'second'])
        self.assertEqual(self.c.extract_metadata(str(book), str(self.work)), ('A & B', '猫', 'ja'))
        self.assertEqual(self.c._extract_epub_native_toc(str(book), pages, str(self.work)), [(1, '猫 & B')])

    @unittest.skipIf(Image is None, 'Pillow is required for image tests')
    def test_fit_portrait_landscape_no_upscale_and_alpha(self):
        for original, expected in [((960, 1600), (480, 800)), ((1600, 960), (800, 480)), ((100, 200), (100, 200))]:
            self.assertEqual(self.c.fit_to_device(Image.new('L', original), (480, 800)).size, expected)
        transparent = Image.new('RGBA', (2, 2), (0, 0, 0, 0))
        self.assertEqual(self.c.normalize_for_output(transparent).getpixel((0, 0)), (255, 255, 255))

    def run_main(self, source, *args):
        with mock.patch.object(sys, 'argv', ['convert_manga.py', '--input', str(source), '--output-dir', str(self.out), *args]), contextlib.redirect_stdout(io.StringIO()):
            self.c.main()

    @unittest.skipIf(Image is None, 'Pillow is required for image tests')
    def test_mono_pipeline_real_grid_full_resolution_crops_and_ocr_coordinates(self):
        source = self.root / 'source'
        source.mkdir()
        img = Image.new('L', (960, 1600), 0)
        draw = ImageDraw.Draw(img)
        draw.rectangle((460, 0, 499, 1599), fill=255)
        draw.rectangle((0, 780, 959, 819), fill=255)
        img.save(source / '1.png')
        response = {'candidates': [{'content': {'parts': [{'text': json.dumps({
            'blocks': [{'text': '猫', 'bbox_2d': [100, 200, 600, 800]}, {'text': '犬'}], 'translation': 'Hi'})}]}}]}
        # curl is the actual external boundary: request construction/parsing, grid detection,
        # crop transforms, OCR mapping, and writer remain real. No keys are loaded.
        with mock.patch.dict('os.environ', {'GEMINI_API_KEY': 'offline-test-placeholder'}), mock.patch(
                'subprocess.run', return_value=subprocess.CompletedProcess([], 0, stdout=json.dumps(response))):
            self.run_main(source, '--x4', '--mono')
        with Image.open(self.out / 'page_0000.bmp') as page:
            self.assertEqual((page.mode, page.size), ('1', (480, 800)))
        with Image.open(self.out / 'panels/p0_0.bmp') as panel:
            self.assertEqual((panel.mode, panel.size), ('1', (480, 787)))
        data = (self.out / 'panels.dat').read_bytes()
        self.assertEqual(data[:16], bytes.fromhex('0400 f0000000f0009001 0200 0200 4869'))
        # Crop starts at (230,0) in page space, measures 250x410. Normalized box
        # maps to x=280,y=41,width=150,height=205; fallback uses panel x/y/w/h.
        self.assertEqual(data[16:42], bytes.fromhex('180129009600cd00 0300 e78cab f0000000f0009001 0300 e78aac'))
        # Bottom-right crop starts at y=390 (the panel starts at y=400).
        self.assertEqual(struct.unpack_from('<HHHH', data, 96), (280, 431, 150, 205))
        self.assertEqual(struct.unpack_from('<HHHH', data, 109), (240, 400, 240, 400))

    @unittest.skipIf(Image is None, 'Pillow is required for image tests')
    def test_no_ocr_full_page_and_progressive_jpeg_baseline(self):
        source = self.root / 'source'
        source.mkdir()
        Image.new('RGB', (100, 200), 'black').save(source / '1.jpg', progressive=True)
        self.run_main(source, '--no-ocr', '--x4')
        with Image.open(self.out / 'page_0000.jpg') as image:
            self.assertFalse(image.info.get('progressive'))
            self.assertEqual(image.size, (100, 200))
        self.assertFalse((self.out / 'panels').exists())
        self.assertEqual((self.out / 'panels.dat').read_bytes(), bytes.fromhex('0100 000000006400c800 00000000'))

    @unittest.skipUnless(importlib.util.find_spec('fitz') and Image, 'PyMuPDF and Pillow required for PDF integration')
    def test_pdf_rasterization_order_and_metadata(self):
        import fitz
        source = self.root / 'book.pdf'
        with fitz.open() as doc:
            doc.new_page(width=100, height=200)
            doc.new_page(width=150, height=250)
            doc.set_metadata({'title': 'Test book', 'author': 'Test author'})
            doc.save(source)
        pages = self.c.collect_pages(str(source), str(self.work), None)
        self.assertEqual([Path(p).name for p in pages], ['pdfpage_0000.png', 'pdfpage_0001.png'])
        for path, size in zip(pages, [(200, 400), (300, 500)]):
            with Image.open(path) as image:
                self.assertEqual(image.size, size)
        self.assertEqual(self.c.extract_metadata(str(source), str(self.work)), ('Test book', 'Test author', ''))


if __name__ == '__main__':
    unittest.main()
