#ifdef SIMULATOR

#include "SimulatorSmokeTest.h"

#include <AnkiDeck.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MangaBook.h>
#include <MangaCover.h>
#include <ReviewStateStore.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>
#include <vector>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DeviceCapabilities.h"
#include "GlobalActions.h"
#include "MangaStatusSmoke.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/BookActions.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/BookStatsView.h"
#include "activities/reader/EpubReaderMenuActivity.h"
#include "activities/reader/EpubReaderWordLookupActivity.h"
#include "activities/reader/MangaPrefetch.h"
#include "activities/reader/MangaProgressStore.h"
#include "activities/reader/MangaReaderActivity.h"
#include "activities/reader/MangaTranslationActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/reader/ReaderOptionsActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/settings/QuickActionsActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "simulator/SimulatorHomeKeyInput.h"
#include "util/LookupHistory.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {

enum class SmokeStep : uint8_t {
  Start,
  Home,
  OpdsBrowser,
  OpdsSelectBook,
  OpdsDownloadRelease,
  OpdsOpen,
  OpdsOpenRelease,
  OpdsExit,
  OpdsExitBrowser,
  OpdsExitRelease,
  OpdsExitCheck,
  OpdsExitHome,
  FileBrowser,
  FileBrowserSettings,
  RecentBooks,
  Settings,
  ReaderOptions,
  ReaderMenu,
  Sleep,
  OpenDeckFromBooks,
  Reader,
  ReaderInput,
  Done,
};

class SimulatorSmokeTest {
 public:
  void tick() {
    if (!enabled()) return;

    try {
      tickImpl();
    } catch (const std::exception& e) {
      fail("Unhandled exception: %s", e.what());
    } catch (...) {
      fail("Unhandled non-standard exception");
    }
  }

 private:
  enum class ScriptActionType : uint8_t {
    Press,
    Release,
    HomeTap,
    HomeLongPress,
    ConfigureHomeButtonPowerLock,
    WaitForPowerLongPress,
    AssertHomeButtonDisabled,
    AssertHomeButtonEnabled,
    AssertTouchscreenDisabled,
    AssertTouchscreenEnabled,
    OpenSmokeBook,
    OpenBooks,
    ManualReaderRefresh,
    MangaAutoEvents,
    MangaTouchPageTo,
    MangaTouchMenuGesture,
    MangaQrContracts,
    MangaReviewInterleave,
    MangaCacheContracts,
    MangaMenuContracts,
    MangaRenderFailure,
    MangaStatusPlanes,
    MangaMenuGoHome,
    MangaMenuReset,
    MangaTouchOptionDown,
    MangaTouchOptionRelease,
    MangaCheckMenuAction,
    MangaCheckAuto,
    MangaShortcutLookup,
    MangaPrefetchDwell,
    MangaPrefetchPush,
    MangaPrefetchReplace,
    MangaPrefetchPop,
    MangaPrefetchSleep,
    MangaBoundaryJump,
    MangaQueueCurrentSource,
    MangaHoldConsumption,
    MangaWaitCompletion,
    MangaReleaseCompletion,
    MangaAssertPosition,
    MangaLookupReady,
    MangaAssertScanCache,
    MangaOpenMenu,
    MangaTranslationPage,
    MangaRememberFont,
    MangaAssertHistory,
    MangaAssertFeedback,
    MangaAssertEmptyTranslation,
    MangaLookupUnavailable,
    MangaOpenFixture,
    MangaHideDictionaries,
    MangaRestoreDictionaries,
    MangaForceLookupExit,
    MangaDuplicateMenu,
    MangaConfirmOnCompletion,
    DisableReaderTouch,
    EnableReaderTouch,
    SetLookupPowerShortcut,
    ResetPowerShortcut,
    TouchDown,
    TouchMove,
    TouchRelease,
    TouchButtonDown,
    TouchButtonRelease,
    AssertActivity,
    AssertAnkiNextCandidate,
    AssertMangaBookmark,
    AssertMangaLibrary,
    OpenMangaLanguageStats,
    AssertMangaLanguageStats,
    MangaStatsSaveFailure,
    OpenMangaRecents,
    MangaReadingDwell,
    MangaFinalPageDwell,
    Render
  };

  struct ScriptAction {
    ScriptActionType type;
    MappedInputManager::Button button;
    const char* label;
    int settleFrames;
    int x;
    int y;
  };

  SmokeStep step = SmokeStep::Start;
  int settleFrames = 0;
  const char* activeStepName = nullptr;
  std::vector<ScriptAction> inputScript;
  size_t scriptIndex = 0;
  SmokeStep inputCompletionStep = SmokeStep::Done;
  unsigned long mangaPrefetchDwellAt = 0;
  int mangaReaderFontId = 0, mangaReaderFontWidth = 0, mangaReaderLineHeight = 0;
  int mangaReaderOrientation = 0;
  uint32_t mangaReaderImageHash = 0;
  ReadingStatsDate mangaStatsStartBefore;

  int touchButtonX = 0;
  int touchButtonY = 0;
  static bool enabled() { return std::getenv("CROSSINK_SIMULATOR_SMOKE_TEST") != nullptr; }

  static int pageTurnCount() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS");
    if (raw == nullptr || raw[0] == '\0') {
      return 2;
    }
    return std::max(0, std::atoi(raw));
  }

  static bool landscapeReaderRequested() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_LANDSCAPE_READER");
    return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
  }

  static bool isAnkiDeckSmokeBook() {
    const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
    return bookPath != nullptr && FsHelpers::hasAnkiDeckExtension(bookPath);
  }

  static bool isMangaSmokeBook() {
    const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
    return bookPath != nullptr && bookPath[0] != '\0' && manga::MangaBook::isMangaFolder(bookPath);
  }

  static void applyRequestedTheme() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_THEME");
    if (raw == nullptr || raw[0] == '\0') {
      return;
    }

    const int theme = std::atoi(raw);
    if (theme < 0 || theme >= CrossPointSettings::UI_THEME_COUNT) {
      fail("Invalid smoke test theme index: %d", theme);
    }

    SETTINGS.uiTheme = static_cast<uint8_t>(theme);
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Using theme index %d", theme);
  }

  static void verifyMixedPageGestures() {
#if CROSSINK_APP_CAP_TOUCH
    if (!gpio.hasTouch()) return;
    const uint8_t savedNext = SETTINGS.pageTurnGesture;
    const uint8_t savedPrevious = SETTINGS.previousPageGesture;
    const int width = renderer.getScreenWidth();
    const int y = renderer.getScreenHeight() / 2;
    mappedInputManager.setReaderMode(true);
    for (uint8_t next = 0; next < CrossPointSettings::PAGE_TURN_GESTURE_COUNT; ++next) {
      for (uint8_t previous = 0; previous < CrossPointSettings::PAGE_TURN_GESTURE_COUNT; ++previous) {
        SETTINGS.pageTurnGesture = next;
        SETTINGS.previousPageGesture = previous;
        const bool inverted = next == CrossPointSettings::INVERTED_TAP || previous == CrossPointSettings::INVERTED_TAP;
        const bool nextTap = next == CrossPointSettings::TAP_AND_SWIPE || next == CrossPointSettings::TAP_ONLY ||
                             next == CrossPointSettings::INVERTED_TAP;
        const bool previousTap = previous == CrossPointSettings::TAP_AND_SWIPE ||
                                 previous == CrossPointSettings::TAP_ONLY ||
                                 previous == CrossPointSettings::INVERTED_TAP;
        for (const int x : {0, width / 3 - 1, width / 3, width * 2 / 3 - 1, width * 2 / 3, width - 1}) {
          mappedInputManager.simulatorInjectTouchDown(x, y);
          mappedInputManager.simulatorInjectTouchRelease(x, y);
          const auto result = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
          const bool nextZone = inverted ? x < width * 2 / 3 : x >= width / 3;
          const bool expectedNext = nextTap && (!previousTap || nextZone);
          const bool expectedPrevious = previousTap && (!nextTap || !nextZone);
          if (!result.tapped || result.next != expectedNext || result.prev != expectedPrevious) {
            fail("Mixed page tap mismatch: next=%u previous=%u x=%d", next, previous, x);
          }
          mappedInputManager.simulatorClearInputFrame();
        }
        for (const bool right : {false, true}) {
          const int startX = right ? 1 : width - 2;
          const int endX = right ? width - 2 : 1;
          mappedInputManager.simulatorInjectTouchDown(startX, y);
          mappedInputManager.simulatorInjectTouchMove(endX, y);
          mappedInputManager.simulatorInjectTouchRelease(endX, y);
          const auto result = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
          const uint8_t mode = right ? previous : next;
          const bool expected = (mode == CrossPointSettings::TAP_AND_SWIPE || mode == CrossPointSettings::SWIPE_ONLY);
          if (result.next != (!right && expected) || result.prev != (right && expected) ||
              (right && !expected && mappedInputManager.wasReleased(MappedInputManager::Button::Back))) {
            fail("Mixed page swipe mismatch: next=%u previous=%u right=%d", next, previous, right);
          }
          mappedInputManager.simulatorClearInputFrame();
        }
      }
    }
    SETTINGS.pageTurnGesture = savedNext;
    SETTINGS.previousPageGesture = savedPrevious;
    mappedInputManager.setReaderMode(false);
    LOG_INF("SMOKE", "All 25 mixed page gesture combinations passed");
#endif
  }

  static void verifyReaderControlsSettings() {
    const auto& base = getBaseSettingsList();
    if (base.size() > BASE_SETTINGS_CAPACITY || base.capacity() < BASE_SETTINGS_CAPACITY) {
      fail("Base settings allocation mismatch: size=%zu capacity=%zu", base.size(), base.capacity());
    }
    const auto all = getSettingsList();
    const auto gestures = buildControlsTapsGesturesSettingsList(all);
    if (gpio.hasTouch()) {
      if (gestures.size() < 3 || gestures[0].nameId != StrId::STR_NEXT_PAGE ||
          gestures[1].nameId != StrId::STR_PREV_PAGE || gestures[0].enumValues != gestures[1].enumValues) {
        fail("Page gesture settings order/options mismatch");
      }
      if (gpio.supportsMultiTouch() && (gestures.size() < 4 || gestures[3].nameId != StrId::STR_TWO_FINGER_ROTATION)) {
        fail("Two-finger rotation gesture setting order mismatch");
      }
      const size_t statusIndex = gpio.supportsMultiTouch() ? 4 : 2;
      if (gestures.size() <= statusIndex || gestures[statusIndex].nameId != StrId::STR_TAP_HIDE_STATUS_BAR) {
        fail("Status bar gesture setting order mismatch");
      }
    } else if (!gestures.empty()) {
      fail("Touch gestures exposed on a button-only device");
    }
    const auto device = buildSystemDeviceSettingsList(all);
    if (device.size() < 3 || device[1].nameId != StrId::STR_TIME_TO_SLEEP ||
        device[2].nameId != StrId::STR_CUSTOM_BOOTSCREEN) {
      fail("Custom bootscreen setting order mismatch");
    }
    JsonDocument original;
    SETTINGS.toJson(original);
    for (uint8_t mode = 0; mode <= CrossPointSettings::PAGE_TURN_GESTURE_DISABLED; ++mode) {
      JsonDocument legacy;
      legacy["pageTurnGesture"] = mode;
      SETTINGS.fromJson(legacy.as<JsonVariantConst>());
      if (SETTINGS.pageTurnGesture != mode || SETTINGS.previousPageGesture != mode) {
        fail("Legacy page gesture migration mismatch");
      }
    }
    constexpr uint8_t importedGestures[] = {CrossPointSettings::TAP_AND_SWIPE, CrossPointSettings::TAP_ONLY,
                                            CrossPointSettings::SWIPE_ONLY, CrossPointSettings::INVERTED_TAP};
    for (uint8_t mode = 0; mode < 4; ++mode) {
      JsonDocument crosspoint;
      crosspoint["touchReaderControls"] = mode;
      crosspoint["disableReaderTouchscreen"] = 1;
      SETTINGS.fromJson(crosspoint.as<JsonVariantConst>(), true);
      if (SETTINGS.disableReaderTouchscreen || SETTINGS.touchReaderControls != (mode != 0) ||
          SETTINGS.pageTurnGesture != importedGestures[mode] ||
          SETTINGS.previousPageGesture != importedGestures[mode]) {
        fail("CrossPoint touch settings migration mismatch");
      }
      JsonDocument migrated;
      SETTINGS.toJson(migrated);
      SETTINGS.disableReaderTouchscreen = 1;
      SETTINGS.pageTurnGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
      SETTINGS.previousPageGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
      SETTINGS.fromJson(migrated.as<JsonVariantConst>());
      if (SETTINGS.disableReaderTouchscreen || SETTINGS.touchReaderControls != (mode != 0) ||
          SETTINGS.pageTurnGesture != importedGestures[mode] ||
          SETTINGS.previousPageGesture != importedGestures[mode]) {
        fail("Migrated CrossPoint touch settings did not survive reload");
      }
    }
    // The namespaced file must preserve intentional locks, including older files without gesture keys.
    JsonDocument locked;
    locked["touchReaderControls"] = 1;
    locked["disableReaderTouchscreen"] = 1;
    SETTINGS.fromJson(locked.as<JsonVariantConst>());
    if (!SETTINGS.disableReaderTouchscreen) fail("CrossInk touch lock was lost");
    locked["pageTurnGesture"] = CrossPointSettings::TAP_ONLY;
    locked["previousPageGesture"] = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    SETTINGS.fromJson(locked.as<JsonVariantConst>(), true);
    if (!SETTINGS.disableReaderTouchscreen || SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::PAGE_TURN_GESTURE_DISABLED) {
      fail("Legacy CrossInk gesture settings were treated as CrossPoint");
    }
    SETTINGS.previousPageGesture = CrossPointSettings::SWIPE_ONLY;
    SETTINGS.pageTurnGesture = CrossPointSettings::TAP_ONLY;
    SETTINGS.customBootscreenEnabled = 0;
    SETTINGS.tapToHideStatusBar = 0;
    JsonDocument saved;
    SETTINGS.toJson(saved);
    SETTINGS.previousPageGesture = CrossPointSettings::TAP_AND_SWIPE;
    SETTINGS.customBootscreenEnabled = 1;
    SETTINGS.tapToHideStatusBar = 1;
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY || SETTINGS.customBootscreenEnabled ||
        SETTINGS.tapToHideStatusBar) {
      fail("Reader controls settings round-trip mismatch");
    }
    SETTINGS.fromJson(original.as<JsonVariantConst>());
  }

  static void verifyUpDownShortcutAvailability() {
    const auto allSettings = getSettingsList();
    const auto sideButtonSettings = buildControlsSideButtonSettingsList(allSettings);
    const bool hasSideButtonChord =
        std::any_of(sideButtonSettings.begin(), sideButtonSettings.end(),
                    [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SIDE_BUTTON_CHORD; });
    if (hasSideButtonChord != deviceSupportsSideButtonChord(gpio)) {
      fail("Side-button chord availability does not match device controls");
    }

    if (QuickActionsActivityTest::isTriggerAvailable(QuickActions::Trigger::UpDown) !=
        deviceSupportsSideButtonChord(gpio)) {
      fail("Quick Actions Up + Down availability does not match device controls");
    }

    const auto chordSetting = std::find_if(allSettings.begin(), allSettings.end(), [](const SettingInfo& setting) {
      return settingKeyIs(setting, "powerChordAction");
    });
    if (chordSetting == allSettings.end()) fail("Power chord setting is missing");
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_SLEEP) != chordSetting->enumRawValues.end()) {
      fail("Sleep is still offered for a chord that cannot wake the device");
    }
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_QUICK_ACTIONS) == chordSetting->enumRawValues.end()) {
      fail("Quick Actions is missing from the Power + Up chord setting");
    }
    if (!gpio.hasHomeKey() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_HOME_BUTTON) != chordSetting->enumRawValues.end()) {
      fail("Toggle Home Button is still offered without a Home key");
    }
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_PREVIOUS_PAGE) == chordSetting->enumRawValues.end()) {
      fail("Previous Page was removed by an unrelated power-button action ID");
    }
    if (!gpio.hasTouch() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_TOUCHSCREEN) != chordSetting->enumRawValues.end()) {
      fail("Toggle Touchscreen is still offered without touch hardware");
    }
    if (!Frontlight.present() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_FRONTLIGHT) != chordSetting->enumRawValues.end()) {
      fail("Toggle Frontlight is still offered without a frontlight");
    }
  }

  static bool verifyWordLookupSideButtonMenuContract() {
    const auto allSettings = getSettingsList();
    const auto sideButtonSettings = buildControlsSideButtonSettingsList(allSettings);
    if (sideButtonSettings.size() != 4 + (gpio.hasTouch() ? 1u : 0u)) return false;

    const SettingInfo& setting = sideButtonSettings[1];
    return settingKeyIs(setting, "wordLookupSideButtons") && setting.nameId == StrId::STR_WORD_LOOKUP_SIDE_BUTTONS &&
           setting.type == SettingType::ENUM && setting.valuePtr == &CrossPointSettings::wordLookupSideButtons &&
           setting.category == StrId::STR_CAT_CONTROLS &&
           setting.enumValues == std::vector<StrId>{StrId::STR_NO, StrId::STR_YES} && setting.enumRawValues.empty();
  }

  static void verifyDictionarySettingsMenuContract() {
    DictionaryRegistry registry;
    if (!registry.discover(/*autoSelectDefault=*/false)) {
      fail("Dictionary settings fixture discovery failed");
    }

    const SettingInfo editable = buildDictionarySetting(&registry, {}, nullptr, false);
    if (editable.nameId != StrId::STR_DICT_FALLBACK || !settingKeyIs(editable, "dictionary") || !editable.valueGetter ||
        !editable.valueSetter) {
      fail("Global dictionary fallback row contract failed");
    }
    const int globalIndex = registry.indexOfExactOrEquivalent("/dictionaries/en/smoke/dict-data");
    if (globalIndex < 0 || static_cast<size_t>(globalIndex + 1) >= editable.enumStringValues.size() ||
        editable.valueGetter() != static_cast<uint8_t>(globalIndex + 1) ||
        editable.enumStringValues[static_cast<size_t>(globalIndex + 1)] != "en/smoke") {
      fail("Global dictionary fallback value contract failed");
    }

    const SettingInfo automaticJapanese = buildDictionarySetting(&registry, "ja-JP", "/smoke-book-cache", true);
    if (automaticJapanese.nameId != StrId::STR_DICTIONARY || automaticJapanese.valueSetter ||
        automaticJapanese.enumStringValues.size() != 1 ||
        automaticJapanese.enumStringValues.front() != tr(STR_DICT_EFFECTIVE_JAPANESE)) {
      fail("Automatic Japanese applied dictionary contract failed");
    }

    const SettingInfo perBookFallback = buildDictionarySetting(&registry, "de-DE", "/smoke-book-cache", true);
    if (perBookFallback.enumStringValues.size() != 1 || perBookFallback.enumStringValues.front() != "fr/book") {
      fail("Per-book applied dictionary fallback contract failed");
    }

    HalFile malformedIndex;
    if (!Storage.openFileForWrite("SMOKE", "/dictionaries/jp/vocab.idx", malformedIndex)) {
      fail("Could not create malformed Japanese settings fixture");
    }
    const uint8_t truncatedRecord = 1;
    malformedIndex.write(&truncatedRecord, sizeof(truncatedRecord));
    malformedIndex.close();
    HalFile emptyData;
    if (!Storage.openFileForWrite("SMOKE", "/dictionaries/jp/vocab.dat", emptyData)) {
      fail("Could not create empty Japanese settings fixture");
    }
    emptyData.close();

    DictionaryRegistry malformedRegistry;
    if (!malformedRegistry.discover(/*autoSelectDefault=*/false)) {
      fail("Malformed Japanese fallback fixture discovery failed");
    }
    const SettingInfo malformedJapaneseFallback =
        buildDictionarySetting(&malformedRegistry, "ja", "/smoke-book-cache", true);
    if (malformedJapaneseFallback.enumStringValues.size() != 1 ||
        malformedJapaneseFallback.enumStringValues.front() != "fr/book") {
      fail("Malformed Japanese applied dictionary fallback contract failed");
    }
    malformedRegistry.clear();
    registry.clear();
  }

  [[noreturn]] static void fail(const char* message) {
    LOG_ERR("SMOKE", "%s", message);
    std::_Exit(2);
  }

  template <typename... Args>
  [[noreturn]] static void fail(const char* format, Args... args) {
    logPrintf("ERR", "SMOKE", format, args...);
    logPrintf("ERR", "SMOKE", "\n");
    std::_Exit(2);
  }

  static void renderCurrentStep(const char* name) {
    LOG_INF("SMOKE", "Rendering %s", name);
    if (activityManager.requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      fail("Render was rejected for %s", name);
    }
  }

  void queueStep(const char* name, SmokeStep nextStep, int framesToSettle = 3) {
    activeStepName = name;
    settleFrames = framesToSettle;
    step = nextStep;
  }

  void tickImpl() {
    mappedInputManager.simulatorClearInputFrame();

    if (settleFrames > 0) {
      --settleFrames;
      if (settleFrames == 0 && activeStepName != nullptr) {
        renderCurrentStep(activeStepName);
        activeStepName = nullptr;
      }
      return;
    }

    switch (step) {
      case SmokeStep::Start:
        LOG_INF("SMOKE", "Starting simulator smoke test");
        if (!CrossPointSettings::verifySleepTimeoutMigrationContract()) {
          fail("Sleep timeout migration contract failed");
        }
        if (!CrossPointSettings::verifySleepScreenMigrationContract()) {
          fail("Sleep screen migration contract failed");
        }
        if (!CrossPointSettings::verifyWordLookupSideButtonsPersistenceContract()) {
          fail("Word Lookup side-button persistence contract failed");
        }
        if (!verifyWordLookupSideButtonMenuContract()) {
          fail("Word Lookup side-button menu contract failed");
        }
        verifyDictionarySettingsMenuContract();
        if (!SimulatorHomeKeyInput::verifyTimingContract()) {
          fail("Simulator Home key timing contract failed");
        }
        verifyUpDownShortcutAvailability();
        verifyReaderControlsSettings();
        verifyMixedPageGestures();
        applyRequestedTheme();
        activityManager.goHome();
        queueStep("Home", SmokeStep::Home);
        break;

      case SmokeStep::Home:
        activityManager.replaceActivity(std::make_unique<OpdsBookBrowserActivity>(
            renderer, mappedInputManager, OpdsServer{"Simulator", "simulator://"}));
        queueStep("OPDS Browser", SmokeStep::OpdsBrowser);
        break;

      case SmokeStep::OpdsBrowser:
        if (!activityManager.isCurrentActivityNamed("OpdsBookBrowser")) fail("OPDS browser did not open");
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Down);
        step = SmokeStep::OpdsSelectBook;
        break;

      case SmokeStep::OpdsSelectBook:
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
        queueStep("OPDS book selected", SmokeStep::OpdsDownloadRelease);
        break;

      case SmokeStep::OpdsDownloadRelease:
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
        step = SmokeStep::OpdsOpen;
        break;

      case SmokeStep::OpdsOpen:
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
        queueStep("OPDS book downloaded", SmokeStep::OpdsOpenRelease);
        break;

      case SmokeStep::OpdsOpenRelease:
        if (!activityManager.isCurrentActivityNamed("OpdsBookBrowser"))
          fail("OPDS download did not return to the browser");
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
        step = SmokeStep::OpdsExit;
        break;

      case SmokeStep::OpdsExit:
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
        queueStep("Reader opened from OPDS", SmokeStep::OpdsExitBrowser, 8);
        break;

      case SmokeStep::OpdsExitBrowser:
        if (!activityManager.isCurrentActivityNamed("EpubReader")) fail("OPDS Open did not start the reader");
        activityManager.replaceActivity(std::make_unique<OpdsBookBrowserActivity>(
            renderer, mappedInputManager, OpdsServer{"Simulator", "simulator://"}));
        queueStep("OPDS Browser before Exit", SmokeStep::OpdsExitRelease);
        break;

      case SmokeStep::OpdsExitRelease:
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Right);
        step = SmokeStep::OpdsExitCheck;
        break;

      case SmokeStep::OpdsExitCheck:
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Right);
        queueStep("Home after OPDS Exit", SmokeStep::OpdsExitHome);
        break;

      case SmokeStep::OpdsExitHome:
        if (!activityManager.isCurrentActivityNamed("Home")) fail("OPDS Exit did not return home");
        activityManager.goToFileBrowser("/books");
        queueStep("File Browser", SmokeStep::FileBrowser);
        break;

      case SmokeStep::FileBrowser:
        if (isAnkiDeckSmokeBook()) {
          LOG_INF("SMOKE", "Opening Anki deck from Books");
#if CROSSINK_APP_CAP_TOUCH
          if (mappedInputManager.hasTouch()) {
            if (!findTouchButtonHint(MappedInputManager::Button::Confirm, touchButtonX, touchButtonY)) {
              fail("Missing touch button hint for Anki Books open");
            }
            mappedInputManager.simulatorInjectTouchDown(touchButtonX, touchButtonY);
          } else
#endif
          {
            mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
          }
          queueStep(nullptr, SmokeStep::OpenDeckFromBooks, 0);
          break;
        }
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouchHardware()) {
          buildFileBrowserInputScript();
          step = SmokeStep::ReaderInput;
          break;
        }
