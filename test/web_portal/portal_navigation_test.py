import runpy
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXPECTED_PAGES = {"home", "files", "anki", "settings", "fonts"}


class PortalNavigationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.preview = runpy.run_path(str(ROOT / "scripts/preview_web.py"))

    def test_portal_exposes_anki_without_manga_navigation_or_route(self):
        self.assertEqual(set(self.preview["PAGES"]), EXPECTED_PAGES)
        self.assertNotIn("/manga", self.preview["ROUTE_TO_SLUG"])
        for slug in self.preview["PAGES"]:
            html = self.preview["render_page"](slug)
            self.assertNotIn('href="/manga"', html)
            self.assertIn('href="/anki"', html)


if __name__ == "__main__":
    unittest.main()
