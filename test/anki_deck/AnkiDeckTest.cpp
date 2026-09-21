#include <AnkiDeck.h>
#include <HalStorage.h>
#include <ReviewStateStore.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}
void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void put64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

struct FixtureOptions {
  uint16_t version = 1;
  uint16_t headerSize = 32;
  uint32_t indexOffset = 0;
  bool invalidTitle = false;
  bool invalidTextRange = false;
  bool invalidMagic = false;
  bool duplicateSourceId = false;
  uint64_t deckId = 0x1122334455667788ULL;
  uint32_t cardCount = 2;
  bool allReview = false;
};

std::vector<uint8_t> makeDeck(const FixtureOptions& options = {}) {
  const std::string title = options.invalidTitle ? std::string("\xc3\x28", 2) : "Deck";
  const uint32_t indexOffset = options.indexOffset ? options.indexOffset : 32 + title.size();
  const uint32_t textOffset = indexOffset + options.cardCount * 29;
  std::vector<uint8_t> out;
  out.insert(out.end(), options.invalidMagic ? std::initializer_list<uint8_t>{'B', 'A', 'D', '!'}
                                             : std::initializer_list<uint8_t>{'C', 'K', 'D', 'K'});
  put16(out, options.version);
  put16(out, options.headerSize);
  put64(out, options.deckId);
  put32(out, options.cardCount);
  put16(out, static_cast<uint16_t>(title.size()));
  put16(out, 0);
  put32(out, indexOffset);
  put32(out, textOffset);
  out.insert(out.end(), title.begin(), title.end());
  while (out.size() < indexOffset) out.push_back(0);

  std::vector<uint8_t> text;
  for (uint32_t i = 0; i < options.cardCount; ++i) {
    const std::string prompt = "Q" + std::to_string(i);
    const std::string answer = "A" + std::to_string(i);
    put64(out, options.duplicateSourceId && i == 1 ? 1 : i + 1);
    put32(out, options.invalidTextRange && i == 0 ? 9999 : static_cast<uint32_t>(text.size()));
    put16(out, static_cast<uint16_t>(prompt.size()));
    text.insert(text.end(), prompt.begin(), prompt.end());
    put32(out, static_cast<uint32_t>(text.size()));
    put16(out, static_cast<uint16_t>(answer.size()));
    text.insert(text.end(), answer.begin(), answer.end());
    put32(out, i == 0 ? 3 : (options.allReview ? 7 : 0));
    put16(out, i == 0 ? 4 : (options.allReview ? 2 : 1));
    put16(out, static_cast<uint16_t>(i));
    out.push_back(static_cast<uint8_t>(i == 0 || options.allReview ? ReviewKind::Review : ReviewKind::New));
  }
  EXPECT_EQ(out.size(), textOffset);
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

using StyledBlock = std::pair<std::string, bool>;

std::vector<uint8_t> encodeStyledSide(const std::vector<StyledBlock>& fields) {
  std::vector<uint8_t> bytes;
  bytes.push_back(static_cast<uint8_t>(fields.size()));
  for (const StyledBlock& field : fields) {
    bytes.push_back(field.second ? 1 : 0);
    put16(bytes, static_cast<uint16_t>(field.first.size()));
    bytes.insert(bytes.end(), field.first.begin(), field.first.end());
  }
  return bytes;
}

std::vector<uint8_t> makeV2Deck(const std::vector<StyledBlock>& promptFields,
                                const std::vector<StyledBlock>& answerFields) {
  constexpr std::string_view title = "Deck";
  const std::vector<uint8_t> prompt = encodeStyledSide(promptFields);
  const std::vector<uint8_t> answer = encodeStyledSide(answerFields);
  constexpr uint32_t indexOffset = 32 + title.size();
  constexpr uint32_t textOffset = indexOffset + 29;

  std::vector<uint8_t> bytes;
  bytes.insert(bytes.end(), {'C', 'K', 'D', 'K'});
  put16(bytes, 2);
  put16(bytes, 32);
  put64(bytes, 0x1122334455667788ULL);
  put32(bytes, 1);
  put16(bytes, title.size());
  put16(bytes, 29);
  put32(bytes, indexOffset);
  put32(bytes, textOffset);
  bytes.insert(bytes.end(), title.begin(), title.end());
  put64(bytes, 1);
  put32(bytes, 0);
  put16(bytes, static_cast<uint16_t>(prompt.size()));
  put32(bytes, static_cast<uint32_t>(prompt.size()));
  put16(bytes, static_cast<uint16_t>(answer.size()));
  put32(bytes, 0);
  put16(bytes, 1);
  put16(bytes, 0);
  bytes.push_back(static_cast<uint8_t>(ReviewKind::New));
  EXPECT_EQ(bytes.size(), textOffset);
  bytes.insert(bytes.end(), prompt.begin(), prompt.end());
  bytes.insert(bytes.end(), answer.begin(), answer.end());
  return bytes;
}

class AnkiDeckTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("anki-deck-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root_);
    HalStorage::setRoot(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }
  std::string writeDeck(const FixtureOptions& options = {}) {
    const auto bytes = makeDeck(options);
    const auto file = root_ / "deck.cdeck";
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return "/deck.cdeck";
  }
  std::filesystem::path hostPath(const std::string& devicePath) const {
    return root_ / std::filesystem::path(devicePath).relative_path();
  }
  std::string writeBytes(const std::vector<uint8_t>& bytes) {
    const auto file = root_ / "deck.cdeck";
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return "/deck.cdeck";
  }
  std::filesystem::path root_;
};

TEST_F(AnkiDeckTest, RejectsInvalidHeaderVersionHeaderSizeAndUtf8OnLoad) {
  for (const FixtureOptions& options : {FixtureOptions{.invalidMagic = true}, FixtureOptions{.version = 2},
                                        FixtureOptions{.headerSize = 31}, FixtureOptions{.invalidTitle = true}}) {
    AnkiDeck deck;
    EXPECT_FALSE(deck.load(writeDeck(options)));
  }
}

TEST_F(AnkiDeckTest, RejectsInvalidRangesAndDuplicateIdsBeforeStateUse) {
  for (const FixtureOptions& options :
       {FixtureOptions{.invalidTextRange = true}, FixtureOptions{.duplicateSourceId = true}}) {
    AnkiDeck deck;
    ASSERT_TRUE(deck.load(writeDeck(options)));
    ReviewStateStore state;
    EXPECT_FALSE(state.open(deck));
  }
}

TEST_F(AnkiDeckTest, LoadsAndReadsV1FieldsWithoutCatalog) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  EXPECT_STREQ(deck.title(), "Deck");
  EXPECT_EQ(deck.cardCount(), 2U);

  std::array<std::array<char, 2049>, kMaxCardFields> promptBuffers{};
  std::array<std::array<char, 2049>, kMaxCardFields> answerBuffers{};
  CardFields fields{};
  for (uint8_t index = 0; index < kMaxCardFields; ++index) {
    fields.prompt[index].text = promptBuffers[index].data();
    fields.answer[index].text = answerBuffers[index].data();
  }

  ASSERT_TRUE(deck.readCardFields(1, fields));
  ASSERT_EQ(fields.promptCount, 1U);
  ASSERT_EQ(fields.answerCount, 1U);
  EXPECT_STREQ(fields.prompt[0].text, "Q1");
  EXPECT_STREQ(fields.answer[0].text, "A1");
  EXPECT_TRUE(fields.prompt[0].primary);
  EXPECT_TRUE(fields.answer[0].primary);
  EXPECT_FALSE(deck.readCardFields(2, fields));
}

