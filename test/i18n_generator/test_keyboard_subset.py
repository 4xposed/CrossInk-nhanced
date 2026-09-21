from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class KeyboardSubsetTest(unittest.TestCase):
    def test_old_masks_and_language_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'FreeInkUI.h').write_text('''#pragma once
namespace freeink { namespace ui {
enum class KeyboardLayoutId { QwertyEn, AzertyFr, QwertzDe, SpanishEs,
CyrillicRu, CyrillicUk, CyrillicBe, CyrillicKk, HebrewIl };
}}
''')
            (tmp / 'CrossPointSettings.h').write_text('''#pragma once
#include <cstdint>
struct SettingsStub { uint16_t keyboardLayouts = 0; };
extern SettingsStub settingsStub;
#define SETTINGS settingsStub
''')
            (tmp / 'check.cpp').write_text('''#include "KeyboardLayoutSet.h"
#include "CrossPointSettings.h"
#include <cassert>
SettingsStub settingsStub;
int main() {
 using namespace keyboard_layouts;
 using Id = freeink::ui::KeyboardLayoutId;
 assert(COUNT == 3);
 I18N.setLanguage(Language::DE);
 SETTINGS.keyboardLayouts = 4; // Original German bit, not table index.
 assert(enabled() == 4 && startingLayout() == Id::QwertzDe);
 SETTINGS.keyboardLayouts = 8;
 assert(startingLayout() == Id::SpanishEs);
 SETTINGS.keyboardLayouts = 2 | 16 | 256; // Removed layouts only.
 assert(enabled() == 5 && startingLayout() == Id::QwertzDe);
 SETTINGS.keyboardLayouts = 511;
 assert(enabled() == 13);
 assert(next(Id::QwertyEn) == Id::QwertzDe);
 assert(next(Id::QwertzDe) == Id::SpanishEs);
 assert(next(Id::SpanishEs) == Id::QwertyEn);
 SETTINGS.keyboardLayouts = 0;
 I18N.setLanguage(Language::HU);
 assert(enabled() == 1 && startingLayout() == Id::QwertyEn);
 I18N.setLanguage(Language::JA);
 assert(enabled() == 1 && startingLayout() == Id::QwertyEn);
}
''')
            subprocess.run(['c++', '-std=c++17', '-I'+str(tmp),
                            '-I'+str(ROOT/'lib/I18n'), '-I'+str(ROOT/'src/activities/util'),
                            str(tmp/'check.cpp'), str(ROOT/'src/activities/util/KeyboardLayoutSet.cpp'),
                            str(ROOT/'lib/I18n/I18n.cpp'), str(ROOT/'lib/I18n/I18nStrings.cpp'),
                            '-o', str(tmp/'check')], check=True)
            subprocess.run([str(tmp/'check')], check=True)

if __name__ == '__main__':
    unittest.main()
