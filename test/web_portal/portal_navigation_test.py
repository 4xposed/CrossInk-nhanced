import runpy
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_PAGES = {"home", "files", "anki", "settings", "fonts"}


class PortalNavigationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.preview = runpy.run_path(str(ROOT / "scripts/preview_web.py"))

    def test_files_loads_chapter_optimizer_before_uploader(self):
        html = self.preview["render_page"]("files")
        self.assertLess(html.index("window.EpubChapters="), html.index("async function uploadFile()"))
        build = (ROOT / "scripts/build_web.py").read_text()
        self.assertIn('("epub-chapters.js", "files.js")', build)

    def test_portal_exposes_anki_without_manga_navigation_or_route(self):
        self.assertEqual(set(self.preview["PAGES"]), EXPECTED_PAGES)
        self.assertNotIn("/manga", self.preview["ROUTE_TO_SLUG"])
        for slug in self.preview["PAGES"]:
            html = self.preview["render_page"](slug)
            self.assertNotIn('href="/manga"', html)
            self.assertIn('href="/anki"', html)


if __name__ == "__main__":
    unittest.main()
