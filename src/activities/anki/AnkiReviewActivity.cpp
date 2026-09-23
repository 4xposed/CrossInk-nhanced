#include "AnkiReviewActivity.h"

#include <Arduino.h>
#include <CrossPointSettings.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontSystem.h>
#include <fontIds.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/TouchUi.h"
#include "components/UITheme.h"

namespace {
constexpr const char* kLogTag = "ANKI";

// Shared geometry for drawing and hit testing; no per-frame heap allocations.
struct ReviewTouchLayout {
  Rect header{};
  Rect back{};
  Rect content{};
  Rect action{};
  Rect ratings[4] = {Rect{}, Rect{}, Rect{}, Rect{}};
  Rect progress{};
};

ReviewTouchLayout reviewTouchLayout(const GfxRenderer& renderer, const bool answer) {
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int width = renderer.getScreenWidth() - left - right;
  const int padding = std::max(12, width / 22);
  const int gap = std::max(8, width / 45);
  const int height = renderer.getScreenHeight() - top - bottom;
  const int buttonHeight = std::max(48, std::min(width * 16 / 100, height / 10));
  const int headerHeight = std::max(52, std::min(width * 15 / 100, height / 10));
  const int progressHeight = renderer.getLineHeight(TouchUi::smallFont()) + gap * 2;
  const int bottomPadding = std::max(8, width * 3 / 100);
  ReviewTouchLayout layout{};
  layout.header = Rect{left + padding, TouchUi::statusHeight(renderer), width - padding * 2, headerHeight};
  layout.back =
      Rect{layout.header.x, layout.header.y,
           std::max(64, renderer.getTextWidth(TouchUi::bodyFont(), tr(STR_BACK)) + padding * 2), headerHeight};
  layout.progress = Rect{left + padding, renderer.getScreenHeight() - bottom - bottomPadding - progressHeight,
                         width - padding * 2, progressHeight};
  layout.action = Rect{layout.progress.x, layout.progress.y - gap - buttonHeight, layout.progress.width, buttonHeight};
  const int halfWidth = (layout.action.width - gap) / 2;
  for (int index = 0; index < 4; ++index) {
    layout.ratings[index] =
        Rect{layout.action.x + (index % 2) * (halfWidth + gap),
             layout.action.y - buttonHeight - gap + (index / 2) * (buttonHeight + gap), halfWidth, buttonHeight};
  }
  const int contentTop = layout.header.y + layout.header.height + gap;
  const int controlsTop = answer ? layout.ratings[0].y : layout.action.y;
  layout.content = Rect{layout.header.x, contentTop, layout.header.width, std::max(1, controlsTop - gap - contentTop)};
  return layout;
}

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

void drawTouchAction(const GfxRenderer& renderer, const Rect& rect, const char* label, const bool filled) {
  const bool ink = ReaderUtils::readerForegroundBlack();
  if (filled)
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, ink);
  else
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, ink);
  const int font = filled ? UI_12_FONT_ID : TouchUi::bodyFont();
  renderer.drawText(font, rect.x + (rect.width - renderer.getTextWidth(font, label)) / 2,
                    rect.y + (rect.height - renderer.getLineHeight(font)) / 2, label, filled ? !ink : ink);
}

bool isShortBackRelease(MappedInputManager& mappedInput) {
  return mappedInput.wasReleased(MappedInputManager::Button::Back) &&
         mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS;
}

int nextLargerBuiltInReaderFontId() {
  const auto size = SETTINGS.getEffectiveReaderFontSize();
  const bool useBitter = SETTINGS.fontFamily == CrossPointSettings::BITTER;
  switch (size) {
    case CrossPointSettings::TINY:
      return useBitter ? BITTER_12_FONT_ID : LEXENDDECA_12_FONT_ID;
    case CrossPointSettings::SMALL:
      return useBitter ? BITTER_14_FONT_ID : LEXENDDECA_14_FONT_ID;
    case CrossPointSettings::MEDIUM:
      return useBitter ? BITTER_16_FONT_ID : LEXENDDECA_16_FONT_ID;
    case CrossPointSettings::LARGE:
    default:
      return 0;
  }
}

}  // namespace