TEST_F(AnkiDeckTest, ReadsV2StyledFieldBlocksAndPrimaryFlags) {
  AnkiDeck deck;
  ASSERT_TRUE(
      deck.load(writeBytes(makeV2Deck({{"Term", true}, {"Reading", false}}, {{"Meaning", false}, {"Hint", true}}))));

  std::array<std::array<char, 2049>, kMaxCardFields> promptBuffers{};
  std::array<std::array<char, 2049>, kMaxCardFields> answerBuffers{};
  CardFields fields{};
  for (uint8_t index = 0; index < kMaxCardFields; ++index) {
    fields.prompt[index].text = promptBuffers[index].data();
    fields.answer[index].text = answerBuffers[index].data();
  }

  ASSERT_TRUE(deck.readCardFields(0, fields));
  ASSERT_EQ(fields.promptCount, 2U);
  ASSERT_EQ(fields.answerCount, 2U);
  EXPECT_STREQ(fields.prompt[0].text, "Term");
  EXPECT_EQ(fields.prompt[0].length, 4U);
  EXPECT_TRUE(fields.prompt[0].primary);
  EXPECT_STREQ(fields.prompt[1].text, "Reading");
  EXPECT_FALSE(fields.prompt[1].primary);
  EXPECT_STREQ(fields.answer[0].text, "Meaning");
  EXPECT_FALSE(fields.answer[0].primary);
  EXPECT_STREQ(fields.answer[1].text, "Hint");
  EXPECT_TRUE(fields.answer[1].primary);
}

