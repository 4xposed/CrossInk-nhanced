#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

class Page;
struct PageTextSourceView;

// Reader-menu availability is intentionally cached between menu/shortcut
// checks. A unified lookup child may change the per-book or global StarDict
// selection, so every child-return path must invalidate both positive and
// negative cached results before the next check.
class EpubLookupAvailabilityCache {
 public:
  bool known() const { return known_; }
  bool value() const { return available_; }

  void store(const bool available) {
    available_ = available;
    known_ = true;
  }

  void reset() {
    known_ = false;
    available_ = false;
  }

  void invalidateAfterUnifiedChildReturn() { known_ = false; }

 private:
  bool known_ = false;
  bool available_ = false;
};

struct EpubLookupPageRequest {
  std::string bookLanguage;
  std::string bookCachePath;
  // Explicit external-source cache location; persistence awaits verified identity.
  std::string scanCacheFilePath;
  uint16_t spineIndex = 0;
  uint16_t pageIndex = 0;
  int marginLeft = 0;
  int marginTop = 0;
  int reservedBottomHeight = 0;
  int initialTouchX = -1;
  int initialTouchY = -1;
  bool autoLookupInitialWord = false;
  bool framebufferContainsPage = false;
  bool recordLookupHistory = true;
  const char* dictionaryFontFamilyName = nullptr;
  uint8_t dictionaryFontPointSize = 0;
  void* readerContext = nullptr;
  void (*renderReaderBackground)(void*) = nullptr;
  // Synchronous borrowed view; never retain it beyond this locked call.
  void (*renderExternalBackground)(void*, PageTextSourceView) = nullptr;
  std::unique_ptr<Page> (*reloadReaderPage)(void*) = nullptr;
};

// Complete reader-owned snapshot used by every page lookup entry point. The
// strings move into the request; the activity copies the borrowed fixed-size
// font name synchronously in its constructor.
struct EpubLookupPageSnapshot {
  std::string bookLanguage;
  std::string bookCachePath;
  uint16_t spineIndex = 0;
  uint16_t pageIndex = 0;
  int marginLeft = 0;
  int marginTop = 0;
  int reservedBottomHeight = 0;
  int initialTouchX = -1;
  int initialTouchY = -1;
  bool autoLookupInitialWord = false;
  bool framebufferContainsPage = false;
  const char* dictionaryFontFamilyName = nullptr;
  uint8_t dictionaryFontPointSize = 0;
  void* readerContext = nullptr;
  void (*renderReaderBackground)(void*) = nullptr;
  std::unique_ptr<Page> (*reloadReaderPage)(void*) = nullptr;
};

inline EpubLookupPageRequest makeEpubLookupPageRequest(EpubLookupPageSnapshot snapshot) {
  EpubLookupPageRequest request;
  request.bookLanguage = std::move(snapshot.bookLanguage);
  request.bookCachePath = std::move(snapshot.bookCachePath);
  request.spineIndex = snapshot.spineIndex;
  request.pageIndex = snapshot.pageIndex;
  request.marginLeft = snapshot.marginLeft;
  request.marginTop = snapshot.marginTop;
  request.reservedBottomHeight = snapshot.reservedBottomHeight;
  request.initialTouchX = snapshot.initialTouchX;
  request.initialTouchY = snapshot.initialTouchY;
  request.autoLookupInitialWord = snapshot.autoLookupInitialWord;
  request.framebufferContainsPage = snapshot.framebufferContainsPage;
  request.dictionaryFontFamilyName = snapshot.dictionaryFontFamilyName;
  request.dictionaryFontPointSize = snapshot.dictionaryFontPointSize;
  request.readerContext = snapshot.readerContext;
  request.renderReaderBackground = snapshot.renderReaderBackground;
  request.reloadReaderPage = snapshot.reloadReaderPage;
  return request;
}

inline EpubLookupPageRequest makeEpubLookupDirectRequest(std::string bookLanguage, std::string bookCachePath,
                                                         const char* dictionaryFontFamilyName,
                                                         const uint8_t dictionaryFontPointSize) {
  EpubLookupPageRequest request;
  request.bookLanguage = std::move(bookLanguage);
  request.bookCachePath = std::move(bookCachePath);
  request.recordLookupHistory = false;
  request.dictionaryFontFamilyName = dictionaryFontFamilyName;
  request.dictionaryFontPointSize = dictionaryFontPointSize;
  return request;
}