AnkiReviewActivity::AnkiReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::unique_ptr<AnkiDeck> deck, const int initialRefreshCountdown)
    : Activity("AnkiReview", renderer, mappedInput),
      deck_(std::move(deck)),
      refreshCountdown_(initialRefreshCountdown) {}

void AnkiReviewActivity::onEnter() {
  Activity::onEnter();
  if (!deck_) {
    LOG_ERR(kLogTag, "Cannot enter without a deck");
    showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
    return;
  }
  // The two field decode buffers are allocated once; flattened render text reuses
  // their unused tails, so transitions and rendering do not allocate.
  promptBuffer_ = makeUniqueNoThrow<char[]>(kCardSideBufferBytes);
  answerBuffer_ = makeUniqueNoThrow<char[]>(kCardSideBufferBytes);
  if (!promptBuffer_ || !answerBuffer_) {
    LOG_ERR(kLogTag, "OOM: card field buffers (%u bytes each)", static_cast<unsigned>(kCardSideBufferBytes));
    showAndReturnHome(tr(STR_MEMORY_ERROR));
    return;
  }
  bindCardFieldBuffers();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  mappedInput.setReaderMode(true);

  if (!reviewState_.open(*deck_)) {
    LOG_ERR(kLogTag, "Could not open review state");
    showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
    return;
  }
  stateOpen_ = true;

  if (!rebuildCandidates()) {
    LOG_ERR(kLogTag, "Could not build review candidates");
    showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
    return;
  }
  if (candidates_.count() != 0 && !loadCurrentCard()) {
    LOG_ERR(kLogTag, "Could not load review card");
    showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
    return;
  }

  requestUpdate();
}

void AnkiReviewActivity::onResume() {
  // Settings may have changed while the review was beneath the light panel.
  // ActivityManager holds the render lock while restoring this activity.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  mappedInput.setReaderMode(true);
  if (screen_ == Screen::Prompt || screen_ == Screen::Answer) {
    const auto& text = screen_ == Screen::Prompt ? promptText_ : answerText_;
    if (!prepareCardTextLayout(text, SETTINGS.getReaderFontId())) {
      LOG_ERR(kLogTag, "Could not rebuild card layout after settings return");
      onGoHome();
      return;
    }
  }
  requestUpdate();
}

void AnkiReviewActivity::onExit() {
  Activity::onExit();
  closeReviewState();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  promptText_ = {};
  answerText_ = {};
  cardLayout_ = {};
  promptBuffer_.reset();
  answerBuffer_.reset();
  deck_.reset();
}

bool AnkiReviewActivity::rebuildCandidates() {
  candidates_.reset(reviewState_.reviewCount());
  dueCards_ = 0;
  if (!reviewState_.beginStream()) return false;

  for (uint32_t cardIndex = 0; cardIndex < reviewState_.cardCount(); ++cardIndex) {
    ReviewState state;
    if (!reviewState_.readNext(state)) {
      reviewState_.endStream();
      return false;
    }
    if (state.kind == ReviewKind::New || state.dueDay <= reviewState_.reviewCount()) ++dueCards_;
    candidates_.add(cardIndex, state);
  }
  reviewState_.endStream();
  screen_ = candidates_.count() == 0 ? Screen::Empty : Screen::Prompt;
  return true;
}

bool AnkiReviewActivity::loadCurrentCard() {
  if (candidates_.count() == 0 || !promptBuffer_ || !answerBuffer_) return false;
  currentCardIndex_ = candidates_.currentIndex();
  promptText_ = {};
  answerText_ = {};
  if (!reviewState_.read(currentCardIndex_, currentState_) || !deck_->readCardFields(currentCardIndex_, cardFields_)) {
    return false;
  }

  if (!flattenCardSide(cardFields_.prompt, cardFields_.promptCount, promptBuffer_.get(), promptText_)) return false;
  return prepareCardTextLayout(promptText_, SETTINGS.getReaderFontId());
}