#endif
        activityManager.goToRecentBooks();
        queueStep("Recent Books", SmokeStep::RecentBooks);
        break;

      case SmokeStep::FileBrowserSettings:
        activityManager.goToRecentBooks();
        queueStep("Recent Books", SmokeStep::RecentBooks);
        break;

      case SmokeStep::RecentBooks:
        if (mappedInputManager.hasHomeKey()) {
          renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
        }
        activityManager.goToSettings();
        queueStep(mappedInputManager.hasHomeKey() ? "Settings landscape" : "Settings", SmokeStep::Settings);
        break;

      case SmokeStep::Settings:
        renderer.setOrientation(GfxRenderer::Orientation::Portrait);
        activityManager.replaceActivity(std::make_unique<ReaderOptionsActivity>(renderer, mappedInputManager));
        queueStep("Reader Options", SmokeStep::ReaderOptions);
        break;

      case SmokeStep::ReaderOptions:
        activityManager.replaceActivity(
            std::make_unique<EpubReaderMenuActivity>(renderer, mappedInputManager, "Smoke Test", 1, 1, 0,
                                                     SETTINGS.orientation, false, false, false, false, false, false));
        queueStep("Reader Menu", SmokeStep::ReaderMenu);
        break;

      case SmokeStep::ReaderMenu:
        activityManager.goToSleep();
        queueStep("Sleep", SmokeStep::Sleep);
        break;

      case SmokeStep::Sleep: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') {
          LOG_INF("SMOKE", "Skipping Reader step; CROSSINK_SIMULATOR_SMOKE_BOOK is not set");
          step = SmokeStep::Reader;
          break;
        }
        if (!Storage.exists(bookPath)) {
          fail("Smoke test book is missing: %s", bookPath);
        }
        if (landscapeReaderRequested()) {
          SETTINGS.orientation = CrossPointSettings::LANDSCAPE_CCW;
          LOG_INF("SMOKE", "Opening smoke reader in landscape");
        }
        activityManager.goToReader(bookPath, true);
        queueStep("Reader", SmokeStep::Reader, 8);
        break;
      }

      case SmokeStep::OpenDeckFromBooks:
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouch()) {
          mappedInputManager.simulatorInjectTouchRelease(touchButtonX, touchButtonY);
        } else