TEST_F(AnkiDeckTest, ReadsPortalGeneratedV2ContractFixture) {
  const auto fixture = std::filesystem::path(ANKI_DECK_FIXTURES_DIR) / "portal-v2.cdeck";
  const auto deckFile = root_ / "portal-v2.cdeck";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::copy_file(fixture, deckFile, std::filesystem::copy_options::overwrite_existing, error))
      << error.message();

  AnkiDeck deck;
  ASSERT_TRUE(deck.load("/portal-v2.cdeck"));
  EXPECT_STREQ(deck.title(), "Study - Basic");
  ASSERT_EQ(deck.cardCount(), 1U);

  std::array<std::array<char, 2049>, kMaxCardFields> promptBuffers{};
  std::array<std::array<char, 2049>, kMaxCardFields> answerBuffers{};
  CardFields fields{};
  for (uint8_t index = 0; index < kMaxCardFields; ++index) {
    fields.prompt[index].text = promptBuffers[index].data();
    fields.answer[index].text = answerBuffers[index].data();
  }

  ASSERT_TRUE(deck.readCardFields(0, fields));
  ASSERT_EQ(fields.promptCount, 1U);
  EXPECT_STREQ(fields.prompt[0].text, "Front");
  EXPECT_TRUE(fields.prompt[0].primary);
  ASSERT_EQ(fields.answerCount, 2U);
  EXPECT_STREQ(fields.answer[0].text, "Back");
  EXPECT_FALSE(fields.answer[0].primary);
  EXPECT_STREQ(fields.answer[1].text, "Hint");
  EXPECT_TRUE(fields.answer[1].primary);

  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  ReviewState review{};
  ASSERT_TRUE(state.read(0, review));
  EXPECT_EQ(review.dueDay, 0U);
  EXPECT_EQ(review.intervalDays, 1U);
  EXPECT_EQ(review.kind, ReviewKind::New);
}

TEST_F(AnkiDeckTest, AcceptsV2FieldAndSideTextLimits) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeBytes(
      makeV2Deck({{std::string(2048, 'a'), true}, {std::string(2048, 'b'), false}}, {{"definition", false}}))));

  std::array<std::array<char, 2049>, kMaxCardFields> promptBuffers{};
  std::array<std::array<char, 2049>, kMaxCardFields> answerBuffers{};
  CardFields fields{};
  for (uint8_t index = 0; index < kMaxCardFields; ++index) {
    fields.prompt[index].text = promptBuffers[index].data();
    fields.answer[index].text = answerBuffers[index].data();
  }

  ASSERT_TRUE(deck.readCardFields(0, fields));
  ASSERT_EQ(fields.promptCount, 2U);
  EXPECT_EQ(fields.prompt[0].length, 2048U);
  EXPECT_EQ(fields.prompt[1].length, 2048U);
}
TEST_F(AnkiDeckTest, RejectsMalformedV2FieldPayloadsBeforeCopyingText) {
  const std::vector<uint8_t> valid = makeV2Deck({{"term", true}}, {{"definition", false}});
  std::vector<std::vector<uint8_t>> malformed;

  auto invalidFlags = valid;
  invalidFlags[66] = 0x02;
  malformed.push_back(std::move(invalidFlags));

  auto invalidUtf8 = valid;
  invalidUtf8[69] = 0xC3;
  malformed.push_back(std::move(invalidUtf8));

  auto tooManyBlocks = valid;
  tooManyBlocks[65] = kMaxCardFields + 1;
  malformed.push_back(std::move(tooManyBlocks));

  malformed.push_back(makeV2Deck({{std::string(2049, 'a'), true}}, {{"definition", false}}));

  malformed.push_back(makeV2Deck({{std::string(2048, 'a'), false}, {std::string(2048, 'b'), false}, {"x", true}},
                                 {{"definition", false}}));

  auto outOfRange = valid;
  outOfRange[48] = 0xff;
  outOfRange[49] = 0xff;
  malformed.push_back(std::move(outOfRange));

  for (const std::vector<uint8_t>& bytes : malformed) {
    AnkiDeck deck;
    ASSERT_TRUE(deck.load(writeBytes(bytes)));
    std::array<std::array<char, 2049>, kMaxCardFields> promptBuffers{};
    std::array<std::array<char, 2049>, kMaxCardFields> answerBuffers{};
    CardFields fields{};
    for (uint8_t index = 0; index < kMaxCardFields; ++index) {
      fields.prompt[index].text = promptBuffers[index].data();
      fields.answer[index].text = answerBuffers[index].data();
    }
    EXPECT_FALSE(deck.readCardFields(0, fields));
    ReviewStateStore state;
    EXPECT_FALSE(state.open(deck));
  }
}

