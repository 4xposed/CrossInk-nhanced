#pragma once

#include <AnkiDeck.h>
#include <ReviewStateStore.h>

#include <array>
#include <cstdint>
#include <memory>

#include "AnkiCardText.h"
#include "AnkiReviewCandidates.h"
#include "AnkiReviewSchedule.h"
#include "activities/Activity.h"

class AnkiReviewActivity final : public Activity {
 public:
  AnkiReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<AnkiDeck> deck,
                     int initialRefreshCountdown);

  void onEnter() override;
  void onResume() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool prepareManualRefresh() override;
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  std::string getCurrentBookPath() const override;
#ifdef SIMULATOR
  uint8_t simulatorPromptScale() const { return promptScale_; }
#endif

 private:
  static constexpr size_t kCardFieldBufferBytes = kMaxCardFieldTextBytes + 1;
  static constexpr size_t kCardSideBufferBytes = kMaxCardFields * kCardFieldBufferBytes;

  enum class Screen : uint8_t {
    Prompt,
    Answer,
    Empty,
  };

  AnkiReviewCandidates candidates_;
  std::unique_ptr<AnkiDeck> deck_;
  ReviewStateStore reviewState_;
  // Two 16,392-byte source allocations are needed because v2 decode supplies
  // one 2,049-byte slot per field. Flattened text reuses each allocation tail.
  std::unique_ptr<char[]> promptBuffer_;
  std::unique_ptr<char[]> answerBuffer_;
  CardFields cardFields_{};
  FlattenedCardText promptText_{};
  FlattenedCardText answerText_{};
  CardTextLayout cardLayout_{};
  int primaryFontId_ = 0;
  uint8_t primarySdFontSize_ = 0;
  uint8_t promptScale_ = 1;
  uint32_t currentCardIndex_ = 0;
  uint32_t sessionReviews_ = 0;
  uint32_t dueCards_ = 0;
  int refreshCountdown_ = 0;
  ReviewState currentState_{};
  Screen screen_ = Screen::Empty;
  bool stateOpen_ = false;
  bool primaryLayoutActive_ = false;

  bool rebuildCandidates();
  bool loadCurrentCard();
  bool gradeCurrentCard(AnkiReviewGrade grade, uint64_t nowMilliseconds);
  bool flushIfDue(uint64_t nowMilliseconds);
  void closeReviewState();
  void showAndReturnHome(const char* message);
  void bindCardFieldBuffers();
  bool flattenCardSide(const std::array<CardField, kMaxCardFields>& fields, uint8_t fieldCount, char* buffer,
                       FlattenedCardText& out);
  void getCardContentBounds(int& left, int& top, int& right, int& bottom) const;
  bool prepareCardText(const FlattenedCardText& text, int activeFontId);
  bool prepareCardTextLayout(const FlattenedCardText& text, int activeFontId);
  bool prepareCardTextLayoutAtScale(const FlattenedCardText& text, int activeFontId);
  bool preparePrimaryLayout(const FlattenedCardText& text, int primaryFontId, int normalLineHeight);
  bool tryPreparePrimaryLayout(const FlattenedCardText& text, int activeFontId);
  void renderQuestionContext(int fontId);
  void renderCardText(char* text, int activeFontId, FontRole role) const;
};
