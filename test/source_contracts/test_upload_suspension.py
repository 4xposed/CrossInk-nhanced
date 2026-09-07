"""Compile exact activity suspension hooks with a transfer-owner double.

This isolates the ActivityManager contract: a pending transition suppresses loop,
so only prepareToSuspend can close its main-owner upload. Real WS callbacks and
managed transitions are exercised separately by the localhost simulator gate.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from extract_functions import extract

ROOT = Path(__file__).resolve().parents[2]
class UploadSuspensionTest(unittest.TestCase):
    def test_both_activity_hooks_quiesce_without_ordinary_loop(self):
        definitions = []
        for name in ('CalibreConnectActivity', 'CrossPointWebServerActivity'):
            source = (ROOT / 'src/activities/network' / (name + '.h')).read_text()
            hook = extract(source, 'bool prepareToSuspend() override')
            definitions.append('struct ' + name + ' : Base { std::unique_ptr<Server> webServer; ' + hook + ' };')
        program = r"""
#include <cassert>
#include <memory>
struct Server {
 bool active = true; int cancellations = 0;
 bool hasActiveUpload() const { return active; }
 void cancelActiveUploads() { active = false; ++cancellations; }
};
struct Base { virtual bool prepareToSuspend() = 0; };
""" + '\n'.join(definitions) + r"""
template<class Activity> void check() {
 Activity activity; activity.webServer = std::make_unique<Server>();
 // START has retained a file. Manager/sleep has queued intent and suppresses
 // ordinary input and socket pumping; suspension must not depend on that loop.
 assert(activity.prepareToSuspend());
 assert(!activity.webServer->active);
 assert(activity.prepareToSuspend());
}
int main() { check<CalibreConnectActivity>(); check<CrossPointWebServerActivity>(); }
"""
        with tempfile.TemporaryDirectory(prefix='crossink-upload-suspend-') as folder:
            source = Path(folder) / 'test.cpp'; source.write_text(program)
            executable = Path(folder) / 'test'
            subprocess.run([shutil.which('c++'), '-std=c++20', str(source), '-o', str(executable)], check=True)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

if __name__ == '__main__':
    unittest.main()