TEST_F(AnkiDeckTest, FirstOpenInitializesCountBasedStateFromDeck) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  EXPECT_EQ(state.reviewCount(), 0U);
  ReviewState first{};
  ReviewState second{};
  ASSERT_TRUE(state.read(0, first));
  ASSERT_TRUE(state.read(1, second));
  EXPECT_EQ(first.dueDay, 0U);
  EXPECT_EQ(first.intervalDays, 4U);
  EXPECT_EQ(first.kind, ReviewKind::Review);
  EXPECT_EQ(second.dueDay, 0U);
  EXPECT_EQ(second.intervalDays, 1U);
  EXPECT_EQ(second.kind, ReviewKind::New);
}

TEST_F(AnkiDeckTest, FirstInitializationStreamsDeckMetadataWithOneOpenAfterValidation) {
  AnkiDeck deck;
  constexpr uint32_t kCardCount = 24;
  ASSERT_TRUE(deck.load(writeDeck(FixtureOptions{.cardCount = kCardCount, .allReview = true})));
  HalStorage::resetOpenCounts();

  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));

  // open() validates once, then initialization must stream all metadata through one additional deck handle.
  EXPECT_EQ(HalStorage::readOpenCount(), 2U);
  for (uint32_t index = 0; index < kCardCount; ++index) {
    ReviewState record{};
    ASSERT_TRUE(state.read(index, record));
    EXPECT_EQ(record.dueDay, 0U);
    EXPECT_EQ(record.intervalDays, index == 0 ? 4U : 2U);
    EXPECT_EQ(record.kind, ReviewKind::Review);
  }
}

TEST_F(AnkiDeckTest, FirstOpenMakesAllImportedReviewCardsImmediatelyEligible) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck(FixtureOptions{.allReview = true})));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));

  ReviewState first{};
  ReviewState second{};
  ASSERT_TRUE(state.read(0, first));
  ASSERT_TRUE(state.read(1, second));
  EXPECT_EQ(first.kind, ReviewKind::Review);
  EXPECT_EQ(second.kind, ReviewKind::Review);
  EXPECT_EQ(first.dueDay, 0U);
  EXPECT_EQ(second.dueDay, 0U);
}