bool AnkiReviewActivity::gradeCurrentCard(const AnkiReviewGrade grade, const uint64_t nowMilliseconds) {
  const uint32_t currentReviewCount = reviewState_.reviewCount();
  if (currentReviewCount == std::numeric_limits<uint32_t>::max()) return false;

  const ReviewState next =
      AnkiReviewSchedule::applyGrade(currentState_, grade, static_cast<uint32_t>(currentReviewCount + 1));
  if (!reviewState_.replaceAndAdvance(currentCardIndex_, next, nowMilliseconds)) return false;
  ++sessionReviews_;
  if (!rebuildCandidates()) return false;
  return candidates_.count() == 0 || loadCurrentCard();
}

bool AnkiReviewActivity::flushIfDue(const uint64_t nowMilliseconds) {
  if (!stateOpen_) return true;
  if (reviewState_.flushIfDue(nowMilliseconds)) return true;
  LOG_ERR(kLogTag, "Could not flush review state");
  return false;
}

void AnkiReviewActivity::closeReviewState() {
  if (!stateOpen_) return;
  if (!reviewState_.onExit()) LOG_ERR(kLogTag, "Could not flush review state on exit");
  stateOpen_ = false;
}

void AnkiReviewActivity::showAndReturnHome(const char* message) {
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  GUI.drawPopup(renderer, message);
  renderer.displayBuffer();
  onGoHome();
}

void AnkiReviewActivity::loop() {
  if (!deck_) return;
  // Idle reviews still reach the five-minute durability boundary without allocating.
  if (!flushIfDue(millis())) {
    showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
    return;
  }

  int tapX = 0, tapY = 0;
  const bool touchUi = TouchUi::enabled(mappedInput);
  const bool tapped = touchUi && mappedInput.wasScreenTapped(tapX, tapY);
  const ReviewTouchLayout layout =
      touchUi ? reviewTouchLayout(renderer, screen_ == Screen::Answer) : ReviewTouchLayout{};
  if (tapped &&
      (contains(layout.back, tapX, tapY) || (screen_ == Screen::Empty && contains(layout.action, tapX, tapY)))) {
    closeReviewState();
    onGoHome();
    return;
  }

  if (screen_ == Screen::Prompt) {
    if ((tapped && contains(layout.action, tapX, tapY)) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Answer layout reserves space for both rating rows before measuring text.
      screen_ = Screen::Answer;
      if (!flattenCardSide(cardFields_.answer, cardFields_.answerCount, answerBuffer_.get(), answerText_) ||
          !prepareCardTextLayout(answerText_, SETTINGS.getReaderFontId())) {
        showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
        return;
      }
      if (!flushIfDue(millis())) {
        showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
        return;
      }
      requestUpdate();
      return;
    }
    if (isShortBackRelease(mappedInput)) {
      closeReviewState();
      onGoHome();
      return;
    }
    return;
  }

  if (screen_ == Screen::Empty) {
    if (isShortBackRelease(mappedInput)) {
      closeReviewState();
      onGoHome();
    }
    return;
  }

  AnkiReviewGrade grade;
  int touchedGrade = -1;
  if (tapped) {
    for (int index = 0; index < 4; ++index) {
      if (contains(layout.ratings[index], tapX, tapY)) touchedGrade = index;
    }
  }
  if (touchedGrade >= 0) {
    static constexpr AnkiReviewGrade grades[] = {AnkiReviewGrade::Again, AnkiReviewGrade::Hard, AnkiReviewGrade::Good,
                                                 AnkiReviewGrade::Easy};
    grade = grades[touchedGrade];
  } else if (isShortBackRelease(mappedInput)) {
    grade = AnkiReviewGrade::Again;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    grade = AnkiReviewGrade::Good;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    grade = AnkiReviewGrade::Hard;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    grade = AnkiReviewGrade::Easy;
  } else {
    return;
  }

  const uint64_t nowMilliseconds = millis();
  if (!gradeCurrentCard(grade, nowMilliseconds) || !flushIfDue(nowMilliseconds)) {
    showAndReturnHome(tr(STR_ANKI_INVALID_DECK));
    return;
  }
  requestUpdate();
}

