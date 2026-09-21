"""Exact sleep preparation/render functions with observable source/codec doubles.

The companion MangaBook tests exercise cancellation inside real index/discovery
loops. Here source operations count complete passes, exposing the integration
bug where fallback restarted them after the same deadline had expired.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from extract_functions import extract
ROOT = Path(__file__).resolve().parents[2]
class SleepCoverIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='crossink-sleep-cover-')
        assets = (ROOT / 'src/activities/boot_sleep/SleepCoverAssets.cpp').read_text()
        sleep = (ROOT / 'src/activities/boot_sleep/SleepActivity.cpp').read_text()
        helpers = ['bool fileExists(', 'int readerFontIdForRenderer(', 'bool isManga(', 'bool mangaCoverCancelled(',
                   'bool mangaFullDimensions(', 'bool prepareFullManga(', 'std::string fullMangaPath(']
        program = r"""
#include <iostream>
#include <string>
#include <memory>
#include <cstdlib>
#include "SleepCoverAssets.h"
#define LOG_ERR(...)
#define LOG_DBG(...)
static bool expired = false, expireDiscovery = false, expireConversion = false;
static int sourcePasses = 0, dimensionCalls = 0, defaults = 0, rendered = 0;
struct Budget { CooperativeCancellation cancellation() { return {[](void*) { return expired; }, nullptr}; } };
struct CrossPointSettings {
 enum class SLEEP_SCREEN_MODE { COVER, COVER_CUSTOM };
 enum class SLEEP_SCREEN_COVER_FILTER { NO_FILTER };
 SLEEP_SCREEN_COVER_FILTER sleepScreenCoverFilter = SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
 enum class SLEEP_SCREEN_COVER_MODE { CROP, FIT };
 SLEEP_SCREEN_MODE sleepScreen = SLEEP_SCREEN_MODE::COVER;
 SLEEP_SCREEN_COVER_MODE sleepScreenCoverMode = SLEEP_SCREEN_COVER_MODE::FIT;
 int getReaderFontId() { return 0; }
} SETTINGS;
struct { std::string openEpubPath; } APP_STATE;
struct GfxRenderer { bool supportsAbsoluteGrayscale() const { return false; } int getScreenWidth() const { return 480; } int getScreenHeight() const { return 800; } };
struct FsFile { void close() {} };
struct { bool exists(const char*) { return true; }
 bool openFileForRead(const char*, const std::string&, FsFile&) { return true; }
} Storage;
namespace FsHelpers {
 bool hasEpubExtension(const std::string&) { return false; }
 bool hasXtcExtension(const std::string&) { return false; }
 bool hasTxtExtension(const std::string&) { return false; }
 bool hasMarkdownExtension(const std::string&) { return false; }
 bool hasBmpExtension(const char*) { return false; }
}
struct Epub {
 enum class XLocationLoadMode { Skip };
 Epub(const std::string&, const char*) {}
 bool load(bool, bool, XLocationLoadMode) { return true; }
 bool generateCoverBmp(bool,const GfxRenderer*,int,int) { return true; }
 std::string getCoverBmpPath(bool,int) { return "cache.bmp"; }
};
struct Xtc {
 Xtc(const std::string&,const char*) {} bool load() { return true; }
 bool generateCoverBmp() { return true; } std::string getCoverBmpPath() { return "cache.bmp"; }
};
struct Txt { Txt(const std::string&,const char*) {} bool generateCoverBmp(int) { return true; }
 std::string getCoverBmpPath(int) { return "cache.bmp"; } };