TEST_F(AnkiDeckTest, ReinitializesLegacyV1StateInsteadOfReadingDateBasedDueValues) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  const auto primary = hostPath(deck.getCachePath() + "/review.bin");
  std::filesystem::create_directories(primary.parent_path());
  std::vector<uint8_t> legacy;
  legacy.insert(legacy.end(), {'C', 'K', 'R', 'S'});
  put16(legacy, 1);
  put16(legacy, 20);
  put64(legacy, 0x1122334455667788ULL);
  put32(legacy, 2);
  put32(legacy, 99);
  put16(legacy, 4);
  legacy.push_back(static_cast<uint8_t>(ReviewKind::Review));
  legacy.push_back(0);
  put32(legacy, 77);
  put16(legacy, 1);
  legacy.push_back(static_cast<uint8_t>(ReviewKind::New));
  legacy.push_back(0);
  std::ofstream output(primary, std::ios::binary);
  output.write(reinterpret_cast<const char*>(legacy.data()), static_cast<std::streamsize>(legacy.size()));
  output.close();

  ReviewState state;
  ReviewStateStore store;
  ASSERT_TRUE(store.open(deck));
  EXPECT_EQ(store.reviewCount(), 0U);
  ASSERT_TRUE(store.read(0, state));
  EXPECT_EQ(state.dueDay, 0U);
}

TEST_F(AnkiDeckTest, PersistsReviewCountAndDueReviewCountAcrossReopen) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  ASSERT_TRUE(state.replace(0, ReviewState{7, 5, ReviewKind::Review, 1}, 10));
  ASSERT_TRUE(state.advanceReviewCount(10));
  ASSERT_TRUE(state.advanceReviewCount(11));
  ASSERT_TRUE(state.advanceReviewCount(12));
  ASSERT_TRUE(state.flush());

  ReviewStateStore reloaded;
  ASSERT_TRUE(reloaded.open(deck));
  EXPECT_EQ(reloaded.reviewCount(), 3U);
  ReviewState persisted{};
  ASSERT_TRUE(reloaded.read(0, persisted));
  EXPECT_EQ(persisted.dueDay, 7U);
  EXPECT_EQ(persisted.intervalDays, 5U);
  EXPECT_EQ(persisted.flags, 1U);
}

TEST_F(AnkiDeckTest, DeckIdMismatchReinitializesState) {
  const std::string path = writeDeck();
  AnkiDeck firstDeck;
  ASSERT_TRUE(firstDeck.load(path));
  ReviewStateStore firstState;
  ASSERT_TRUE(firstState.open(firstDeck));
  ASSERT_TRUE(firstState.replace(0, ReviewState{99, 9, ReviewKind::Review, 0}, 0));
  ASSERT_TRUE(firstState.flush());

  writeDeck(FixtureOptions{.deckId = 9});
  AnkiDeck replacedDeck;
  ASSERT_TRUE(replacedDeck.load(path));
  ReviewStateStore replacedState;
  ASSERT_TRUE(replacedState.open(replacedDeck));
  ReviewState recovered{};
  ASSERT_TRUE(replacedState.read(0, recovered));
  EXPECT_EQ(recovered.dueDay, 0U);
  EXPECT_EQ(recovered.intervalDays, 4U);
}
TEST_F(AnkiDeckTest, DoesNotQueueCardMutationWhenReviewCountIsSaturated) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  const auto primary = hostPath(deck.getCachePath() + "/review.bin");
  std::filesystem::create_directories(primary.parent_path());
  std::vector<uint8_t> persisted{'C', 'K', 'R', 'S'};
  put16(persisted, 2);
  put16(persisted, 24);
  put64(persisted, 0x1122334455667788ULL);
  put32(persisted, 2);
  put32(persisted, std::numeric_limits<uint32_t>::max());
  put32(persisted, 0);
  put16(persisted, 4);
  persisted.push_back(static_cast<uint8_t>(ReviewKind::Review));
  persisted.push_back(0);
  put32(persisted, 0);
  put16(persisted, 1);
  persisted.push_back(static_cast<uint8_t>(ReviewKind::New));
  persisted.push_back(0);
  std::ofstream output(primary, std::ios::binary);
  output.write(reinterpret_cast<const char*>(persisted.data()), static_cast<std::streamsize>(persisted.size()));
  output.close();

  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  EXPECT_EQ(state.reviewCount(), std::numeric_limits<uint32_t>::max());
  EXPECT_FALSE(state.replaceAndAdvance(0, ReviewState{9, 5, ReviewKind::Review, 1}, 100));

  ReviewState current{};
  ASSERT_TRUE(state.read(0, current));
  EXPECT_EQ(current.dueDay, 0U);
  EXPECT_EQ(current.intervalDays, 4U);
  EXPECT_EQ(current.flags, 0U);
}