void AnkiReviewActivity::bindCardFieldBuffers() {
  for (size_t index = 0; index < kMaxCardFields; ++index) {
    cardFields_.prompt[index].text = promptBuffer_.get() + index * kCardFieldBufferBytes;
    cardFields_.answer[index].text = answerBuffer_.get() + index * kCardFieldBufferBytes;
  }
}

void AnkiReviewActivity::getCardContentBounds(int& left, int& top, int& right, int& bottom) const {
  if (TouchUi::enabled(mappedInput)) {
    const Rect content = reviewTouchLayout(renderer, screen_ == Screen::Answer).content;
    left = content.x;
    top = content.y;
    right = content.x + content.width / promptScale_;
    bottom = content.y + content.height / promptScale_;
    return;
  }
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int viewRight = renderer.getScreenWidth() - marginRight;
  const int viewBottom = renderer.getScreenHeight() - marginBottom;
  const int headerHeight = std::min(metrics.headerHeight, viewBottom - marginTop);
  left = marginLeft + metrics.contentSidePadding;
  right = viewRight - metrics.contentSidePadding;
  top = marginTop + headerHeight + metrics.verticalSpacing;
  bottom = viewBottom - metrics.buttonHintsHeight - metrics.verticalSpacing;
  right = left + (right - left) / promptScale_;
  bottom = top + (bottom - top) / promptScale_;
}

bool AnkiReviewActivity::flattenCardSide(const std::array<CardField, kMaxCardFields>& fields, const uint8_t fieldCount,
                                         char* const buffer, FlattenedCardText& out) {
  return flattenCardFieldsInPlace(fields, fieldCount, buffer, kCardSideBufferBytes, out);
}
bool AnkiReviewActivity::prepareCardText(const FlattenedCardText& text, const int activeFontId) {
  if (text.text == nullptr || text.length == 0 || activeFontId == 0) return false;
  if (renderer.isSdCardFont(activeFontId)) {
    renderer.ensureSdCardFontReady(activeFontId, text.text, /*styleMask=*/0x01);
  }
  return renderer.getFontCacheManager() != nullptr;
}

bool AnkiReviewActivity::prepareCardTextLayout(const FlattenedCardText& text, const int activeFontId) {
  promptScale_ = &text == &promptText_ ? std::clamp<uint8_t>(SETTINGS.ankiFontScale, 1, 3) : 1;
  for (;;) {
    const bool ready = prepareCardTextLayoutAtScale(text, activeFontId);
    if (promptScale_ == 1 || (ready && !cardLayout_.topLeftFallback)) return ready;
    // Step down for long fronts before falling back to the full layout area.
    --promptScale_;
  }
}

bool AnkiReviewActivity::prepareCardTextLayoutAtScale(const FlattenedCardText& text, const int activeFontId) {
  primaryLayoutActive_ = false;
  primaryFontId_ = 0;
  primarySdFontSize_ = 0;
  if (!prepareCardText(text, activeFontId)) return false;

  // Built-in primary layout writes into cardLayout_ even when it cannot fit.
  // Try it first so failure cannot erase the normal layout used as fallback.
  const bool sdFont = renderer.isSdCardFont(activeFontId);
  if (!sdFont && tryPreparePrimaryLayout(text, activeFontId)) return true;

  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  getCardContentBounds(left, top, right, bottom);
  const int lineHeight = renderer.getLineHeight(activeFontId);
  if (!layoutCardText(
          text.text, text.length, left, top, right, bottom, lineHeight, cardLayout_,
          [this, activeFontId](const char* const line) { return renderer.getTextWidth(activeFontId, line); })) {
    return false;
  }
  if (sdFont) tryPreparePrimaryLayout(text, activeFontId);
  return true;
}

