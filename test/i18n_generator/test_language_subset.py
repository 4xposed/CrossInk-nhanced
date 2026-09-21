import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('gen_i18n', ROOT / 'scripts/gen_i18n.py')
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


class LanguageSubsetTest(unittest.TestCase):
    def test_legacy_indices_keep_their_meaning_after_removal(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'keys.h'
            gen.generate_keys_header(['EN', 'ES', 'DE', 'HU', 'JA'],
                                     ['English', 'Español', 'Deutsch', 'Magyar', '日本語'],
                                     ['STR_TEST'], str(output))
            text = output.read_text()
        table = re.search(r'V1_LANGUAGES\[\] = \{(.*?)\};', text, re.S).group(1)
        codes = re.findall(r'Language::(\w+)', table)
        self.assertEqual(len(codes), 22)
        self.assertEqual(codes[0], 'EN')
        self.assertEqual(codes[1], 'ES')
        self.assertEqual(codes[3], 'DE')
        self.assertEqual(codes[19], 'HU')
        for index in set(range(22)) - {0, 1, 3, 19}:
            self.assertEqual(codes[index], 'EN', index)

    def test_selected_languages_and_removed_code_fallback(self):
        source = r'''#include "I18n.h"
#include <cassert>
#include <cstring>
int main() {
  assert(getLanguageCount() == 5);
  for (const char* code : {"EN", "ES", "DE", "HU", "JA"}) {
    auto language = I18n::languageFromCode(code);
    assert(strcmp(LANGUAGE_CODES[static_cast<unsigned>(language)], code) == 0);
    I18N.setLanguage(language);
    assert(strlen(tr(STR_LIBRARY)) > 0);
  }
  for (const char* code : {"FR", "RU", "HE", "AR", "unknown"})
    assert(I18n::languageFromCode(code) == Language::EN);
  assert(V1_LANGUAGES[2] == Language::EN);
  assert(V1_LANGUAGES[19] == Language::HU);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'check.cpp'
            cpp.write_text('#include <initializer_list>\n' + source)
            program = Path(directory) / 'check'
            subprocess.run(['c++', '-std=c++17', '-I' + str(ROOT / 'lib/I18n'),
                            str(cpp), str(ROOT / 'lib/I18n/I18n.cpp'),
                            str(ROOT / 'lib/I18n/I18nStrings.cpp'), '-o', str(program)], check=True)
            subprocess.run([str(program)], check=True)


if __name__ == '__main__':
    unittest.main()