TEST_F(AnkiDeckTest, PendingReplacementWinsBeforeFlush) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  ASSERT_TRUE(state.replace(0, ReviewState{3, 2, ReviewKind::Learning, 0}, 10));
  ASSERT_TRUE(state.replace(0, ReviewState{7, 5, ReviewKind::Review, 1}, 11));
  ReviewState current{};
  ASSERT_TRUE(state.read(0, current));
  EXPECT_EQ(current.dueDay, 7U);
  EXPECT_EQ(current.intervalDays, 5U);
  EXPECT_EQ(current.flags, 1U);
}

TEST_F(AnkiDeckTest, ReportsTenUpdatesAndFiveMinuteFlushBoundaries) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck(FixtureOptions{.cardCount = 10})));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  for (uint32_t index = 0; index < 9; ++index) {
    ASSERT_TRUE(state.replace(index, ReviewState{index + 2, 1, ReviewKind::Review, 0}, 1000));
  }
  EXPECT_FALSE(state.shouldFlush(1000));
  ASSERT_TRUE(state.replace(9, ReviewState{11, 1, ReviewKind::Review, 0}, 1000));
  EXPECT_TRUE(state.shouldFlush(1000));
  ASSERT_TRUE(state.flush());
  ASSERT_TRUE(state.replace(0, ReviewState{12, 2, ReviewKind::Review, 0}, 1000));
  EXPECT_FALSE(state.shouldFlush(300999));
  EXPECT_TRUE(state.shouldFlush(301000));
}

TEST_F(AnkiDeckTest, FallsBackToBackupWhenPrimaryStateIsCorrupt) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  ASSERT_TRUE(state.replace(0, ReviewState{55, 6, ReviewKind::Review, 0}, 0));
  ASSERT_TRUE(state.flush());
  ASSERT_TRUE(state.replace(0, ReviewState{66, 7, ReviewKind::Review, 0}, 1));
  ASSERT_TRUE(state.flush());
  const auto primary = hostPath(deck.getCachePath() + "/review.bin");
  std::ofstream corrupt(primary, std::ios::binary | std::ios::trunc);
  corrupt << "broken";
  corrupt.close();

  ReviewStateStore restored;
  ASSERT_TRUE(restored.open(deck));
  ReviewState current{};
  ASSERT_TRUE(restored.read(0, current));
  EXPECT_EQ(current.dueDay, 55U);
  EXPECT_EQ(current.intervalDays, 6U);
}

TEST_F(AnkiDeckTest, StreamsStateAndMergesPendingUpdates) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  ASSERT_TRUE(state.replace(0, ReviewState{9, 2, ReviewKind::Learning, 0}, 0));
  ASSERT_TRUE(state.beginStream());
  ReviewState streamed{};
  ASSERT_TRUE(state.readNext(streamed));
  EXPECT_EQ(streamed.dueDay, 9U);
  EXPECT_EQ(streamed.kind, ReviewKind::Learning);
  ASSERT_TRUE(state.readNext(streamed));
  EXPECT_EQ(streamed.kind, ReviewKind::New);
  EXPECT_FALSE(state.readNext(streamed));
  state.endStream();
}
TEST_F(AnkiDeckTest, RejectsIndexCorruptionAfterLoadBeforeCopyingFields) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  const auto deckFile = hostPath(deck.getPath());
  std::fstream corrupt(deckFile, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(corrupt.is_open());
  corrupt.seekp(36 + 12);
  const std::array<uint8_t, 2> zeroLength{0, 0};
  corrupt.write(reinterpret_cast<const char*>(zeroLength.data()), zeroLength.size());
  corrupt.close();

  std::array<std::array<char, 2049>, kMaxCardFields> promptBuffers{};
  std::array<std::array<char, 2049>, kMaxCardFields> answerBuffers{};
  CardFields fields{};
  for (uint8_t index = 0; index < kMaxCardFields; ++index) {
    fields.prompt[index].text = promptBuffers[index].data();
    fields.answer[index].text = answerBuffers[index].data();
  }
  EXPECT_FALSE(deck.readCardFields(0, fields));
}