bool AnkiReviewActivity::preparePrimaryLayout(const FlattenedCardText& text, const int primaryFontId,
                                              const int normalLineHeight) {
  if (cardLayout_.topLeftFallback || primaryFontId == 0) return false;

  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  getCardContentBounds(left, top, right, bottom);
  const int primaryLineHeight = renderer.getLineHeight(primaryFontId);
  if (right <= left || bottom <= top || primaryLineHeight <= normalLineHeight || normalLineHeight <= 0) return false;

  std::array<bool, kMaxCardTextLayoutLines> primaryLines{};
  bool hasPrimary = false;
  for (uint8_t fieldIndex = 0; fieldIndex < text.fieldCount; ++fieldIndex) {
    if (!text.fieldPrimary[fieldIndex]) continue;
    const uint16_t fieldStart = text.fieldStarts[fieldIndex];
    const uint16_t fieldLength = text.fieldLengths[fieldIndex];
    char* const fieldText = text.text + fieldStart;
    if (fieldLength == 0 || fieldStart > text.length || fieldLength > text.length - fieldStart ||
        std::memchr(fieldText, '\n', fieldLength) != nullptr ||
        renderer.getTextWidth(primaryFontId, fieldText) > right - left) {
      return false;
    }
    hasPrimary = true;
  }
  if (!hasPrimary) return false;

  int totalHeight = 0;
  for (uint16_t lineIndex = 0; lineIndex < cardLayout_.lineCount; ++lineIndex) {
    const CardTextLine& line = cardLayout_.lines[lineIndex];
    for (uint8_t fieldIndex = 0; fieldIndex < text.fieldCount; ++fieldIndex) {
      if (!text.fieldPrimary[fieldIndex] || line.length == 0) continue;
      const uint16_t fieldStart = text.fieldStarts[fieldIndex];
      const uint16_t fieldEnd = static_cast<uint16_t>(fieldStart + text.fieldLengths[fieldIndex]);
      const uint16_t lineEnd = static_cast<uint16_t>(line.start + line.length);
      if (line.start >= fieldStart && lineEnd <= fieldEnd) {
        primaryLines[lineIndex] = true;
        break;
      }
    }
    totalHeight += primaryLines[lineIndex] ? primaryLineHeight : normalLineHeight;
  }
  if (totalHeight > bottom - top) return false;

  int y = top + (bottom - top - totalHeight) / 2;
  for (uint16_t lineIndex = 0; lineIndex < cardLayout_.lineCount; ++lineIndex) {
    CardTextLine& line = cardLayout_.lines[lineIndex];
    line.role = primaryLines[lineIndex] ? FontRole::Primary : FontRole::Normal;
    if (line.role == FontRole::Primary) {
      char* const lineText = text.text + line.start;
      char* const lineEnd = lineText + line.length;
      const char saved = *lineEnd;
      *lineEnd = '\0';
      line.x = static_cast<int16_t>(left + (right - left - renderer.getTextWidth(primaryFontId, lineText)) / 2);
      *lineEnd = saved;
    }
    line.y = static_cast<int16_t>(y);
    y += line.role == FontRole::Primary ? primaryLineHeight : normalLineHeight;
  }
  return true;
}

bool AnkiReviewActivity::tryPreparePrimaryLayout(const FlattenedCardText& text, const int activeFontId) {
  bool hasPrimary = false;
  for (uint8_t index = 0; index < text.fieldCount; ++index) hasPrimary = hasPrimary || text.fieldPrimary[index];
  if (!hasPrimary) return false;

  if (!renderer.isSdCardFont(activeFontId)) {
    const int primaryFontId =
        TouchUi::enabled(mappedInput)
            ? (SETTINGS.fontFamily == CrossPointSettings::BITTER ? BITTER_16_FONT_ID : LEXENDDECA_16_FONT_ID)
            : nextLargerBuiltInReaderFontId();
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    getCardContentBounds(left, top, right, bottom);
    if (primaryFontId == 0 ||
        !layoutCardTextWithPrimaryFields(
            text, left, top, right, bottom, renderer.getLineHeight(primaryFontId), renderer.getLineHeight(activeFontId),
            cardLayout_, [this, primaryFontId, activeFontId](const FontRole role, const char* const line) {
              return renderer.getTextWidth(role == FontRole::Primary ? primaryFontId : activeFontId, line);
            })) {
      return false;
    }
    primaryFontId_ = primaryFontId;
    primaryLayoutActive_ = true;
    return true;
  }

  const int normalLineHeight = renderer.getLineHeight(activeFontId);

  std::array<uint8_t, kMaxCardFields> candidateSizes{};
  const size_t candidateCount =
      sdFontSystem.listActiveFamilyCandidateSizes(candidateSizes.data(), candidateSizes.size());
  for (size_t index = 0; index + 1 < candidateCount; ++index) {
    const int primaryFontId = sdFontSystem.activateActiveFamilySize(renderer, candidateSizes[index]);
    const bool fits = primaryFontId != 0 && prepareCardText(text, primaryFontId) &&
                      preparePrimaryLayout(text, primaryFontId, normalLineHeight);
    const int restoredFontId = sdFontSystem.restoreReaderFont(renderer);
    if (fits && restoredFontId != 0 && prepareCardText(text, restoredFontId)) {
      primaryFontId_ = primaryFontId;
      primarySdFontSize_ = candidateSizes[index];
      primaryLayoutActive_ = true;
      return true;
    }
  }
  return false;
}