#endif
        {
          mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
        }
        queueStep("Anki review opened from Books", SmokeStep::Reader, 8);
        break;
      case SmokeStep::Reader:
        buildReaderInputScript();
        step = SmokeStep::ReaderInput;
        break;

      case SmokeStep::ReaderInput:
        runReaderInputScript();
        break;

      case SmokeStep::Done:
        LOG_INF("SMOKE", "Simulator smoke test passed");
        std::_Exit(0);
    }
  }

  static ScriptAction press(MappedInputManager::Button button) {
    return {ScriptActionType::Press, button, nullptr, 0, 0, 0};
  }

  static ScriptAction release(MappedInputManager::Button button) {
    return {ScriptActionType::Release, button, nullptr, 0, 0, 0};
  }

  static ScriptAction homeTap() {
    return {ScriptActionType::HomeTap, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction homeLongPress() {
    return {ScriptActionType::HomeLongPress, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction configureHomeButtonPowerLock() {
    return {ScriptActionType::ConfigureHomeButtonPowerLock, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction waitForPowerLongPress() {
    return {ScriptActionType::WaitForPowerLongPress, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertHomeButtonDisabled() {
    return {ScriptActionType::AssertHomeButtonDisabled, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertHomeButtonEnabled() {
    return {ScriptActionType::AssertHomeButtonEnabled, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenDisabled() {
    return {ScriptActionType::AssertTouchscreenDisabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenEnabled() {
    return {ScriptActionType::AssertTouchscreenEnabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction openSmokeBook() {
    return {ScriptActionType::OpenSmokeBook, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction disableReaderTouch() {
    return {ScriptActionType::DisableReaderTouch, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction enableReaderTouch() {
    return {ScriptActionType::EnableReaderTouch, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction setLookupPowerShortcut() {
    return {ScriptActionType::SetLookupPowerShortcut, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction resetPowerShortcut() {
    return {ScriptActionType::ResetPowerShortcut, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction openBooks() {
    return {ScriptActionType::OpenBooks, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction manualReaderRefresh() {
    return {ScriptActionType::ManualReaderRefresh, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction render(const char* label, int framesToSettle = 3) {
    return {ScriptActionType::Render, MappedInputManager::Button::Back, label, framesToSettle, 0, 0};
  }

#if CROSSINK_APP_CAP_TOUCH
  static ScriptAction touchDown(const int x, const int y) {
    return {ScriptActionType::TouchDown, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
  static ScriptAction touchMove(const int x, const int y) {
    return {ScriptActionType::TouchMove, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
  static ScriptAction touchRelease(const int x, const int y) {
    return {ScriptActionType::TouchRelease, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
#endif

  static ScriptAction assertActivity(const char* name) {
    return {ScriptActionType::AssertActivity, MappedInputManager::Button::Back, name, 0, 0, 0};
  }

  static ScriptAction assertAnkiNextCandidate() {
    return {ScriptActionType::AssertAnkiNextCandidate, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction touchButtonDown(const MappedInputManager::Button button) {
    return {ScriptActionType::TouchButtonDown, button, nullptr, 0, 0, 0};
  }

  static ScriptAction touchButtonRelease(const MappedInputManager::Button button) {
    return {ScriptActionType::TouchButtonRelease, button, nullptr, 0, 0, 0};
  }

  void addTap(MappedInputManager::Button button) {
    inputScript.push_back(press(button));
    inputScript.push_back(release(button));
  }

  void addAnkiTap(const MappedInputManager::Button button) {
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouch()) {
      inputScript.push_back(touchButtonDown(button));
      inputScript.push_back(touchButtonRelease(button));
      return;
    }
#endif
    addTap(button);
  }

#if CROSSINK_APP_CAP_TOUCH
  int touchTargetIdFor(const MappedInputManager::Button button) const {
    const bool readerMapping = SETTINGS.readerFrontButtonsEnabled != 0;
    switch (button) {
      case MappedInputManager::Button::Back:
        return readerMapping ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
      case MappedInputManager::Button::Confirm:
        return readerMapping ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
      case MappedInputManager::Button::Left:
        return readerMapping ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
      case MappedInputManager::Button::Right:
        return readerMapping ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;
      default:
        return -1;
    }
  }

  bool findTouchButtonHint(const MappedInputManager::Button button, int& x, int& y) const {
    const int targetId = touchTargetIdFor(button);
    if (targetId < 0) return false;

    for (int row = 0; row < renderer.getScreenHeight(); ++row) {
      for (int column = 0; column < renderer.getScreenWidth(); ++column) {
        int hitId = -1;
        if (TouchRegistry::getInstance().hitTest(column, row, TouchRegistry::Kind::Button, hitId) &&
            hitId == targetId) {
          x = column;
          y = row;
          return true;
        }
      }
    }
    return false;
  }
#endif

  void buildAnkiDeckInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::Done;

    inputScript.push_back(assertActivity("AnkiReview"));
    addAnkiTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Anki answer revealed", 4));
    inputScript.push_back(assertActivity("AnkiReview"));
    addAnkiTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Anki next card after Good", 4));
    inputScript.push_back(assertActivity("AnkiReview"));
    addAnkiTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Home after Anki exit", 4));
    inputScript.push_back(assertActivity("Home"));
    inputScript.push_back(assertAnkiNextCandidate());
    inputScript.push_back(openBooks());
    inputScript.push_back(render("Books reopened for Anki", 4));
    addAnkiTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Anki review reopened on next card", 8));
    inputScript.push_back(assertActivity("AnkiReview"));
    addAnkiTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Home after reopened Anki exit", 4));
    inputScript.push_back(assertActivity("Home"));
  }

  void addMangaTouchMenuGesture() {
    for (int phase = 0; phase < 3; ++phase)
      inputScript.push_back({ScriptActionType::MangaTouchMenuGesture, {}, nullptr, 0, phase, 0});
  }
  void buildMangaActiveEvents() {
    const int count = mappedInputManager.hasTouchHardware() ? 6 : 5;
    for (int event = 0; event < count; ++event) {
      inputScript.push_back({ScriptActionType::MangaMenuGoHome, {}, nullptr, 0, 0, 0});
      inputScript.push_back(render("Home before active cancellation", 3));
      inputScript.push_back({ScriptActionType::MangaMenuReset, {}, nullptr, 0, 0, -1});
      inputScript.push_back(openSmokeBook());
      inputScript.push_back(render("Manga active cancellation fixture", 4));
      inputScript.push_back({ScriptActionType::MangaOpenMenu, {}, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga active cancellation menu", 3));
      for (int i = 0; i < 12; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga active cancellation rate", 3));
      for (int i = 0; i < 4; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga automatic mode before event", 3));
      inputScript.push_back({ScriptActionType::MangaCheckAuto, {}, nullptr, 0, 1, 0});
      if (event < 2)
        addTap(event ? MappedInputManager::Button::Back : MappedInputManager::Button::Confirm);
      else if (event == 5)
        addMangaTouchMenuGesture();
      else
        inputScript.push_back({ScriptActionType::MangaAutoEvents, {}, nullptr, 0, event, 0});
      inputScript.push_back(render("Manga active event consumed", 4));
      if (event == 3) {
        inputScript.push_back(assertActivity("ReaderOptions"));
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Manga active suspension child returned", 4));
      }
      if (event == 4) {
        inputScript.push_back(assertActivity("Home"));
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Manga active exit reopened", 4));
      }
      inputScript.push_back({ScriptActionType::MangaAutoEvents, {}, nullptr, 0, 6, event});
    }
  }
  void buildMangaTouchPaging() {
    inputScript.push_back({ScriptActionType::MangaAutoEvents, {}, nullptr, 0, 7, 0});
    inputScript.push_back(render("Manga landscape touch paging", 4));
    addMangaTouchMenuGesture();
    inputScript.push_back(render("Manga landscape popup", 3));
    inputScript.push_back({ScriptActionType::MangaAutoEvents, {}, nullptr, 0, 8, 0});
    for (int row : {15, 0, 15}) inputScript.push_back({ScriptActionType::MangaTouchPageTo, {}, nullptr, 0, row, 0});
    inputScript.push_back({ScriptActionType::MangaTouchOptionDown, {}, nullptr, 0, 15, 0});
    inputScript.push_back({ScriptActionType::MangaTouchOptionRelease, {}, nullptr, 0, 15, 0});
    inputScript.push_back(render("Manga touch-only last row after reverse paging", 4));
    inputScript.push_back(assertActivity("QrDisplay"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Manga touch-paged QR returns", 4));
    addMangaTouchMenuGesture();
    inputScript.push_back(render("Manga reopened popup invalidates old hitboxes", 3));
    inputScript.push_back({ScriptActionType::MangaTouchPageTo, {}, nullptr, 0, 0, 0});
    inputScript.push_back({ScriptActionType::MangaTouchOptionDown, {}, nullptr, 0, 0, 0});
    inputScript.push_back({ScriptActionType::MangaTouchOptionRelease, {}, nullptr, 0, 0, 0});
    inputScript.push_back(render("Manga first row after popup reopen", 4));
    inputScript.push_back(assertActivity("MangaReaderSelection"));
    inputScript.push_back({ScriptActionType::MangaAutoEvents, {}, nullptr, 0, 9, 0});
  }

  void buildMangaQrContracts() {
    for (int mode = 0; mode < 3; ++mode) {
      inputScript.push_back({ScriptActionType::MangaQrContracts, {}, nullptr, 0, 0, mode});
      inputScript.push_back({ScriptActionType::MangaOpenMenu, {}, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga QR ownership menu", 3));
      for (int i = 0; i < 15; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga QR allocation result", 5));
      inputScript.push_back({ScriptActionType::MangaQrContracts, {}, nullptr, 0, 1, mode});
      inputScript.push_back(render("Manga QR repeated redraw", 4));
      inputScript.push_back({ScriptActionType::MangaQrContracts, {}, nullptr, 0, 2, mode});
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga QR same-view return", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, -1});
    }
  }

  void buildMangaReviewInterleave() {
    const std::string_view mode = std::getenv("CROSSINK_SIMULATOR_MANGA_REVIEW");
    if (mode == "active_cancel") {
      inputScript.push_back({ScriptActionType::MangaOpenMenu, {}, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga completion cancellation menu", 3));
      for (int i = 0; i < 12; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga completion cancellation rate", 3));
      for (int i = 0; i < 4; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga auto before held refresh", 3));
      inputScript.push_back({ScriptActionType::MangaCheckAuto, {}, nullptr, 0, 1, 0});
    }
    inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, mode == "finished" ? 1 : 0, 0});
    if (mode == "finished")
      inputScript.push_back({ScriptActionType::MangaWaitCompletion, {}, nullptr, 0, 0, 0});
    else
      inputScript.push_back({ScriptActionType::MangaPrefetchDwell, {}, nullptr, 230, 0, 0});
    if (mode == "release" || mode == "active_cancel") {
      inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 2, 0});
      inputScript.push_back(render("Manga refresh waits for held completion", 3));
      inputScript.push_back({ScriptActionType::MangaWaitCompletion, {}, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 7, 0});
      // Inject exactly one edge in the iteration which consumes worker completion.
      const auto edge =
          mode == "release" ? MappedInputManager::Button::PageForward : MappedInputManager::Button::Confirm;
      inputScript.push_back({ScriptActionType::MangaReleaseCompletion, edge, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga completion edge serviced", 4));
      inputScript.push_back(
          {ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 8, mode == "active_cancel" ? 1 : 0});
      inputScript.push_back(render("Manga completion edge is not replayed", 4));
      inputScript.push_back(
          {ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 8, mode == "active_cancel" ? 1 : 0});
    } else if (mode == "deferred") {
      inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 2, 0});
      inputScript.push_back(render("Manga refresh deferred behind worker", 3));
      inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 3, 0});
      inputScript.push_back(render("Manga menu after deferred render", 4));
      for (int i = 0; i < 12; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga interleaved auto popup", 3));
      for (int i = 0; i < 4; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga auto starts after deferred frame", 3));
      inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 5400, 6, 0});
      inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 4, 0});
      addTap(MappedInputManager::Button::Back);
    } else {
      inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 5, mode == "finished" ? 1 : 0});
      inputScript.push_back(render("One-shot manga lookup after worker drain", 6));
      inputScript.push_back(assertActivity("EpubReaderWordLookup"));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("One-shot shortcut restores same view", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, -1});
      inputScript.push_back({ScriptActionType::MangaReviewInterleave, {}, nullptr, 0, 6, 0});
    }
  }

  void addMangaCacheDelete() {
    inputScript.push_back({ScriptActionType::MangaOpenMenu, {}, nullptr, 0, 0, 0});
    inputScript.push_back(render("Manga cache gate menu", 3));
    for (int i = 0; i < 14; ++i) addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga cache gate confirmation", 3));
    addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga cache gate result", 6));
  }

  void buildMangaFailureContracts() {
    inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 11000, 6, 0});
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 0, 0});
    addMangaCacheDelete();
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 1, 0});
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga resumes after blocked cache deletion", 4));
    inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 3100, 6, 0});
    addTap(MappedInputManager::Button::PageForward);
    inputScript.push_back(render("Manga live panel change while stats target pending", 4));
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 2, 0});
    inputScript.push_back(render("Manga exit retained after repeated stats failure", 5));
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 1, 1});
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga second failure dismissed", 3));
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 3, 0});
    addMangaCacheDelete();
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 4, 0});
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Home after cache retry succeeds", 4));
    inputScript.push_back(assertActivity("Home"));
    inputScript.push_back(openSmokeBook());
    inputScript.push_back(render("Manga regenerates pixels after cache retry", 6));
    inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, 0});
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 5, 0});
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 6, 0});
    inputScript.push_back(render("Manga screenshot SD failure feedback", 5));
    inputScript.push_back({ScriptActionType::MangaCacheContracts, {}, nullptr, 0, 7, 0});
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga screenshot failure restores reading", 4));
  }

  void buildMangaRenderFailureScript() {
    const bool restore = std::string_view(std::getenv("CROSSINK_SIMULATOR_MANGA_RENDER_FAILURE")) == "restore";
    if (restore) {
      inputScript.push_back({ScriptActionType::MangaRenderFailure, {}, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga failed BW restore screenshot request", 6));
      inputScript.push_back({ScriptActionType::MangaRenderFailure, {}, nullptr, 0, 1, 0});
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga restored after error feedback", 4));
    } else {
      inputScript.push_back({ScriptActionType::MangaOpenMenu, {}, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga delete cache menu", 3));
      for (int i = 0; i < 14; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga delete cache confirmation", 4));
      addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga cache deletion result", 6));
      inputScript.push_back({ScriptActionType::MangaRenderFailure, {}, nullptr, 0, 2, 0});
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Home after manga cache deletion", 5));
      inputScript.push_back(assertActivity("Home"));
    }
  }

  void buildMangaMenuScript() {
    inputScript.push_back({ScriptActionType::MangaStatusPlanes, {}, nullptr, 0, 0, 0});
    static constexpr const char* names[] = {"Chapter",     "Percent",        "Bookmarks",    "Toggle bookmark",
                                            "Panels only", "Panel rotation", "Orientation",  "Home",
                                            "Lookup",      "Translation",    "History",      "Manga settings",
                                            "Auto turn",   "Screenshot",     "Delete cache", "OCR QR"};
    for (int scope : {-1, 0}) {
      for (int row = 0; row < 16; ++row) {
        inputScript.push_back({ScriptActionType::MangaMenuGoHome, {}, nullptr, 0, 0, 0});
        inputScript.push_back(render("Home before menu row", 3));
        inputScript.push_back({ScriptActionType::MangaMenuReset, {}, nullptr, 0, 0, scope});
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Manga menu fixture reopened", 5));
        inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, scope});
        if (row == 13) inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 0, 3, scope});
        if (scope < 0)
          addTap(MappedInputManager::Button::Confirm);
        else {
#if CROSSINK_APP_CAP_TOUCH
          if (mappedInputManager.hasTouch()) {
            const int x = renderer.getScreenWidth() / 2;
            const int y = mappedInputManager.hasHomeKey() ? renderer.getScreenHeight() - 8 : 8;
            const int end =
                mappedInputManager.hasHomeKey() ? renderer.getScreenHeight() * 3 / 4 : renderer.getScreenHeight() / 4;
            inputScript.push_back(touchDown(x, y));
            inputScript.push_back(touchMove(x, end));
            inputScript.push_back(touchRelease(x, end));
          } else
#endif
            inputScript.push_back({ScriptActionType::MangaOpenMenu, {}, nullptr, 0, 0, 0});
        }
        inputScript.push_back(render("Manga sixteen-row popup", 3));
        inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 0, 0, scope});
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouch()) {
          inputScript.push_back({ScriptActionType::MangaTouchPageTo, {}, nullptr, 0, row, 0});
          inputScript.push_back({ScriptActionType::MangaTouchOptionDown, {}, nullptr, 0, row, 0});
          inputScript.push_back({ScriptActionType::MangaTouchOptionRelease, {}, nullptr, 0, row, 0});
        } else
#endif
        {
          for (int i = 0; i < row; ++i) addTap(MappedInputManager::Button::Down);
          addTap(MappedInputManager::Button::Confirm);
        }
        inputScript.push_back(render(names[row], 5));
        inputScript.push_back({ScriptActionType::MangaCheckMenuAction, {}, nullptr, 0, row, scope});
        if (row == 11) {
          inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 0, 5, scope});
          if (!mappedInputManager.hasTouchHardware()) {
            addTap(MappedInputManager::Button::Down);
            addTap(MappedInputManager::Button::Down);
          }
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Manga applicable setting changed", 3));
        }
        if (row == 13) inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 0, 4, scope});
        if (row == 12) {
          for (int i = 0; i < 4; ++i) addTap(MappedInputManager::Button::Down);
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Manga auto 12 selected", 3));
          inputScript.push_back({ScriptActionType::MangaCheckAuto, {}, nullptr, 0, 1, scope});
          addTap(MappedInputManager::Button::PageForward);
          inputScript.push_back(render("Manga ignores competing manual turn", 2));
          inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, scope});
          inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 5400, 6, scope});
          inputScript.push_back(render("Manga automatic next physical page", 3));
          inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 1, -1});
          addTap(MappedInputManager::Button::Back);
          inputScript.push_back(render("Manga auto cancelled by Back", 3));
          inputScript.push_back({ScriptActionType::MangaCheckAuto, {}, nullptr, 0, 0, scope});
        }
        if (row == 11 || row == 14 || row == 15) {
          addTap(MappedInputManager::Button::Back);
          inputScript.push_back(render("Manga child returns to same view", 4));
          inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, scope});
          if (row == 11) inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 0, 2, scope});
        }
      }
    }
    inputScript.push_back({ScriptActionType::MangaMenuContracts, {}, nullptr, 0, 1, 0});
    // Both ActivityManager overloads reach the same current-panel lookup.
    for (int route = 0; route < 2; ++route) {
      inputScript.push_back({ScriptActionType::MangaShortcutLookup, {}, nullptr, 0, route, 0});
      inputScript.push_back(render("Manga configured lookup shortcut", 5));
      inputScript.push_back(assertActivity("EpubReaderWordLookup"));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga shortcut returns to same panel", 5));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, 0});
    }
  }

  void buildMangaInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_ACTIVE_EVENTS")) {
      buildMangaActiveEvents();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_TOUCH_PAGING")) {
      buildMangaTouchPaging();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_QR_REVIEW")) {
      buildMangaQrContracts();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_REVIEW")) {
      buildMangaReviewInterleave();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_FAILURES")) {
      buildMangaFailureContracts();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_RENDER_FAILURE")) {
      buildMangaRenderFailureScript();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_MENU")) {
      buildMangaMenuScript();
      return;
    }

    inputScript.push_back(assertActivity("MangaReader"));
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS")) {
      const auto dwell = ScriptAction{ScriptActionType::MangaPrefetchDwell, MappedInputManager::Button::Back,
                                      nullptr, 230, 0, 0};
      for (int boundary = 0; boundary < 2; ++boundary) {
        const int panel = boundary ? 0 : -1;
        const auto retained = boundary ? MappedInputManager::Button::PageBack : MappedInputManager::Button::PageForward;
        const auto fresh = boundary ? MappedInputManager::Button::PageForward : MappedInputManager::Button::PageBack;
        inputScript.push_back({ScriptActionType::MangaBoundaryJump, {}, nullptr, 0, boundary, panel});
        inputScript.push_back(render("Manga boundary prepared", 4));
        inputScript.push_back({ScriptActionType::MangaQueueCurrentSource, {}, nullptr, 0, 0, 0});
        inputScript.push_back(dwell);
        inputScript.push_back({ScriptActionType::MangaHoldConsumption, {}, nullptr, 0, 0, 0});
        inputScript.push_back(release(retained));
        inputScript.push_back({ScriptActionType::MangaWaitCompletion, {}, nullptr, 0, 0, 0});
        inputScript.push_back({ScriptActionType::MangaReleaseCompletion, fresh, nullptr, 0, 0, 0});
        inputScript.push_back(render("Manga opposing directions drained", 4));
        inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, boundary, panel});
      }
      inputScript.push_back({ScriptActionType::MangaBoundaryJump, {}, nullptr, 0, 0, -1});
      inputScript.push_back(render("Manga menu race prepared", 4));
      inputScript.push_back({ScriptActionType::MangaQueueCurrentSource, {}, nullptr, 0, 0, 0});
      inputScript.push_back(dwell);
      inputScript.push_back({ScriptActionType::MangaHoldConsumption, {}, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaDuplicateMenu, {}, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaWaitCompletion, {}, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaConfirmOnCompletion, {}, nullptr, 0, 0, 0});
      inputScript.push_back(release(MappedInputManager::Button::Confirm));
      inputScript.push_back(render("Manga Confirm on menu drain selected chapter", 4));
      inputScript.push_back(assertActivity("MangaReaderSelection"));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga duplicate menu request consumed", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, {}, nullptr, 0, 0, -1});

      inputScript.push_back(dwell);
      inputScript.push_back({ScriptActionType::MangaPrefetchPush, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Prefetch child push drained", 4));
      inputScript.push_back(assertActivity("ReaderOptions"));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Prefetch child pop resumed manga", 4));
      inputScript.push_back(assertActivity("MangaReader"));
      inputScript.push_back(dwell);
      inputScript.push_back(manualReaderRefresh());
      inputScript.push_back(render("Prefetch manual refresh drained", 4));
      inputScript.push_back(assertActivity("MangaReader"));
      inputScript.push_back(dwell);
      inputScript.push_back({ScriptActionType::MangaPrefetchReplace, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Prefetch replace drained", 4));
      inputScript.push_back(assertActivity("ReaderOptions"));
      inputScript.push_back(openSmokeBook());
      inputScript.push_back(render("Manga restored after prefetch replace", 4));
      inputScript.push_back(dwell);
      inputScript.push_back({ScriptActionType::MangaPrefetchPop, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Prefetch reader pop drained", 4));
      inputScript.push_back(assertActivity("Home"));
      inputScript.push_back(openSmokeBook());
      inputScript.push_back(render("Manga restored after prefetch pop", 4));
      inputScript.push_back(dwell);
      inputScript.push_back({ScriptActionType::MangaPrefetchSleep, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Prefetch main sleep drained", 4));
      inputScript.push_back(assertActivity("Sleep"));
      inputScript.push_back(openSmokeBook());
      inputScript.push_back(render("Manga restored after prefetch sleep", 4));
      inputScript.push_back(assertActivity("MangaReader"));
    }
    inputScript.push_back({ScriptActionType::MangaPrefetchDwell, MappedInputManager::Button::Back, nullptr, 0, 0, 0});

#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouch()) {
      SETTINGS.touchReaderControls = 1;
      SETTINGS.pageTurnGesture = CrossPointSettings::TAP_AND_SWIPE;
      inputScript.push_back(touchDown(renderer.getScreenWidth() * 5 / 6, renderer.getScreenHeight() / 2));
      inputScript.push_back(touchRelease(renderer.getScreenWidth() * 5 / 6, renderer.getScreenHeight() / 2));
    } else
#endif
    {
      addTap(MappedInputManager::Button::PageForward);
    }
    inputScript.push_back(render("Manga first panel from overview", 4));
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_OCR")) {
      const auto openMenuRow = [this](int index) {
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouch()) {
          const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
          const int start = mappedInputManager.hasHomeKey() ? h - 8 : 8;
          const int end = mappedInputManager.hasHomeKey() ? h * 3 / 4 : h / 4;
          inputScript.push_back(touchDown(w / 2, start));
          inputScript.push_back(touchMove(w / 2, end));
          inputScript.push_back(touchRelease(w / 2, end));
        } else
#endif
          inputScript.push_back({ScriptActionType::MangaOpenMenu, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
        for (int i = 0; i < index; ++i) addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
      };
      inputScript.push_back({ScriptActionType::MangaRememberFont, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaQueueCurrentSource, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga panel shared lookup", 4));
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaAssertScanCache, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
#if CROSSINK_APP_CAP_TOUCH
      if (mappedInputManager.hasTouch()) {
        const int w = renderer.getScreenWidth(), y = renderer.getScreenHeight() / 4;
        inputScript.push_back(touchDown(w * 3 / 4, y));
        inputScript.push_back(touchMove(w / 4, y));
        inputScript.push_back(touchRelease(w / 4, y));
      } else
#endif
        addTap(MappedInputManager::Button::Right);
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, nullptr, 0, 1, 0});
      // Close a completed verified scan at cursor 1, then open the identical
      // physical page/panel through the real reader input path.
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga image restored after OCR child", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga warm panel shared lookup", 4));
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, nullptr, 0, 1, 0});
      inputScript.push_back({ScriptActionType::MangaAssertScanCache, MappedInputManager::Button::Back, nullptr, 0, 1, 1});
#if CROSSINK_APP_CAP_TOUCH
      if (mappedInputManager.hasTouch()) {
        const int w = renderer.getScreenWidth(), y = renderer.getScreenHeight() / 4;
        inputScript.push_back(touchDown(w / 2, y));
        inputScript.push_back(touchMove(w * 3 / 4, y));
        inputScript.push_back(touchRelease(w * 3 / 4, y));
      } else
#endif
        addTap(MappedInputManager::Button::Left);
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Down);
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, "Reader", 0, 0, 1});
      addTap(MappedInputManager::Button::Confirm);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, "text", 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, "Reader", 0, 0, 1});
      addTap(MappedInputManager::Button::Up);
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, "Reader", 0, 0, 0});
      inputScript.push_back(press(MappedInputManager::Button::Left));
      inputScript.push_back({ScriptActionType::MangaReadingDwell, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(release(MappedInputManager::Button::Left));
      inputScript.push_back(render("Manga dictionary picker", 4));
      inputScript.push_back(assertActivity("DictionarySelect"));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, "Reader", 0, 0, 0});
      inputScript.push_back(press(MappedInputManager::Button::Confirm));
      inputScript.push_back({ScriptActionType::MangaReadingDwell, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(release(MappedInputManager::Button::Confirm));
      inputScript.push_back(render("Manga clipping feedback", 4));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga image restored after OCR child", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      openMenuRow(9);
      inputScript.push_back(render("Manga stored panel translation", 4));
      inputScript.push_back(assertActivity("MangaTranslation"));
#if CROSSINK_APP_CAP_TOUCH
      if (mappedInputManager.hasTouch()) {
        const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
        inputScript.push_back(touchDown(w * 5 / 6, h / 2));
        inputScript.push_back(touchRelease(w * 5 / 6, h / 2));
      } else
#endif
        addTap(MappedInputManager::Button::PageForward);
      inputScript.push_back(render("Manga translation next page", 4));
      inputScript.push_back({ScriptActionType::MangaTranslationPage, MappedInputManager::Button::Back, nullptr, 0, 1, 0});
      addTap(MappedInputManager::Button::PageBack);
      inputScript.push_back({ScriptActionType::MangaTranslationPage, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga image restored after OCR child", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      openMenuRow(10);
      inputScript.push_back(render("Manga lookup history", 4));
      inputScript.push_back(assertActivity("LookedUpWords"));
      inputScript.push_back({ScriptActionType::MangaAssertHistory, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);

      // A translation-only physical page remains usable without OCR.
      inputScript.push_back({ScriptActionType::MangaBoundaryJump, MappedInputManager::Button::Back, nullptr, 0, 1, 0});
      inputScript.push_back(render("Manga translation-only panel", 4));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga empty OCR feedback", 4));
      inputScript.push_back({ScriptActionType::MangaAssertFeedback, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      openMenuRow(9);
      inputScript.push_back(render("Manga translation without OCR", 4));
      inputScript.push_back({ScriptActionType::MangaTranslationPage, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back({ScriptActionType::MangaBoundaryJump, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga restored before pending Confirm", 4));

      // Hold completion consumption: two real Confirm edges must queue one child.
      inputScript.push_back({ScriptActionType::MangaHoldConsumption, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaQueueCurrentSource, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Confirm);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(assertActivity("MangaReader"));
      inputScript.push_back({ScriptActionType::MangaWaitCompletion, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaReleaseCompletion, MappedInputManager::Button::Confirm, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga lookup after repeated pending Confirm", 4));
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, "Reader", 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaForceLookupExit, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga forced lookup exit", 4));
      inputScript.push_back(assertActivity("Home"));
      inputScript.push_back(openSmokeBook());
      inputScript.push_back(render("Manga reopened after forced lookup exit", 4));
      inputScript.push_back(render("Manga image restored after OCR child", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, MappedInputManager::Button::Back, nullptr, 0, 0, 0});

      inputScript.push_back({ScriptActionType::MangaOpenFixture, MappedInputManager::Button::Back, "/manga-ocr-fixtures/crop-only", 0, 0, 0});
      inputScript.push_back(render("Manga legacy crop-only reader", 4));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga text fallback shared lookup", 4));
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, "Reader", 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga image restored after OCR child", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, MappedInputManager::Button::Back, nullptr, 0, 0, 0});

      inputScript.push_back({ScriptActionType::MangaOpenFixture, MappedInputManager::Button::Back, "/manga-ocr-fixtures/empty", 0, 0, 0});
      inputScript.push_back(render("Manga empty fixture", 4));
      inputScript.push_back({ScriptActionType::MangaBoundaryJump, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga empty fixture panel", 4));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga empty panel Confirm feedback", 4));
      inputScript.push_back({ScriptActionType::MangaAssertFeedback, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      openMenuRow(9);
      inputScript.push_back(render("Manga empty stored translation", 4));
      inputScript.push_back({ScriptActionType::MangaAssertEmptyTranslation, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);

      // Rename only the runner's synthetic fixture directory, with no lookup
      // worker alive. No user storage or installed dictionary is touched.
      inputScript.push_back({ScriptActionType::MangaHideDictionaries, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::MangaOpenFixture, MappedInputManager::Button::Back, "/manga-ocr-fixtures/crop-only", 0, 0, 0});
      inputScript.push_back(render("Manga no-dictionary reader", 4));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga unavailable dictionary", 4));
      inputScript.push_back({ScriptActionType::MangaLookupUnavailable, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      openMenuRow(9);
      inputScript.push_back(render("Manga translation without dictionary", 4));
      inputScript.push_back({ScriptActionType::MangaTranslationPage, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back({ScriptActionType::MangaRestoreDictionaries, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(openSmokeBook());
      inputScript.push_back(render("Manga restored after edge fixtures", 4));
      inputScript.push_back(render("Manga image restored after OCR child", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    }

    inputScript.push_back({ScriptActionType::MangaReadingDwell, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(assertActivity("MangaReader"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Manga overview restored from panel", 4));
    if (std::getenv("CROSSINK_SIMULATOR_MANGA_OCR")) {
      addTap(MappedInputManager::Button::Confirm);
      for (int i = 0; i < 8; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga overview shared lookup", 4));
      inputScript.push_back({ScriptActionType::MangaLookupReady, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Manga image restored after OCR child", 4));
      inputScript.push_back({ScriptActionType::MangaAssertPosition, MappedInputManager::Button::Back, nullptr, 0, 0, -1});
      addTap(MappedInputManager::Button::Confirm);
      for (int i = 0; i < 9; ++i) addTap(MappedInputManager::Button::Down);
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Manga stored overview translation", 4));
      inputScript.push_back(assertActivity("MangaTranslation"));
      addTap(MappedInputManager::Button::Back);
    }

    inputScript.push_back({ScriptActionType::MangaReadingDwell, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(assertActivity("MangaReader"));

    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga chapter list opened", 4));
    inputScript.push_back(assertActivity("MangaReaderSelection"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Manga reader restored after chapter cancel", 4));
    inputScript.push_back(assertActivity("MangaReader"));

    addTap(MappedInputManager::Button::Confirm);
    for (int index = 0; index < 3; ++index) addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga bookmark toggled", 4));
    inputScript.push_back({ScriptActionType::AssertMangaBookmark, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(assertActivity("MangaReader"));
    addTap(MappedInputManager::Button::Confirm);
    for (int index = 0; index < 2; ++index) addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga bookmark list opened", 4));
    inputScript.push_back(assertActivity("MangaReaderSelection"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Manga reader restored after bookmark cancel", 4));
    inputScript.push_back(assertActivity("MangaReader"));

    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga percent selector opened", 4));
    inputScript.push_back(assertActivity("EpubReaderPercentSelection"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Manga reader restored after percent cancel", 4));
    inputScript.push_back(assertActivity("MangaReader"));
    inputScript.push_back(manualReaderRefresh());
    inputScript.push_back(render("Manga manual refresh after child menu", 4));
    inputScript.push_back(assertActivity("MangaReader"));

    addTap(MappedInputManager::Button::Confirm);
    for (int index = 0; index < 4; ++index) addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga panels-only mode enabled", 4));
    inputScript.push_back(assertActivity("MangaReader"));
    inputScript.push_back({ScriptActionType::MangaOpenMenu, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    for (int index = 0; index < 5; ++index) addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga panel rotation disabled", 4));
    inputScript.push_back({ScriptActionType::MangaReadingDwell, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(assertActivity("MangaReader"));

    addTap(MappedInputManager::Button::PageForward);
    inputScript.push_back(render("Manga second page first panel", 4));
    inputScript.push_back(assertActivity("MangaReader"));
    inputScript.push_back({ScriptActionType::MangaFinalPageDwell, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    addTap(MappedInputManager::Button::PageForward);
    addTap(MappedInputManager::Button::PageForward);
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Home after manga exit", 4));
    inputScript.push_back(assertActivity("Home"));
    inputScript.push_back(openBooks());
    inputScript.push_back(render("Books reopened for manga folder", 4));
    inputScript.push_back(assertActivity("FileBrowser"));
    addAnkiTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga reader reopened at persisted position", 8));
    inputScript.push_back({ScriptActionType::AssertMangaBookmark, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(assertActivity("MangaReader"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Home after reopened manga exit", 4));
    inputScript.push_back(assertActivity("Home"));
    inputScript.push_back({ScriptActionType::AssertMangaLibrary, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(
        {ScriptActionType::OpenMangaLanguageStats, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(render("Manga statistics summary", 4));
    inputScript.push_back(assertActivity("BookStats"));
    addAnkiTap(MappedInputManager::Button::Right);
    inputScript.push_back(render("Manga reading language totals", 4));
    inputScript.push_back(
        {ScriptActionType::AssertMangaLanguageStats, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    addAnkiTap(MappedInputManager::Button::Right);
    inputScript.push_back(render("Device reading language totals", 4));
    inputScript.push_back(assertActivity("BookStats"));
    addAnkiTap(MappedInputManager::Button::Left);
    inputScript.push_back(render("Manga languages restored before statistics exit", 4));
    inputScript.push_back(assertActivity("BookStats"));
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouchHardware()) {
      // Earlier injected logical Back releases leave reader suppression armed.
      // Exercise the real header exit without changing global simulator input behavior.
      const Rect back = TouchHeaderBackButton::layout(TouchHeaderBackButton::compactHeaderRect(renderer)).touchRect;
      const int x = back.x + back.width / 2, y = back.y + back.height / 2;
      inputScript.push_back(touchDown(x, y));
      inputScript.push_back(touchRelease(x, y));
    } else
#endif
    {
      addTap(MappedInputManager::Button::Back);
    }
    inputScript.push_back(render("Home after language statistics", 4));
    inputScript.push_back(assertActivity("Home"));

    if (halClock.isAvailable()) {
      inputScript.push_back(
          {ScriptActionType::OpenMangaLanguageStats, MappedInputManager::Button::Back, nullptr, 1, 0, 0});
      inputScript.push_back(render("Manga statistics before save failure", 4));
      addTap(MappedInputManager::Button::Left);
      inputScript.push_back(render("Manga date editor before save failure", 4));
      addTap(MappedInputManager::Button::Right);
      inputScript.push_back(
          {ScriptActionType::MangaStatsSaveFailure, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Manga dirty edits retained after failed Home save", 4));
      inputScript.push_back(assertActivity("BookStats"));
      inputScript.push_back(
          {ScriptActionType::MangaStatsSaveFailure, MappedInputManager::Button::Back, nullptr, 0, 1, 0});
      inputScript.push_back(render("Manga dirty edits retained after failed sleep save", 4));
      inputScript.push_back(assertActivity("BookStats"));
      inputScript.push_back(
          {ScriptActionType::MangaStatsSaveFailure, MappedInputManager::Button::Back, nullptr, 0, 2, 0});
      inputScript.push_back(render("Home after successful manga stats retry", 4));
      inputScript.push_back(assertActivity("Home"));
      inputScript.push_back(
          {ScriptActionType::MangaStatsSaveFailure, MappedInputManager::Button::Back, nullptr, 0, 3, 0});
    }

    inputScript.push_back({ScriptActionType::OpenMangaRecents, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    inputScript.push_back(render("Manga Recent Books grid", 4));
    inputScript.push_back(assertActivity("RecentBooksGrid"));
    addAnkiTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Manga reopened from Recent Books", 4));
    inputScript.push_back(assertActivity("MangaReader"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Home after Recent Books manga exit", 4));
    inputScript.push_back(assertActivity("Home"));
    if (halClock.isAvailable()) {
      inputScript.push_back(
          {ScriptActionType::MangaStatsSaveFailure, MappedInputManager::Button::Back, nullptr, 0, 4, 0});
    }
  }

  void buildReaderInputScript() {
    if (isAnkiDeckSmokeBook()) {
      buildAnkiDeckInputScript();
      return;
    }
    if (isMangaSmokeBook()) {
      buildMangaInputScript();
      return;
    }

    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::Done;
    const int turns = pageTurnCount();
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouch()) {
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      if (width <= 0 || height <= 0) fail("Touch smoke test has invalid screen dimensions");
      LOG_INF("SMOKE", "Running touch reader input script with %d page turn(s)", turns);
      for (int i = 0; i < turns; ++i) {
        inputScript.push_back(touchDown(width * 5 / 6, height / 2));
        inputScript.push_back(touchRelease(width * 5 / 6, height / 2));
        inputScript.push_back(render("Reader after touch page forward", 4));
      }
      inputScript.push_back(setLookupPowerShortcut());
      addTap(MappedInputManager::Button::Power);
      inputScript.push_back(render("Word Lookup opened from configured shortcut", 8));
      inputScript.push_back(assertActivity("EpubReaderWordLookup"));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Reader restored after shortcut lookup", 4));
      inputScript.push_back(assertActivity("EpubReader"));
      inputScript.push_back(resetPowerShortcut());
      if (mappedInputManager.hasHomeKey()) {
        // Reader long-Power actions fire at the hold threshold. Their release
        // must not reach main.cpp's global shortcut route and run the same
        // action again. Repeat the gesture to verify the consumed release does
        // not leave the next one latched.
        inputScript.push_back(configureHomeButtonPowerLock());
        inputScript.push_back(press(MappedInputManager::Button::Power));
        inputScript.push_back(waitForPowerLongPress());
        inputScript.push_back(assertHomeButtonDisabled());
        inputScript.push_back(release(MappedInputManager::Button::Power));
        inputScript.push_back(assertHomeButtonDisabled());
        inputScript.push_back(press(MappedInputManager::Button::Power));
        inputScript.push_back(waitForPowerLongPress());
        inputScript.push_back(assertHomeButtonEnabled());
        inputScript.push_back(release(MappedInputManager::Button::Power));
        inputScript.push_back(assertHomeButtonEnabled());

        // X4 Pro reserves the top-edge swipe for its frontlight overlay and
        // moves the reader menu to the bottom edge.
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel opened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
        inputScript.push_back(touchDown(header.x + header.width - 32, header.y + header.height / 2));
        inputScript.push_back(touchRelease(header.x + header.width - 32, header.y + header.height / 2));
        inputScript.push_back(render("Home opened by Frontlight Panel Home button", 4));
        inputScript.push_back(assertActivity("Home"));
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Reader reopened after Frontlight Panel Home button", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened after Home button", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(20, height / 3));
        inputScript.push_back(touchMove(20, 8));
        inputScript.push_back(touchRelease(20, 8));
        inputScript.push_back(render("Frontlight Panel remains open after in-drawer swipe up", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        // X4 Pro's portrait frontlight sheet ends just below mid-screen; this
        // point lands in its centered 29 px handle band.
        inputScript.push_back(touchDown(width / 2, height * 21 / 40));
        inputScript.push_back(touchMove(width / 2, 8));
        inputScript.push_back(touchRelease(width / 2, 8));
        inputScript.push_back(render("Reader restored after Frontlight Panel handle drag up", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        // The fourth action-bar slot opens Global Settings through the real
        // FrontlightPanelActivity callback path.
        inputScript.push_back(touchDown(width * 7 / 10, height * 15 / 32));
        inputScript.push_back(touchRelease(width * 7 / 10, height * 15 / 32));
        inputScript.push_back(render("Global Settings opened from Frontlight Panel", 4));
        inputScript.push_back(assertActivity("Settings"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchMove(width / 2, height / 2));
        inputScript.push_back(touchRelease(width / 2, height / 2));
        inputScript.push_back(render("Global Settings remains open after interior swipe up", 4));
        inputScript.push_back(assertActivity("Settings"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader restored after Settings bottom-edge swipe", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened after Global Settings", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width * 3 / 10, height * 3 / 8));
        inputScript.push_back(touchRelease(width * 3 / 10, height * 3 / 8));
        inputScript.push_back(render("Sync dialog opened from Frontlight Panel", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width / 2, height - 60));
        inputScript.push_back(touchRelease(width / 2, height - 60));
        inputScript.push_back(render("Reader restored after dismissing Frontlight sync dialog", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height / 2 + 24));
        inputScript.push_back(touchRelease(width / 2, height / 2 + 24));
        inputScript.push_back(render("Reader Font opened from touch reader menu", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader Menu root restored by simulated Home key tap", 8));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader restored by simulated Home key tap at drawer root", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu reopened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height / 2 - 14));
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader restored after Reader Menu handle drag down", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(disableReaderTouch());
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from Home key hold with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
        inputScript.push_back(touchDown(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Reader restored after Home key menu with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Home opened from simulated Home key tap", 8));
        inputScript.push_back(assertActivity("Home"));
        inputScript.push_back(enableReaderTouch());
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Reader reopened after simulated Home key tap", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      } else {
        // Sticky uses the same vertical gesture split as X4 Pro: swipe down
        // opens reader details/actions and swipe up opens the bottom menu.
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Sticky Reader Details opened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(20, height / 3));
        inputScript.push_back(touchMove(20, 8));
        inputScript.push_back(touchRelease(20, 8));
        inputScript.push_back(render("Sticky Reader Details remains open after in-drawer swipe up", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader restored after Sticky details outside tap", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      }
      inputScript.push_back(render("Reader Menu opened from touch gesture", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));

      // Touch every bottom-drawer tab slot, then dismiss from its handle.
      const int tabY = height - 28;
      for (int tab = 0; tab < static_cast<int>(READER_DRAWER_TAB_COUNT); ++tab) {
        const int tabX = width * (tab * 2 + 1) / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2);
        inputScript.push_back(touchDown(tabX, tabY));
        inputScript.push_back(touchRelease(tabX, tabY));
        inputScript.push_back(render("Touch Reader Menu tab", 3));
        inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      }

      const int moreTabX = width / 2;
      const int drawerTop = height / 2;
      constexpr int rootRowStep = 60;
      constexpr int rootRowCenterOffset = 31;
      inputScript.push_back(touchDown(moreTabX, tabY));
      inputScript.push_back(touchRelease(moreTabX, tabY));
      inputScript.push_back(render("Touch Reader Menu More tab", 3));
      inputScript.push_back(touchDown(width / 2, drawerTop + rootRowStep + rootRowCenterOffset));
      inputScript.push_back(touchRelease(width / 2, drawerTop + rootRowStep + rootRowCenterOffset));
      inputScript.push_back(render("Touch Reader Go to Percent pane", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(touchRelease(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(render("Touch Reader Auto Page Turn pane", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height - 8));
      inputScript.push_back(touchRelease(width / 2, height - 8));
      inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(width / 2, drawerTop - 14));
      inputScript.push_back(touchRelease(width / 2, drawerTop - 14));
      inputScript.push_back(render("Reader restored after drawer handle tap", 4));
      inputScript.push_back(assertActivity("EpubReader"));

      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Reader Menu reopened for bottom-edge Home gesture", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height / 2 + 8));
      inputScript.push_back(touchRelease(width / 2, height / 2 + 8));
      inputScript.push_back(render("Reader Menu remains open after interior swipe up", 4));
      inputScript.push_back(assertActivity("EpubReaderTouchMenu"));
      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Home opened from Reader Menu bottom-edge swipe", 6));
      inputScript.push_back(assertActivity("Home"));
      return;
    }
#endif
    for (int i = 0; i < turns; i++) {
      addTap(MappedInputManager::Button::PageForward);
      inputScript.push_back(render("Reader after page forward", 4));
    }

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu opened from EPUB", 4));

    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Menu Lookup selection", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Word Lookup opened from Reader Menu", 8));
    inputScript.push_back(assertActivity("EpubReaderWordLookup"));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader restored after menu lookup", 4));
    inputScript.push_back(assertActivity("EpubReader"));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu reopened for Lookup History", 4));
    inputScript.push_back(assertActivity("EpubReaderMenu"));

    addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Menu Lookup History selection", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Lookup History opened from Reader Menu", 4));
    inputScript.push_back(assertActivity("LookedUpWords"));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Word Lookup opened directly from history", 8));
    inputScript.push_back(assertActivity("EpubReaderWordLookup"));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Lookup History restored after direct lookup", 4));
    inputScript.push_back(assertActivity("LookedUpWords"));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader restored after Lookup History", 4));
    inputScript.push_back(assertActivity("EpubReader"));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu reopened for Reader Options", 4));
    inputScript.push_back(assertActivity("EpubReaderMenu"));
    for (int index = 0; index < 4; ++index) addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Menu Reader Options selection", 3));
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Options opened with live dictionary context", 4));
    inputScript.push_back(assertActivity("ReaderOptions"));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Menu restored after Reader Options", 4));
    inputScript.push_back(assertActivity("EpubReaderMenu"));
    addTap(MappedInputManager::Button::Back);
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader restored after Reader Options", 4));
    inputScript.push_back(assertActivity("EpubReader"));

    LOG_INF("SMOKE", "Running reader input script with %d page turn(s)", turns);
  }

#if CROSSINK_APP_CAP_TOUCH
  void buildFileBrowserInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::FileBrowserSettings;

    if (mappedInputManager.hasHomeKey()) {
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight Panel opened outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width * 9 / 10, height / 2));
      inputScript.push_back(touchRelease(width * 9 / 10, height / 2));
      inputScript.push_back(render("Reader touchscreen disabled from Frontlight Panel outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width / 2, height - 60));
      inputScript.push_back(touchRelease(width / 2, height - 60));
      inputScript.push_back(render("File Browser restored after disabling reader touchscreen", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(assertTouchscreenDisabled());
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight Panel reopened outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width * 9 / 10, height / 2));
      inputScript.push_back(touchRelease(width * 9 / 10, height / 2));
      inputScript.push_back(render("Reader touchscreen enabled from Frontlight Panel outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width / 2, height - 60));
      inputScript.push_back(touchRelease(width / 2, height - 60));
      inputScript.push_back(render("File Browser restored after enabling reader touchscreen", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(assertTouchscreenEnabled());
    }

    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
    const auto backLayout = TouchHeaderBackButton::layout(header);
    const int x = header.x + header.width - backLayout.iconRect.width / 2;
    const int y = backLayout.iconRect.y + backLayout.iconRect.height / 2;
    inputScript.push_back(touchDown(x, y));
    inputScript.push_back(touchRelease(x, y));
    inputScript.push_back(render("File Browser Settings opened from header shortcut", 4));
    inputScript.push_back(assertActivity("FileBrowserSettings"));
    const int rowY = header.y + header.height + 32;
    inputScript.push_back(touchDown(renderer.getScreenWidth() / 2, rowY));
    inputScript.push_back(touchRelease(renderer.getScreenWidth() / 2, rowY));
    inputScript.push_back(render("File Browser Settings toggle without row highlight", 4));
    inputScript.push_back(assertActivity("FileBrowserSettings"));
  }
#endif
  uint32_t mangaImageHashLocked() const {
    uint32_t hash = 2166136261U;
    const uint8_t* pixels = renderer.getFrameBuffer();
    for (size_t i = 0; pixels && i < renderer.getBufferSize(); ++i) hash = (hash ^ pixels[i]) * 16777619U;
    return hash;
  }

  MangaReaderActivity& mangaReaderForTest() {
    auto* reader = dynamic_cast<MangaReaderActivity*>(activityManager.currentForSimulatorTest());
    if (!reader) fail("Expected actual manga reader for deterministic input test");
    return *reader;
  }

  void runReaderInputScript() {
    if (scriptIndex >= inputScript.size()) {
      step = inputCompletionStep;
      return;
    }

    const auto& action = inputScript[scriptIndex++];
    switch (action.type) {
      case ScriptActionType::MangaAutoEvents:
        if (action.x == 2) {
          activityManager.notifyInputLockChanged(true);
          if (mangaReaderForTest().preventAutoSleep()) fail("Lock did not cancel active manga auto mode");
          activityManager.notifyInputLockChanged(false);
        } else if (action.x == 3) {
          activityManager.pushActivity(
              std::make_unique<ReaderOptionsActivity>(renderer, mappedInputManager, ReaderSettingsScope::Manga));
        } else if (action.x == 4)
          activityManager.goHome();
        else if (action.x == 6) {
          const auto position = mangaReaderForTest().simulatorPosition();
          if (mangaReaderForTest().preventAutoSleep() || mangaReaderForTest().simulatorMenuActive() ||
              position.page != 0 || position.panel != -1)
            fail("Active manga event did not cancel and consume safely");
          LOG_INF("SMOKE", "Verified active manga automatic event cancellation mode=%d", action.y);
        } else if (action.x == 7) {
          if (!mappedInputManager.hasTouchHardware()) fail("Touch paging needs touch simulator");
          SETTINGS.orientation = CrossPointSettings::LANDSCAPE_CW;
          SETTINGS.touchReaderControls = true;
          activityManager.requestManualReaderRefresh();
        } else if (action.x == 8) {
          int x, y;
          if (mangaReaderForTest().simulatorMenuOptionCenter(15, x, y))
            fail("Touch paging fixture did not require scrolling");
        } else if (action.x == 9)
          LOG_INF("SMOKE", "Verified touch-only manga paging in both directions and fresh popup hitboxes");
        break;
      case ScriptActionType::MangaTouchMenuGesture:
#if CROSSINK_APP_CAP_TOUCH
      {
        static int endY = 0;
        if (action.x == 0) {
          touchButtonX = renderer.getScreenWidth() / 2;
          touchButtonY = mappedInputManager.hasHomeKey() ? renderer.getScreenHeight() - 8 : 8;
          endY = mappedInputManager.hasHomeKey() ? renderer.getScreenHeight() * 3 / 4 : renderer.getScreenHeight() / 4;
          mappedInputManager.simulatorInjectTouchDown(touchButtonX, touchButtonY);
        } else if (action.x == 1)
          mappedInputManager.simulatorInjectTouchMove(touchButtonX, endY);
        else
          mappedInputManager.simulatorInjectTouchRelease(touchButtonX, endY);
      }
#endif
      break;
      case ScriptActionType::MangaTouchPageTo:
#if CROSSINK_APP_CAP_TOUCH
      {
        static int phase = 0, endY = 0;
        int x, y;
        if (phase == 0) {
          if (mangaReaderForTest().simulatorMenuOptionCenter(action.x, x, y)) break;
          int first = -1, last = -1, firstY = 0, lastY = 0;
          for (int row = 0; row < 16; ++row)
            if (mangaReaderForTest().simulatorMenuOptionCenter(row, x, y)) {
              if (first < 0) {
                first = row;
                firstY = y;
              }
              last = row;
              lastY = y;
            }
          if (first < 0 || first == last) fail("No swipe range in manga popup");
          const bool up = action.x > last;
          touchButtonX = x;
          touchButtonY = up ? lastY : firstY;
          endY = up ? firstY : lastY;
          mappedInputManager.simulatorInjectTouchDown(touchButtonX, touchButtonY);
          phase = 1;
        } else if (phase == 1) {
          mappedInputManager.simulatorInjectTouchMove(touchButtonX, endY);
          phase = 2;
        } else if (phase == 2) {
          mappedInputManager.simulatorInjectTouchRelease(touchButtonX, endY);
          // Match hardware's mutually exclusive swipe/tap release contract.
          // Otherwise the popup handles the synthetic tap before its swipe.
          if (mappedInputManager.wasSwipe() == MappedInputManager::SwipeDir::None ||
              mappedInputManager.wasScreenTapped(x, y))
            fail("Injected popup swipe release was also reported as a tap");
          phase = 3;
        } else {
          if (!mangaReaderForTest().simulatorMenuActive()) fail("Popup swipe selected a stale touched row");
          phase = 0;
        }
        --scriptIndex;
      }
#endif
      break;
      case ScriptActionType::MangaQrContracts:
        if (action.x == 0) {
          if (action.y) QrDisplayActivity::simulatorFailAllocation(action.y == 2);
        } else {
          if (action.y == 1) {
            if (!mangaReaderForTest().simulatorFeedbackIs(StrId::STR_MEMORY_ERROR))
              fail("QR child OOM lost parent feedback");
          } else {
            auto* qr = dynamic_cast<QrDisplayActivity*>(activityManager.currentForSimulatorTest());
            if (!qr || qr->simulatorPayloadBytes() != 2953 ||
                qr->simulatorModuleBytes() != (action.y == 2 ? 0u : 3917u))
              fail("Maximum mixed UTF-8 QR child allocation contract failed");
            RenderLock lock;
            if (action.x == 1)
              mangaReaderImageHash = mangaImageHashLocked();
            else if (mangaReaderImageHash != mangaImageHashLocked())
              fail("QR child redraw changed owned payload/frame");
          }
          if (action.x == 2)
            LOG_INF("SMOKE", "Verified maximum mixed UTF-8 QR child redraw/allocation mode=%d", action.y);
        }
        break;
      case ScriptActionType::MangaReviewInterleave:
        if (action.x <= 1) {
          if (!action.x) setenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS", "1", 1);
          manga::prefetchTestHoldConsumption(false);
          if (!mangaReaderForTest().simulatorStartWarmWhenIdle()) {
            --scriptIndex;
            break;
          }
          manga::prefetchTestHoldConsumption(true);
        } else if (action.x == 2) {
          activityManager.requestManualReaderRefresh();
        } else if (action.x == 3) {
          if (!mangaReaderForTest().simulatorPendingRender()) fail("Test did not reach deferred render");
          if (!mangaReaderForTest().openReaderSettingsMenu()) fail("Cannot queue menu behind worker");
          manga::prefetchTestHoldConsumption(false);
          unsetenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS");
        } else if (action.x == 4) {
          const auto position = mangaReaderForTest().simulatorPosition();
          if (position.page != 1 || position.panel != -1 || mangaReaderForTest().simulatorPendingRender())
            fail("Deferred render stranded automatic turn");
          LOG_INF("SMOKE", "Verified deferred-render menu interleaving rearms one automatic deadline");
        } else if (action.x == 5) {
          const bool accepted =
              action.y ? activityManager.handleShortcutAction(static_cast<uint8_t>(CrossPointSettings::LOOKUP_WORD))
                       : activityManager.handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD);
          if (!accepted) fail("One-shot lookup discarded ordinary prefetch warming");
          if (activityManager.handleShortcutAction(static_cast<uint8_t>(CrossPointSettings::LOOKUP_WORD)) ||
              activityManager.handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD))
            fail("Lookup accepted an already-owned foreground drain");
          manga::prefetchTestHoldConsumption(false);
          unsetenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS");
        } else if (action.x == 7) {
          if (!mangaReaderForTest().simulatorPendingRender() || !manga::prefetchTestCompletionReady())
            fail("Completion edge test did not hold a deferred render and finished worker");
          unsetenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS");
        } else if (action.x == 8) {
          const auto position = mangaReaderForTest().simulatorPosition();
          if (action.y) {
            if (mangaReaderForTest().preventAutoSleep() || mangaReaderForTest().simulatorMenuActive() ||
                position.page != 0 || position.panel != -1)
              fail("Deferred render consumed active cancellation edge");
            LOG_INF("SMOKE", "Verified active cancellation precedes completion render service");
          } else {
            if (position.page != 0 || position.panel != 0)
              fail("Deferred render discarded or replayed one-shot page release");
            LOG_INF("SMOKE", "Verified one-shot page release survives completion render service exactly once");
          }
        } else
          LOG_INF("SMOKE", "Verified one-shot lookup survives warming and rejects owned drain");
        break;
      case ScriptActionType::MangaCacheContracts: {
        const std::string cache = manga::cachePath(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"));
        static uint32_t frozenSeconds = 0, finalSeconds = 0;
        constexpr const char* obstruction = "/.crosspoint/global_stats.bin.tmp";
        constexpr const char* marker = "/.crosspoint/global_stats.bin.tmp/blocked";
        const auto writeMarker = [&](const std::string& path) {
          FsFile file;
          if (!Storage.openFileForWrite("SMOKE", path, file)) fail("Cannot write menu preservation marker");
          if (file.write(static_cast<uint8_t>(73)) != 1 || !file.close()) fail("Cannot close menu marker");
        };
        if (action.x == 0) {
          if (!Storage.mkdir(obstruction)) fail("Cannot obstruct pending global stats");
          writeMarker(marker);
          for (const char* name : {"dictionary.bin", "dictionary_history.txt", "derived-menu.bin"})
            writeMarker(cache + "/" + name);
          // Real legacy layout from BookReadingStats.cpp: v4 is 69 bytes,
          // version byte 4 followed by zero counters/flags/unknown dates.
          const uint8_t legacyStats[69] = {4};
          FsFile legacy;
          if (!Storage.openFileForWrite("SMOKE", cache + "/stats_v4.bin", legacy)) fail("Cannot seed legacy stats");
          const bool wroteLegacy = legacy.write(legacyStats, sizeof(legacyStats)) == sizeof(legacyStats);
          if (!legacy.close() || !wroteLegacy) fail("Cannot close legacy stats fixture");
        } else if (action.x == 1) {
          if (!mangaReaderForTest().simulatorFeedbackIs(StrId::STR_STATS_SAVE_FAILED) ||
              !activityManager.preventAutoSleep() || !Storage.exists((cache + "/derived-menu.bin").c_str()))
            fail("Failed stats gate did not retain cache and reader");
          const auto book = BookReadingStats::load(cache);
          if (!action.y) frozenSeconds = book.totalReadingSeconds;
          if (frozenSeconds < 10 || book.totalReadingSeconds != frozenSeconds)
            fail("Successful book target replayed during pending global retry");
          LOG_INF("SMOKE", "Verified cache stats failure retains frozen target and usable reader retry=%d", action.y);
        } else if (action.x == 2) {
          if (mangaReaderForTest().simulatorPosition().panel != 0)
            fail("Reader input did not resume after save failure");
          activityManager.goHome();
        } else if (action.x == 3) {
          if (!Storage.remove(marker) || !Storage.rmdir(obstruction)) fail("Cannot remove global stats obstruction");
        } else if (action.x == 4) {
          if (!mangaReaderForTest().simulatorFeedbackIs(StrId::STR_BOOK_CACHE_DELETED) ||
              mangaReaderForTest().simulatorRenderErrors() || Storage.exists((cache + "/derived-menu.bin").c_str()))
            fail("Cache retry did not delete derived content cleanly");
          for (const char* name : {"dictionary.bin", "dictionary_history.txt", "stats_v4.bin"}) {
            FsFile file;
            if (!Storage.openFileForRead("SMOKE", cache + "/" + name, file)) fail("Cache deletion lost user data");
            const bool legacy = std::string_view(name) == "stats_v4.bin";
            bool valid = file.fileSize64() == (legacy ? 69u : 1u) && file.read() == (legacy ? 4 : 73);
            if (legacy)
              for (int i = 1; i < 69; ++i) valid = file.read() == 0 && valid;
            if (!file.close() || !valid) fail("Cache deletion changed user data bytes");
          }
          const auto book = BookReadingStats::load(cache);
          finalSeconds = book.totalReadingSeconds;
          if (finalSeconds < frozenSeconds + 3 || GlobalReadingStats::load().totalReadingSeconds != finalSeconds)
            fail("Cache retry lost new accepted reading interval");
          LOG_INF("SMOKE", "Verified menu cache retry preserves dictionary/history/stats and accepted tail");
        } else if (action.x == 5) {
          if (BookReadingStats::load(cache).totalReadingSeconds != finalSeconds)
            fail("Successful cache exit replayed stats");
          manga::PixelIdentity identity;
          if (std::getenv("CROSSINK_SIMULATOR_MANGA_GRAYSCALE") &&
              !manga::MangaPixelCache::sourceIdentity(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), 0, 0, identity))
            fail("Manga pixels did not regenerate after confirmed cache deletion");
          LOG_INF("SMOKE", "Verified menu cache regeneration and idempotent exit");
          writeMarker("/screenshots");
        } else if (action.x == 6) {
          if (!activityManager.handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::SCREENSHOT)) {
            activityManager.requestManualReaderRefresh();
            --scriptIndex;
          }
        } else {
          if (!mangaReaderForTest().simulatorFeedbackIs(StrId::STR_MANGA_SCREENSHOT_FAILED))
            fail("Screenshot SD failure did not produce usable feedback");
          if (!Storage.remove("/screenshots")) fail("Cannot remove screenshot obstruction");
          LOG_INF("SMOKE", "Verified deferred manga screenshot write failure feedback");
        }
        break;
      }
      case ScriptActionType::MangaMenuContracts: {
        static uint8_t oldSetting = 0;
        const auto rejected = [&] {
          if (activityManager.handleShortcutAction(static_cast<uint8_t>(CrossPointSettings::LOOKUP_WORD)) ||
              activityManager.handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD))
            fail("Manga lookup shortcut accepted while unavailable");
        };
        if (action.x == 0)
          rejected();
        else if (action.x == 1) {
          activityManager.notifyInputLockChanged(true);
          rejected();
          activityManager.notifyInputLockChanged(false);
          {
            RenderLock lock;
            mangaReaderForTest().prepareToSuspend();
          }
          rejected();
          {
            RenderLock lock;
            mangaReaderForTest().onResume();
          }
          activityManager.requestUpdate();
          LOG_INF("SMOKE", "Verified both manga shortcuts reject modal, lock and suspension");
        } else if (action.x == 2) {
          const uint8_t changed = mappedInputManager.hasTouchHardware() ? SETTINGS.disableReaderTouchscreen
                                                                        : SETTINGS.sideButtonOrientationAware;
          if (changed == oldSetting) fail("Applicable manga settings widget did not change value");
          if (!SETTINGS.loadFromFile()) fail("Cannot reload manga settings");
          if (changed != (mappedInputManager.hasTouchHardware() ? SETTINGS.disableReaderTouchscreen
                                                                : SETTINGS.sideButtonOrientationAware))
            fail("Manga settings change did not persist on return");
          LOG_INF("SMOKE", "Verified applicable manga settings change persists on child return");
        } else if (action.x == 3) {
          RenderLock lock;
          mangaReaderImageHash = mangaImageHashLocked();
        } else if (action.x == 4) {
          RenderLock lock;
          const auto info = activityManager.getScreenshotInfo();
          if (info.readerType != ScreenshotInfo::ReaderType::Manga || info.currentPage != 1 || info.totalPages != 2 ||
              info.progressPercent != 0 || std::string_view(info.title) != "Manga OCR smoke")
            fail("Manga screenshot metadata incorrect");
          if (mangaImageHashLocked() != mangaReaderImageHash)
            fail("Manga screenshot left popup or border in restored frame");
          LOG_INF("SMOKE", "Verified manga screenshot scope=%d framebuffer=%08lx", action.y,
                  static_cast<unsigned long>(mangaReaderImageHash));
        } else if (action.x == 6) {
          if (!mangaPrefetchDwellAt) mangaPrefetchDwellAt = millis();
          if (millis() - mangaPrefetchDwellAt < action.settleFrames)
            --scriptIndex;
          else
            mangaPrefetchDwellAt = 0;
        } else
          oldSetting = mappedInputManager.hasTouchHardware() ? SETTINGS.disableReaderTouchscreen
                                                             : SETTINGS.sideButtonOrientationAware;
        break;
      }
      case ScriptActionType::MangaRenderFailure:
        if (action.x == 0) {
          mangaReaderForTest().simulatorFailNextBwRestore();
          if (!activityManager.handleShortcutAction(static_cast<uint8_t>(CrossPointSettings::SCREENSHOT))) {
            activityManager.requestManualReaderRefresh();
            --scriptIndex;
          }
        } else if (action.x == 1) {
          if (!mangaReaderForTest().simulatorFeedbackIs(StrId::STR_PAGE_LOAD_ERROR))
            fail("Screenshot after failed BW restore did not report page error");
          LOG_INF("SMOKE", "Verified failed BW restore prevents manga screenshot");
        } else {
          if (mangaReaderForTest().simulatorRenderErrors()) fail("Cache feedback attempted closed-book rendering");
          if (!mangaReaderForTest().simulatorFeedbackIs(StrId::STR_BOOK_CACHE_DELETED))
            fail("Missing cache deletion result");
          LOG_INF("SMOKE", "Verified cache deletion feedback without closed-book rendering");
        }
        break;
      case ScriptActionType::MangaStatusPlanes: {
        RenderLock lock;
        if (!verifyMangaStatusPlanes(renderer)) fail("Manga actual renderer status plane contract failed");
        activityManager.requestUpdate();
        break;
      }
      case ScriptActionType::MangaMenuGoHome:
        activityManager.goHome();
        break;
      case ScriptActionType::MangaMenuReset: {
        manga::Progress progress;
        manga::MangaProgressStore store(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"));
        store.load(progress);
        progress.page = 0;
        progress.panel = action.y;
        progress.panelsOnly = false;
        progress.rotatePanels = false;
        if (!store.save(progress)) fail("Cannot reset menu fixture progress");
        SETTINGS.orientation = CrossPointSettings::PORTRAIT;
        SETTINGS.disableReaderTouchscreen = false;
        break;
      }
      case ScriptActionType::MangaTouchOptionDown:
#if CROSSINK_APP_CAP_TOUCH
        if (!mangaReaderForTest().simulatorMenuOptionCenter(action.x, touchButtonX, touchButtonY))
          fail("Manga popup target is not visible");
        mappedInputManager.simulatorInjectTouchDown(touchButtonX, touchButtonY);
#endif
        break;
      case ScriptActionType::MangaTouchOptionRelease:
#if CROSSINK_APP_CAP_TOUCH
      {
        mappedInputManager.simulatorInjectTouchRelease(touchButtonX, touchButtonY);
        int x = 0, y = 0;
        if (mappedInputManager.wasSwipe() != MappedInputManager::SwipeDir::None ||
            !mappedInputManager.wasScreenTapped(x, y) || x != touchButtonX || y != touchButtonY)
          fail("Stationary injected popup release did not remain a tap");
      }
#endif
        break;
      case ScriptActionType::MangaCheckMenuAction: {
        static constexpr const char* child[] = {"MangaReaderSelection", "EpubReaderPercentSelection",
                                                "MangaReaderSelection", "MangaReader",
                                                "MangaReader",          "MangaReader",
                                                "MangaReader",          "Home",
                                                "EpubReaderWordLookup", "MangaTranslation",
                                                "LookedUpWords",        "ReaderOptions",
                                                "MangaReader",          "MangaReader",
                                                "Confirmation",         "QrDisplay"};
        if (!activityManager.isCurrentActivityNamed(child[action.x])) fail("Wrong manga menu action %d", action.x);
        if (action.x == 11) {
          auto* options = dynamic_cast<ReaderOptionsActivity*>(activityManager.currentForSimulatorTest());
          const auto& settings = options->simulatorSettings();
          size_t rows = 0;
          for (const auto& setting : settings) {
            if (setting.type == SettingType::SECTION_HEADER) continue;
            if (!setting.key || (std::string_view(setting.key) != "disableReaderTouchscreen" &&
                                 std::string_view(setting.key) != "touchReaderControls" &&
                                 std::string_view(setting.key) != "pageTurnGesture" &&
                                 std::string_view(setting.key) != "sideButtonLayout" &&
                                 std::string_view(setting.key) != "sideButtonOrientationAware" &&
                                 std::string_view(setting.key) != "frontButtonOrientationAware"))
              fail("Unsupported manga settings row");
            ++rows;
          }
          if (rows != (mappedInputManager.hasTouchHardware() ? 5u : 3u))
            fail("Manga settings omitted applicable controls");
        }
        if (action.x == 12 && !mangaReaderForTest().simulatorMenuActive()) fail("Manga auto rate popup missing");
        LOG_INF("SMOKE", "Verified manga menu action %d scope %d", action.x, action.y);
        break;
      }
      case ScriptActionType::MangaCheckAuto:
        if (mangaReaderForTest().preventAutoSleep() != bool(action.x)) fail("Manga auto turn cancellation/rate failed");
        break;
      case ScriptActionType::MangaShortcutLookup:
        if (!(action.x ? activityManager.handleShortcutAction(static_cast<uint8_t>(CrossPointSettings::LOOKUP_WORD))
                       : activityManager.handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD)))
          fail("One-shot configured manga lookup was rejected");
        break;
      case ScriptActionType::Press:
        mappedInputManager.simulatorInjectPress(action.button);
        break;
      case ScriptActionType::Release:
        mappedInputManager.simulatorInjectRelease(action.button);
        break;
      case ScriptActionType::HomeTap:
        simulatorHomeKeyInput.injectTap();
        break;
      case ScriptActionType::HomeLongPress:
        simulatorHomeKeyInput.injectLongPress();
        break;
      case ScriptActionType::ConfigureHomeButtonPowerLock:
        SETTINGS.homeButtonInReaderEnabled = 1;
        SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER;
        SETTINGS.longPwrBtn = CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER;
        break;
      case ScriptActionType::WaitForPowerLongPress:
        if (mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
          --scriptIndex;
        }
        break;
      case ScriptActionType::AssertHomeButtonDisabled:
        if (SETTINGS.homeButtonInReaderEnabled) fail("Long Power did not disable the Home button");
        break;
      case ScriptActionType::AssertHomeButtonEnabled:
        if (!SETTINGS.homeButtonInReaderEnabled) fail("Long Power did not enable the Home button");
        break;
      case ScriptActionType::AssertTouchscreenDisabled:
        if (!SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be disabled");
        break;
      case ScriptActionType::AssertTouchscreenEnabled:
        if (SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be enabled");
        break;
      case ScriptActionType::TouchButtonDown:
#if CROSSINK_APP_CAP_TOUCH
        if (!findTouchButtonHint(action.button, touchButtonX, touchButtonY)) {
          fail("Missing touch button hint for Anki action");
        }
        mappedInputManager.simulatorInjectTouchDown(touchButtonX, touchButtonY);
        break;
#else
        fail("Touch button action is unavailable in this simulator");
#endif
      case ScriptActionType::TouchButtonRelease:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchRelease(touchButtonX, touchButtonY);
        break;
#else
        fail("Touch button action is unavailable in this simulator");
#endif
      case ScriptActionType::OpenSmokeBook: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') fail("Smoke test book path is missing");
        activityManager.goToReader(bookPath, true);
        break;
      }
      case ScriptActionType::DisableReaderTouch:
        SETTINGS.disableReaderTouchscreen = true;
        break;
      case ScriptActionType::EnableReaderTouch:
        SETTINGS.disableReaderTouchscreen = false;
        break;
      case ScriptActionType::SetLookupPowerShortcut:
        SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD;
        break;
      case ScriptActionType::ResetPowerShortcut:
        SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::IGNORE;
        break;
      case ScriptActionType::OpenBooks:
        activityManager.goToFileBrowser("/books");
        break;
      case ScriptActionType::ManualReaderRefresh:
        if (!activityManager.requestManualReaderRefresh()) fail("Manga manual reader refresh was rejected");
        break;
      case ScriptActionType::TouchDown:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchDown(action.x, action.y);
#endif
        break;
      case ScriptActionType::TouchMove:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchMove(action.x, action.y);
#endif
        break;
      case ScriptActionType::TouchRelease:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchRelease(action.x, action.y);
#endif
        break;
      case ScriptActionType::AssertActivity:
        if (!activityManager.isCurrentActivityNamed(action.label)) fail("Expected current activity: %s", action.label);
        break;
      case ScriptActionType::MangaBoundaryJump:
        if (!mangaReaderForTest().simulatorJumpWhenIdle({static_cast<uint32_t>(action.x), static_cast<int16_t>(action.y)}))
          --scriptIndex;
        break;
      case ScriptActionType::MangaQueueCurrentSource:
        if (!mangaReaderForTest().simulatorQueueCurrentSource()) fail("Cannot post current source for boundary test");
        break;
      case ScriptActionType::MangaHoldConsumption:
        manga::prefetchTestHoldConsumption(true);
        break;
      case ScriptActionType::MangaWaitCompletion:
        if (!mangaPrefetchDwellAt) mangaPrefetchDwellAt = millis();
        if (!manga::prefetchTestCompletionReady()) {
          if (millis() - mangaPrefetchDwellAt > 5000) fail("Worker did not acknowledge cleanup");
          --scriptIndex;
        } else mangaPrefetchDwellAt = 0;
        break;
      case ScriptActionType::MangaReleaseCompletion:
        mappedInputManager.simulatorInjectRelease(action.button);
        manga::prefetchTestHoldConsumption(false);
        break;
      case ScriptActionType::MangaDuplicateMenu:
        if (!mangaReaderForTest().openReaderSettingsMenu() || !mangaReaderForTest().openReaderSettingsMenu())
          fail("Duplicate menu intent rejected");
        break;
      case ScriptActionType::MangaConfirmOnCompletion:
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
        manga::prefetchTestHoldConsumption(false);
        break;
      case ScriptActionType::MangaOpenFixture:
        activityManager.goToReader(action.label, true);
        break;
      case ScriptActionType::MangaHideDictionaries:
        if (!Storage.rename("/dictionaries", "/manga-smoke-dictionaries")) fail("Cannot park synthetic dictionaries");
        break;
      case ScriptActionType::MangaRestoreDictionaries:
        if (!Storage.rename("/manga-smoke-dictionaries", "/dictionaries")) fail("Cannot restore synthetic dictionaries");
        break;
      case ScriptActionType::MangaForceLookupExit:
        if (!activityManager.isCurrentActivityNamed("EpubReaderWordLookup")) fail("Expected shared lookup before forced exit");
        activityManager.goHome();
        break;
      case ScriptActionType::MangaAssertFeedback:
        if (!mangaReaderForTest().simulatorNoOcrFeedback()) fail("Missing translated empty OCR message");
        LOG_INF("SMOKE", "Verified manga empty OCR feedback");
        break;
      case ScriptActionType::MangaAssertEmptyTranslation: {
        auto* translation = dynamic_cast<MangaTranslationActivity*>(activityManager.currentForSimulatorTest());
        if (!translation || !translation->simulatorEmpty()) fail("Missing empty translation state");
        LOG_INF("SMOKE", "Verified manga empty translation state");
        break;
      }
      case ScriptActionType::MangaLookupUnavailable: {
        auto* lookup = dynamic_cast<EpubReaderWordLookupActivity*>(activityManager.currentForSimulatorTest());
        if (!lookup || !lookup->simulatorUnavailable()) fail("Missing unavailable dictionary state");
        LOG_INF("SMOKE", "Verified manga unavailable dictionary state");
        break;
      }
      case ScriptActionType::MangaOpenMenu:
        if (!mangaReaderForTest().openReaderSettingsMenu()) fail("Could not request manga menu");
        break;
      case ScriptActionType::MangaRememberFont: {
        RenderLock lock;
        mangaReaderFontId = SETTINGS.getReaderFontId();
        mangaReaderFontWidth = renderer.getTextWidth(mangaReaderFontId, "Reader text");
        mangaReaderLineHeight = renderer.getLineHeight(mangaReaderFontId);
        mangaReaderOrientation = static_cast<int>(renderer.getOrientation());
        mangaReaderImageHash = mangaImageHashLocked();
        break;
      }
      case ScriptActionType::MangaTranslationPage: {
        auto* translation = dynamic_cast<MangaTranslationActivity*>(activityManager.currentForSimulatorTest());
        if (!translation || translation->simulatorPage() != static_cast<uint32_t>(action.x) || translation->simulatorEmpty()) fail("Translation paging or nonempty content failed");
        LOG_INF("SMOKE", "Verified manga translation page=%d", action.x);
        break;
      }
      case ScriptActionType::MangaAssertHistory: {
        const auto entries = LookupHistory::load(manga::cachePath(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK")));
        bool reader = false, text = false;
        for (const auto& entry : entries) { reader |= entry.word == "Reader"; text |= entry.word == "text"; }
        if (!reader || !text) fail("Manga lookup history did not retain both real definitions");
        LOG_INF("SMOKE", "Verified manga saved lookup history");
        break;
      }
      case ScriptActionType::MangaAssertScanCache: {
        auto* lookup = dynamic_cast<EpubReaderWordLookupActivity*>(activityManager.currentForSimulatorTest());
        if (!lookup) fail("Expected shared lookup for scan cache assertion");
        if (!mangaPrefetchDwellAt) mangaPrefetchDwellAt = millis();
        if (!lookup->simulatorVerifiedScanComplete()) {
          if (millis() - mangaPrefetchDwellAt > 10000) fail("Dictionary scan did not become verified and complete");
          --scriptIndex;
          return;
        }
        if (lookup->simulatorVerifiedCacheLoaded() != static_cast<bool>(action.x) ||
            !lookup->simulatorReadyAt(action.y, 0)) fail("Actual activity scan cache/cursor restoration failed");
        mangaPrefetchDwellAt = 0;
        LOG_INF("SMOKE", "Verified manga scan cache loaded=%d cursor=%d", action.x, action.y);
        break;
      }
      case ScriptActionType::MangaLookupReady: {
        auto* lookup = dynamic_cast<EpubReaderWordLookupActivity*>(activityManager.currentForSimulatorTest());
        if (!lookup) fail("Expected shared manga lookup activity");
        if (!mangaPrefetchDwellAt) mangaPrefetchDwellAt = millis();
        if (!lookup->simulatorReadyAt(action.x, action.y, action.label)) {
          if (millis() - mangaPrefetchDwellAt > 10000) { lookup->simulatorLogSelection(); fail("Manga dictionary did not reach requested selection"); }
          --scriptIndex;
          return;
        }
        mangaPrefetchDwellAt = 0;
        lookup->simulatorLogSelection();
        LOG_INF("SMOKE", "Verified manga dictionary ready cursor=%d definitionPage=%d", action.x, action.y);
        break;
      }
      case ScriptActionType::MangaAssertPosition: {
        if (mangaReaderFontId) {
          RenderLock lock;
          if (SETTINGS.getReaderFontId() != mangaReaderFontId || renderer.getTextWidth(mangaReaderFontId, "Reader text") != mangaReaderFontWidth ||
              renderer.getLineHeight(mangaReaderFontId) != mangaReaderLineHeight || static_cast<int>(renderer.getOrientation()) != mangaReaderOrientation) fail("Reader font or orientation not restored after manga child");
          LOG_INF("SMOKE", "Verified manga reader font restored");
          if (action.x == 0 && action.y == 0 && activityManager.currentForSimulatorTest()->getCurrentBookPath() == "/books/smoke-manga") {
            if (mangaImageHashLocked() != mangaReaderImageHash) fail("Manga panel image changed after child return");
            LOG_INF("SMOKE", "Verified manga image framebuffer restored");
          }
        }
        const auto position = mangaReaderForTest().simulatorPosition();
        if (position.page != static_cast<uint32_t>(action.x) || position.panel != action.y ||
            mangaReaderForTest().simulatorMenuActive())
          fail("Pending intent changed boundary or reopened menu: page=%lu panel=%d", position.page, position.panel);
        LOG_INF("SMOKE", "Verified coalesced input page=%lu panel=%d with menu closed", position.page, position.panel);
        break;
      }
      case ScriptActionType::MangaPrefetchPush:
        activityManager.pushActivity(std::make_unique<ReaderOptionsActivity>(renderer, mappedInputManager));
        break;
      case ScriptActionType::MangaPrefetchReplace:
        activityManager.replaceActivity(std::make_unique<ReaderOptionsActivity>(renderer, mappedInputManager));
        break;
      case ScriptActionType::MangaPrefetchPop:
        activityManager.popActivity();
        break;
      case ScriptActionType::MangaPrefetchSleep:
        enterDeepSleep(false);
        if (!activityManager.isCurrentActivityNamed("MangaReader")) fail("Prefetch sleep did not defer");
        LOG_INF("SMOKE", "Verified main sleep defers before persistence while prefetch drains");
        break;
      case ScriptActionType::MangaPrefetchDwell:
        // Leave the main loop running so the reader can post and consume work.
        // A blocking delay here would prevent the idle candidate from posting.
        if (!mangaPrefetchDwellAt) mangaPrefetchDwellAt = millis();
        if (action.settleFrames && std::getenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS")) {
          if (!manga::prefetchTestHoldingSource()) {
            if (millis() - mangaPrefetchDwellAt > 5000) fail("Worker did not reach open-source barrier");
            --scriptIndex;
          } else {
            mangaPrefetchDwellAt = 0;
            LOG_INF("SMOKE", "Verified prefetch holds source before foreground intent");
          }
        } else if (millis() - mangaPrefetchDwellAt < 1200) {
          --scriptIndex;
        } else {
          mangaPrefetchDwellAt = 0;
          LOG_INF("SMOKE", "Completed manga prefetch dwell with active input polling");
        }
        break;
      case ScriptActionType::MangaFinalPageDwell:
        delay(11000);
        break;
      case ScriptActionType::MangaReadingDwell:
        delay(700);
        break;
      case ScriptActionType::OpenMangaRecents:
        SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_GRID;
        activityManager.goToRecentBooks();
        break;
      case ScriptActionType::OpenMangaLanguageStats: {
        const std::string path = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        const auto cache = manga::cachePath(path);
        auto book = BookReadingStats::load(cache);
        const auto global = GlobalReadingStats::load();
        uint64_t sum = 0;
        for (const auto& entry : book.languageTotals.entries) sum += entry.seconds;
        if (sum != book.totalReadingSeconds || !book.languageTotals.entries[2].seconds)
          fail("Manga language durations disagree with committed seconds");
        // Include a visible unknown duration in the preview only, exercising its
        // localized row without mutating the reader's durable accounting fixture.
        mangaStatsStartBefore = book.startDate;
        if (!action.settleFrames) book.languageTotals.entries[0].seconds = 60;
        activityManager.pushActivity(std::make_unique<BookStatsActivity>(renderer, mappedInputManager, "Manga smoke",
                                                                         cache, book, 100.0f, false, 0, global));
        break;
      }
      case ScriptActionType::MangaStatsSaveFailure: {
        constexpr const char* obstruction = "/.crosspoint/global_stats.bin.tmp";
        constexpr const char* marker = "/.crosspoint/global_stats.bin.tmp/blocked";
        if (action.x == 0) {
          if (!Storage.mkdir(obstruction)) fail("Could not create stats failure obstruction");
          FsFile file;
          if (!Storage.openFileForWrite("SMOKE", marker, file)) fail("Could not create stats failure marker");
          const bool wrote = file.write(static_cast<uint8_t>(1)) == 1;
          const bool closed = file.close();
          if (!wrote || !closed) fail("Could not close stats failure marker");
          if (!activityManager.handleHomeButtonBackOrHome()) fail("Stats Home gesture not handled");
        } else if (action.x == 1) {
          if (!activityManager.preventAutoSleep()) fail("Failed dirty stats did not suppress automatic retry");
          // Arm the same one-shot intent as a Quick Lock timeout before the
          // real canceled sleep path; a later ordinary sleep must not retain it.
          APP_STATE.quickLockResumePending = true;
          APP_STATE.quickLockResumeTrigger = static_cast<uint8_t>(QuickLockTrigger::ShortPower);
          enterDeepSleep(true);
          if (APP_STATE.quickLockResumePending ||
              APP_STATE.quickLockResumeTrigger != static_cast<uint8_t>(QuickLockTrigger::None))
            fail("Canceled stats sleep retained Quick Lock resume intent");
          if (activityManager.retrySuspensionAfterFailure()) fail("Failed stats sleep entered drain retry policy");
        } else if (action.x == 2) {
          if (!Storage.remove(marker) || !Storage.rmdir(obstruction))
            fail("Could not remove stats failure obstruction");
          if (!activityManager.handleHomeButtonBackOrHome()) fail("Stats Home retry not handled");
        } else if (action.x == 3) {
          const auto cache = manga::cachePath(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"));
          const auto book = BookReadingStats::load(cache);
          if (!book.startDateManual || compareReadingStatsDate(book.startDate, mangaStatsStartBefore) == 0)
            fail("Retained date edit was not persisted after retry");
          LOG_INF("SMOKE", "Verified dirty manga stats survive Home and sleep save failures then retry");
        } else {
          // Reuse the existing sleep-preparation seam so the HAL's intentional
          // wait-for-wake loop does not stop this persistence assertion.
          const bool hadSleepSeam = std::getenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS") != nullptr;
          if (!hadSleepSeam) setenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS", "1", 1);
          enterDeepSleep(false);
          if (!hadSleepSeam) unsetenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS");
          if (!APP_STATE.loadFromFile()) fail("Could not reload ordinary sleep state");
          if (APP_STATE.quickLockResumePending ||
              APP_STATE.quickLockResumeTrigger != static_cast<uint8_t>(QuickLockTrigger::None))
            fail("Ordinary sleep persisted stale Quick Lock resume intent");
          LOG_INF("SMOKE", "Verified ordinary sleep after canceled stats sleep has no Quick Lock wake intent");
        }
        break;
      }
      case ScriptActionType::AssertMangaLanguageStats: {
        if (!activityManager.isCurrentActivityNamed("BookStats")) fail("Missing actual manga BookStats activity");
        RenderLock lock;
        const auto& metrics = UITheme::getInstance().getMetrics();
        const int dotY = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
        renderer.fillRect(0, dotY, renderer.getScreenWidth(), 8, false);
        const uint32_t actual = mangaImageHashLocked();
        const auto cache = manga::cachePath(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"));
        auto book = BookReadingStats::load(cache);
        book.languageTotals.entries[0].seconds = 60;
        // Compare the routed activity framebuffer with the actual production
        // language view; catches navigation landing on another stats scope/page.
        renderReadingLanguagesPage(renderer, &mappedInputManager, "Manga smoke", book.languageTotals, 0, true);
        // Page dots differ from the standalone view; mask only that ornament.
        renderer.fillRect(0, dotY, renderer.getScreenWidth(), 8, false);
        if (actual != mangaImageHashLocked()) fail("Stats navigation did not render the language breakdown");
        if (readingLanguageRowCount(book.languageTotals) < 2) fail("Manga language/Unknown rows missing");
        LOG_INF("SMOKE", "Verified actual manga language stats activity and language/Unknown totals");
        activityManager.requestUpdate();
        break;
      }
      case ScriptActionType::AssertMangaLibrary: {
        RenderLock lock;
        const std::string path = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        const auto& books = RECENT_BOOKS.getBooks();
        const auto found =
            std::find_if(books.begin(), books.end(), [&](const RecentBook& book) { return book.path == path; });
        if (found == books.end() || found->title.empty() || found->coverBmpPath != manga::thumbnailTemplatePath(path) ||
            RecentBookProgress::loadPercent(*found) != 100.0f)
          fail("Manga recent metadata/progress missing");
        manga::Progress before, after;
        manga::MangaProgressStore store(path);
        if (!store.load(before)) fail("Manga progress missing before cache clear");
        manga::Progress firstPage = before;
        firstPage.page = 0;
        if (!store.save(firstPage) || RecentBookProgress::loadPercent(*found) != 50.0f || !store.save(before))
          fail("Manga first-page percentage or restore failed");
        const auto cache = manga::cachePath(path);
        const auto stats = BookReadingStats::load(cache);
        if (!Storage.exists((cache + "/stats_v6.bin").c_str()) || stats.totalPagesTurned != 2 || !stats.isCompleted ||
            stats.totalReadingSeconds < 10)
          fail("Manga statistics: pages=%lu seconds=%lu completed=%d",
               static_cast<unsigned long>(stats.totalPagesTurned),
               static_cast<unsigned long>(stats.totalReadingSeconds), stats.isCompleted);
        // Explicit migration sources must survive alongside the current full v6 file.
        for (const char* oldName : {"stats_v4.bin", "stats_v5.bin"}) {
          FsFile old;
          if (!Storage.openFileForWrite("SMOKE", cache + "/" + oldName, old))
            fail("Cannot seed legacy cache preservation");
          const uint8_t marker = oldName[7] == '4' ? 4 : 5;
          if (old.write(&marker, 1) != 1 || !old.close()) fail("Cannot close legacy preservation fixture");
        }
        if (!BookActions::clearBookCache(path) || !store.load(after) || before.page != after.page ||
            before.panel != after.panel || !Storage.exists((cache + "/stats_v6.bin").c_str()) ||
            BookReadingStats::load(cache).totalPagesTurned != stats.totalPagesTurned)
          fail("Manga cache clear lost durable state");
        if (!Storage.exists((cache + "/stats_v4.bin").c_str()) || !Storage.exists((cache + "/stats_v5.bin").c_str()) ||
            BookReadingStats::load(cache).languageTotals.entries[2].seconds != stats.languageTotals.entries[2].seconds)
          fail("Manga cache clear lost v4/v5/v6 stats or language attribution");
        if (!SleepCoverAssets::prepareMinimalCoverForPath(path, &renderer) ||
            !SleepCoverAssets::prepareDashboardCoverForPath(path, &renderer) ||
            !SleepCoverAssets::prepareFullCoverForPath(path, false, &renderer))
          fail("Manga sleep cover generation failed");
        const std::string fullCover = SleepCoverAssets::cachedCoverPathFor(path, false, &renderer);
        if (fullCover.empty()) fail("Manga full sleep cover path is missing");
        FsFile corruptCover;
        if (!Storage.openFileForWrite("SMOKE", fullCover, corruptCover)) fail("Cannot inject corrupt sleep cover");
        const uint8_t invalid = 0;
        if (corruptCover.write(&invalid, 1) != 1 || !corruptCover.close()) fail("Cannot close corrupt sleep cover");
        if (!SleepCoverAssets::prepareFullCoverForPath(path, true, &renderer) ||
            SleepCoverAssets::cachedCoverPathFor(path, true, &renderer).empty())
          fail("Manga sleep cover did not recover");
        LOG_INF("SMOKE", "Verified manga sleep cover generation and corrupt-cache recovery");
        LOG_INF("SMOKE", "Verified manga recents, progress, stats, and safe cache clear");
        break;
      }
      case ScriptActionType::AssertMangaBookmark: {
        RenderLock lock;
        const auto& bookmarks = BOOKMARKS.getBookmarks();
        if (bookmarks.size() != 1 || bookmarks[0].spineIndex != 0 || bookmarks[0].paragraphIndex != 0) {
          fail("Manga overview bookmark did not survive save/reopen");
        }
        LOG_INF("SMOKE", "Verified manga overview bookmark");
        break;
      }
      case ScriptActionType::AssertAnkiNextCandidate: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') fail("Smoke test book path is missing");

        AnkiDeck deck;
        if (!deck.load(bookPath)) fail("Could not reload Anki smoke deck");
        ReviewStateStore state;
        if (!state.open(deck)) fail("Could not reopen Anki review state");

        ReviewState first{};
        ReviewState second{};
        if (!state.read(0, first) || !state.read(1, second)) fail("Could not read Anki review state");
        if (state.reviewCount() != 1 || first.kind != ReviewKind::Review || first.dueDay != state.reviewCount() + 1 ||
            second.kind != ReviewKind::New) {
          fail("Good grade did not persist the review count and next Anki candidate");
        }
        LOG_INF("SMOKE", "Verified Anki review counter and next candidate after Good");
        break;
      }
      case ScriptActionType::Render:
        queueStep(action.label, SmokeStep::ReaderInput, action.settleFrames);
        break;
    }
  }
};

SimulatorSmokeTest smokeTest;

}  // namespace

void runSimulatorSmokeTestTick() {
  // Isolated localhost lifecycle regression: the external WS client writes the
  // trigger only after a real START/READY and incomplete binary upload.
  const char* transferAction = std::getenv("CROSSINK_SIMULATOR_UPLOAD_SUSPEND");
  static bool transferTriggered = false;
  if (transferAction && !transferTriggered && Storage.exists("/upload-suspend.trigger") &&
      (activityManager.isCurrentActivityNamed("CrossPointWebServer") ||
       activityManager.isCurrentActivityNamed("CalibreConnect"))) {
    transferTriggered = true;
    LOG_INF("SMOKE", "Upload suspend trigger: %s", transferAction);
    if (strcmp(transferAction, "sleep") == 0) {
      const bool hadSleepSeam = std::getenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS") != nullptr;
      if (!hadSleepSeam) setenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS", "1", 1);
      enterDeepSleep(false);
      if (!hadSleepSeam) unsetenv("CROSSINK_SIMULATOR_MANGA_PREFETCH_STRESS");
      LOG_INF("SMOKE", "Upload sleep call returned");
    } else if (strcmp(transferAction, "pop") == 0) {
      activityManager.popActivity();
    } else {
      activityManager.handleHomeButtonBackOrHome();
    }
  }
  smokeTest.tick();
}

#endif
