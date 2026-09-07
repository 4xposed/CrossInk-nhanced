"""Real completion modal methods: pending state retains an interactive retry owner."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from extract_functions import extract
ROOT = Path(__file__).resolve().parents[2]
class CompletionLifecycleTest(unittest.TestCase):
    def test_failed_modal_blocks_suspension_without_hidden_replays(self):
        header = (ROOT/'src/activities/home/BookCompletionActivity.h').read_text()
        source = (ROOT/'src/activities/home/BookCompletionActivity.cpp').read_text()
        program = r"""
#include <cassert>
#include <functional>
#include <string>
#include "activities/home/BookCompletionEdit.h"
struct ActivityResult { bool isCancelled = false; };
struct OptionSelectionResult { uint8_t index; };
struct RenderLock { template<class T> explicit RenderLock(T&) {} };
struct Input { bool retry = false, dismiss = false; };
struct OptionPopup {
 bool active = false; std::function<void(int)> selected;
 void show(const char*,const char* const*,int,int,std::function<void(int)> callback) { active=true; selected=callback; }
 void setPrimaryOptionIndex(int) {}
 bool handleInput(Input& input,std::function<void()>) {
  if (input.dismiss) { input.dismiss=false;active=false;return false; }
  if (input.retry) { input.retry=false;active=false;selected(0);return true; }
  return active;
 }
 bool processRender(int,Input&) { return active; }
};
constexpr int STR_RETRY=1,STR_STATS_SAVE_FAILED=2;
const char* tr(int) { return "message"; }
struct Activity {
 int renderer=0,finishes=0; Input mappedInput;
 virtual void onEnter() {} virtual void onExit() {} virtual void loop() {} virtual void render(RenderLock&&) {}
 virtual bool prepareToSuspend() { return true; } virtual bool cancelSuspensionOnFailure() const { return false; }
 virtual bool preventAutoSleep() { return false; } virtual bool blocksGlobalInput() const { return false; }
 virtual bool allowGlobalHomeGesture() const { return true; }
 void requestUpdate() {} void finish() { ++finishes; }
 void setResult(ActivityResult&&) {} void setResult(OptionSelectionResult&&) {}
};
static int attempts = 0; static bool saveWorks = false;
namespace BookActions {
bool toggleBookCompleted(const std::string&,const std::string&,bool& completed,CompletionEdit& edit) {
 ++attempts; edit.initialized=true; edit.persistence.dirty=!saveWorks; completed=true; return saveWorks;
}
}
class BookCompletionActivity : public Activity {
 std::string path_="/book",displayName_="Book";
 BookActions::CompletionEdit edit_; OptionPopup retryPopup_; bool retryRequested_=false;
 void attempt();void showRetry();
 public: void onEnter() override;void loop() override;void render(RenderLock&&) override;
"""
        for signature in ['bool prepareToSuspend() override', 'bool cancelSuspensionOnFailure() const override',
                          'bool preventAutoSleep() override', 'bool blocksGlobalInput() const override',
                          'bool allowGlobalHomeGesture() const override']:
            program += extract(header,signature)
        program += '};\n'
        for method in ['onEnter()', 'showRetry()', 'attempt()', 'loop()', 'render(RenderLock&&)']:
            program += extract(source,'void BookCompletionActivity::'+method)
        program += r"""
int main() {
 BookCompletionActivity activity;
 activity.onEnter(); assert(attempts==1);
 for (int i=0;i<3;++i) {
  assert(!activity.prepareToSuspend()); assert(activity.cancelSuspensionOnFailure());
  assert(activity.preventAutoSleep()); assert(activity.blocksGlobalInput());
  activity.loop(); assert(attempts==1);
 }
 activity.mappedInput.dismiss=true; activity.loop(); activity.loop();
 assert(activity.finishes==0); assert(attempts==1);
 activity.mappedInput.retry=true; activity.loop(); assert(attempts==2);
 assert(!activity.prepareToSuspend()); assert(activity.finishes==0);
 saveWorks=true; activity.mappedInput.retry=true;activity.loop();
 assert(attempts==3);assert(activity.finishes==1);assert(activity.prepareToSuspend());
 activity.onExit();assert(attempts==3);
}
"""
        with tempfile.TemporaryDirectory(prefix='crossink-completion-lifecycle-') as folder:
            sourcefile=Path(folder)/'test.cpp';sourcefile.write_text(program)
            executable=Path(folder)/'test'
            subprocess.run([shutil.which('c++'),'-std=c++20','-I',str(ROOT/'src'),str(sourcefile),'-o',str(executable)],check=True)
            result=subprocess.run([executable],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__': unittest.main()