void AnkiReviewActivity::renderCardText(char* const text, const int activeFontId, const FontRole role) const {
  if (text == nullptr) return;

  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  getCardContentBounds(left, top, right, bottom);
  const int lineHeight = renderer.getLineHeight(activeFontId);
  if (right <= left || bottom <= top || lineHeight <= 0) return;

  if (cardLayout_.topLeftFallback) {
    if (role != FontRole::Normal) return;
    int y = top;
    char* line = text;
    while (y + lineHeight <= bottom) {
      char* const lineEnd = std::strchr(line, '\n');
      if (lineEnd != nullptr) *lineEnd = '\0';
      if (*line != '\0') renderer.drawText(activeFontId, left, y, line, ReaderUtils::readerForegroundBlack());
      if (lineEnd == nullptr) return;
      *lineEnd = '\n';
      line = lineEnd + 1;
      y += lineHeight;
    }
    return;
  }

  for (uint16_t index = 0; index < cardLayout_.lineCount; ++index) {
    const CardTextLine& line = cardLayout_.lines[index];
    if (primaryLayoutActive_ && line.role != role) continue;
    char* const lineStart = text + line.start;
    char* const lineEnd = lineStart + line.length;
    const char saved = *lineEnd;
    *lineEnd = '\0';
    if (line.length != 0)
      renderer.drawText(activeFontId, line.x, line.y, lineStart, ReaderUtils::readerForegroundBlack());
    *lineEnd = saved;
  }
}

void AnkiReviewActivity::renderQuestionContext(const int fontId) {
  // Keep the original question visible above a short answer. Reuse its decoded
  // field buffer and the answer's measured position, without another layout or
  // allocation. Dense cards retain all their answer space instead.
  if (fontId == 0 || cardLayout_.topLeftFallback || cardLayout_.lineCount == 0 || promptText_.text == nullptr ||
      promptText_.fieldCount == 0 || answerText_.text == nullptr ||
      std::strcmp(promptText_.text, answerText_.text) == 0)
    return;
  uint8_t field = 0;
  for (uint8_t index = 0; index < promptText_.fieldCount; ++index) {
    if (promptText_.fieldPrimary[index]) {
      field = index;
      break;
    }
  }
  const uint16_t length = promptText_.fieldLengths[field];
  char* const question = promptText_.text + promptText_.fieldStarts[field];
  if (length == 0 || std::memchr(question, '\n', length) != nullptr) return;
  const char saved = question[length];
  question[length] = '\0';
  const Rect content = reviewTouchLayout(renderer, true).content;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int gap = std::max(12, content.width / 16);
  const int y = cardLayout_.lines[0].y - lineHeight - gap;
  auto* const cache = renderer.getFontCacheManager();
  if (y >= content.y && cache != nullptr && prepareCardText(promptText_, fontId) &&
      renderer.getTextWidth(fontId, question) <= content.width) {
    renderCardTextWithPrewarm(*cache, question, fontId, [this, content, y](char* text, const int font) {
      UITheme::drawCenteredText(renderer, content, font, y, text, ReaderUtils::readerForegroundBlack());
    });
  }
  question[length] = saved;
}

