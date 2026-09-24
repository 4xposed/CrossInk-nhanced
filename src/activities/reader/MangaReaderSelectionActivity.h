#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <MangaBook.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "BookmarkStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MangaReaderSelectionActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  static constexpr size_t ROW_WINDOW_SIZE = 20;
  static constexpr size_t TOC_LABEL_SIZE = 128;

  manga::MangaBook& book;
  BookmarkStore& bookmarkStore;
  bool showBookmarks;
  uint32_t currentPage;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  bool initialViewportPending = true;

  // Activities are heap-owned. Keeping this bounded scratch here avoids a
  // roughly 2.5 KB render-task stack allocation and copies TOC borrowed views.
  std::array<std::array<char, TOC_LABEL_SIZE>, ROW_WINDOW_SIZE> tocLabels{};
  std::array<freeink::ui::ListItem, ROW_WINDOW_SIZE> itemWindow{};
  std::array<char, ROW_WINDOW_SIZE * 8> valueWindow{};

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void selectItem();
  int totalItems() const;
  int findInitialIndex() const;

 public:
  MangaReaderSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, manga::MangaBook& book,
                               BookmarkStore& bookmarkStore, bool showBookmarks, uint32_t currentPage);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