TEST_F(AnkiDeckTest, PreservesValidBackupAfterRecoveryFlush) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  ASSERT_TRUE(state.replace(0, ReviewState{55, 6, ReviewKind::Review, 0}, 0));
  ASSERT_TRUE(state.flush());
  ASSERT_TRUE(state.replace(0, ReviewState{66, 7, ReviewKind::Review, 0}, 1));
  ASSERT_TRUE(state.flush());
  const auto primary = hostPath(deck.getCachePath() + "/review.bin");
  std::ofstream corrupt(primary, std::ios::binary | std::ios::trunc);
  corrupt << "broken";
  corrupt.close();

  ReviewStateStore recovered;
  ASSERT_TRUE(recovered.open(deck));
  ASSERT_TRUE(recovered.replace(0, ReviewState{77, 8, ReviewKind::Review, 0}, 2));
  ASSERT_TRUE(recovered.flush());
  std::ofstream corruptAgain(primary, std::ios::binary | std::ios::trunc);
  corruptAgain << "broken-again";
  corruptAgain.close();

  ReviewStateStore recoveredAgain;
  ASSERT_TRUE(recoveredAgain.open(deck));
  ReviewState current{};
  ASSERT_TRUE(recoveredAgain.read(0, current));
  EXPECT_EQ(current.dueDay, 55U);
  EXPECT_EQ(current.intervalDays, 6U);
}

TEST_F(AnkiDeckTest, ReplacesStateWithZeroIntervalAsCorrupt) {
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck()));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  const auto primary = hostPath(deck.getCachePath() + "/review.bin");
  std::fstream corrupt(primary, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(corrupt.is_open());
  corrupt.seekp(24 + 4);
  const std::array<uint8_t, 2> zeroInterval{0, 0};
  corrupt.write(reinterpret_cast<const char*>(zeroInterval.data()), zeroInterval.size());
  corrupt.close();

  ReviewStateStore recovered;
  ASSERT_TRUE(recovered.open(deck));
  ReviewState first{};
  ASSERT_TRUE(recovered.read(0, first));
  EXPECT_EQ(first.dueDay, 0U);
  EXPECT_EQ(first.intervalDays, 4U);
}

}  // namespace

TEST_F(AnkiDeckTest, ExitBatchesLargeDeckSaveAndPreservesAllRecords) {
  FixtureOptions options;
  options.cardCount = 1025;  // Includes a partial final copy chunk.
  AnkiDeck deck;
  ASSERT_TRUE(deck.load(writeDeck(options)));
  ReviewStateStore state;
  ASSERT_TRUE(state.open(deck));
  const ReviewState updated{42, 7, ReviewKind::Learning, 3};
  for (uint32_t index : {0u, 31u, 32u, 1024u}) {
    ASSERT_TRUE(state.replaceAndAdvance(index, updated, 1000));
  }
  HalFile::readCalls = 0;
  HalFile::writeCalls = 0;
  ASSERT_TRUE(state.onExit());
  // Save must amortize storage locking rather than issuing I/O per card.
  EXPECT_LT(HalFile::readCalls, 100u);
  EXPECT_LT(HalFile::writeCalls, 100u);

  ReviewStateStore reopened;
  ASSERT_TRUE(reopened.open(deck));
  EXPECT_EQ(reopened.reviewCount(), 4u);
  ASSERT_TRUE(reopened.beginStream());
  for (uint32_t index = 0; index < options.cardCount; ++index) {
    ReviewState actual;
    ASSERT_TRUE(reopened.readNext(actual));
    const bool changed = index == 0 || index == 31 || index == 32 || index == 1024;
    EXPECT_EQ(actual.dueDay, changed ? 42u : 0u);
    EXPECT_EQ(actual.intervalDays, changed ? 7u : 1u);
    EXPECT_EQ(actual.kind, changed ? ReviewKind::Learning : ReviewKind::New);
    EXPECT_EQ(actual.flags, changed ? 3u : 0u);
  }
  reopened.endStream();
  HalFile::readCalls = 0;
  HalFile::writeCalls = 0;
  ASSERT_TRUE(reopened.onExit());
  EXPECT_EQ(HalFile::readCalls, 0u);
  EXPECT_EQ(HalFile::writeCalls, 0u);
}