namespace manga {
 enum class PathResult { Found, Missing, Error };
 enum class OpenMode { Cover };
 class MangaBook { public:
  static bool isMangaFolder(const char*) { return true; }
  bool open(const char*, OpenMode, CooperativeCancellation = {}) { ++sourcePasses; return true; }
  PathResult pageImagePath(int, char* out, size_t, CooperativeCancellation = {}) {
   std::string("source.jpg").copy(out, 11); if (expireDiscovery) expired = true; return PathResult::Found;
  }
 };
 bool fitThumbnailDimensions(int,int,int,int,int& w,int& h) { w=480;h=800;return true; }
 std::string thumbnailPath(const std::string&,int,int) { return "cache.bmp"; }
 ThumbnailResult generateThumbnailControlled(MangaBook&,const std::string&,int,int,CooperativeCancellation token,
                                             ThumbnailDiagnostics* diagnostics) {
  if (expireConversion) expired = true;
  diagnostics->result = token.requested() ? ThumbnailResult::Cancelled : ThumbnailResult::Cached;
  return diagnostics->result;
 }
}
struct ImageDimensions { int width=0,height=0; };
struct ImageToFramebufferDecoder { bool getDimensions(const std::string&,ImageDimensions& out) {
 ++dimensionCalls; out={480,800}; return true; } };
struct ImageDecoderFactory { static ImageToFramebufferDecoder* getDecoder(const std::string&) {
 static ImageToFramebufferDecoder decoder; return &decoder; } };
enum class BmpReaderError { Ok };
struct Bitmap { Bitmap(FsFile&,bool = false,bool = false) {} BmpReaderError parseHeaders() { return BmpReaderError::Ok; }
 int getWidth() { return 480; } int getHeight() { return 800; } };
template<class T> auto makeUniqueNoThrow(size_t n) { return std::make_unique<T>(n); }
"""
        program += '\n'.join(extract(assets, signature) for signature in helpers)
        program += '\nnamespace SleepCoverAssets {\n'
        program += extract(assets, 'bool prepareFullCoverForPath(')
        program += extract(assets, 'std::string cachedCoverPathFor(')
        program += '\n}\n'
        program += r"""
class SleepActivity { public:
 std::string currentBookPath = "/book";
 GfxRenderer renderer; mutable Budget mangaCoverBudget; mutable manga::ThumbnailDiagnostics mangaCoverDiagnostics;
 void renderCoverSleepScreen() const;
 void renderCustomSleepScreen() const { ++defaults; }
 void renderDefaultSleepScreen() const { ++defaults; }
 bool renderBitmapSleepScreen(Bitmap&) const { ++rendered; return true; }
 void logMangaCoverAttempt(bool) const {}
};
"""
        program += extract(sleep, 'void SleepActivity::renderCoverSleepScreen() const')
        program += r"""
int main(int argc,char** argv) {
 const int scenario = argc > 1 ? std::atoi(argv[1]) : 0;
 expired = scenario == 0; expireDiscovery = scenario == 1; expireConversion = scenario == 3;
 SleepActivity activity; activity.renderCoverSleepScreen();
 std::cout << sourcePasses << ' ' << dimensionCalls << ' ' << defaults << ' ' << rendered;
}
"""
        source = Path(cls.tmp.name)/'test.cpp'; source.write_text(program)
        cls.executable = Path(cls.tmp.name)/'test'
        includes = ['src/activities/boot_sleep', 'lib/MangaPanel', 'lib/CooperativeCancellation', 'lib/GfxRenderer']
        command = [shutil.which('c++'), '-std=c++20', str(source), '-o', str(cls.executable)]
        for directory in includes: command += ['-I',str(ROOT/directory)]
        subprocess.run(command,check=True)
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def result(self,scenario):
        return tuple(map(int,subprocess.check_output([self.executable,str(scenario)],text=True).split()))
    def test_expired_budget_starts_no_source_pass_and_renders_default(self):
        self.assertEqual(self.result(0),(0,0,1,0))
    def test_expiry_during_discovery_stops_before_codec_and_uses_default(self):
        self.assertEqual(self.result(1),(1,0,1,0))
    def test_success_retains_prepared_cache_path_without_second_source_pass(self):
        self.assertEqual(self.result(2),(1,1,0,1))
    def test_conversion_cancellation_uses_default_without_second_source_pass(self):
        self.assertEqual(self.result(3),(1,1,1,0))
if __name__ == '__main__': unittest.main()