void AnkiReviewActivity::render(RenderLock&&) {
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());

  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int viewRight = renderer.getScreenWidth() - marginRight;
  const int viewBottom = renderer.getScreenHeight() - marginBottom;
  const int headerHeight = std::min(metrics.headerHeight, viewBottom - marginTop);
  const bool touchUi = TouchUi::enabled(mappedInput);
  const ReviewTouchLayout touchLayout =
      touchUi ? reviewTouchLayout(renderer, screen_ == Screen::Answer) : ReviewTouchLayout{};
  if (touchUi) {
    TouchUi::drawStatus(renderer, !ReaderUtils::readerForegroundBlack());
    const bool ink = ReaderUtils::readerForegroundBlack();
    const int headerFont = TouchUi::bodyFont();
    const int headerY = touchLayout.header.y + (touchLayout.header.height - renderer.getLineHeight(headerFont)) / 2;
    const int chevronX = touchLayout.back.x + 2;
    const int chevronY = touchLayout.back.y + touchLayout.back.height / 2;
    renderer.drawLine(chevronX + 6, chevronY - 6, chevronX, chevronY, ink);
    renderer.drawLine(chevronX, chevronY, chevronX + 6, chevronY + 6, ink);
    renderer.drawText(headerFont, touchLayout.back.x + 18, headerY, tr(STR_BACK), ink);
    const char* title = deck_ ? deck_->title() : tr(STR_ANKI);
    const int titleWidth = renderer.getTextWidth(headerFont, title);
    // Clip long titles to their lane without allocating a truncated string.
    const int titleLeft = touchLayout.back.x + touchLayout.back.width;
    const int titleRight = touchLayout.header.x + touchLayout.header.width;
    renderer.beginTextClip(titleLeft, touchLayout.header.y, titleRight - titleLeft, touchLayout.header.height);
    renderer.drawText(headerFont, std::max(titleLeft, titleRight - titleWidth), headerY, title, ink);
    renderer.endTextClip();
  } else {
    GUI.drawHeader(renderer, Rect{marginLeft, marginTop, viewRight - marginLeft, headerHeight},
                   deck_ ? deck_->title() : tr(STR_ANKI), tr(STR_ANKI), true);
  }

  const int contentLeft = marginLeft + metrics.contentSidePadding;
  const int contentRight = viewRight - metrics.contentSidePadding;
  const int contentTop = marginTop + headerHeight + metrics.verticalSpacing;
  const int contentBottom = viewBottom - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (screen_ == Screen::Empty) {
    if (touchUi) {
      const Rect& content = touchLayout.content;
      UITheme::drawCenteredText(renderer, content, UI_12_FONT_ID,
                                content.y + (content.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                                tr(STR_ANKI_NO_CARDS_DUE), ReaderUtils::readerForegroundBlack());
    } else {
      UITheme::drawCenteredText(
          renderer, Rect{contentLeft, contentTop, contentRight - contentLeft, contentBottom - contentTop},
          SETTINGS.getReaderFontId(), contentTop, tr(STR_ANKI_NO_CARDS_DUE), ReaderUtils::readerForegroundBlack());
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else {
    if (touchUi) {
      const Rect& content = touchLayout.content;
      renderer.beginTextClip(content.x, content.y, content.width, content.height);
    }
    const int activeFontId = SETTINGS.getReaderFontId();
    if (touchUi && screen_ == Screen::Answer) renderQuestionContext(activeFontId);
    FlattenedCardText& visibleText = screen_ == Screen::Prompt ? promptText_ : answerText_;
    auto* const fontCache = renderer.getFontCacheManager();
    if (activeFontId == 0 || fontCache == nullptr) {
      LOG_ERR(kLogTag, "Could not prepare selected reader font");
    } else if (!primaryLayoutActive_) {
      if (!renderCardTextWithPrewarm(
              *fontCache, visibleText.text, activeFontId,
              [this](char* const text, const int fontId) { renderCardText(text, fontId, FontRole::Normal); })) {
        LOG_ERR(kLogTag, "Could not prewarm selected reader font");
      }
    } else if (primarySdFontSize_ != 0) {
      const int primaryFontId = sdFontSystem.activateActiveFamilySize(renderer, primarySdFontSize_);
      const bool primaryReady = primaryFontId != 0 && prepareCardText(visibleText, primaryFontId);
      const bool primaryDrawn =
          primaryReady && renderCardTextWithPrewarm(*fontCache, visibleText.text, primaryFontId,
                                                    [this](char* const text, const int fontId) {
                                                      renderCardText(text, fontId, FontRole::Primary);
                                                    });
      const int normalFontId = sdFontSystem.restoreReaderFont(renderer);
      const bool normalReady = normalFontId != 0 && prepareCardText(visibleText, normalFontId);
      const bool normalDrawn =
          normalReady && renderCardTextWithPrewarm(*fontCache, visibleText.text, normalFontId,
                                                   [this](char* const text, const int fontId) {
                                                     renderCardText(text, fontId, FontRole::Normal);
                                                   });
      if (!primaryDrawn || !normalDrawn) LOG_ERR(kLogTag, "Could not render primary card text");
    } else if (!renderCardTextWithPrimaryAndNormalPrewarm(*fontCache, true, primaryFontId_, activeFontId,
                                                          [this, &visibleText](const FontRole role, const int fontId) {
                                                            renderCardText(visibleText.text, fontId, role);
                                                          })) {
      LOG_ERR(kLogTag, "Could not prewarm primary card text");
    }
    if (screen_ == Screen::Prompt && promptScale_ > 1) {
      const Rect content = touchUi ? touchLayout.content
                                   : Rect{contentLeft, contentTop, contentRight - contentLeft, contentBottom - contentTop};
      // Expand in reverse logical order so unread source pixels survive.
      // Reuses the framebuffer: no extra buffer or font tables, including CJK.
      const int width = content.width / promptScale_ * promptScale_;
      const int height = content.height / promptScale_ * promptScale_;
      for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
          const bool ink = renderer.isPixelBlack(content.x + x / promptScale_, content.y + y / promptScale_);
          renderer.drawPixel(content.x + x, content.y + y, ink);
        }
      }
    }
    if (touchUi) renderer.endTextClip();
    if (!touchUi && screen_ == Screen::Prompt) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ANKI_SHOW_ANSWER), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, false,
                          ButtonHintLayout::CompactPrimary);
    } else if (!touchUi) {
      const auto labels =
          mappedInput.mapLabels(tr(STR_ANKI_AGAIN), tr(STR_ANKI_GOOD), tr(STR_ANKI_HARD), tr(STR_ANKI_EASY));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }
  if (touchUi) {
    if (screen_ == Screen::Answer) {
      const char* labels[] = {tr(STR_ANKI_AGAIN), tr(STR_ANKI_HARD), tr(STR_ANKI_GOOD), tr(STR_ANKI_EASY)};
      for (int index = 0; index < 4; ++index)
        drawTouchAction(renderer, touchLayout.ratings[index], labels[index], false);
    } else {
      drawTouchAction(renderer, touchLayout.action, screen_ == Screen::Empty ? tr(STR_BACK) : tr(STR_ANKI_SHOW_ANSWER),
                      true);
    }
    char progress[96];
    snprintf(progress, sizeof(progress), tr(STR_ANKI_REVIEW_PROGRESS), static_cast<unsigned long>(sessionReviews_),
             static_cast<unsigned long>(dueCards_));
    const Rect& footer = touchLayout.progress;
    renderer.drawLine(footer.x, footer.y, footer.x + footer.width, footer.y, ReaderUtils::readerForegroundBlack());
    UITheme::drawCenteredText(renderer, footer, TouchUi::smallFont(),
                              footer.y + (footer.height - renderer.getLineHeight(TouchUi::smallFont())) / 2, progress,
                              ReaderUtils::readerForegroundBlack());
  }
  ReaderUtils::displayWithRefreshCycle(renderer, refreshCountdown_);
}

bool AnkiReviewActivity::prepareManualRefresh() {
  refreshCountdown_ = 1;
  return true;
}

std::string AnkiReviewActivity::getCurrentBookPath() const { return deck_ ? deck_->getPath() : std::string{}; }
