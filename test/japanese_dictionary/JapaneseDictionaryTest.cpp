#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "test/UniqueTempDirectory.h"
#include "Arduino.h"
#include "Deinflector.h"
#include "DictIndex.h"
#include "DictionaryDefinitionModel.h"
#include "DictionaryEngine.h"
#include "DictionaryLookupFlow.h"
#include "DictionaryLookupWorker.h"
#include "DictionaryRegistry.h"
#include "Epub/Page.h"
#include "Epub/RubyGlossary.h"
#include "EpubLookupRequest.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "JapaneseLookupContext.h"
#include "MangaPageTextSource.h"
#include "Memory.h"
#include "PageTextSource.h"
#include "PageTextViewport.h"
#include "PageWordScanCache.h"
#include "PageWordScanner.h"
#include "Utf8.h"
#include "WordLookup.h"
#include "activities/RenderLock.h"
#include "freertos/task.h"

namespace dictionary_definition_model_test {
using ReadyPublishHook = void (*)(void*);
void setReadyPublishHook(ReadyPublishHook hook, void* context);
using CancelAfterFlagHook = void (*)(void*);
void setCancelAfterFlagHook(CancelAfterFlagHook hook, void* context);
}  // namespace dictionary_definition_model_test

namespace {
struct InputRecord {
  std::string headword;
  std::string definition;
  uint8_t priority = 0;
  uint8_t posFlags = 0;
};

struct ParityCase {
  std::string label;
  std::string surface;
  std::string canonicalHeadword;
  size_t matchedUtf8Bytes = 0;
  uint8_t sourceMask = 0;
  bool deinflected = false;
  uint8_t priority = 0;
  JapaneseDictStatus status = JapaneseDictStatus::Found;
};

class JsonCursor {
 public:
  explicit JsonCursor(std::string_view input) : input_(input) {}

  bool consume(char wanted) {
    whitespace();
    if (offset_ >= input_.size() || input_[offset_] != wanted) return fail("expected JSON punctuation");
    ++offset_;
    return true;
  }

  bool take(char wanted) {
    whitespace();
    if (offset_ >= input_.size() || input_[offset_] != wanted) return false;
    ++offset_;
    return true;
  }

  bool string(std::string& out) {
    whitespace();
    if (offset_ >= input_.size() || input_[offset_++] != '"') return fail("expected JSON string");
    out.clear();
    while (offset_ < input_.size()) {
      const char value = input_[offset_++];
      if (value == '"') return true;
      if (static_cast<unsigned char>(value) < 0x20) return fail("control byte in JSON string");
      if (value != '\\') {
        out.push_back(value);
        continue;
      }
      if (offset_ >= input_.size()) return fail("truncated JSON escape");
      const char escaped = input_[offset_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          return fail("unsupported JSON escape");
      }
    }
    return fail("unterminated JSON string");
  }

  bool unsignedInteger(size_t& out) {
    whitespace();
    if (offset_ >= input_.size() || input_[offset_] < '0' || input_[offset_] > '9') {
      return fail("expected unsigned JSON integer");
    }
    out = 0;
    do {
      out = out * 10 + static_cast<size_t>(input_[offset_++] - '0');
    } while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9');
    return true;
  }

  bool boolean(bool& out) {
    whitespace();
    if (input_.substr(offset_, 4) == "true") {
      offset_ += 4;
      out = true;
      return true;
    }
    if (input_.substr(offset_, 5) == "false") {
      offset_ += 5;
      out = false;
      return true;
    }
    return fail("expected JSON boolean");
  }

  bool done() {
    whitespace();
    return offset_ == input_.size() || fail("unconsumed JSON input");
  }

  const std::string& error() const { return error_; }

 private:
  void whitespace() {
    while (offset_ < input_.size() &&
           (input_[offset_] == ' ' || input_[offset_] == '\n' || input_[offset_] == '\r' || input_[offset_] == '\t')) {
      ++offset_;
    }
  }

  bool fail(const char* message) {
    if (error_.empty()) error_ = std::string(message) + " at byte " + std::to_string(offset_);
    return false;
  }

  std::string_view input_;
  size_t offset_ = 0;
  std::string error_;
};

bool parseSourceMasks(JsonCursor& json, std::string& error) {
  if (!json.consume('{')) return false;
  bool vocab = false;
  bool grammar = false;
  bool names = false;
  for (;;) {
    std::string key;
    if (!json.string(key) || !json.consume(':')) return false;
    size_t value = 0;
    if (!json.unsignedInteger(value)) return false;
    if (key == "vocab" && value == DictIndex::DICT_JMDICT)
      vocab = true;
    else if (key == "grammar" && value == DictIndex::DICT_GRAMMAR)
      grammar = true;
    else if (key == "names" && value == DictIndex::DICT_NAMES)
      names = true;
    else {
      error = "invalid source_masks entry: " + key;
      return false;
    }
    if (json.take('}')) break;
    if (!json.consume(',')) return false;
  }
  if (!vocab || !grammar || !names) {
    error = "source_masks is incomplete";
    return false;
  }
  return true;
}

bool parseParityCase(JsonCursor& json, ParityCase& out, std::string& error) {
  if (!json.consume('{')) return false;
  bool label = false, surface = false, canonical = false, matched = false, source = false, deinflected = false,
       priority = false;
  bool statusSeen = false;
  for (;;) {
    std::string key;
    if (!json.string(key) || !json.consume(':')) return false;
    if (key == "label") {
      label = json.string(out.label);
    } else if (key == "surface") {
      surface = json.string(out.surface);
    } else if (key == "canonical_headword") {
      canonical = json.string(out.canonicalHeadword);
    } else if (key == "matched_utf8_bytes") {
      matched = json.unsignedInteger(out.matchedUtf8Bytes);
    } else if (key == "source_mask") {
      size_t value = 0;
      source = json.unsignedInteger(value) && value <= UINT8_MAX;
      out.sourceMask = static_cast<uint8_t>(value);
    } else if (key == "deinflected") {
      deinflected = json.boolean(out.deinflected);
    } else if (key == "priority") {
      size_t value = 0;
      priority = json.unsignedInteger(value) && value <= UINT8_MAX;
      out.priority = static_cast<uint8_t>(value);
    } else if (key == "expected_status") {
      std::string status;
      statusSeen = json.string(status);
      if (status != "not_found") {
        error = "unsupported expected_status: " + status;
        return false;
      }
      out.status = JapaneseDictStatus::NotFound;
    } else {
      error = "unknown parity case field: " + key;
      return false;
    }
    if ((!label && key == "label") || (!surface && key == "surface") || (!canonical && key == "canonical_headword") ||
        (!matched && key == "matched_utf8_bytes") || (!source && key == "source_mask") ||
        (!deinflected && key == "deinflected") || (!priority && key == "priority") ||
        (!statusSeen && key == "expected_status")) {
      return false;
    }
    if (json.take('}')) break;
    if (!json.consume(',')) return false;
  }
  if (!label || !surface || !canonical || !matched || !source || !deinflected || !priority) {
    error = "parity case has missing or invalid required fields";
    return false;
  }
  return true;
}

bool loadParityCases(std::vector<ParityCase>& cases, std::string& error) {
  std::ifstream input(JAPANESE_DICTIONARY_PARITY_CASES, std::ios::binary);
  if (!input) {
    error = "cannot open parity fixture";
    return false;
  }
  const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  JsonCursor json(contents);
  if (!json.consume('{')) return false;
  bool format = false, masks = false, parsedCases = false;
  for (;;) {
    std::string key;
    if (!json.string(key) || !json.consume(':')) break;
    if (key == "format") {
      size_t value = 0;
      format = json.unsignedInteger(value) && value == 1;
    } else if (key == "source_masks") {
      masks = parseSourceMasks(json, error);
    } else if (key == "cases") {
      if (!json.consume('[')) break;
      cases.reserve(16);
      if (!json.take(']')) {
        for (;;) {
          ParityCase parsed;
          if (!parseParityCase(json, parsed, error)) break;
          cases.push_back(std::move(parsed));
          if (json.take(']')) {
            parsedCases = true;
            break;
          }
          if (!json.consume(',')) break;
        }
      } else {
        error = "parity cases array is empty";
      }
    } else {
      error = "unknown parity root field: " + key;
      return false;
    }
    if (!error.empty()) return false;
    if (json.take('}')) break;
    if (!json.consume(',')) break;
  }
  if (error.empty()) error = json.error();
  if (!format || !masks || !parsedCases || cases.empty() || !json.done()) {
    if (error.empty()) error = "parity fixture is missing required root data";
    return false;
  }
  return true;
}

static_assert(!std::is_copy_constructible_v<DictEntry>);
static_assert(!std::is_copy_assignable_v<DictEntry>);
static_assert(std::is_nothrow_move_constructible_v<DictEntry>);
static_assert(std::is_nothrow_move_assignable_v<DictEntry>);
static_assert(!std::is_copy_constructible_v<DictionaryOwnedText>);
static_assert(!std::is_copy_assignable_v<DictionaryOwnedText>);
static_assert(std::is_nothrow_move_constructible_v<DictionaryOwnedText>);
static_assert(std::is_nothrow_move_assignable_v<DictionaryOwnedText>);

struct WorkerJobContext {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  std::atomic_uint32_t calls = 0;
};

void runBlockingWorkerJob(void* context) {
  auto* job = static_cast<WorkerJobContext*>(context);
  std::unique_lock<std::mutex> lock(job->mutex);
  ++job->calls;
  job->entered = true;
  job->condition.notify_all();
  job->condition.wait(lock, [job]() { return job->release; });
}

void runCountingWorkerJob(void* context) { ++static_cast<WorkerJobContext*>(context)->calls; }
void runNoopWorkerJob(void*) {}

bool waitForWorkerEntry(WorkerJobContext& job) {
  std::unique_lock<std::mutex> lock(job.mutex);
  return job.condition.wait_for(lock, std::chrono::seconds(2), [&job]() { return job.entered; });
}

void releaseWorkerJob(WorkerJobContext& job) {
  {
    std::lock_guard<std::mutex> lock(job.mutex);
    job.release = true;
  }
  job.condition.notify_all();
}

TEST(DictionaryLookupWorkerTest, RejectsIncompleteJobsWithoutClaimingTheWorker) {
  DictionaryLookupWorker& worker = DictionaryLookupWorker::instance();
  WorkerJobContext owner;

  EXPECT_FALSE(worker.start(DictionaryWorkerJob{nullptr, &runNoopWorkerJob}));
  EXPECT_FALSE(worker.start(DictionaryWorkerJob{&owner, nullptr}));
  EXPECT_FALSE(worker.isBusy());
  EXPECT_FALSE(worker.owns(&owner));
}

TEST(DictionaryLookupWorkerTest, RejectsBusyContenderWithoutReplacingActiveCallbackAndWaitsForCompletion) {
  DictionaryLookupWorker& worker = DictionaryLookupWorker::instance();
  WorkerJobContext active;
  WorkerJobContext contender;

  ASSERT_TRUE(worker.start(DictionaryWorkerJob{&active, &runBlockingWorkerJob}));
  ASSERT_TRUE(waitForWorkerEntry(active));
  EXPECT_TRUE(worker.isBusy());
  EXPECT_TRUE(worker.owns(&active));
  EXPECT_FALSE(worker.start(DictionaryWorkerJob{&contender, &runCountingWorkerJob}));
  EXPECT_FALSE(worker.owns(&contender));

  freertos_test::delayCallCount = 0;
  std::atomic_bool waitStarted = false;
  std::atomic_bool waitReturned = false;
  std::thread waiter([&]() {
    waitStarted = true;
    worker.waitForOwner(&active);
    waitReturned = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((!waitStarted.load() || freertos_test::delayCallCount.load() == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(waitStarted);
  EXPECT_GT(freertos_test::delayCallCount, 0U);
  EXPECT_FALSE(waitReturned);

  releaseWorkerJob(active);
  waiter.join();
  EXPECT_TRUE(waitReturned);
  EXPECT_EQ(active.calls, 1U);
  EXPECT_EQ(contender.calls, 0U);
  EXPECT_FALSE(worker.isBusy());
}

TEST(DictionaryLookupWorkerTest, ReleasesOwnerAndCanBeReusedByAnotherOwner) {
  DictionaryLookupWorker& worker = DictionaryLookupWorker::instance();
  WorkerJobContext firstOwner;
  WorkerJobContext secondOwner;

  ASSERT_TRUE(worker.start(DictionaryWorkerJob{&firstOwner, &runCountingWorkerJob}));
  worker.waitForOwner(&firstOwner);
  ASSERT_EQ(firstOwner.calls, 1U);
  ASSERT_FALSE(worker.isBusy());

  ASSERT_TRUE(worker.start(DictionaryWorkerJob{&secondOwner, &runCountingWorkerJob}));
  worker.waitForOwner(&secondOwner);

  EXPECT_EQ(secondOwner.calls, 1U);
  EXPECT_FALSE(worker.owns(&secondOwner));
  EXPECT_FALSE(worker.isBusy());
}

TEST(DictionaryOwnedTextTest, MovedFromValueCanBeReused) {
  DictionaryOwnedText first;
  ASSERT_TRUE(first.assign("maximum-length-value"));
  DictionaryOwnedText second(std::move(first));
  EXPECT_EQ(second.view(), "maximum-length-value");
  EXPECT_TRUE(first.empty());
  ASSERT_TRUE(first.assign("x"));
  EXPECT_EQ(first.view(), "x");
}

void appendLe32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void appendLe16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void appendBe32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value));
}

void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(output.good());
}

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> encode(std::vector<InputRecord> records) {
  std::stable_sort(records.begin(), records.end(),
                   [](const auto& left, const auto& right) { return left.headword < right.headword; });
  std::vector<uint8_t> idx;
  std::vector<uint8_t> dat;
  idx.reserve(records.size() * sizeof(DictIndexRecord));
  for (const auto& record : records) {
    std::array<uint8_t, DictIndexRecord::HEADWORD_SIZE> key{};
    EXPECT_LT(record.headword.size(), key.size());
    std::memcpy(key.data(), record.headword.data(), record.headword.size());
    idx.insert(idx.end(), key.begin(), key.end());
    appendLe32(idx, static_cast<uint32_t>(dat.size()));
    appendLe16(idx, static_cast<uint16_t>(record.definition.size()));
    idx.push_back(record.priority);
    idx.push_back(record.posFlags);
    dat.insert(dat.end(), record.definition.begin(), record.definition.end());
  }
  return {idx, dat};
}

std::vector<uint8_t> makeSpx(const std::vector<uint8_t>& idx, uint32_t declaredCount = UINT32_MAX) {
  constexpr uint32_t stride = 48;
  const uint32_t count = static_cast<uint32_t>(idx.size() / sizeof(DictIndexRecord));
  const uint32_t fineCount = (count + stride - 1) / stride;
  std::vector<uint8_t> spx{'C', 'P', 'S', 'P', 'X', '1', 0, 0};
  appendLe32(spx, 1);
  appendLe32(spx, stride);
  appendLe32(spx, declaredCount == UINT32_MAX ? count : declaredCount);
  appendLe32(spx, fineCount);
  appendLe32(spx, 0);
  spx.resize(32, 0);
  for (uint32_t record = 0; record < count; record += stride) {
    const size_t offset = static_cast<size_t>(record) * sizeof(DictIndexRecord);
    spx.insert(spx.end(), idx.begin() + static_cast<std::ptrdiff_t>(offset),
               idx.begin() + static_cast<std::ptrdiff_t>(offset + DictIndexRecord::HEADWORD_SIZE));
  }
  return spx;
}

class JapaneseDictionaryTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = uniqueTempDirectory("crossink-japanese-dictionary");
    HalStorage::setRoot(root_);
    hal_storage_test::reset();
    dict_memory_test::reset();
    dict_arduino_test::reset();
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path resolve(std::string_view firmwarePath) const {
    return root_ / std::filesystem::path(firmwarePath).relative_path();
  }

  std::pair<std::vector<uint8_t>, std::vector<uint8_t>> writeSource(std::string_view basename,
                                                                    std::vector<InputRecord> records, bool spx = false,
                                                                    uint32_t spxDeclaredCount = UINT32_MAX) {
    auto [idx, dat] = encode(std::move(records));
    writeBytes(resolve(std::string(basename) + ".idx"), idx);
    writeBytes(resolve(std::string(basename) + ".dat"), dat);
    if (spx) writeBytes(resolve(std::string(basename) + ".spx"), makeSpx(idx, spxDeclaredCount));
    return {idx, dat};
  }

  void writeVocab(std::vector<InputRecord> records, bool spx = false, uint32_t spxDeclaredCount = UINT32_MAX) {
    writeSource("/dictionaries/jp/vocab", std::move(records), spx, spxDeclaredCount);
  }

  void writeMaximumCacheSource(std::string_view basename) {
    constexpr size_t fineCount = 128 * 122;
    constexpr size_t recordCount = (fineCount - 1) * 48 + 1;
    const auto idxPath = resolve(std::string(basename) + ".idx");
    std::filesystem::create_directories(idxPath.parent_path());
    std::ofstream idx(idxPath, std::ios::binary | std::ios::trunc);
    idx.seekp(static_cast<std::streamoff>(recordCount * sizeof(DictIndexRecord) - 1));
    idx.put('\0');
    ASSERT_TRUE(idx.good());
    idx.close();
    writeBytes(resolve(std::string(basename) + ".dat"), {});

    std::vector<uint8_t> spx{'C', 'P', 'S', 'P', 'X', '1', 0, 0};
    appendLe32(spx, 1);
    appendLe32(spx, 48);
    appendLe32(spx, static_cast<uint32_t>(recordCount));
    appendLe32(spx, static_cast<uint32_t>(fineCount));
    appendLe32(spx, 0);
    spx.resize(32 + fineCount * DictIndexRecord::HEADWORD_SIZE, 0);
    writeBytes(resolve(std::string(basename) + ".spx"), spx);
  }

  void writeText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(output.good());
  }

  void writeStarDict(std::vector<std::pair<std::string, std::string>> records, char type = 'm',
                     std::string_view alternate = {}, std::string_view alternateTarget = {}) {
    std::stable_sort(records.begin(), records.end(),
                     [](const auto& left, const auto& right) { return left.first < right.first; });
    const std::string base = "/dictionaries/en/dict-data";
    std::vector<uint8_t> idx;
    std::vector<uint8_t> dict;
    uint32_t alternateOrdinal = 0;
    for (uint32_t ordinal = 0; ordinal < records.size(); ++ordinal) {
      const auto& [word, definition] = records[ordinal];
      if (word == alternateTarget) alternateOrdinal = ordinal;
      idx.insert(idx.end(), word.begin(), word.end());
      idx.push_back(0);
      appendBe32(idx, static_cast<uint32_t>(dict.size()));
      appendBe32(idx, static_cast<uint32_t>(definition.size()));
      dict.insert(dict.end(), definition.begin(), definition.end());
    }
    writeBytes(resolve(base + ".idx"), idx);
    writeBytes(resolve(base + ".dict"), dict);
    std::string ifo = "StarDict's dict ifo file\nversion=3.0.0\nbookname=Fixture\nwordcount=";
    ifo += std::to_string(records.size());
    ifo += "\nidxfilesize=" + std::to_string(idx.size()) + "\nsametypesequence=";
    ifo.push_back(type);
    ifo += "\n";
    writeText(resolve(base + ".ifo"), ifo);
    writeBytes(resolve(base + ".idx.oft"), std::vector<uint8_t>(38, 0));
    if (!alternate.empty()) {
      std::vector<uint8_t> syn(alternate.begin(), alternate.end());
      syn.push_back(0);
      appendBe32(syn, alternateOrdinal);
      writeBytes(resolve(base + ".syn"), syn);
    }
    Dictionary::setLookupDictPathOverride(base.c_str());
  }

  std::vector<uint8_t> encodeRubyRecords(const std::vector<RubyGlossary::Pair>& records,
                                         uint16_t declaredCount = UINT16_MAX) {
    std::vector<uint8_t> bytes{RubyGlossary::kFileVersion};
    appendLe16(bytes, declaredCount == UINT16_MAX ? static_cast<uint16_t>(records.size()) : declaredCount);
    for (const auto& [base, ruby] : records) {
      bytes.push_back(static_cast<uint8_t>(base.size()));
      bytes.insert(bytes.end(), base.begin(), base.end());
      bytes.push_back(static_cast<uint8_t>(ruby.size()));
      bytes.insert(bytes.end(), ruby.begin(), ruby.end());
    }
    return bytes;
  }

  void writeRubyRecords(const std::vector<RubyGlossary::Pair>& records, uint16_t declaredCount = UINT16_MAX,
                        std::string_view path = "/book-cache/ruby.bin") {
    writeBytes(resolve(path), encodeRubyRecords(records, declaredCount));
  }

  std::filesystem::path root_;
};

TEST_F(JapaneseDictionaryTest, RubyGlossaryWritesTheExactVersionOneBytes) {
  std::vector<RubyGlossary::Pair> pairs;
  RubyGlossary::collect(pairs, "\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93");  // \u732b -> \u306d\u3053
  RubyGlossary::merge("/book-cache", pairs);

  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")),
            (std::vector<uint8_t>{1, 1, 0, 3, 0xE7, 0x8C, 0xAB, 6, 0xE3, 0x81, 0xAD, 0xE3, 0x81, 0x93}));
}

TEST_F(JapaneseDictionaryTest, RubyHarvestIncludesKanaRunsInAKanjiBearingCompound) {
  std::vector<RubyGlossary::Pair> pairs;
  std::string elementBase;
  std::string elementRuby;
  int runCount = 0;
  RubyGlossary::resetElement(elementBase, elementRuby, runCount);
  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, "\xE9\xA3\x9F", "\xE3\x81\x9F");
  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, "\xE3\x81\xB9", "\xE3\x81\xB9");
  RubyGlossary::finishElement(pairs, elementBase, elementRuby, runCount);

  EXPECT_NE(
      std::find(pairs.begin(), pairs.end(), RubyGlossary::Pair{"\xE9\xA3\x9F\xE3\x81\xB9", "\xE3\x81\x9F\xE3\x81\xB9"}),
      pairs.end());
}

TEST_F(JapaneseDictionaryTest, RubyCollectFiltersInvalidPairsAndPreservesBoundedCjkOrder) {
  std::vector<RubyGlossary::Pair> pairs;
  const std::string exactBase = "\xE6\xBC\xA2" + std::string(29, 'a');
  const std::string exactRuby(32, 'r');
  const std::string extensionB = "\xF0\xA0\x80\x80";  // U+20000
  RubyGlossary::collect(pairs, "", "reading");
  RubyGlossary::collect(pairs, "\xE6\xBC\xA2", "");
  RubyGlossary::collect(pairs, "same", "same");
  RubyGlossary::collect(pairs, "kana", "reading");
  RubyGlossary::collect(pairs, "\xE6\xBC\xA2" + std::string(30, 'a'), "reading");
  RubyGlossary::collect(pairs, "\xE6\xBC\xA2", std::string(33, 'r'));
  RubyGlossary::collect(pairs, std::string("\xE6\xBC\xA2\xFF", 4), "reading");
  RubyGlossary::collect(pairs, exactBase, exactRuby);
  RubyGlossary::collect(pairs, extensionB, "ext-b");
  RubyGlossary::collect(pairs, "\xE6\xBC\xA2", "kan");
  RubyGlossary::collect(pairs, extensionB, "ext-b");

  ASSERT_EQ(pairs.size(), 3u);
  EXPECT_EQ(pairs[0], (RubyGlossary::Pair{exactBase, exactRuby}));
  EXPECT_EQ(pairs[1], (RubyGlossary::Pair{extensionB, "ext-b"}));
  EXPECT_EQ(pairs[2], (RubyGlossary::Pair{"\xE6\xBC\xA2", "kan"}));
}

TEST_F(JapaneseDictionaryTest, RubyCollectCapsAtTwoHundredAndGuardsEveryGrowthAllocation) {
  std::vector<RubyGlossary::Pair> pairs;
  for (size_t index = 0; index < RubyGlossary::kMaxPairsPerSection + 8; ++index) {
    RubyGlossary::collect(pairs, "\xE6\xBC\xA2" + std::to_string(index), "r" + std::to_string(index));
  }
  ASSERT_EQ(pairs.size(), RubyGlossary::kMaxPairsPerSection);

  std::vector<RubyGlossary::Pair> lowHeap;
  dict_arduino_test::maxAllocHeap = 9 * 1024;
  RubyGlossary::collect(lowHeap, "\xE6\xBC\xA2", "reading");
  EXPECT_TRUE(lowHeap.empty());

  dict_arduino_test::reset();
  std::vector<RubyGlossary::Pair> copyGuarded;
  copyGuarded.reserve(1);
  dict_arduino_test::maxAllocHeap = 8 * 1024;
  RubyGlossary::collect(copyGuarded, "\xE6\xBC\xA2" + std::string(29, 'a'), std::string(32, 'r'));
  EXPECT_TRUE(copyGuarded.empty());
}

TEST_F(JapaneseDictionaryTest, RubyHarvestCollectsRunsAndCompoundInOrderAndBoundsTheAggregate) {
  std::vector<RubyGlossary::Pair> pairs;
  std::string elementBase;
  std::string elementRuby;
  int runCount = 0;
  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, "\xE5\xB0\x8F", "\xE3\x81\x93");
  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, "\xE6\x9E\x97",
                           "\xE3\x81\xB0\xE3\x82\x84\xE3\x81\x97");
  RubyGlossary::finishElement(pairs, elementBase, elementRuby, runCount);
  ASSERT_EQ(pairs.size(), 3u);
  EXPECT_EQ(pairs[0], (RubyGlossary::Pair{"\xE5\xB0\x8F", "\xE3\x81\x93"}));
  EXPECT_EQ(pairs[1], (RubyGlossary::Pair{"\xE6\x9E\x97", "\xE3\x81\xB0\xE3\x82\x84\xE3\x81\x97"}));
  EXPECT_EQ(pairs[2],
            (RubyGlossary::Pair{"\xE5\xB0\x8F\xE6\x9E\x97", "\xE3\x81\x93\xE3\x81\xB0\xE3\x82\x84\xE3\x81\x97"}));
  EXPECT_TRUE(elementBase.empty());
  EXPECT_TRUE(elementRuby.empty());
  EXPECT_EQ(runCount, 0);

  pairs.clear();
  const std::string longBase = "\xE6\xBC\xA2" + std::string(27, 'a');
  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, longBase, "first");
  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, "\xE5\xAD\x97", "second");
  RubyGlossary::finishElement(pairs, elementBase, elementRuby, runCount);
  EXPECT_EQ(pairs.size(), 2u);
}

TEST_F(JapaneseDictionaryTest, RubyHarvestResetDropsAbortedRunsAndPreviouslyCollectedPairs) {
  std::vector<RubyGlossary::Pair> pairs;
  std::string elementBase;
  std::string elementRuby;
  int runCount = 0;
  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, "\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93");
  ASSERT_FALSE(pairs.empty());
  ASSERT_FALSE(elementBase.empty());

  RubyGlossary::resetHarvest(pairs, elementBase, elementRuby, runCount);

  EXPECT_TRUE(pairs.empty());
  EXPECT_TRUE(elementBase.empty());
  EXPECT_TRUE(elementRuby.empty());
  EXPECT_EQ(runCount, 0);
}

TEST_F(JapaneseDictionaryTest, RubyHarvestRejectsAPreexistingOversizedAggregate) {
  std::vector<RubyGlossary::Pair> pairs;
  std::string elementBase(RubyGlossary::kMaxTextBytes + 1, 'x');
  std::string elementRuby = "old";
  int runCount = 1;

  RubyGlossary::collectRun(pairs, elementBase, elementRuby, runCount, "\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93");

  EXPECT_TRUE(elementBase.empty());
  EXPECT_TRUE(elementRuby.empty());
  EXPECT_EQ(runCount, -1);
}

TEST_F(JapaneseDictionaryTest, RubyHarvestCompletionExcludesPreviewStopsAndMalformedPrefixes) {
  EXPECT_TRUE(RubyGlossary::HarvestCompletion{}.canMerge());
  RubyGlossary::HarvestCompletion preview;
  preview.previewBuild = true;
  EXPECT_FALSE(preview.canMerge());
  RubyGlossary::HarvestCompletion previewStop;
  previewStop.previewStopped = true;
  EXPECT_FALSE(previewStop.canMerge());
  RubyGlossary::HarvestCompletion malformed;
  malformed.malformedMarkupTruncated = true;
  EXPECT_FALSE(malformed.canMerge());
}

TEST_F(JapaneseDictionaryTest, RubyMergePreservesAndDeduplicatesOldRecordsAndJoinsReadingsInFileOrder) {
  writeRubyRecords({{"\xE7\x94\x9F", "\xE3\x81\x9B\xE3\x81\x84"},
                    {"\xE7\x94\x9F", "\xE3\x81\x9B\xE3\x81\x84"},
                    {"\xE7\x94\x9F", "\xE3\x81\x97\xE3\x82\x87\xE3\x81\x86"}});
  std::vector<RubyGlossary::Pair> additions{{"\xE7\x94\x9F", "\xE3\x81\x97\xE3\x82\x87\xE3\x81\x86"},
                                            {"\xE7\x94\x9F", "\xE3\x81\x8A\xE3\x81\x86"},
                                            {"\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93"}};
  RubyGlossary::merge("/book-cache", additions);

  std::string readings;
  ASSERT_TRUE(RubyGlossary::lookup("/book-cache", "\xE7\x94\x9F", readings));
  EXPECT_EQ(
      readings,
      "\xE3\x81\x9B\xE3\x81\x84\xE3\x83\xBB\xE3\x81\x97\xE3\x82\x87\xE3\x81\x86\xE3\x83\xBB\xE3\x81\x8A\xE3\x81\x86");
  const auto bytes = readBytes(resolve("/book-cache/ruby.bin"));
  ASSERT_GE(bytes.size(), 3u);
  EXPECT_EQ(static_cast<uint16_t>(bytes[1] | (bytes[2] << 8)), 4u);
}

TEST_F(JapaneseDictionaryTest, RubyMergeHonorsRecordAndFileByteCaps) {
  std::vector<RubyGlossary::Pair> old;
  old.reserve(RubyGlossary::kMaxFileRecords - 1);
  for (size_t index = 0; index < RubyGlossary::kMaxFileRecords - 1; ++index) {
    char base[8];
    std::snprintf(base, sizeof(base), "b%04u", static_cast<unsigned>(index));
    old.emplace_back(base, "r");
  }
  writeRubyRecords(old);
  RubyGlossary::merge("/book-cache", {{"\xE6\xBC\xA2", "one"}, {"\xE5\xAD\x97", "two"}});
  auto bytes = readBytes(resolve("/book-cache/ruby.bin"));
  ASSERT_GE(bytes.size(), 3u);
  EXPECT_EQ(static_cast<uint16_t>(bytes[1] | (bytes[2] << 8)), RubyGlossary::kMaxFileRecords);
  std::string readings;
  EXPECT_TRUE(RubyGlossary::lookup("/book-cache", "\xE6\xBC\xA2", readings));
  EXPECT_FALSE(RubyGlossary::lookup("/book-cache", "\xE5\xAD\x97", readings));

  old.clear();
  old.reserve(200);
  for (size_t index = 0; index < 200; ++index) {
    char suffix[30];
    std::snprintf(suffix, sizeof(suffix), "%029u", static_cast<unsigned>(index));
    old.emplace_back(std::string(3, 'b') + suffix, std::string(32, 'r'));
  }
  writeRubyRecords(old);
  std::vector<RubyGlossary::Pair> additions;
  additions.reserve(200);
  for (size_t index = 0; index < 200; ++index) {
    char suffix[30];
    std::snprintf(suffix, sizeof(suffix), "%029u", static_cast<unsigned>(index));
    additions.emplace_back("\xE6\xBC\xA2" + std::string(suffix), std::string(32, 'n'));
  }
  RubyGlossary::merge("/book-cache", additions);
  bytes = readBytes(resolve("/book-cache/ruby.bin"));
  ASSERT_LE(bytes.size(), RubyGlossary::kMaxFileBytes);
  EXPECT_EQ(static_cast<uint16_t>(bytes[1] | (bytes[2] << 8)), 248u);
}

TEST_F(JapaneseDictionaryTest, RubyLookupRejectsEveryWholeFileCorruptionWithoutPublishingAPrefix) {
  const auto valid = encodeRubyRecords({{"\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93"}});
  std::vector<std::vector<uint8_t>> corrupt{
      {},
      {2, 0, 0},
      {1, 1, 4},
      {1, 1, 0, 0},
      {1, 1, 0, 33},
      {1, 1, 0, 1, 0xFF, 1, 'x'},
      {1, 1, 0, 3, 0xE7, 0x8C},
      valid,
      std::vector<uint8_t>(RubyGlossary::kMaxFileBytes + 1, 0),
  };
  corrupt[7].push_back(0xAA);
  for (size_t index = 0; index < corrupt.size(); ++index) {
    SCOPED_TRACE(index);
    writeBytes(resolve("/book-cache/ruby.bin"), corrupt[index]);
    std::string readings = "sentinel";
    EXPECT_FALSE(RubyGlossary::lookup("/book-cache", "\xE7\x8C\xAB", readings));
    EXPECT_TRUE(readings.empty());
  }
  std::string readings = "sentinel";
  EXPECT_FALSE(RubyGlossary::lookup("/missing", "\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());
}

TEST_F(JapaneseDictionaryTest, RubyMergeRewritesCorruptInputFromOnlyTheNewHarvest) {
  writeBytes(resolve("/book-cache/ruby.bin"), {1, 2, 0, 3, 0xE7});
  RubyGlossary::merge("/book-cache", {{"\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93"}});
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")),
            (std::vector<uint8_t>{1, 1, 0, 3, 0xE7, 0x8C, 0xAB, 6, 0xE3, 0x81, 0xAD, 0xE3, 0x81, 0x93}));
}

TEST_F(JapaneseDictionaryTest, RubyIoFailuresNeverReplaceThePreviousValidGlossary) {
  writeRubyRecords({{"\xE7\x8C\xAB", "cat"}});
  const std::vector<uint8_t> original = readBytes(resolve("/book-cache/ruby.bin"));
  const std::vector<RubyGlossary::Pair> additions{{"\xE7\x8A\xAC", "dog"}};

  hal_storage_test::readOpenFailurePath = "/book-cache/ruby.bin";
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::shortReadPath = "/book-cache/ruby.bin";
  hal_storage_test::shortReadOffset = 0;
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::closeFailurePath = "/book-cache/ruby.bin";
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::writeOpenFailurePath = "/book-cache/ruby.bin.tmp";
  hal_storage_test::writeOpenFailureCreatesFile = true;
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);
  EXPECT_FALSE(std::filesystem::exists(resolve("/book-cache/ruby.bin.tmp")));

  hal_storage_test::reset();
  hal_storage_test::shortWritePath = "/book-cache/ruby.bin.tmp";
  hal_storage_test::shortWriteOffset = 3;
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::syncFailurePath = "/book-cache/ruby.bin.tmp";
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::closeFailurePath = "/book-cache/ruby.bin.tmp";
  hal_storage_test::closeFailureOrdinal = 1;
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::shortReadPath = "/book-cache/ruby.bin.tmp";
  hal_storage_test::shortReadOffset = 0;
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::closeFailurePath = "/book-cache/ruby.bin.tmp";
  hal_storage_test::closeFailureOrdinal = 2;
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);
}

TEST_F(JapaneseDictionaryTest, RubyInvalidSizeCloseFailurePreservesTheOldGeneration) {
  const std::vector<std::vector<uint8_t>> invalidSizes{
      {RubyGlossary::kFileVersion, 0},
      std::vector<uint8_t>(RubyGlossary::kMaxFileBytes + 1, 0xA5),
  };
  for (const auto& original : invalidSizes) {
    SCOPED_TRACE(original.size());
    hal_storage_test::reset();
    writeBytes(resolve("/book-cache/ruby.bin"), original);
    hal_storage_test::closeFailurePath = "/book-cache/ruby.bin";

    RubyGlossary::merge("/book-cache", {{"\xE7\x8C\xAB", "cat"}});

    EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);
    EXPECT_FALSE(std::filesystem::exists(resolve("/book-cache/ruby.bin.tmp")));
  }
}

TEST_F(JapaneseDictionaryTest, RubyPromotionAndBackupRecoveryKeepAValidGenerationRecoverable) {
  writeRubyRecords({{"\xE7\x8C\xAB", "cat"}});
  const auto original = readBytes(resolve("/book-cache/ruby.bin"));
  const std::vector<RubyGlossary::Pair> additions{{"\xE7\x8A\xAC", "dog"}};

  hal_storage_test::renameFailures.push_back({"/book-cache/ruby.bin", "/book-cache/ruby.bin.bak"});
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);

  hal_storage_test::reset();
  hal_storage_test::renameFailures.push_back({"/book-cache/ruby.bin.tmp", "/book-cache/ruby.bin"});
  RubyGlossary::merge("/book-cache", additions);
  EXPECT_EQ(readBytes(resolve("/book-cache/ruby.bin")), original);
  EXPECT_FALSE(std::filesystem::exists(resolve("/book-cache/ruby.bin.bak")));

  std::filesystem::rename(resolve("/book-cache/ruby.bin"), resolve("/book-cache/ruby.bin.bak"));
  std::string readings;
  ASSERT_TRUE(RubyGlossary::lookup("/book-cache", "\xE7\x8C\xAB", readings));
  EXPECT_EQ(readings, "cat");
  EXPECT_TRUE(std::filesystem::exists(resolve("/book-cache/ruby.bin")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/book-cache/ruby.bin.bak")));
}

TEST_F(JapaneseDictionaryTest, RubyFailedRestoreLeavesTheBackupForTheNextLookup) {
  writeRubyRecords({{"\xE7\x8C\xAB", "cat"}});
  hal_storage_test::renameFailures.push_back({"/book-cache/ruby.bin.tmp", "/book-cache/ruby.bin"});
  hal_storage_test::renameFailures.push_back({"/book-cache/ruby.bin.bak", "/book-cache/ruby.bin"});
  RubyGlossary::merge("/book-cache", {{"\xE7\x8A\xAC", "dog"}});
  EXPECT_FALSE(std::filesystem::exists(resolve("/book-cache/ruby.bin")));
  EXPECT_TRUE(std::filesystem::exists(resolve("/book-cache/ruby.bin.bak")));

  hal_storage_test::reset();
  std::string readings;
  EXPECT_TRUE(RubyGlossary::lookup("/book-cache", "\xE7\x8C\xAB", readings));
  EXPECT_EQ(readings, "cat");
}

TEST_F(JapaneseDictionaryTest, RubyFirstPromotionFailureNeverPublishesATemporaryFile) {
  hal_storage_test::renameFailures.push_back({"/book-cache/ruby.bin.tmp", "/book-cache/ruby.bin"});
  RubyGlossary::merge("/book-cache", {{"\xE7\x8C\xAB", "cat"}});
  EXPECT_FALSE(std::filesystem::exists(resolve("/book-cache/ruby.bin")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/book-cache/ruby.bin.tmp")));
}

class DictionaryRegistryTest : public JapaneseDictionaryTest {
 protected:
  void writeRegistryDictionary(std::string_view basePath) {
    writeText(resolve(std::string(basePath) + ".idx"), "index");
    writeText(resolve(std::string(basePath) + ".ifo"), "metadata");
    writeText(resolve(std::string(basePath) + ".dict"), "definition");
  }

  void writeConfiguredPath(std::string_view cachePath, std::string_view basePath) {
    const std::string path = std::string(cachePath) + "/dictionary.bin";
    writeText(resolve(path), basePath);
  }
};

TEST_F(DictionaryRegistryTest, DiscoversCaseInsensitiveVisibleRootAndPrefersHiddenRootWhenBothExist) {
  writeRegistryDictionary("/DiCtIoNaRiEs/en/visible");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  EXPECT_EQ(registry.root(), "/DiCtIoNaRiEs");
  ASSERT_EQ(registry.count(), 1);
  EXPECT_EQ(registry.getEntries()[0].basePath, "/DiCtIoNaRiEs/en/visible");

  writeRegistryDictionary("/.DiCtIoNaRiEs/de/hidden");
  ASSERT_TRUE(registry.discover());
  EXPECT_EQ(registry.root(), "/.DiCtIoNaRiEs");
  ASSERT_EQ(registry.count(), 2);
  EXPECT_EQ(registry.getEntries()[0].basePath, "/.DiCtIoNaRiEs/de/hidden");
  EXPECT_EQ(registry.getEntries()[1].basePath, "/DiCtIoNaRiEs/en/visible");
}

TEST_F(DictionaryRegistryTest, ScansBothRootsAndDeduplicatesEquivalentFoldersWithFirstRootWinning) {
  writeRegistryDictionary("/.DiCtIoNaRiEs/en/shared/dict-data");
  writeRegistryDictionary("/DiCtIoNaRiEs/EN/shared/dict-data");
  writeRegistryDictionary("/DiCtIoNaRiEs/fr/visible/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  ASSERT_EQ(registry.count(), 2);
  EXPECT_EQ(registry.getEntries()[0].name, "en/shared");
  EXPECT_EQ(registry.getEntries()[0].basePath, "/.DiCtIoNaRiEs/en/shared/dict-data");
  EXPECT_EQ(registry.getEntries()[1].name, "fr/visible");
  EXPECT_EQ(registry.getEntries()[1].basePath, "/DiCtIoNaRiEs/fr/visible/dict-data");
}

TEST_F(DictionaryRegistryTest, KeepsConfiguredFallbackInSecondRootEvenWhenItsFolderCollides) {
  writeRegistryDictionary("/.dictionaries/en/first/dict-data");
  writeRegistryDictionary("/dictionaries/EN/first/dict-data");
  writeConfiguredPath("/.crosspoint", "/dictionaries/EN/first/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  ASSERT_EQ(registry.count(), 1);
  EXPECT_EQ(registry.getEntries()[0].basePath, "/.dictionaries/en/first/dict-data");
  EXPECT_EQ(Dictionary::readConfiguredDictPath(), "/dictionaries/EN/first/dict-data");
  std::string effective;
  EXPECT_TRUE(registry.resolveEffectiveStarDict("de-DE", nullptr, effective));
  EXPECT_EQ(effective, "/.dictionaries/en/first/dict-data");
  EXPECT_EQ(Dictionary::readConfiguredDictPath(), "/dictionaries/EN/first/dict-data");
}

TEST_F(DictionaryRegistryTest, MapsOnlyExactPathsAndVerifiedCollisionAliasesToRetainedEntry) {
  writeRegistryDictionary("/.dictionaries/en/shared/dict-data");
  writeRegistryDictionary("/dictionaries/EN/shared/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  ASSERT_EQ(registry.count(), 1);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/.dictionaries/en/shared/dict-data"), 0);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/DiCtIoNaRiEs/EN/shared/DICT-DATA"), 0);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/tmp/dictionaries/EN/shared/dict-data"), -1);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/dictionaries/extra/EN/shared/dict-data"), -1);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/dictionaries/EN/shared/dict-data/extra"), -1);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/dictionaries/EN/../shared/dict-data"), -1);
}

TEST_F(DictionaryRegistryTest, RejectsUndiscoveredLowerRootLookalike) {
  writeRegistryDictionary("/.dictionaries/en/shared/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  ASSERT_EQ(registry.count(), 1);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/.dictionaries/en/shared/dict-data"), 0);
  EXPECT_EQ(registry.indexOfExactOrEquivalent("/dictionaries/en/shared/dict-data"), -1);
}

TEST_F(DictionaryRegistryTest, DropsRemovedCollisionAliasAndCanonicalizesStaleFallbackOnRediscovery) {
  constexpr std::string_view hiddenBase = "/.dictionaries/en/shared/dict-data";
  constexpr std::string_view visibleBase = "/dictionaries/EN/shared/dict-data";
  writeRegistryDictionary(hiddenBase);
  writeRegistryDictionary(visibleBase);
  writeConfiguredPath("/.crosspoint", visibleBase);

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  ASSERT_EQ(registry.count(), 1);
  EXPECT_EQ(registry.indexOfExactOrEquivalent(visibleBase), 0);
  EXPECT_EQ(Dictionary::readConfiguredDictPath(), visibleBase);

  ASSERT_TRUE(std::filesystem::remove(resolve(std::string(visibleBase) + ".idx")));
  ASSERT_TRUE(std::filesystem::remove(resolve(std::string(visibleBase) + ".dict")));
  ASSERT_TRUE(registry.discover());

  EXPECT_EQ(registry.indexOfExactOrEquivalent(visibleBase), -1);
  EXPECT_EQ(Dictionary::readConfiguredDictPath(), hiddenBase);
  std::string effective;
  EXPECT_TRUE(registry.resolveEffectiveStarDict("de-DE", nullptr, effective));
  EXPECT_EQ(effective, hiddenBase);
}

TEST_F(DictionaryRegistryTest, ReservesBareJapaneseBundleAndDiscoversExactlyOneNestedStarDictLevel) {
  writeText(resolve("/dictionaries/jp/vocab.idx"), "index");
  writeText(resolve("/dictionaries/jp/vocab.dat"), "data");
  writeText(resolve("/dictionaries/jp/names.idx"), "index");
  writeText(resolve("/dictionaries/jp/names.dat"), "data");
  writeText(resolve("/dictionaries/jp/grammar.idx"), "index-only");
  writeRegistryDictionary("/dictionaries/jp/Zeta/dict-data");
  writeRegistryDictionary("/dictionaries/jp/alpha/dict-data");
  writeRegistryDictionary("/dictionaries/jp/too-deep/child/dict-data");
  writeRegistryDictionary("/dictionaries/jp/bad\\segment/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  const JapaneseDictionaryBundle bundle = registry.japaneseBundle();
  EXPECT_TRUE(bundle.vocabulary);
  EXPECT_TRUE(bundle.names);
  EXPECT_FALSE(bundle.grammar);
  ASSERT_EQ(registry.count(), 2);
  EXPECT_EQ(registry.getEntries()[0].name, "jp/alpha");
  EXPECT_EQ(registry.getEntries()[1].name, "jp/Zeta");
}

TEST_F(DictionaryRegistryTest, JapaneseOnlyDiscoveryIsAvailableWithoutPersistingTheAutomaticBackend) {
  writeConfiguredPath("/.crosspoint", "/outside/fallback/dict-data");
  writeText(resolve("/dictionaries/jp/vocab.idx"), "index");
  writeText(resolve("/dictionaries/jp/vocab.dat"), "data");

  DictionaryRegistry registry;
  EXPECT_TRUE(registry.discover());
  EXPECT_TRUE(registry.japaneseBundle().vocabulary);
  EXPECT_EQ(registry.count(), 0);
  EXPECT_EQ(Dictionary::readConfiguredDictPath(), "/outside/fallback/dict-data");
}

TEST_F(DictionaryRegistryTest, JapaneseBundleUsesBackendPreferredAndLegacyPathsIndependentOfStarDictRoots) {
  writeText(resolve("/.dictionaries/jp/vocab.idx"), "unsupported-index");
  writeText(resolve("/.dictionaries/jp/vocab.dat"), "unsupported-data");
  writeText(resolve("/dictionaries/jp/vocab.idx"), "incomplete-preferred-index");
  writeText(resolve("/dict/jmdict.idx"), "legacy-index");
  writeText(resolve("/dict/jmdict.dat"), "legacy-data");
  writeText(resolve("/dict/jmnedict.idx"), "legacy-names-index");
  writeText(resolve("/dict/jmnedict.dat"), "legacy-names-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  const JapaneseDictionaryBundle bundle = registry.japaneseBundle();
  EXPECT_TRUE(bundle.vocabulary);
  EXPECT_TRUE(bundle.names);
  EXPECT_FALSE(bundle.grammar);
  EXPECT_EQ(registry.count(), 0);
}

TEST_F(DictionaryRegistryTest, IgnoresUnsupportedJapaneseFilesInHiddenStarDictRoot) {
  writeText(resolve("/.dictionaries/jp/vocab.idx"), "index");
  writeText(resolve("/.dictionaries/jp/vocab.dat"), "data");

  DictionaryRegistry registry;
  EXPECT_FALSE(registry.discover());
  EXPECT_FALSE(registry.japaneseBundle().vocabulary);
  EXPECT_EQ(registry.count(), 0);
}

TEST_F(DictionaryRegistryTest, LookupAvailabilityMatchesJapaneseValidationAndClosesProbeFiles) {
  writeVocab({{"\xE7\x8C\xAB", "cat", 200, DictIndexRecord::POS_OTHER}});

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  const uint32_t opensBefore = hal_storage_test::openCount;
  const uint32_t closesBefore = hal_storage_test::closeCount;
  EXPECT_TRUE(registry.lookupAvailable("ja-JP", "/book-cache"));
  std::string starDictPath = "stale";
  EXPECT_TRUE(registry.resolveLookupRoute("ja-JP", "/book-cache", starDictPath));
  EXPECT_TRUE(starDictPath.empty());
  EXPECT_EQ(hal_storage_test::openCount - opensBefore, hal_storage_test::closeCount - closesBefore);
}

TEST_F(DictionaryRegistryTest, LookupAvailabilityFallsBackFromMalformedJapaneseToEffectiveStarDict) {
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), {1});
  writeBytes(resolve("/dictionaries/jp/vocab.dat"), {});
  writeRegistryDictionary("/dictionaries/jp/fallback/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  EXPECT_TRUE(registry.japaneseBundle().vocabulary);
  EXPECT_TRUE(registry.lookupAvailable("ja", "/book-cache"));
  std::string starDictPath;
  EXPECT_TRUE(registry.resolveLookupRoute("ja", "/book-cache", starDictPath));
  EXPECT_EQ(starDictPath, "/dictionaries/jp/fallback/dict-data");

  ASSERT_TRUE(std::filesystem::remove(resolve("/dictionaries/jp/fallback/dict-data.dict")));
  EXPECT_FALSE(registry.lookupAvailable("ja", "/book-cache"));
}

TEST_F(DictionaryRegistryTest, LookupAvailabilityUsesConfiguredStarDictFallbackAndRejectsMissingFiles) {
  writeRegistryDictionary("/dictionaries/en/language/dict-data");
  writeRegistryDictionary("/dictionaries/de/configured/dict-data");
  writeConfiguredPath("/book-cache", "/dictionaries/de/configured/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  EXPECT_TRUE(registry.lookupAvailable("en-US", "/book-cache"));
  EXPECT_TRUE(registry.lookupAvailable("fr", "/book-cache"));
  std::string starDictPath;
  EXPECT_TRUE(registry.resolveLookupRoute("en-US", "/book-cache", starDictPath));
  EXPECT_EQ(starDictPath, "/dictionaries/en/language/dict-data");
  EXPECT_TRUE(registry.resolveLookupRoute("fr", "/book-cache", starDictPath));
  EXPECT_EQ(starDictPath, "/dictionaries/de/configured/dict-data");

  ASSERT_TRUE(std::filesystem::remove(resolve("/dictionaries/de/configured/dict-data.idx")));
  EXPECT_FALSE(registry.lookupAvailable("fr", "/book-cache"));
}

TEST_F(DictionaryRegistryTest, ReadOnlyLookupDiscoveryDoesNotPersistAnAutomaticStarDictSelection) {
  writeRegistryDictionary("/dictionaries/en/language/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover(/*autoSelectDefault=*/false));
  EXPECT_TRUE(registry.lookupAvailable("en-US", "/book-cache"));
  EXPECT_FALSE(std::filesystem::exists(resolve("/.crosspoint/dictionary.bin")));
}

TEST_F(DictionaryRegistryTest, TransientLookupRoutingLeavesAPreviouslyDiscoveredRegistryIntact) {
  writeRegistryDictionary("/dictionaries/en/language/dict-data");
  DictionaryRegistry settingsRegistry;
  ASSERT_TRUE(settingsRegistry.discover(/*autoSelectDefault=*/false));
  ASSERT_EQ(settingsRegistry.count(), 1);

  std::string starDictPath;
  ASSERT_TRUE(resolveTransientDictionaryLookupRoute("en-US", "/book-cache", starDictPath));
  EXPECT_EQ(starDictPath, "/dictionaries/en/language/dict-data");
  EXPECT_EQ(settingsRegistry.count(), 1);
  EXPECT_EQ(settingsRegistry.getEntries()[0].basePath, "/dictionaries/en/language/dict-data");
}

TEST(EpubLookupRequestTest, PageSnapshotPreservesEveryReaderEntryField) {
  EpubLookupPageSnapshot snapshot;
  snapshot.bookLanguage = "ja-JP";
  snapshot.bookCachePath = "/.crosspoint/epub_123";
  snapshot.spineIndex = 7;
  snapshot.pageIndex = 9;
  snapshot.marginLeft = 11;
  snapshot.marginTop = 13;
  snapshot.reservedBottomHeight = 17;
  snapshot.initialTouchX = 19;
  snapshot.initialTouchY = 23;
  snapshot.autoLookupInitialWord = true;
  snapshot.framebufferContainsPage = true;
  snapshot.dictionaryFontFamilyName = "JP Font";
  snapshot.dictionaryFontPointSize = 21;
  snapshot.readerContext = reinterpret_cast<void*>(0x1234);
  snapshot.renderReaderBackground = reinterpret_cast<void (*)(void*)>(0x2345);
  snapshot.reloadReaderPage = reinterpret_cast<std::unique_ptr<Page> (*)(void*)>(0x3456);

  const EpubLookupPageRequest request = makeEpubLookupPageRequest(std::move(snapshot));
  EXPECT_EQ(request.bookLanguage, "ja-JP");
  EXPECT_EQ(request.bookCachePath, "/.crosspoint/epub_123");
  EXPECT_EQ(request.spineIndex, 7);
  EXPECT_EQ(request.pageIndex, 9);
  EXPECT_EQ(request.marginLeft, 11);
  EXPECT_EQ(request.marginTop, 13);
  EXPECT_EQ(request.reservedBottomHeight, 17);
  EXPECT_EQ(request.initialTouchX, 19);
  EXPECT_EQ(request.initialTouchY, 23);
  EXPECT_TRUE(request.autoLookupInitialWord);
  EXPECT_TRUE(request.framebufferContainsPage);
  EXPECT_STREQ(request.dictionaryFontFamilyName, "JP Font");
  EXPECT_EQ(request.dictionaryFontPointSize, 21);
  EXPECT_EQ(request.readerContext, reinterpret_cast<void*>(0x1234));
  EXPECT_EQ(request.renderReaderBackground, reinterpret_cast<void (*)(void*)>(0x2345));
  EXPECT_EQ(request.reloadReaderPage, reinterpret_cast<std::unique_ptr<Page> (*)(void*)>(0x3456));
  EXPECT_TRUE(request.recordLookupHistory);
}

TEST(EpubLookupRequestTest, HistoryDirectRequestSuppressesPassiveRerecording) {
  const EpubLookupPageRequest request =
      makeEpubLookupDirectRequest("ja", "/.crosspoint/epub_history", "History Font", 18);
  EXPECT_EQ(request.bookLanguage, "ja");
  EXPECT_EQ(request.bookCachePath, "/.crosspoint/epub_history");
  EXPECT_STREQ(request.dictionaryFontFamilyName, "History Font");
  EXPECT_EQ(request.dictionaryFontPointSize, 18);
  EXPECT_FALSE(request.recordLookupHistory);
  EXPECT_EQ(request.initialTouchX, -1);
  EXPECT_EQ(request.initialTouchY, -1);
}

TEST(EpubLookupRequestTest, UnifiedChildReturnInvalidatesPositiveAndNegativeAvailabilityCache) {
  EpubLookupAvailabilityCache cache;

  cache.store(true);
  ASSERT_TRUE(cache.known());
  ASSERT_TRUE(cache.value());
  cache.invalidateAfterUnifiedChildReturn();
  EXPECT_FALSE(cache.known());

  cache.store(false);
  ASSERT_TRUE(cache.known());
  ASSERT_FALSE(cache.value());
  cache.invalidateAfterUnifiedChildReturn();
  EXPECT_FALSE(cache.known());
}

TEST_F(DictionaryRegistryTest, AllocationFailureWhileEnumeratingRootDiscardsPartialResultsAndDoesNotPersist) {
  writeRegistryDictionary("/dictionaries/en/first/dict-data");
  writeRegistryDictionary("/dictionaries/fr/second/dict-data");
  writeConfiguredPath("/.crosspoint", "");
  hal_storage_test::directoryAllocationFailurePath = "/dictionaries";
  hal_storage_test::directoryAllocationFailureAfterEntries = 1;

  DictionaryRegistry registry;
  EXPECT_FALSE(registry.discover());
  EXPECT_EQ(hal_storage_test::directoryAllocationFailureCount, 1u);
  EXPECT_EQ(registry.count(), 0);
  EXPECT_FALSE(registry.japaneseBundle().vocabulary);
  EXPECT_EQ(Dictionary::readConfiguredDictPath(), "");
}

TEST_F(DictionaryRegistryTest, AllocationFailureInFolderOrNestedEnumerationFailsClosed) {
  writeRegistryDictionary("/dictionaries/en/alpha/dict-data");
  writeRegistryDictionary("/dictionaries/en/beta/dict-data");

  for (const uint32_t openOrdinal : {2u, 3u}) {
    SCOPED_TRACE(openOrdinal);
    hal_storage_test::reset();
    hal_storage_test::directoryAllocationFailurePath = "/dictionaries/en";
    hal_storage_test::directoryAllocationFailureOpenOrdinal = openOrdinal;
    hal_storage_test::directoryAllocationFailureAfterEntries = 1;

    DictionaryRegistry registry;
    EXPECT_FALSE(registry.discover());
    EXPECT_EQ(hal_storage_test::directoryAllocationFailureCount, 1u);
    EXPECT_EQ(registry.count(), 0);
    EXPECT_EQ(Dictionary::readConfiguredDictPath(), "");
  }
}

TEST_F(DictionaryRegistryTest, NormalizesPrimaryLanguageAndRejectsTraversalOrDeeperLanguageInput) {
  writeRegistryDictionary("/dictionaries/EN/Beta/dict-data");
  writeRegistryDictionary("/dictionaries/en/alpha/dict-data");
  writeRegistryDictionary("/dictionaries/jp/kana/dict-data");
  writeRegistryDictionary("/dictionaries/fr/standard/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  ASSERT_NE(registry.firstForLanguage("EN-us"), nullptr);
  EXPECT_EQ(registry.firstForLanguage("EN-us")->name, "EN/alpha");
  ASSERT_NE(registry.firstForLanguage("ja_JP"), nullptr);
  EXPECT_EQ(registry.firstForLanguage("ja_JP")->name, "jp/kana");
  ASSERT_NE(registry.firstForLanguage("fra"), nullptr);
  EXPECT_EQ(registry.firstForLanguage("fra")->name, "fr/standard");
  EXPECT_EQ(registry.firstForLanguage("en/../jp"), nullptr);
  EXPECT_EQ(registry.firstForLanguage("en\\jp"), nullptr);
  EXPECT_EQ(registry.firstForLanguage("en//jp"), nullptr);
}

TEST_F(DictionaryRegistryTest, ResolvesSortedLanguageDictionaryBeforePerBookThenGlobalFallback) {
  writeConfiguredPath("/.crosspoint", "");
  writeRegistryDictionary("/dictionaries/en/Zulu/dict-data");
  writeRegistryDictionary("/dictionaries/en/alpha/dict-data");
  writeRegistryDictionary("/dictionaries/fr/general/dict-data");

  DictionaryRegistry registry;
  ASSERT_TRUE(registry.discover());
  writeConfiguredPath("/.crosspoint", "/outside/global/dict-data");
  writeConfiguredPath("/.crosspoint/books/one", "/outside/book/dict-data");

  std::string effective;
  EXPECT_TRUE(registry.resolveEffectiveStarDict("en-US", "/.crosspoint/books/one", effective));
  EXPECT_EQ(effective, "/dictionaries/en/alpha/dict-data");
  EXPECT_TRUE(registry.resolveEffectiveStarDict("de-DE", "/.crosspoint/books/one", effective));
  EXPECT_EQ(effective, "/outside/book/dict-data");
  EXPECT_TRUE(registry.resolveEffectiveStarDict("de-DE", nullptr, effective));
  EXPECT_EQ(effective, "/outside/global/dict-data");

  writeConfiguredPath("/.crosspoint/books/one", "");
  EXPECT_TRUE(registry.resolveEffectiveStarDict("de-DE", "/.crosspoint/books/one", effective));
  EXPECT_EQ(effective, "/outside/global/dict-data");
}

struct FakeDictionaryBackend {
  DictionaryStatus openStatus = DictionaryStatus::Found;
  DictionaryStatus probeStatus = DictionaryStatus::NotFound;
  DictionaryStatus lookupStatus = DictionaryStatus::NotFound;
  DictionaryStatus suggestionStatus = DictionaryStatus::NotFound;
  DictionaryStatus streamStatus = DictionaryStatus::Found;
  DictionaryCapabilities capabilities{};
  uint64_t signature = 0;
  uint32_t openCalls = 0;
  uint32_t closeCalls = 0;
  uint32_t cancelCalls = 0;
  DictionaryEngine* cancelDuringProbe = nullptr;
  DictionaryEngine* cancelDuringLookup = nullptr;
  DictionaryEngine* cancelDuringSuggest = nullptr;

  static DictionaryStatus open(void* context, const DictionaryOpenRequest&) {
    auto& self = *static_cast<FakeDictionaryBackend*>(context);
    ++self.openCalls;
    return self.openStatus;
  }
  static DictionaryStatus probe(void* context, const DictionaryQuery&, DictionaryProbeResult& out) {
    auto& self = *static_cast<FakeDictionaryBackend*>(context);
    out = DictionaryProbeResult{};
    out.status = self.probeStatus;
    out.matchedBytes = self.probeStatus == DictionaryStatus::Found ? 3 : 0;
    if (self.cancelDuringProbe) self.cancelDuringProbe->cancel();
    return self.probeStatus;
  }
  static DictionaryStatus lookup(void* context, const DictionaryQuery&, DictionaryResult& out) {
    auto& self = *static_cast<FakeDictionaryBackend*>(context);
    out = DictionaryResult{};
    out.status = self.lookupStatus;
    if (self.lookupStatus == DictionaryStatus::Found) {
      out.matchedBytes = 3;
      if (!out.surface.assign("hit") || !out.headword.assign("head")) return DictionaryStatus::OutOfMemory;
    }
    if (self.cancelDuringLookup) self.cancelDuringLookup->cancel();
    return self.lookupStatus;
  }
  static bool bookReading(void*, std::string_view, DictionaryOwnedText&) { return false; }
  static DictionaryStatus suggest(void* context, std::string_view, DictionarySuggestions& out) {
    auto& self = *static_cast<FakeDictionaryBackend*>(context);
    out = DictionarySuggestions{};
    if (self.suggestionStatus == DictionaryStatus::Found && !out.items[0].assign("suggestion")) {
      return DictionaryStatus::OutOfMemory;
    }
    out.count = self.suggestionStatus == DictionaryStatus::Found ? 1 : 0;
    if (self.cancelDuringSuggest) self.cancelDuringSuggest->cancel();
    return self.suggestionStatus;
  }
  static DictionaryStatus stream(void* context, DictionaryDefinitionMode, DictionaryDefinitionSink sink) {
    auto& self = *static_cast<FakeDictionaryBackend*>(context);
    if (self.streamStatus != DictionaryStatus::Found) return self.streamStatus;
    const DictionaryDefinitionSpan span{"definition"};
    return sink.onSpan && sink.onSpan(sink.context, span) ? DictionaryStatus::Found : DictionaryStatus::Cancelled;
  }
  static DictionaryCapabilities getCapabilities(void* context) {
    return static_cast<FakeDictionaryBackend*>(context)->capabilities;
  }
  static uint64_t getSignature(void* context) { return static_cast<FakeDictionaryBackend*>(context)->signature; }
  static void cancel(void* context) { ++static_cast<FakeDictionaryBackend*>(context)->cancelCalls; }
  static void close(void* context) { ++static_cast<FakeDictionaryBackend*>(context)->closeCalls; }

  DictionaryBackendFunctions functions() {
    return {this, open, probe, lookup, bookReading, suggest, stream, getCapabilities, getSignature, cancel, close};
  }
};

bool acceptDefinitionSpan(void* context, const DictionaryDefinitionSpan& span) {
  static_cast<std::string*>(context)->append(span.text);
  return true;
}

TEST(DictionaryEngineState, NormalizesJapaneseLanguageTagsAndUsesValidJapaneseBackend) {
  for (const char* language : {"ja", "JA-jp", "ja_JP"}) {
    SCOPED_TRACE(language);
    FakeDictionaryBackend japanese;
    japanese.capabilities.deinflection = true;
    japanese.signature = 41;
    FakeDictionaryBackend star;
    DictionaryEngine engine(japanese.functions(), star.functions());
    ASSERT_EQ(engine.open({language, "/book"}), DictionaryStatus::Found);
    EXPECT_EQ(engine.backendKind(), DictionaryBackendKind::Japanese);
    EXPECT_TRUE(engine.capabilities().deinflection);
    EXPECT_EQ(engine.signature(), 41u);
    EXPECT_EQ(japanese.openCalls, 1u);
    EXPECT_EQ(star.openCalls, 0u);
  }
}

TEST(DictionaryEngineState, FallsBackForUnavailableOrCorruptJapaneseButNotJapaneseOom) {
  for (const DictionaryStatus japaneseStatus : {DictionaryStatus::Unavailable, DictionaryStatus::ReadError}) {
    SCOPED_TRACE(static_cast<int>(japaneseStatus));
    FakeDictionaryBackend japanese;
    japanese.openStatus = japaneseStatus;
    FakeDictionaryBackend star;
    DictionaryEngine engine(japanese.functions(), star.functions());
    EXPECT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
    EXPECT_EQ(engine.backendKind(), DictionaryBackendKind::StarDict);
    EXPECT_EQ(japanese.closeCalls, 1u);
    EXPECT_EQ(star.openCalls, 1u);
  }

  FakeDictionaryBackend japanese;
  japanese.openStatus = DictionaryStatus::OutOfMemory;
  FakeDictionaryBackend star;
  DictionaryEngine engine(japanese.functions(), star.functions());
  EXPECT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::OutOfMemory);
  EXPECT_EQ(star.openCalls, 0u);
}

TEST(DictionaryEngineState, UsesStarDictForNonJapaneseAndAbsentLanguage) {
  for (const char* language : {"en-US", ""}) {
    FakeDictionaryBackend japanese;
    FakeDictionaryBackend star;
    star.signature = 77;
    DictionaryEngine engine(japanese.functions(), star.functions());
    ASSERT_EQ(engine.open({language, nullptr}), DictionaryStatus::Found);
    EXPECT_EQ(engine.backendKind(), DictionaryBackendKind::StarDict);
    EXPECT_EQ(engine.signature(), 77u);
    EXPECT_EQ(japanese.openCalls, 0u);
    EXPECT_EQ(star.openCalls, 1u);
  }
}

TEST(DictionaryEngineState, PreservesStatusesAndPublishesGenerationOnlyAfterCompleteLookup) {
  FakeDictionaryBackend japanese;
  FakeDictionaryBackend star;
  star.lookupStatus = DictionaryStatus::Found;
  star.probeStatus = DictionaryStatus::ReadError;
  DictionaryEngine engine(japanese.functions(), star.functions());
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);

  DictionaryResult first;
  ASSERT_EQ(engine.lookup({"hit"}, first), DictionaryStatus::Found);
  ASSERT_NE(first.definition.generation, 0u);
  DictionaryProbeResult probe;
  EXPECT_EQ(engine.probe({"hit"}, probe), DictionaryStatus::ReadError);
  std::string streamed;
  EXPECT_EQ(
      engine.streamDefinition(first.definition, DictionaryDefinitionMode::Styled, {&streamed, acceptDefinitionSpan}),
      DictionaryStatus::Found);
  EXPECT_EQ(streamed, "definition");

  for (const DictionaryStatus failure : {DictionaryStatus::NotFound, DictionaryStatus::Unavailable,
                                         DictionaryStatus::ReadError, DictionaryStatus::OutOfMemory}) {
    star.lookupStatus = failure;
    DictionaryResult failed;
    EXPECT_EQ(engine.lookup({"miss"}, failed), failure);
    streamed.clear();
    EXPECT_EQ(engine.streamDefinition(first.definition, DictionaryDefinitionMode::PlainFallback,
                                      {&streamed, acceptDefinitionSpan}),
              DictionaryStatus::Found);
  }

  star.lookupStatus = DictionaryStatus::Found;
  DictionaryResult second;
  ASSERT_EQ(engine.lookup({"next"}, second), DictionaryStatus::Found);
  EXPECT_NE(second.definition.generation, first.definition.generation);
  EXPECT_EQ(
      engine.streamDefinition(first.definition, DictionaryDefinitionMode::Styled, {&streamed, acceptDefinitionSpan}),
      DictionaryStatus::Unavailable);
}

TEST(DictionaryEngineState, CancelAndCloseInvalidateHandlesAndRemainDistinct) {
  FakeDictionaryBackend japanese;
  FakeDictionaryBackend star;
  star.lookupStatus = DictionaryStatus::Found;
  DictionaryEngine engine(japanese.functions(), star.functions());
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"hit"}, result), DictionaryStatus::Found);
  engine.cancel();
  DictionaryProbeResult probe;
  EXPECT_EQ(engine.probe({"hit"}, probe), DictionaryStatus::Cancelled);
  std::string text;
  EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&text, acceptDefinitionSpan}),
            DictionaryStatus::Cancelled);
  EXPECT_EQ(star.cancelCalls, 1u);
  engine.close();
  EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&text, acceptDefinitionSpan}),
            DictionaryStatus::Unavailable);
  EXPECT_EQ(star.closeCalls, 1u);
}

TEST(DictionaryEngineState, InFlightLookupCancellationCannotPublishAGeneration) {
  FakeDictionaryBackend japanese;
  FakeDictionaryBackend star;
  star.lookupStatus = DictionaryStatus::Found;
  DictionaryEngine engine(japanese.functions(), star.functions());
  star.cancelDuringLookup = &engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult unchanged;
  ASSERT_TRUE(unchanged.surface.assign("sentinel"));
  EXPECT_EQ(engine.lookup({"hit"}, unchanged), DictionaryStatus::Cancelled);
  EXPECT_EQ(unchanged.surface.view(), "sentinel");
  EXPECT_EQ(unchanged.definition.generation, 0u);

  star.cancelDuringLookup = nullptr;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult published;
  ASSERT_EQ(engine.lookup({"hit"}, published), DictionaryStatus::Found);
  EXPECT_EQ(published.definition.generation, 1u);
}

TEST(DictionaryEngineState, InFlightProbeAndSuggestionCancellationWinsBeforeOutputPublication) {
  FakeDictionaryBackend japanese;
  FakeDictionaryBackend star;
  star.probeStatus = DictionaryStatus::Found;
  star.suggestionStatus = DictionaryStatus::Found;
  DictionaryEngine engine(japanese.functions(), star.functions());
  star.cancelDuringProbe = &engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryProbeResult probe;
  EXPECT_EQ(engine.probe({"hit"}, probe), DictionaryStatus::Cancelled);

  star.cancelDuringProbe = nullptr;
  star.cancelDuringSuggest = &engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionarySuggestions unchanged;
  ASSERT_TRUE(unchanged.items[0].assign("sentinel"));
  unchanged.count = 1;
  EXPECT_EQ(engine.suggest("hit", unchanged), DictionaryStatus::Cancelled);
  EXPECT_EQ(unchanged.count, 1u);
  EXPECT_EQ(unchanged.items[0].view(), "sentinel");
}

TEST(DictionaryOwnedTextTest, JoinsCounterPrefixAndCanonicalHeadwordWithoutAnIntermediateString) {
  DictionaryOwnedText text;
  ASSERT_TRUE(text.assignJoined("１５", "人"));
  EXPECT_EQ(text.view(), "１５人");

  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;
  EXPECT_FALSE(text.assignJoined("2", "年"));
  EXPECT_EQ(text.view(), "１５人");
}

struct CancelFromSinkContext {
  DictionaryEngine* engine = nullptr;
  std::string text;
};

bool cancelFromDefinitionSink(void* context, const DictionaryDefinitionSpan& span) {
  auto& state = *static_cast<CancelFromSinkContext*>(context);
  state.engine->cancel();
  state.text.append(span.text);
  return true;
}

TEST(DictionaryEngineState, InFlightStreamCancellationInvalidatesHandleAndReturnsCancelled) {
  FakeDictionaryBackend japanese;
  FakeDictionaryBackend star;
  star.lookupStatus = DictionaryStatus::Found;
  DictionaryEngine engine(japanese.functions(), star.functions());
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"hit"}, result), DictionaryStatus::Found);
  CancelFromSinkContext context{&engine, {}};
  EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled,
                                    {&context, cancelFromDefinitionSink}),
            DictionaryStatus::Cancelled);
  EXPECT_EQ(context.text, "definition");
  std::string ignored;
  EXPECT_EQ(
      engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&ignored, acceptDefinitionSpan}),
      DictionaryStatus::Cancelled);
}

TEST(DictionaryEngineState, ReopenKeepsOldHandlesStaleAndGenerationWrapSkipsZero) {
  FakeDictionaryBackend japanese;
  FakeDictionaryBackend star;
  star.lookupStatus = DictionaryStatus::Found;
  DictionaryEngine engine(japanese.functions(), star.functions());
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult oldResult;
  ASSERT_EQ(engine.lookup({"old"}, oldResult), DictionaryStatus::Found);
  ASSERT_NE(oldResult.definition.epoch, 0u);
  engine.close();
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult reopened;
  ASSERT_EQ(engine.lookup({"new"}, reopened), DictionaryStatus::Found);
  EXPECT_NE(reopened.definition.epoch, oldResult.definition.epoch);
  std::string ignored;
  EXPECT_EQ(
      engine.streamDefinition(oldResult.definition, DictionaryDefinitionMode::Styled, {&ignored, acceptDefinitionSpan}),
      DictionaryStatus::Unavailable);

  engine.setGenerationCounterForTesting(UINT32_MAX - 1);
  DictionaryResult maximum;
  ASSERT_EQ(engine.lookup({"maximum"}, maximum), DictionaryStatus::Found);
  EXPECT_EQ(maximum.definition.generation, UINT32_MAX);
  DictionaryResult wrapped;
  ASSERT_EQ(engine.lookup({"wrapped"}, wrapped), DictionaryStatus::Found);
  EXPECT_EQ(wrapped.definition.generation, 1u);
  EXPECT_NE(wrapped.definition.epoch, maximum.definition.epoch);
  EXPECT_EQ(
      engine.streamDefinition(maximum.definition, DictionaryDefinitionMode::Styled, {&ignored, acceptDefinitionSpan}),
      DictionaryStatus::Unavailable);
  EXPECT_EQ(
      engine.streamDefinition(oldResult.definition, DictionaryDefinitionMode::Styled, {&ignored, acceptDefinitionSpan}),
      DictionaryStatus::Unavailable);

  engine.close();
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  engine.setGenerationCounterForTesting(0);
  DictionaryResult repeatedAfterReopen;
  ASSERT_EQ(engine.lookup({"repeated"}, repeatedAfterReopen), DictionaryStatus::Found);
  EXPECT_EQ(repeatedAfterReopen.definition.generation, wrapped.definition.generation);
  EXPECT_NE(repeatedAfterReopen.definition.epoch, wrapped.definition.epoch);
  EXPECT_EQ(
      engine.streamDefinition(wrapped.definition, DictionaryDefinitionMode::Styled, {&ignored, acceptDefinitionSpan}),
      DictionaryStatus::Unavailable);
}

TEST_F(JapaneseDictionaryTest, JapaneseAdapterHasZeroAllocationProbesAndMoveOnlyActiveDefinitions) {
  writeVocab({{"食べる", "eat", 200, DictIndexRecord::POS_V1}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja-JP", "/book"}), DictionaryStatus::Found);
  EXPECT_EQ(engine.backendKind(), DictionaryBackendKind::Japanese);
  const DictionaryCapabilities capabilities = engine.capabilities();
  EXPECT_TRUE(capabilities.deinflection);
  EXPECT_FALSE(capabilities.names);
  EXPECT_FALSE(capabilities.grammar);
  EXPECT_FALSE(capabilities.suggestions);

  dict_memory_test::reset();
  DictionaryProbeResult probe;
  ASSERT_EQ(engine.probe({"前食べました後", 3, DictionaryLookupMode::LongestAtOffset}, probe), DictionaryStatus::Found);
  EXPECT_EQ(dict_memory_test::requestCount, 0u);
  EXPECT_EQ(probe.matchedBytes, std::string_view("食べました").size());
  EXPECT_TRUE(probe.transformed);

  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"前食べました後", 3, DictionaryLookupMode::LongestAtOffset}, result),
            DictionaryStatus::Found);
  EXPECT_EQ(result.surface.view(), "食べました");
  EXPECT_EQ(result.headword.view(), "食べる");
  EXPECT_TRUE(result.transformed);
  EXPECT_EQ(result.sourceMask, DictIndex::DICT_JMDICT);
  std::string definition;
  EXPECT_EQ(
      engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&definition, acceptDefinitionSpan}),
      DictionaryStatus::Found);
  EXPECT_EQ(definition, "eat");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, JapaneseAdapterSynthesizesTheFullFictionalKatakanaNameWhenRequested) {
  writeVocab({{"ムー", "partial dictionary entry", 200, DictIndexRecord::POS_OTHER}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);

  DictionaryQuery ordinary{"\xE3\x83\xA0\xE3\x83\xBC\xE3\x83\x9F\xE3\x83\xB3", 0,
                           DictionaryLookupMode::LongestAtOffset};
  DictionaryResult partial;
  ASSERT_EQ(engine.lookup(ordinary, partial), DictionaryStatus::Found);
  EXPECT_EQ(partial.headword.view(), "ムー");
  EXPECT_FALSE(partial.syntheticName);

  DictionaryQuery fictionalName{"\xE3\x83\xA0\xE3\x83\xBC\xE3\x83\x9F\xE3\x83\xB3", 0,
                                DictionaryLookupMode::LongestAtOffset};
  fictionalName.synthesizePartialKatakanaName = true;
  std::string syntheticDefinition = "Name";
  fictionalName.syntheticNameDefinition = syntheticDefinition;
  DictionaryResult synthesized;
  ASSERT_EQ(engine.lookup(fictionalName, synthesized), DictionaryStatus::Found);
  EXPECT_EQ(synthesized.headword.view(), "ムーミン");
  EXPECT_EQ(synthesized.surface.view(), "ムーミン");
  EXPECT_EQ(synthesized.matchedBytes, std::string_view("ムーミン").size());
  EXPECT_EQ(synthesized.sourceMask, DictIndex::DICT_NAMES);
  EXPECT_TRUE(synthesized.syntheticName);
  syntheticDefinition.assign("Gone");
  std::string definition;
  EXPECT_EQ(engine.streamDefinition(synthesized.definition, DictionaryDefinitionMode::Styled,
                                    {&definition, acceptDefinitionSpan}),
            DictionaryStatus::Found);
  EXPECT_EQ(definition, "Name");

  DictionaryResult unchanged;
  ASSERT_TRUE(unchanged.surface.assign("sentinel"));
  unchanged.definition.generation = 99;
  for (const size_t rejectedRequest : {1u, 2u, 3u}) {
    dict_memory_test::reset();
    dict_memory_test::rejectedRequest = rejectedRequest;
    EXPECT_EQ(engine.lookup(fictionalName, unchanged), DictionaryStatus::OutOfMemory);
    EXPECT_EQ(unchanged.surface.view(), "sentinel");
    EXPECT_EQ(unchanged.definition.generation, 99u);
    definition.clear();
    EXPECT_EQ(engine.streamDefinition(synthesized.definition, DictionaryDefinitionMode::Styled,
                                      {&definition, acceptDefinitionSpan}),
              DictionaryStatus::Found);
    EXPECT_EQ(definition, "Name");
  }
  dict_memory_test::reset();
}

TEST_F(JapaneseDictionaryTest, JapaneseBookReadingOwnsItsSessionPathAndPublishesOnlyExactHits) {
  writeVocab({{"\xE7\x8C\xAB", "cat", 200, DictIndexRecord::POS_OTHER}});
  RubyGlossary::merge("/book-cache", {{"\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93"}});
  std::string callerPath = "/book-cache";
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", callerPath.c_str()}), DictionaryStatus::Found);
  callerPath.assign("/destroyed");
  EXPECT_TRUE(engine.capabilities().ruby);

  DictionaryOwnedText readings;
  ASSERT_TRUE(readings.assign("sentinel"));
  ASSERT_TRUE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_EQ(readings.view(), "\xE3\x81\xAD\xE3\x81\x93");
  EXPECT_FALSE(engine.bookReading("\xE7\x8A\xAC", readings));
  EXPECT_TRUE(readings.empty());
}

TEST_F(JapaneseDictionaryTest, JapaneseBookReadingPathOomDisablesOnlyTheRubyCapability) {
  writeVocab({{"\xE7\x8C\xAB", "cat", 200, DictIndexRecord::POS_OTHER}});
  const std::string cachePath = "/" + std::string(100, 'p');
  dict_memory_test::rejectedBytes = cachePath.size() + 1;
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", cachePath.c_str()}), DictionaryStatus::Found);
  EXPECT_FALSE(engine.capabilities().ruby);

  dict_memory_test::reset();
  DictionaryResult result;
  EXPECT_EQ(engine.lookup({"\xE7\x8C\xAB"}, result), DictionaryStatus::Found);
  DictionaryOwnedText readings;
  ASSERT_TRUE(readings.assign("sentinel"));
  EXPECT_FALSE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());
}

TEST_F(JapaneseDictionaryTest, BookReadingClearsMissingCorruptCancelledClosedStarAndOomResults) {
  writeVocab({{"\xE7\x8C\xAB", "cat", 200, DictIndexRecord::POS_OTHER}});
  RubyGlossary::merge("/book-cache", {{"\xE7\x8C\xAB", "\xE3\x81\xAD\xE3\x81\x93"}});
  DictionaryOwnedText readings;
  ASSERT_TRUE(readings.assign("sentinel"));
  DictionaryEngine engine;
  EXPECT_FALSE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());

  ASSERT_EQ(engine.open({"ja", "/missing-cache"}), DictionaryStatus::Found);
  ASSERT_TRUE(readings.assign("sentinel"));
  EXPECT_FALSE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());

  engine.close();
  writeBytes(resolve("/corrupt-cache/ruby.bin"), {1, 1, 0, 3, 0xE7});
  ASSERT_EQ(engine.open({"ja", "/corrupt-cache"}), DictionaryStatus::Found);
  ASSERT_TRUE(readings.assign("sentinel"));
  EXPECT_FALSE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());

  engine.close();
  ASSERT_EQ(engine.open({"ja", "/book-cache"}), DictionaryStatus::Found);
  engine.cancel();
  ASSERT_TRUE(readings.assign("sentinel"));
  EXPECT_FALSE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());
  engine.close();
  EXPECT_FALSE(engine.capabilities().ruby);

  ASSERT_EQ(engine.open({"ja", "/book-cache"}), DictionaryStatus::Found);
  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = 7;  // Exact joined "\u306d\u3053" output plus NUL.
  ASSERT_TRUE(readings.assign("sentinel"));
  EXPECT_FALSE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());

  dict_memory_test::reset();
  const size_t glossaryBytes = readBytes(resolve("/book-cache/ruby.bin")).size();
  dict_memory_test::rejectedBytes = glossaryBytes;
  ASSERT_TRUE(readings.assign("sentinel"));
  EXPECT_FALSE(engine.bookReading("\xE7\x8C\xAB", readings));
  EXPECT_TRUE(readings.empty());

  dict_memory_test::reset();
  engine.close();
  writeStarDict({{"cat", "feline"}});
  ASSERT_EQ(engine.open({"en", "/book-cache"}), DictionaryStatus::Found);
  ASSERT_TRUE(readings.assign("sentinel"));
  EXPECT_FALSE(engine.bookReading("cat", readings));
  EXPECT_TRUE(readings.empty());
  EXPECT_FALSE(engine.capabilities().ruby);
}

TEST_F(JapaneseDictionaryTest, JapaneseInFlightStreamCancelDoesNotFreeBorrowedDefinition) {
  writeVocab({{"本", "definition remains alive", 200, DictIndexRecord::POS_OTHER}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"本"}, result), DictionaryStatus::Found);
  CancelFromSinkContext context{&engine, {}};
  EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled,
                                    {&context, cancelFromDefinitionSink}),
            DictionaryStatus::Cancelled);
  EXPECT_EQ(context.text, "definition remains alive");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, MissingOptionalJapaneseSourcesOnlyClearTheirCapabilities) {
  writeVocab({{"本", "book", 200, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/names", {{"春", "Haru", 200, DictIndexRecord::POS_OTHER}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"JA_jp", nullptr}), DictionaryStatus::Found);
  const DictionaryCapabilities capabilities = engine.capabilities();
  EXPECT_TRUE(capabilities.deinflection);
  EXPECT_TRUE(capabilities.names);
  EXPECT_FALSE(capabilities.grammar);
  engine.close();
}

TEST_F(JapaneseDictionaryTest, JapaneseLookupOomPreservesPreviouslyPublishedDefinition) {
  writeVocab({{"本", "book", 200, DictIndexRecord::POS_OTHER}, {"猫", "cat", 200, DictIndexRecord::POS_OTHER}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryResult first;
  ASSERT_EQ(engine.lookup({"本", 0, DictionaryLookupMode::LongestAtOffset}, first), DictionaryStatus::Found);
  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;
  DictionaryResult failed;
  EXPECT_EQ(engine.lookup({"猫", 0, DictionaryLookupMode::LongestAtOffset}, failed), DictionaryStatus::OutOfMemory);
  std::string definition;
  EXPECT_EQ(engine.streamDefinition(first.definition, DictionaryDefinitionMode::PlainFallback,
                                    {&definition, acceptDefinitionSpan}),
            DictionaryStatus::Found);
  EXPECT_EQ(definition, "book");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, JapanesePublicTextAllocationFailurePreservesOutputAndPublishedDefinition) {
  const std::string maximumHeadword = "😀😀😀😀😀😀😀語";  // 8 codepoints, 31 UTF-8 bytes
  writeVocab({{"old", "old definition", 200, DictIndexRecord::POS_OTHER},
              {maximumHeadword, "new definition", 200, DictIndexRecord::POS_OTHER}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryResult published;
  ASSERT_EQ(engine.lookup({"old", 0, DictionaryLookupMode::LongestAtOffset}, published), DictionaryStatus::Found);

  DictionaryResult unchanged;
  ASSERT_TRUE(unchanged.surface.assign("sentinel"));
  unchanged.definition.generation = 99;
  for (const size_t rejectedRequest : {2u, 3u}) {
    dict_memory_test::reset();
    dict_memory_test::rejectedRequest = rejectedRequest;
    EXPECT_EQ(engine.lookup({maximumHeadword, 0, DictionaryLookupMode::LongestAtOffset}, unchanged),
              DictionaryStatus::OutOfMemory);
    EXPECT_EQ(unchanged.surface.view(), "sentinel");
    EXPECT_EQ(unchanged.definition.generation, 99u);
  }

  std::string definition;
  EXPECT_EQ(engine.streamDefinition(published.definition, DictionaryDefinitionMode::Styled,
                                    {&definition, acceptDefinitionSpan}),
            DictionaryStatus::Found);
  EXPECT_EQ(definition, "old definition");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, RealEngineFallsBackFromCorruptJapaneseIndexToStarDict) {
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), {1});
  writeBytes(resolve("/dictionaries/jp/vocab.dat"), {});
  writeStarDict({{"cat", "feline"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  EXPECT_EQ(engine.backendKind(), DictionaryBackendKind::StarDict);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"cat"}, result), DictionaryStatus::Found);
  EXPECT_EQ(result.headword.view(), "cat");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictAdapterRoutesDirectStemAlternateAndSuggestions) {
  writeStarDict({{"cat", "feline"}, {"cot", "bed"}, {"run", "move quickly"}}, 'm', "jogging", "run");
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en-US", nullptr}), DictionaryStatus::Found);
  EXPECT_EQ(engine.backendKind(), DictionaryBackendKind::StarDict);
  EXPECT_TRUE(engine.capabilities().suggestions);
  EXPECT_TRUE(engine.capabilities().stemVariants);

  DictionaryResult direct;
  ASSERT_EQ(engine.lookup({"cat"}, direct), DictionaryStatus::Found);
  EXPECT_EQ(direct.headword.view(), "cat");
  EXPECT_FALSE(direct.transformed);
  EXPECT_FALSE(direct.alternate);
  DictionaryResult stem;
  ASSERT_EQ(engine.lookup({"running"}, stem), DictionaryStatus::Found);
  EXPECT_EQ(stem.headword.view(), "run");
  EXPECT_TRUE(stem.transformed);
  EXPECT_FALSE(stem.alternate);
  DictionaryResult alternate;
  ASSERT_EQ(engine.lookup({"jogging"}, alternate), DictionaryStatus::Found);
  EXPECT_EQ(alternate.headword.view(), "run");
  EXPECT_TRUE(alternate.transformed);
  EXPECT_TRUE(alternate.alternate);

  DictionarySuggestions suggestions;
  ASSERT_EQ(engine.suggest("cut", suggestions), DictionaryStatus::Found);
  ASSERT_GT(suggestions.count, 0u);
  EXPECT_LE(suggestions.count, DictionarySuggestions::kCapacity);
  EXPECT_TRUE(std::find_if(suggestions.items.begin(), suggestions.items.begin() + suggestions.count,
                           [](const auto& item) { return item.view() == "cat"; }) !=
              suggestions.items.begin() + suggestions.count);
  engine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictCleansSmartQuotePunctuationAndNfcForProbeAndFullLookup) {
  writeStarDict({{{"caf\xC3\xA9"}, "coffee"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  const std::string raw =
      "\xE2\x80\x9C"
      "cafe\xCC\x81!"
      "\xE2\x80\x9D";

  DictionaryProbeResult probe;
  ASSERT_EQ(engine.probe({raw}, probe), DictionaryStatus::Found);
  EXPECT_EQ(probe.matchedBytes, raw.size());

  DictionaryResult result;
  ASSERT_EQ(engine.lookup({raw}, result), DictionaryStatus::Found);
  EXPECT_EQ(result.surface.view(), "caf\xC3\xA9");
  EXPECT_EQ(result.headword.view(), "caf\xC3\xA9");
  engine.close();
}

TEST(Utf8LookupCleanupTest, BoundedCleanupTrimsUnicodeEdgesAndComposesWithoutHeapOutput) {
  const std::string raw =
      "\xE2\x80\x9C"
      "cafe\xCC\x81!"
      "\xE2\x80\x9D";
  char cleaned[16]{};
  size_t cleanedBytes = 99;

  ASSERT_TRUE(utf8CleanLookupWordToBuffer(raw, cleaned, sizeof(cleaned), cleanedBytes));
  EXPECT_EQ(std::string_view(cleaned, cleanedBytes), "caf\xC3\xA9");

  char tooSmall[4] = {'x', 'x', 'x', '\0'};
  cleanedBytes = 99;
  EXPECT_FALSE(utf8CleanLookupWordToBuffer(raw, tooSmall, sizeof(tooSmall), cleanedBytes));
  EXPECT_EQ(cleanedBytes, 0u);
  EXPECT_EQ(tooSmall[0], '\0');
}

struct CapturedSpan {
  std::string text;
  bool bold = false;
  bool italic = false;
  bool superscript = false;
  bool subscript = false;
  bool ipa = false;
  bool listItem = false;
  bool lineBreak = false;
  uint8_t indent = 0;
};

bool captureSpan(void* context, const DictionaryDefinitionSpan& span) {
  static_cast<std::vector<CapturedSpan>*>(context)->push_back({std::string(span.text), span.bold, span.italic,
                                                               span.superscript, span.subscript, span.ipa,
                                                               span.listItem, span.lineBreak, span.indentLevel});
  return true;
}

TEST_F(JapaneseDictionaryTest, StarDictAdapterMapsHtmlStylesToNeutralSpans) {
  writeStarDict({{"markup",
                  "<b>Bold</b><i>Italic</i><sup>Sup</sup><sub>Sub</sub><span class=\"IPA\">tɛst</span>"
                  "<span class=\"term iPa phonetic\">mixed</span>"
                  "<span class=\"principal\">principal</span><span class=\"bipartisan\">bipartisan</span>"
                  "<span class=\"ipa-extra\">hyphenated</span><ul><li>Item</li></ul>"}},
                'h');
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"markup"}, result), DictionaryStatus::Found);
  std::vector<CapturedSpan> spans;
  ASSERT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&spans, captureSpan}),
            DictionaryStatus::Found);
  EXPECT_TRUE(
      std::any_of(spans.begin(), spans.end(), [](const auto& span) { return span.text == "Bold" && span.bold; }));
  EXPECT_TRUE(
      std::any_of(spans.begin(), spans.end(), [](const auto& span) { return span.text == "Italic" && span.italic; }));
  EXPECT_TRUE(
      std::any_of(spans.begin(), spans.end(), [](const auto& span) { return span.text == "Sup" && span.superscript; }));
  EXPECT_TRUE(
      std::any_of(spans.begin(), spans.end(), [](const auto& span) { return span.text == "Sub" && span.subscript; }));
  EXPECT_TRUE(
      std::any_of(spans.begin(), spans.end(), [](const auto& span) { return span.text == "tɛst" && span.ipa; }));
  EXPECT_TRUE(
      std::any_of(spans.begin(), spans.end(), [](const auto& span) { return span.text == "mixed" && span.ipa; }));
  for (const std::string_view text : {"principal", "bipartisan", "hyphenated"}) {
    EXPECT_TRUE(
        std::any_of(spans.begin(), spans.end(), [text](const auto& span) { return span.text == text && !span.ipa; }));
  }
  EXPECT_TRUE(std::any_of(spans.begin(), spans.end(), [](const auto& span) {
    return span.text == "Item" && span.listItem && span.lineBreak && span.indent > 0;
  }));

  spans.clear();
  ASSERT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::PlainFallback, {&spans, captureSpan}),
            DictionaryStatus::Found);
  std::string plain;
  for (const auto& span : spans) {
    plain += span.text;
    EXPECT_FALSE(span.bold);
    EXPECT_FALSE(span.italic);
  }
  EXPECT_EQ(plain, "BoldItalicSupSubtɛstmixedprincipalbipartisanhyphenatedItem");
  engine.close();
}

void appendLegacyRendererSpan(void* context, const StyledSpan& span) {
  if (span.text) static_cast<std::string*>(context)->append(span.text);
}

TEST_F(JapaneseDictionaryTest, LegacyRendererWrappersFailCompatiblyWhenHeapScratchIsUnavailable) {
  const std::string path = "/legacy-renderer.html";
  const std::string html = "<b>definition</b>";
  writeText(resolve(path), html);
  DictHtmlRenderer renderer;
  std::string output;
  const DictHtmlRenderer::SpanSink sink{&output, appendLegacyRendererSpan};

  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = 512;
  EXPECT_FALSE(renderer.renderFromFileStreaming(path.c_str(), 0, html.size(), sink));
  EXPECT_TRUE(output.empty());
  EXPECT_FALSE(renderer.renderPlainTextFromFileStreaming(path.c_str(), 0, html.size(), sink));
  EXPECT_TRUE(output.empty());

  dict_memory_test::reset();
  EXPECT_TRUE(renderer.renderFromFileStreaming(path.c_str(), 0, html.size(), sink));
  EXPECT_EQ(output, "definition");
}

struct RendererControlContext {
  bool cancelled = false;
  bool reject = false;
  uint32_t spanCalls = 0;
};

bool controlledRendererSpan(void* context, const StyledSpan&) {
  auto& state = *static_cast<RendererControlContext*>(context);
  ++state.spanCalls;
  return !state.reject;
}

bool rendererShouldCancel(void* context) { return static_cast<RendererControlContext*>(context)->cancelled; }

TEST_F(JapaneseDictionaryTest, BufferedStyledRendererReportsSeekAndPrematureEofReadErrors) {
  const std::string path = "/renderer.html";
  const std::string html = "<b>definition</b>";
  writeText(resolve(path), html);
  DictHtmlRenderer renderer;
  char chunk[512]{};
  RendererControlContext context;
  const DictHtmlRenderer::ControlledSpanSink sink{&context, controlledRendererSpan, rendererShouldCancel};

  hal_storage_test::seekFailurePath = path;
  EXPECT_EQ(renderer.renderFromFileStreamingBuffered(path.c_str(), 0, html.size(), sink, chunk, sizeof(chunk)),
            DictHtmlStreamStatus::ReadError);
  hal_storage_test::seekFailurePath.clear();
  hal_storage_test::shortReadPath = path;
  hal_storage_test::shortReadOffset = 0;
  EXPECT_EQ(renderer.renderFromFileStreamingBuffered(path.c_str(), 0, html.size(), sink, chunk, sizeof(chunk)),
            DictHtmlStreamStatus::ReadError);
}

TEST_F(JapaneseDictionaryTest, BufferedStyledRendererDistinguishesSinkRejectionFromLaterReadFailure) {
  const std::string path = "/renderer-large.html";
  const std::string html = "<b>first</b>" + std::string(700, 'x');
  writeText(resolve(path), html);
  hal_storage_test::shortReadPath = path;
  hal_storage_test::shortReadOffset = 512;
  DictHtmlRenderer renderer;
  char chunk[512]{};
  RendererControlContext context;
  context.reject = true;
  const DictHtmlRenderer::ControlledSpanSink sink{&context, controlledRendererSpan, rendererShouldCancel};
  EXPECT_EQ(renderer.renderFromFileStreamingBuffered(path.c_str(), 0, html.size(), sink, chunk, sizeof(chunk)),
            DictHtmlStreamStatus::SinkRejected);
  EXPECT_EQ(context.spanCalls, 1u);
}

TEST_F(JapaneseDictionaryTest, StarDictStyledStreamStopsImmediatelyWhenSinkCancelsEngine) {
  writeStarDict({{"markup", "<b>first</b><i>second</i>"}}, 'h');
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"markup"}, result), DictionaryStatus::Found);
  CancelFromSinkContext context{&engine, {}};
  EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled,
                                    {&context, cancelFromDefinitionSink}),
            DictionaryStatus::Cancelled);
  EXPECT_EQ(context.text, "first");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictPlainDefinitionUsesOneReusable512ByteHeapBuffer) {
  const std::string plain(1200, 'x');
  writeStarDict({{"large", plain}}, 'm');
  dict_memory_test::reset();
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  EXPECT_EQ(std::count(dict_memory_test::requests.begin(), dict_memory_test::requests.end(), 512u), 1);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"large"}, result), DictionaryStatus::Found);
  std::vector<CapturedSpan> spans;
  ASSERT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&spans, captureSpan}),
            DictionaryStatus::Found);
  ASSERT_EQ(spans.size(), 3u);
  EXPECT_EQ(spans[0].text.size(), 512u);
  EXPECT_EQ(spans[1].text.size(), 512u);
  EXPECT_EQ(spans[2].text.size(), 176u);
  EXPECT_EQ(spans[0].text + spans[1].text + spans[2].text, plain);
  spans.clear();
  EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::PlainFallback, {&spans, captureSpan}),
            DictionaryStatus::Found);
  EXPECT_EQ(std::count(dict_memory_test::requests.begin(), dict_memory_test::requests.end(), 512u), 1);
  engine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictBufferOomAndMalformedIndexRemainDistinct) {
  writeStarDict({{"word", "definition"}});
  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = 512;
  DictionaryEngine oomEngine;
  EXPECT_EQ(oomEngine.open({"en", nullptr}), DictionaryStatus::OutOfMemory);
  oomEngine.close();
  EXPECT_TRUE(Dictionary::readDictPath().empty());

  Dictionary::setLookupDictPathOverride("/dictionaries/en/dict-data");
  writeBytes(resolve("/dictionaries/en/dict-data.idx"), {'b', 'a', 'd'});
  dict_memory_test::reset();
  DictionaryEngine readErrorEngine;
  ASSERT_EQ(readErrorEngine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  EXPECT_EQ(readErrorEngine.lookup({"word"}, result), DictionaryStatus::ReadError);
  readErrorEngine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictAlternateReadFailureIsNotReportedAsNotFound) {
  writeStarDict({{"target", "definition"}}, 'm', "alias", "target");
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  hal_storage_test::shortReadPath = "/dictionaries/en/dict-data.syn";
  hal_storage_test::shortReadOffset = 0;
  DictionaryResult result;
  EXPECT_EQ(engine.lookup({"alias"}, result), DictionaryStatus::ReadError);
  engine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictSuggestionReadFailureIsNotReportedAsNotFound) {
  writeStarDict({{"cat", "feline"}, {"cot", "bed"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionarySuggestions unchanged;
  ASSERT_TRUE(unchanged.items[0].assign("sentinel"));
  unchanged.count = 1;
  hal_storage_test::shortReadPath = "/dictionaries/en/dict-data.idx";
  hal_storage_test::shortReadOffset = 0;
  EXPECT_EQ(engine.suggest("cut", unchanged), DictionaryStatus::ReadError);
  EXPECT_EQ(unchanged.count, 1u);
  EXPECT_EQ(unchanged.items[0].view(), "sentinel");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictMaximumPublicTextAllocationFailurePreservesState) {
  const std::string maximumWord(255, 'w');
  writeStarDict({{"old", "old definition"}, {maximumWord, "new definition"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult published;
  ASSERT_EQ(engine.lookup({"old"}, published), DictionaryStatus::Found);

  DictionaryResult unchanged;
  ASSERT_TRUE(unchanged.headword.assign("sentinel"));
  unchanged.definition.generation = 91;
  for (const size_t rejectedRequest : {1u, 2u}) {
    dict_memory_test::reset();
    dict_memory_test::rejectedRequest = rejectedRequest;
    EXPECT_EQ(engine.lookup({maximumWord}, unchanged), DictionaryStatus::OutOfMemory);
    EXPECT_EQ(unchanged.headword.view(), "sentinel");
    EXPECT_EQ(unchanged.definition.generation, 91u);
  }

  std::string definition;
  EXPECT_EQ(engine.streamDefinition(published.definition, DictionaryDefinitionMode::Styled,
                                    {&definition, acceptDefinitionSpan}),
            DictionaryStatus::Found);
  EXPECT_EQ(definition, "old definition");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, EighthSuggestionAllocationFailurePreservesCallerOutput) {
  writeStarDict(
      {{"aa0", "0"}, {"aa1", "1"}, {"aa2", "2"}, {"aa3", "3"}, {"aa4", "4"}, {"aa5", "5"}, {"aa6", "6"}, {"aa7", "7"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);

  DictionarySuggestions baseline;
  dict_memory_test::reset();
  ASSERT_EQ(engine.suggest("aaz", baseline), DictionaryStatus::Found);
  ASSERT_EQ(baseline.count, DictionarySuggestions::kCapacity);
  ASSERT_EQ(dict_memory_test::requestCount, 1u + DictionarySuggestions::kCapacity);

  DictionarySuggestions unchanged;
  ASSERT_TRUE(unchanged.items[0].assign("sentinel"));
  unchanged.count = 1;
  dict_memory_test::reset();
  dict_memory_test::rejectedRequest = 1;
  EXPECT_EQ(engine.suggest("aaz", unchanged), DictionaryStatus::OutOfMemory);
  EXPECT_EQ(unchanged.items[0].view(), "sentinel");
  dict_memory_test::reset();
  dict_memory_test::rejectedRequest = 1u + DictionarySuggestions::kCapacity;
  EXPECT_EQ(engine.suggest("aaz", unchanged), DictionaryStatus::OutOfMemory);
  EXPECT_EQ(unchanged.count, 1u);
  EXPECT_EQ(unchanged.items[0].view(), "sentinel");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, HtmlRendererAllocationIsLazyAndFallible) {
  StarDictBackend unopened;
  EXPECT_FALSE(unopened.rendererAllocatedForTesting());
  writeStarDict({{"markup", "<b>definition</b>"}}, 'h');
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  ASSERT_EQ(engine.lookup({"markup"}, result), DictionaryStatus::Found);
  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = sizeof(DictHtmlRenderer);
  std::string definition;
  EXPECT_EQ(
      engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&definition, acceptDefinitionSpan}),
      DictionaryStatus::OutOfMemory);
  EXPECT_TRUE(definition.empty());
  dict_memory_test::reset();
  EXPECT_EQ(
      engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&definition, acceptDefinitionSpan}),
      DictionaryStatus::Found);
  EXPECT_EQ(definition, "definition");
  engine.close();
}

TEST_F(JapaneseDictionaryTest, StarDictSignatureChangesForSameSizedDefinitionReplacement) {
  writeStarDict({{"word", "feline"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  const uint64_t before = engine.signature();
  engine.close();

  writeStarDict({{"word", "canine"}});
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  EXPECT_NE(engine.signature(), before);
  engine.close();
}

TEST_F(JapaneseDictionaryTest, ReadsFrozenConverterOutputAtPreferredPath) {
  const std::filesystem::path golden = std::filesystem::path(JAPANESE_DICTIONARY_GOLDEN_DIR) / "mini_jmdict";
  std::filesystem::create_directories(resolve("/dictionaries/jp"));
  for (const char* name : {"vocab.idx", "vocab.dat", "vocab.spx"}) {
    std::filesystem::copy_file(golden / name, resolve(std::string("/dictionaries/jp/") + name));
  }
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("食べる", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "【たべる】\nto eat");
  EXPECT_EQ(entry.priority, 200);
  EXPECT_EQ(entry.posFlags, DictIndexRecord::POS_V1);
  ASSERT_EQ(index.lookupExact("超長語彙項目候補文字x", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "【超長語彙項目候補文字x】\n31-byte accepted headword");
}

TEST_F(JapaneseDictionaryTest, ReadsFrozenYomitanConverterOutputWithoutReencoding) {
  const std::filesystem::path golden = std::filesystem::path(JAPANESE_DICTIONARY_GOLDEN_DIR) / "mini_yomitan";
  std::filesystem::create_directories(resolve("/dictionaries/jp"));
  for (const char* name : {"vocab.idx", "vocab.dat", "vocab.spx"}) {
    std::filesystem::copy_file(golden / name, resolve(std::string("/dictionaries/jp/") + name));
  }
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("構造語", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(),
            "【こうぞうご】\n\n[noun] \n• structured definition\n• second structured sense\n example sentence");
  EXPECT_EQ(entry.priority, 178);
  EXPECT_EQ(entry.posFlags, DictIndexRecord::POS_OTHER);
}

TEST_F(JapaneseDictionaryTest, ResolvesEveryLegacyVocabularyPath) {
  for (const char* basename : {"/dictionaries/jp/jmdict", "/dict/vocab", "/dict/jmdict"}) {
    std::filesystem::remove_all(root_);
    writeSource(basename, {{"語", basename, 7, DictIndexRecord::POS_OTHER}});
    DictIndex index;
    ASSERT_EQ(index.open(), JapaneseDictStatus::Found) << basename;
    DictEntry entry;
    ASSERT_EQ(index.lookupExact("語", entry), JapaneseDictStatus::Found) << basename;
    EXPECT_EQ(entry.definitionView(), basename);
  }
}

TEST_F(JapaneseDictionaryTest, PrefersNewVocabularyPathOverLegacyPaths) {
  writeVocab({{"語", "preferred", 1, 0}});
  writeSource("/dictionaries/jp/jmdict", {{"語", "legacy", 9, 0}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("語", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "preferred");
}

TEST_F(JapaneseDictionaryTest, DoesNotMixPreferredIndexWithLegacyData) {
  const auto preferred = encode({{"語", "preferred", 1, 0}});
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), preferred.first);
  writeSource("/dict/vocab", {{"語", "legacy", 9, 0}});
  DictIndex index;
  EXPECT_EQ(index.open(), JapaneseDictStatus::Unavailable);
}

TEST_F(JapaneseDictionaryTest, ResolvesEveryNamesPath) {
  const std::array<const char*, 4> paths = {"/dictionaries/jp/names", "/dictionaries/jp/jmnedict", "/dict/names",
                                            "/dict/jmnedict"};
  for (const char* basename : paths) {
    std::filesystem::remove_all(root_);
    writeVocab({{"語", "vocab", 1, 0}});
    writeSource(basename, {{"山田", basename, 8, DictIndexRecord::POS_OTHER}});
    DictIndex index;
    ASSERT_EQ(index.open(), JapaneseDictStatus::Found) << basename;
    DictEntry entry;
    ASSERT_EQ(index.lookupExact("山田", entry, DictIndex::DICT_NAMES), JapaneseDictStatus::Found) << basename;
    EXPECT_EQ(entry.definitionView(), basename);
  }
}

TEST_F(JapaneseDictionaryTest, ResolvesPreferredAndLegacyGrammarPaths) {
  for (const char* basename : {"/dictionaries/jp/grammar", "/dict/grammar"}) {
    std::filesystem::remove_all(root_);
    writeVocab({{"語", "vocab", 1, 0}});
    writeSource(basename, {{"ている", basename, 8, 0}});
    DictIndex index;
    ASSERT_EQ(index.open(), JapaneseDictStatus::Found) << basename;
    DictEntry entry;
    ASSERT_EQ(index.lookupExact("ている", entry, DictIndex::DICT_GRAMMAR), JapaneseDictStatus::Found);
    EXPECT_EQ(entry.definitionView(), basename);
  }
}

TEST_F(JapaneseDictionaryTest, MissingOptionalNamesAndGrammarDoNotDisableVocabulary) {
  writeVocab({{"本", "book", 4, 0}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  EXPECT_EQ(index.availableSources(), DictIndex::DICT_JMDICT);
  DictEntry entry;
  EXPECT_EQ(index.lookupExact("本", entry), JapaneseDictStatus::Found);
}

TEST_F(JapaneseDictionaryTest, MissingAndStaleSparseIndexesFallBackToBinarySearch) {
  writeVocab({{"本", "missing spx", 4, 0}});
  DictIndex missing;
  ASSERT_EQ(missing.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(missing.lookupExact("本", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "missing spx");
  missing.close();

  auto [idx, dat] = encode({{"本", "stale spx", 5, 0}});
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), idx);
  writeBytes(resolve("/dictionaries/jp/vocab.dat"), dat);
  writeBytes(resolve("/dictionaries/jp/vocab.spx"), makeSpx(idx, 99));
  DictIndex stale;
  ASSERT_EQ(stale.open(), JapaneseDictStatus::Found);
  ASSERT_EQ(stale.lookupExact("本", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "stale spx");
}

TEST_F(JapaneseDictionaryTest, ValidSparseIndexMissDoesNotSearchTheWholeDictionary) {
  std::vector<InputRecord> records;
  records.reserve(8192);
  for (unsigned i = 0; i < 8192; ++i) {
    char word[16];
    std::snprintf(word, sizeof(word), "word%05u", i * 2);
    records.push_back({word, "definition"});
  }
  writeVocab(std::move(records), true);
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  const auto before = hal_storage_test::readCount;
  DictProbe result;
  EXPECT_EQ(index.probeExact("word08193", result), JapaneseDictStatus::NotFound);
  EXPECT_LE(hal_storage_test::readCount - before, 4u);
}

TEST_F(JapaneseDictionaryTest, ValidSparseIndexFindsLastCheckpointWindow) {
  std::vector<InputRecord> records;
  records.reserve(97);
  for (int i = 0; i < 97; ++i) {
    char key[8];
    std::snprintf(key, sizeof(key), "k%03d", i);
    records.push_back({key, key, static_cast<uint8_t>(i), 0});
  }
  writeVocab(std::move(records), true);
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("k096", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "k096");
}

TEST_F(JapaneseDictionaryTest, DamagedSparseCheckpointsCannotHideExistingWords) {
  std::vector<InputRecord> records;
  records.reserve(144);
  for (unsigned i = 0; i < 144; ++i) {
    char word[8];
    std::snprintf(word, sizeof(word), "k%03u", i);
    records.push_back({word, word});
  }
  auto [idx, dat] = writeSource("/dictionaries/jp/vocab", std::move(records));
  for (const char fill : {'a', 'z'}) {
    auto spx = makeSpx(idx);
    for (size_t offset = 32; offset < spx.size(); offset += 32) {
      std::fill_n(spx.data() + offset, 32, 0);
      spx[offset] = fill;
    }
    writeBytes(resolve("/dictionaries/jp/vocab.spx"), spx);
    DictIndex index;
    ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
    for (const char* word : {"k000", "k047", "k048", "k095", "k096", "k143"}) {
      DictEntry entry;
      ASSERT_EQ(index.lookupExact(word, entry), JapaneseDictStatus::Found) << word << fill;
      EXPECT_EQ(entry.definitionView(), word);
    }
    DictProbe result;
    EXPECT_EQ(index.probeExact("k050x", result), JapaneseDictStatus::NotFound);
    index.close();
  }
}

TEST_F(JapaneseDictionaryTest, MergesDuplicateDefinitionsInDescendingPriorityOrder) {
  writeVocab({{"方", "low", 10, 0},
              {"方", "highest", 200, DictIndexRecord::POS_OTHER},
              {"方", "middle", 100, DictIndexRecord::POS_V5}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("方", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "highest\n\n---\nmiddle\n\n---\nlow");
  EXPECT_EQ(entry.priority, 200);
  EXPECT_EQ(entry.posFlags, DictIndexRecord::POS_OTHER);
}

TEST_F(JapaneseDictionaryTest, RanksLateDuplicateSensesAndMergesOnlyTheBestFive) {
  std::vector<InputRecord> records;
  records.reserve(40);
  for (int i = 0; i < 35; ++i) records.push_back({"方", "low" + std::to_string(i), 1, 0});
  for (int priority = 100; priority <= 104; ++priority) {
    records.push_back({"方", "p" + std::to_string(priority), static_cast<uint8_t>(priority), 0});
  }
  writeVocab(std::move(records));
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("方", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "p104\n\n---\np103\n\n---\np102\n\n---\np101\n\n---\np100");
  EXPECT_EQ(entry.priority, 104);
}

TEST_F(JapaneseDictionaryTest, ProbeReportsHighestPrioritySiblingWithoutReadingDefinitions) {
  writeVocab({{"方", "low", 10, 0}, {"方", "high", 200, DictIndexRecord::POS_V5}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  ASSERT_EQ(index.probeExact("方", probe), JapaneseDictStatus::Found);
  EXPECT_STREQ(probe.headword, "方");
  EXPECT_EQ(probe.priority, 200);
  EXPECT_EQ(probe.posFlags, DictIndexRecord::POS_V5);
}

TEST_F(JapaneseDictionaryTest, SourceFilteringAndSourceOrderMatchMatcha) {
  writeVocab({{"同じ", "vocab", 1, 0}});
  writeSource("/dictionaries/jp/grammar", {{"同じ", "grammar", 200, 0}});
  writeSource("/dictionaries/jp/names", {{"同じ", "names", 255, 0}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("同じ", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "vocab");
  EXPECT_EQ(entry.sourceDict, DictIndex::DICT_JMDICT);
  ASSERT_EQ(index.lookupExact("同じ", entry, DictIndex::DICT_GRAMMAR | DictIndex::DICT_NAMES),
            JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "grammar");
  EXPECT_EQ(entry.sourceDict, DictIndex::DICT_GRAMMAR);
}

TEST_F(JapaneseDictionaryTest, PosMaskAcceptsIntersectingAndLegacyZeroFlagsOnly) {
  writeVocab({{"する", "noun", 220, DictIndexRecord::POS_OTHER},
              {"する", "legacy", 100, 0},
              {"する", "suru", 200, DictIndexRecord::POS_VS}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("する", entry, DictIndex::DICT_ALL, DictIndexRecord::POS_VS), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "suru\n\n---\nlegacy");
  EXPECT_EQ(entry.posFlags, DictIndexRecord::POS_VS);
  EXPECT_EQ(index.lookupExact("する", entry, DictIndex::DICT_ALL, DictIndexRecord::POS_V1), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "legacy");
}

TEST_F(JapaneseDictionaryTest, ReadingCollisionFlagSurvivesProbeAndLookup) {
  writeVocab({{"かな", "reading", 200, DictIndexRecord::POS_READING | DictIndexRecord::POS_OTHER}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  ASSERT_EQ(index.probeExact("かな", probe), JapaneseDictStatus::Found);
  EXPECT_EQ(probe.posFlags, DictIndexRecord::POS_READING | DictIndexRecord::POS_OTHER);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("かな", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.posFlags, DictIndexRecord::POS_READING | DictIndexRecord::POS_OTHER);
}

TEST_F(JapaneseDictionaryTest, RejectsIndexRecordSizeRemainderAtOpen) {
  auto [idx, dat] = encode({{"本", "book", 1, 0}});
  idx.push_back(0xff);
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), idx);
  writeBytes(resolve("/dictionaries/jp/vocab.dat"), dat);
  DictIndex index;
  EXPECT_EQ(index.open(), JapaneseDictStatus::ReadError);
}

TEST_F(JapaneseDictionaryTest, RejectsMissingNulAndNonzeroPadding) {
  auto [idx, dat] = encode({{"本", "book", 1, 0}});
  std::fill_n(idx.begin(), DictIndexRecord::HEADWORD_SIZE, static_cast<uint8_t>('x'));
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), idx);
  writeBytes(resolve("/dictionaries/jp/vocab.dat"), dat);
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  EXPECT_EQ(index.probeExact("x", probe), JapaneseDictStatus::ReadError);

  idx[1] = 0;
  idx[2] = 'x';
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), idx);
  index.close();
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  EXPECT_EQ(index.probeExact("x", probe), JapaneseDictStatus::ReadError);
}

TEST_F(JapaneseDictionaryTest, RejectsInvalidUtf8Headword) {
  auto [idx, dat] = encode({{"x", "bad", 1, 0}});
  idx[0] = 0xc0;
  idx[1] = 0xaf;
  idx[2] = 0;
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), idx);
  writeBytes(resolve("/dictionaries/jp/vocab.dat"), dat);
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  EXPECT_EQ(index.probeExact("x", probe), JapaneseDictStatus::ReadError);
}

TEST_F(JapaneseDictionaryTest, RejectsOutOfRangeDataSliceBeforeFound) {
  auto [idx, dat] = encode({{"本", "book", 1, 0}});
  idx[32] = 3;
  idx[33] = idx[34] = idx[35] = 0;
  idx[36] = 8;
  idx[37] = 0;
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), idx);
  writeBytes(resolve("/dictionaries/jp/vocab.dat"), dat);
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  EXPECT_EQ(index.probeExact("本", probe), JapaneseDictStatus::ReadError);
}

TEST_F(JapaneseDictionaryTest, ReportsShortIndexAndDataReads) {
  writeVocab({{"本", "book", 1, 0}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  hal_storage_test::shortReadPath = "/dictionaries/jp/vocab.idx";
  hal_storage_test::shortReadOffset = 0;
  DictProbe probe;
  EXPECT_EQ(index.probeExact("本", probe), JapaneseDictStatus::ReadError);

  index.close();
  hal_storage_test::shortReadPath.clear();
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  hal_storage_test::shortReadPath = "/dictionaries/jp/vocab.dat";
  hal_storage_test::shortReadOffset = 0;
  EXPECT_EQ(index.probeExact("本", probe), JapaneseDictStatus::Found);
  DictEntry entry;
  EXPECT_EQ(index.lookupExact("本", entry), JapaneseDictStatus::ReadError);
}

TEST_F(JapaneseDictionaryTest, CloseAndReopenReleaseHandlesAndObserveReplacement) {
  writeVocab({{"本", "first", 1, 0}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  const uint64_t firstSignature = index.signature();
  EXPECT_GE(hal_storage_test::openCount, 2u);
  index.close();
  EXPECT_EQ(index.availableSources(), 0);
  EXPECT_EQ(index.signature(), 0u);
  EXPECT_EQ(hal_storage_test::closeCount, hal_storage_test::openCount);

  writeVocab({{"本", "second", 2, 0}});
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  EXPECT_NE(index.signature(), firstSignature);
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("本", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "second");
}

TEST_F(JapaneseDictionaryTest, SignatureChangesForSameSizedIndexReplacementAndPathTag) {
  writeVocab({{"a", "one", 1, 0}});
  DictIndex first;
  ASSERT_EQ(first.open(), JapaneseDictStatus::Found);
  const uint64_t firstSignature = first.signature();
  EXPECT_EQ(firstSignature, 0xe9514a69bfc93808ULL);
  first.close();

  writeVocab({{"b", "one", 1, 0}});
  DictIndex replacement;
  ASSERT_EQ(replacement.open(), JapaneseDictStatus::Found);
  const uint64_t replacementSignature = replacement.signature();
  EXPECT_NE(replacementSignature, firstSignature);
  replacement.close();

  std::filesystem::remove_all(root_);
  writeSource("/dict/vocab", {{"b", "one", 1, 0}});
  DictIndex moved;
  ASSERT_EQ(moved.open(), JapaneseDictStatus::Found);
  EXPECT_NE(moved.signature(), replacementSignature);
}

TEST_F(JapaneseDictionaryTest, AcceleratorAllocationFailuresDegradeToDirectSearch) {
  std::vector<InputRecord> records;
  records.reserve(97);
  for (int i = 0; i < 97; ++i) {
    char key[8];
    std::snprintf(key, sizeof(key), "k%03d", i);
    records.push_back({key, key, static_cast<uint8_t>(i), 0});
  }
  writeVocab(std::move(records), true);
  // Three coarse records, a 32-byte fine slice, then the 2,560-byte block cache.
  const std::array<size_t, 3> rejectedSizes = {3u * 36u, 32u, 64u * sizeof(DictIndexRecord)};
  for (const size_t rejectedBytes : rejectedSizes) {
    SCOPED_TRACE(rejectedBytes);
    dict_memory_test::reset();
    dict_memory_test::rejectedBytes = rejectedBytes;
    DictIndex index;
    ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
    bool attemptedRejectedSize = false;
    for (size_t request = 0; request < dict_memory_test::requestCount; ++request) {
      attemptedRejectedSize |= dict_memory_test::requests[request] == rejectedBytes;
    }
    EXPECT_TRUE(attemptedRejectedSize);

    dict_memory_test::reset();
    DictProbe probe;
    ASSERT_EQ(index.probeExact("k096", probe), JapaneseDictStatus::Found);
    EXPECT_EQ(probe.priority, 96);
    EXPECT_EQ(dict_memory_test::requestCount, 0u);
  }
}

TEST_F(JapaneseDictionaryTest, ReportsSessionAllocationFailure) {
  dict_memory_test::rejectAll = true;
  DictIndex noSession;
  EXPECT_EQ(noSession.open(), JapaneseDictStatus::OutOfMemory);
}

TEST_F(JapaneseDictionaryTest, ReportsBestSenseAllocationFailure) {
  writeVocab({{"本", "book", 1, 0}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  ASSERT_EQ(index.probeExact("本", probe), JapaneseDictStatus::Found);
  dict_memory_test::rejectedBytes = 5;
  DictEntry entry;
  EXPECT_EQ(index.lookupExact("本", entry), JapaneseDictStatus::OutOfMemory);
  EXPECT_TRUE(entry.definitionView().empty());
  EXPECT_EQ(entry.definition, nullptr);
}

TEST_F(JapaneseDictionaryTest, OversizedHighestPrioritySenseFallsBackToReadableSibling) {
  writeVocab({{"語", "usable", 100, DictIndexRecord::POS_OTHER},
              {"語", std::string(16 * 1024 + 1, 'x'), 250, DictIndexRecord::POS_V5}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  ASSERT_EQ(index.probeExact("語", probe), JapaneseDictStatus::Found);
  dict_memory_test::reset();
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("語", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "usable");
  EXPECT_EQ(entry.headwordView(), "語");
  EXPECT_EQ(entry.priority, 100);
  EXPECT_EQ(entry.posFlags, DictIndexRecord::POS_OTHER);
  ASSERT_EQ(dict_memory_test::requestCount, 1u);
  EXPECT_EQ(dict_memory_test::requests[0], 7u);
}

TEST_F(JapaneseDictionaryTest, LowHeapMergePreservesBestReadableSense) {
  writeVocab({{"語", "secondary", 100, 0}, {"語", "primary", 200, DictIndexRecord::POS_OTHER}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  ASSERT_EQ(index.probeExact("語", probe), JapaneseDictStatus::Found);
  // Exact merged allocation is 7 + 6 + 9 + NUL = 23 bytes. Leave one
  // byte less than that allocation plus the required 8 KiB headroom, first
  // in total free heap and then in the largest contiguous block.
  constexpr uint32_t required = 23 + 8 * 1024;
  const std::array<std::pair<uint32_t, uint32_t>, 2> budgets = {std::pair{required - 1, 96u * 1024},
                                                                std::pair{96u * 1024, required - 1}};
  for (const auto& [freeHeap, maxAllocHeap] : budgets) {
    SCOPED_TRACE(testing::Message() << "free=" << freeHeap << " max=" << maxAllocHeap);
    dict_memory_test::reset();
    dict_arduino_test::freeHeap = freeHeap;
    dict_arduino_test::maxAllocHeap = maxAllocHeap;
    DictEntry entry;
    ASSERT_EQ(index.lookupExact("語", entry), JapaneseDictStatus::Found);
    EXPECT_EQ(entry.definitionView(), "primary");
    ASSERT_EQ(dict_memory_test::requestCount, 1u);
    EXPECT_EQ(dict_memory_test::requests[0], 8u);
  }
}

TEST_F(JapaneseDictionaryTest, FailedExactMergeAllocationPreservesBestReadableSense) {
  writeVocab({{"語", "secondary", 100, 0}, {"語", "primary", 200, DictIndexRecord::POS_OTHER}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  ASSERT_EQ(index.probeExact("語", probe), JapaneseDictStatus::Found);
  dict_memory_test::reset();
  dict_memory_test::rejectedRequest = 2;
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("語", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionView(), "primary");
  ASSERT_EQ(dict_memory_test::requestCount, 2u);
  EXPECT_EQ(dict_memory_test::requests[0], 8u);
  EXPECT_EQ(dict_memory_test::requests[1], 23u);
}

TEST_F(JapaneseDictionaryTest, FiveMaximumSensesUseOnlyBestAndExactMergedAllocations) {
  std::vector<InputRecord> records;
  for (uint8_t priority = 1; priority <= 5; ++priority) {
    records.push_back({"語", std::string(16 * 1024, static_cast<char>('a' + priority - 1)), priority, 0});
  }
  writeVocab(std::move(records));
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  DictProbe probe;
  ASSERT_EQ(index.probeExact("語", probe), JapaneseDictStatus::Found);
  dict_memory_test::reset();
  dict_arduino_test::freeHeap = 128 * 1024;
  dict_arduino_test::maxAllocHeap = 96 * 1024;
  DictEntry entry;
  ASSERT_EQ(index.lookupExact("語", entry), JapaneseDictStatus::Found);
  EXPECT_EQ(entry.definitionLength, 5 * 16 * 1024 + 4 * 6);
  ASSERT_NE(entry.definition, nullptr);
  EXPECT_EQ(entry.definition[entry.definitionLength], '\0');
  ASSERT_EQ(dict_memory_test::requestCount, 2u);
  EXPECT_EQ(dict_memory_test::requests[0], 16 * 1024 + 1);
  EXPECT_EQ(dict_memory_test::requests[1], 5 * 16 * 1024 + 4 * 6 + 1);
  EXPECT_EQ(dict_memory_test::largestRequest, 81945u);
  // The best buffer remains active while the exact merged result is built;
  // these are the only two allocations, so their sum is the definition-path peak.
  EXPECT_EQ(dict_memory_test::totalRequested, 16385u + 81945u);
}

TEST_F(JapaneseDictionaryTest, AllThreeSourcesOwnIndependentBoundedAccelerators) {
  writeMaximumCacheSource("/dictionaries/jp/vocab");
  writeMaximumCacheSource("/dictionaries/jp/grammar");
  writeMaximumCacheSource("/dictionaries/jp/names");
  dict_memory_test::reset();
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  ASSERT_EQ(index.availableSources(), DictIndex::DICT_ALL);
  ASSERT_EQ(dict_memory_test::requestCount, 10u);
  EXPECT_EQ(dict_memory_test::totalRequested - dict_memory_test::requests[0], 3u * (128u * 36u + 3904u + 64u * 40u));
  dict_memory_test::reset();
  DictProbe probe;
  EXPECT_EQ(index.probeExact("語", probe, DictIndex::DICT_JMDICT), JapaneseDictStatus::ReadError);
  EXPECT_EQ(index.probeExact("語", probe, DictIndex::DICT_GRAMMAR), JapaneseDictStatus::ReadError);
  EXPECT_EQ(index.probeExact("語", probe, DictIndex::DICT_NAMES), JapaneseDictStatus::ReadError);
  EXPECT_EQ(dict_memory_test::requestCount, 0u);
}

TEST_F(JapaneseDictionaryTest, MissingRequiredPairIsUnavailable) {
  DictIndex missing;
  EXPECT_EQ(missing.open(), JapaneseDictStatus::Unavailable);
  auto [idx, dat] = encode({{"本", "book", 1, 0}});
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), idx);
  DictIndex halfPair;
  EXPECT_EQ(halfPair.open(), JapaneseDictStatus::Unavailable);
}

std::string_view candidateText(const DeinflectionCandidate& candidate) {
  return {candidate.text, candidate.byteLength};
}

TEST_F(JapaneseDictionaryTest, DeinflectionRuleTableHasPinnedMatchaCardinality) {
  EXPECT_EQ(Deinflector::kRuleCount, 260u);
  EXPECT_EQ(DeinflectionBuffer::kCapacity, 64u);
}

TEST_F(JapaneseDictionaryTest, DeinflectsEveryWordClassAndCommonConjugationFamily) {
  struct Case {
    const char* surface;
    const char* canonical;
    WordCondition condition;
  };
  static constexpr Case cases[] = {
      {"食べました", "食べる", WordCondition::V1},     {"食べない", "食べる", WordCondition::V1},
      {"食べた", "食べる", WordCondition::V1},         {"食べて", "食べる", WordCondition::V1},
      {"食べられる", "食べる", WordCondition::V1},     {"食べさせる", "食べる", WordCondition::V1},
      {"書かなかった", "書く", WordCondition::V5},     {"書いて", "書く", WordCondition::V5},
      {"急いだ", "急ぐ", WordCondition::V5},           {"話した", "話す", WordCondition::V5},
      {"待った", "待つ", WordCondition::V5},           {"死んだ", "死ぬ", WordCondition::V5},
      {"遊んだ", "遊ぶ", WordCondition::V5},           {"読んだ", "読む", WordCondition::V5},
      {"取った", "取る", WordCondition::V5},           {"買った", "買う", WordCondition::V5},
      {"書かれる", "書く", WordCondition::V5},         {"書かせる", "書く", WordCondition::V5},
      {"書ける", "書く", WordCondition::V5},           {"勉強している", "勉強する", WordCondition::VS},
      {"勉強された", "勉強する", WordCondition::VS},   {"きた", "くる", WordCondition::VK},
      {"こられる", "くる", WordCondition::VK},         {"高かった", "高い", WordCondition::ADJ_I},
      {"高くない", "高い", WordCondition::ADJ_I},      {"高くて", "高い", WordCondition::ADJ_I},
      {"食べさせられた", "食べる", WordCondition::V1},
  };

  for (const auto& test : cases) {
    SCOPED_TRACE(test.surface);
    DeinflectionBuffer buffer;
    Deinflector::deinflect(test.surface, buffer);
    bool found = false;
    for (uint8_t index = 0; index < buffer.count; ++index) {
      if (candidateText(buffer.candidates[index]) == test.canonical &&
          buffer.candidates[index].condition == test.condition) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found);
  }
}

TEST_F(JapaneseDictionaryTest, DeinflectionIsBreadthFirstDeduplicatedBoundedAndSkipsOversizedCandidates) {
  DeinflectionBuffer buffer;
  Deinflector::deinflect("行った", buffer);
  struct Expected {
    const char* text;
    WordCondition condition;
  };
  static constexpr Expected expected[] = {
      {"行った", WordCondition::DICT}, {"行っる", WordCondition::V1}, {"行つ", WordCondition::V5},
      {"行る", WordCondition::V5},     {"行う", WordCondition::V5},   {"行く", WordCondition::V5},
      {"行い", WordCondition::ADJ_I},
  };
  ASSERT_EQ(buffer.count, std::size(expected));
  for (uint8_t index = 0; index < buffer.count; ++index) {
    EXPECT_EQ(candidateText(buffer.candidates[index]), expected[index].text);
    EXPECT_EQ(buffer.candidates[index].condition, expected[index].condition);
  }
  for (uint8_t left = 0; left < buffer.count; ++left) {
    EXPECT_LT(buffer.candidates[left].byteLength, DictIndexRecord::HEADWORD_SIZE);
    for (uint8_t right = left + 1; right < buffer.count; ++right) {
      EXPECT_NE(candidateText(buffer.candidates[left]), candidateText(buffer.candidates[right]));
    }
  }
  EXPECT_LE(buffer.count, DeinflectionBuffer::kCapacity);

  Deinflector::deinflect("あああああああああああ", buffer);
  EXPECT_EQ(buffer.count, 0u);
}

TEST_F(JapaneseDictionaryTest, SharedBfsCoreStopsAtExactly64AndKeepsFirstDuplicate) {
  static constexpr DeinflectionRuleForTest rules[] = {
      {"", "a", WordCondition::DICT, WordCondition::DICT},
      {"", "a", WordCondition::DICT, WordCondition::V1},
      {"", "b", WordCondition::DICT, WordCondition::DICT},
  };
  DeinflectionBuffer buffer;
  Deinflector::deinflectForTest("x", rules, std::size(rules), buffer);
  ASSERT_EQ(buffer.count, DeinflectionBuffer::kCapacity);
  EXPECT_EQ(candidateText(buffer.candidates[0]), "x");
  EXPECT_EQ(candidateText(buffer.candidates[1]), "xa");
  EXPECT_EQ(buffer.candidates[1].condition, WordCondition::DICT);
  EXPECT_EQ(candidateText(buffer.candidates[2]), "xb");
  EXPECT_EQ(candidateText(buffer.candidates[3]), "xaa");
  EXPECT_EQ(candidateText(buffer.candidates[4]), "xab");
  EXPECT_EQ(candidateText(buffer.candidates[5]), "xba");
  EXPECT_EQ(candidateText(buffer.candidates[6]), "xbb");
  for (uint8_t left = 0; left < buffer.count; ++left) {
    for (uint8_t right = left + 1; right < buffer.count; ++right) {
      EXPECT_NE(candidateText(buffer.candidates[left]), candidateText(buffer.candidates[right]));
    }
  }
}

TEST_F(JapaneseDictionaryTest, LookupMatchesFrozenParityCasesAndPinnedUnsupportedKuruForm) {
  writeVocab({{"食べる", "eat", 200, DictIndexRecord::POS_V1},
              {"書く", "write", 200, DictIndexRecord::POS_V5},
              {"勉強する", "study", 200, DictIndexRecord::POS_VS},
              {"くる", "come", 200, DictIndexRecord::POS_VK},
              {"来る", "come in kanji", 200, DictIndexRecord::POS_VK},
              {"高い", "high", 200, DictIndexRecord::POS_ADJ_I},
              {"冊", "counter", 200, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/names", {{"サクラ", "Sakura", 200, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/grammar", {{"に違いない", "must be", 200, DictIndexRecord::POS_OTHER}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  WordLookup lookup(index);
  std::vector<ParityCase> cases;
  std::string parseError;
  ASSERT_TRUE(loadParityCases(cases, parseError)) << parseError;
  ASSERT_EQ(cases.size(), 9u) << "fixture cases must all be consumed";
  EXPECT_EQ(std::count_if(cases.begin(), cases.end(),
                          [](const ParityCase& test) { return test.status == JapaneseDictStatus::NotFound; }),
            1u)
      << "the fixture's explicit expected_status:not_found case must be parsed";
  for (const auto& test : cases) {
    SCOPED_TRACE(test.label);
    WordLookupProbe probe;
    EXPECT_EQ(lookup.probe(test.surface, 0, probe), test.status);
    EXPECT_EQ(probe.matchLength, test.matchedUtf8Bytes);
    EXPECT_EQ(probe.sourceDict, test.sourceMask);
    EXPECT_EQ(probe.deinflected, test.deinflected);
    EXPECT_EQ(probe.priority, test.priority);
    WordLookupResult result;
    EXPECT_EQ(lookup.lookup(test.surface, 0, result), test.status);
    EXPECT_EQ(result.matchLength, test.matchedUtf8Bytes);
    EXPECT_EQ(result.entry.headwordView(), test.canonicalHeadword);
    EXPECT_EQ(result.entry.sourceDict, test.sourceMask);
    EXPECT_EQ(result.deinflected, test.deinflected);
    EXPECT_EQ(result.entry.priority, test.priority);
  }
}

TEST_F(JapaneseDictionaryTest, LookupUsesUtf8BoundariesLongestFirstAndRejectsInvalidOffsets) {
  writeVocab({{"食", "eat character", 100, DictIndexRecord::POS_OTHER},
              {"食べる", "eat", 200, DictIndexRecord::POS_V1},
              {"食べる物", "food", 210, DictIndexRecord::POS_OTHER}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  WordLookup lookup(index);
  WordLookupProbe probe;
  ASSERT_EQ(lookup.probe("前食べる物後", 3, probe), JapaneseDictStatus::Found);
  EXPECT_EQ(probe.matchLength, std::string_view("食べる物").size());
  EXPECT_FALSE(probe.deinflected);
  EXPECT_EQ(lookup.probe("前食べる物後", 4, probe), JapaneseDictStatus::NotFound);
  EXPECT_EQ(lookup.probe(std::string_view("食\xf0\x28\x8c\x28", 7), 0, probe), JapaneseDictStatus::NotFound);
}

TEST_F(JapaneseDictionaryTest, ProbeDoesNotAllocateOrReadDefinitionsAndLookupOwnsOnlyActiveResult) {
  writeVocab({{"食べる", "eat", 200, DictIndexRecord::POS_V1}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  WordLookup lookup(index);
  dict_memory_test::reset();
  hal_storage_test::shortReadPath = "/dictionaries/jp/vocab.dat";
  hal_storage_test::shortReadOffset = 0;
  WordLookupProbe probe;
  EXPECT_EQ(lookup.probe("食べました", 0, probe), JapaneseDictStatus::Found);
  EXPECT_EQ(dict_memory_test::requestCount, 0u);

  hal_storage_test::shortReadPath.clear();
  dict_memory_test::reset();
  WordLookupResult result;
  ASSERT_EQ(lookup.lookup("食べました", 0, result), JapaneseDictStatus::Found);
  EXPECT_EQ(result.entry.definitionView(), "eat");
  ASSERT_EQ(dict_memory_test::requestCount, 1u);
  EXPECT_EQ(dict_memory_test::requests[0], 4u);
}

TEST_F(JapaneseDictionaryTest, FirstProbeAllocatesNothingForValidMissingAndStaleSparseIndexes) {
  for (const int sparseCase : {0, 1, 2}) {
    SCOPED_TRACE(sparseCase);
    std::filesystem::remove_all(root_);
    hal_storage_test::reset();
    if (sparseCase == 0)
      writeVocab({{"食べる", "missing", 200, DictIndexRecord::POS_V1}});
    else
      writeVocab({{"食べる", "sparse", 200, DictIndexRecord::POS_V1}}, true, sparseCase == 1 ? UINT32_MAX : 99);

    DictIndex index;
    ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
    dict_memory_test::reset();
    hal_storage_test::shortReadPath = "/dictionaries/jp/vocab.dat";
    hal_storage_test::shortReadOffset = 0;
    WordLookup lookup(index);
    WordLookupProbe probe;
    EXPECT_EQ(lookup.probe("食べました", 0, probe), JapaneseDictStatus::Found);
    EXPECT_EQ(dict_memory_test::requestCount, 0u);
  }
}

TEST_F(JapaneseDictionaryTest, DeinflectedLookupRejectsWrongPosAndSuppressesReadingRecords) {
  writeVocab({{"食べる", "noun collision", 250, DictIndexRecord::POS_OTHER},
              {"食べる", "reading collision", 240, DictIndexRecord::POS_READING | DictIndexRecord::POS_OTHER},
              {"食べる", "verb", 200, DictIndexRecord::POS_V1},
              {"だとう", "rare reading", 100, DictIndexRecord::POS_READING | DictIndexRecord::POS_OTHER}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  WordLookup lookup(index);
  WordLookupResult result;
  ASSERT_EQ(lookup.lookup("食べました", 0, result), JapaneseDictStatus::Found);
  EXPECT_EQ(result.entry.definitionView(), "verb");
  EXPECT_EQ(result.entry.posFlags, DictIndexRecord::POS_V1);
  EXPECT_EQ(lookup.lookup("だとい", 0, result), JapaneseDictStatus::NotFound);
}

TEST_F(JapaneseDictionaryTest, RawLookupUsesSourceOrderAndMergesSameHeadwordWithinWinningSource) {
  writeVocab({{"同じ", "vocab-low", 100, DictIndexRecord::POS_OTHER},
              {"同じ", "vocab-high", 210, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/grammar", {{"同じ", "grammar", 250, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/names", {{"同じ", "name", 255, DictIndexRecord::POS_OTHER}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  WordLookup lookup(index);
  WordLookupResult result;
  ASSERT_EQ(lookup.lookup("同じ", 0, result), JapaneseDictStatus::Found);
  EXPECT_EQ(result.entry.sourceDict, DictIndex::DICT_JMDICT);
  EXPECT_EQ(result.entry.priority, 210);
  EXPECT_EQ(result.entry.definitionView(), "vocab-high\n\n---\nvocab-low");
}

std::shared_ptr<TextBlock> makePageTextBlock(std::vector<std::string> words, std::vector<int16_t> positions,
                                             std::vector<EpdFontFamily::Style> styles = {},
                                             std::vector<uint8_t> bionicBoundaries = {},
                                             std::vector<uint16_t> bionicOffsets = {}, std::vector<uint8_t> flags = {},
                                             bool rtl = false, bool ruby = false) {
  return std::make_shared<TextBlock>(std::move(words), std::move(positions), std::move(styles),
                                     std::move(bionicBoundaries), std::move(bionicOffsets), std::move(flags), rtl,
                                     ruby);
}

void addPageTextLine(Page& page, std::shared_ptr<TextBlock> block, const int16_t x, const int16_t y) {
  page.elements.push_back(std::make_unique<PageLine>(std::move(block), x, y));
}

std::vector<uint32_t> pageTextCodepoints(const PageTextSourceView view) {
  std::vector<uint32_t> result;
  result.reserve(view.glyphCount);
  for (uint16_t i = 0; i < view.glyphCount; ++i) result.push_back(view.glyphs[i].codepoint);
  return result;
}

class HorizontalPageTextSourceTest : public testing::Test {
 protected:
  void SetUp() override { dict_memory_test::reset(); }
};

TEST_F(HorizontalPageTextSourceTest, ConcatenatesCjkAcrossWordsAndPreservesLineOrderAndPageWordOrdinals) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"日本", "語"}, {0, 20}), 10, 20);
  page.elements.push_back(std::make_unique<PageImage>(0, 0));
  addPageTextLine(page, makePageTextBlock({"辞書"}, {0}), 10, 50);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 7, 3, 4), DictionaryStatus::Found);
  const auto view = source.view();
  EXPECT_EQ(pageTextCodepoints(view), (std::vector<uint32_t>{0x65E5, 0x672C, 0x8A9E, 0x8F9E, 0x66F8}));
  ASSERT_EQ(view.glyphCount, 5);
  EXPECT_EQ(view.glyphs[0].pageWord, 0);
  EXPECT_EQ(view.glyphs[1].pageWord, 0);
  EXPECT_EQ(view.glyphs[2].pageWord, 1);
  EXPECT_EQ(view.glyphs[3].pageWord, 2);
  EXPECT_EQ(view.glyphs[4].pageWord, 2);
  EXPECT_EQ(view.glyphs[0].paragraph, 0);
  EXPECT_EQ(view.glyphs[4].paragraph, 0);
}

TEST_F(HorizontalPageTextSourceTest, InsertsOnlyAsciiWordSeparatorsWithNonselectableZeroGeometry) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"Hello", "world", "日本", "語"}, {0, 20, 50, 70}), 2, 3);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 5, 7), DictionaryStatus::Found);
  const auto view = source.view();
  const std::vector<uint32_t> expected{'H', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', 0x65E5, 0x672C, 0x8A9E};
  ASSERT_EQ(pageTextCodepoints(view), expected);
  const auto& separator = view.glyphs[5];
  EXPECT_EQ(separator.pageWord, PageTextGlyph::kSyntheticPageWord);
  EXPECT_EQ(separator.x, 0);
  EXPECT_EQ(separator.y, 0);
  EXPECT_EQ(separator.width, 0);
  EXPECT_EQ(separator.height, 0);
}

TEST_F(HorizontalPageTextSourceTest, GivesVisibleNonlookupCodepointsTheirSourceWordRectangle) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"!"}, {4}), 10, 20);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 2, 3), DictionaryStatus::Found);
  const auto glyph = source.view().glyphs[0];
  EXPECT_EQ(glyph.codepoint, '!');
  EXPECT_EQ(glyph.pageWord, 0);
  EXPECT_EQ(glyph.x, 16);
  EXPECT_EQ(glyph.y, 23);
  EXPECT_EQ(glyph.width, 1);
  EXPECT_EQ(glyph.height, 16);
}

TEST_F(HorizontalPageTextSourceTest, CountPassDoesNotMeasureWordGeometryBeforePopulation) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"語"}, {0}), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);
  EXPECT_EQ(renderer.measurementCalls, 2U);  // Natural space plus the one source-word rectangle.
}

TEST_F(HorizontalPageTextSourceTest, DecodesFourByteUtf8WithoutSplittingCodepoints) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"A\xF0\x9F\x98\x80\xE3\x81\x82"}, {0}), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);
  EXPECT_EQ(pageTextCodepoints(source.view()), (std::vector<uint32_t>{'A', 0x1F600, 0x3042}));
}

TEST_F(HorizontalPageTextSourceTest, RejectsInvalidUtf8AndDoesNotPublishAPartialView) {
  for (const std::string& invalid : {std::string("\xE3\x81", 2), std::string("\xC0\xAF", 2),
                                     std::string("\xED\xA0\x80", 3), std::string("\x80", 1)}) {
    SCOPED_TRACE(testing::PrintToString(invalid));
    Page page;
    addPageTextLine(page, makePageTextBlock({invalid}, {0}), 0, 0);
    GfxRenderer renderer;
    HorizontalPageTextSource source;

    EXPECT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::ReadError);
    EXPECT_EQ(source.view().glyphs, nullptr);
    EXPECT_EQ(source.view().glyphCount, 0);
    EXPECT_FALSE(source.truncated());
  }
}

TEST_F(HorizontalPageTextSourceTest, AppliesRubyShiftAndLogicalMarginsToScreenCoordinates) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"語"}, {6}, {}, {}, {}, {}, false, true), 10, 20);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 3, 4), DictionaryStatus::Found);
  const auto glyph = source.view().glyphs[0];
  EXPECT_EQ(glyph.x, 19);
  EXPECT_EQ(glyph.y, 28);
  EXPECT_EQ(glyph.height, 16);
}

TEST_F(HorizontalPageTextSourceTest, RemovesOnlyLayoutInsertedTrailingHyphensAndJoinsTheContinuation) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"exam-"}, {0}, {}, {}, {}, {0x02}), 0, 0);
  addPageTextLine(page, makePageTextBlock({"ple"}, {0}), 0, 20);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);
  EXPECT_EQ(pageTextCodepoints(source.view()), (std::vector<uint32_t>{'e', 'x', 'a', 'm', 'p', 'l', 'e'}));
  EXPECT_EQ(source.view().glyphs[3].pageWord, 0);
  EXPECT_EQ(source.view().glyphs[4].pageWord, 1);
}

TEST_F(HorizontalPageTextSourceTest, MeasuresSoftHyphenWordRectangleLikeRenderedText) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"co\xC2\xADoperate"}, {0}), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);
  const auto view = source.view();
  ASSERT_EQ(view.glyphCount, 10);
  for (uint16_t i = 0; i < view.glyphCount; ++i) EXPECT_EQ(view.glyphs[i].width, 9);
}

TEST_F(HorizontalPageTextSourceTest, ClearsAndReportsOutOfMemoryWhenSoftHyphenScratchCannotAllocate) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"co\xC2\xADoperate"}, {0}), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;
  dict_memory_test::rejectedRequest = 2;

  EXPECT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::OutOfMemory);
  EXPECT_EQ(source.view().glyphs, nullptr);
  EXPECT_EQ(source.view().glyphCount, 0);
  EXPECT_EQ(source.view().contentHash, 0U);
  EXPECT_FALSE(source.truncated());
  EXPECT_EQ(renderer.releaseCalls, 0U);
  ASSERT_EQ(dict_memory_test::requestCount, 2U);
  EXPECT_EQ(dict_memory_test::requests[0], 10U * sizeof(PageTextGlyph));
  EXPECT_EQ(dict_memory_test::requests[1], 12U);
}

TEST_F(HorizontalPageTextSourceTest, UsesLegacyBionicRunWidthForEveryCodepointInTheSourceWord) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"abcd"}, {0}, {EpdFontFamily::REGULAR}, {2}, {10}), 4, 5);
  GfxRenderer renderer;
  HorizontalPageTextSource source;

  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);
  const auto view = source.view();
  ASSERT_EQ(view.glyphCount, 4);
  for (uint16_t i = 0; i < view.glyphCount; ++i) EXPECT_EQ(view.glyphs[i].width, 12);
}

TEST_F(HorizontalPageTextSourceTest, UnionsSourceWordRectanglesAcrossAMultiwordMatchAndIgnoresSeparators) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"aa", "b"}, {0, 20}), 10, 20);
  GfxRenderer renderer;
  HorizontalPageTextSource source;
  ASSERT_EQ(source.build(page, renderer, 1, 5, 3), DictionaryStatus::Found);

  PageTextBounds bounds;
  ASSERT_TRUE(unionPageTextGlyphBounds(source.view(), 0, source.view().glyphCount, bounds));
  EXPECT_EQ(bounds.x, 15);
  EXPECT_EQ(bounds.y, 23);
  EXPECT_EQ(bounds.width, 21);
  EXPECT_EQ(bounds.height, 16);
}

TEST_F(HorizontalPageTextSourceTest, CoordinatesAndHashStayInLogicalSpaceAcrossRendererOrientation) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"word"}, {7}), 11, 13);
  GfxRenderer renderer;
  HorizontalPageTextSource source;
  ASSERT_EQ(source.build(page, renderer, 1, 2, 3), DictionaryStatus::Found);
  const PageTextGlyph portrait = source.view().glyphs[0];
  const uint32_t portraitHash = source.view().contentHash;

  renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
  ASSERT_EQ(source.build(page, renderer, 1, 2, 3), DictionaryStatus::Found);
  const PageTextGlyph landscape = source.view().glyphs[0];
  EXPECT_EQ(landscape.x, portrait.x);
  EXPECT_EQ(landscape.y, portrait.y);
  EXPECT_EQ(landscape.width, portrait.width);
  EXPECT_EQ(landscape.height, portrait.height);
  EXPECT_EQ(source.view().contentHash, portraitHash);
}

TEST_F(HorizontalPageTextSourceTest, RetriesExactAllocationOnceAfterReleasingEligibleFontCaches) {
  Page page;
  std::vector<std::string> words(300, "日");
  std::vector<int16_t> positions(300);
  addPageTextLine(page, makePageTextBlock(std::move(words), std::move(positions)), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;
  dict_memory_test::reset();
  dict_memory_test::rejectedRequest = 1;

  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);
  EXPECT_EQ(source.view().glyphCount, 300);
  EXPECT_FALSE(source.truncated());
  EXPECT_EQ(renderer.releaseCalls, 1U);
  ASSERT_EQ(dict_memory_test::requestCount, 2U);
  EXPECT_EQ(dict_memory_test::requests[0], 300U * sizeof(PageTextGlyph));
  EXPECT_EQ(dict_memory_test::requests[1], 300U * sizeof(PageTextGlyph));
}

TEST_F(HorizontalPageTextSourceTest, FallsBackToFirst256GlyphsAndMarksTheViewTruncated) {
  Page page;
  std::vector<std::string> words(300, "日");
  std::vector<int16_t> positions(300);
  addPageTextLine(page, makePageTextBlock(std::move(words), std::move(positions)), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;
  dict_memory_test::reset();
  dict_memory_test::rejectThroughRequest = 2;

  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);
  EXPECT_EQ(source.view().glyphCount, 256);
  EXPECT_TRUE(source.truncated());
  EXPECT_EQ(renderer.releaseCalls, 1U);
  ASSERT_EQ(dict_memory_test::requestCount, 3U);
  EXPECT_EQ(dict_memory_test::requests[2], 256U * sizeof(PageTextGlyph));
}

TEST_F(HorizontalPageTextSourceTest, ReportsOutOfMemoryWhenEvenTheBoundedFallbackCannotAllocate) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"日本語"}, {0}), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;
  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;

  EXPECT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::OutOfMemory);
  EXPECT_EQ(source.view().glyphs, nullptr);
  EXPECT_EQ(source.view().glyphCount, 0);
}

TEST_F(HorizontalPageTextSourceTest, ClearReleasesTheViewAndResetsTruncation) {
  Page page;
  addPageTextLine(page, makePageTextBlock({"語"}, {0}), 0, 0);
  GfxRenderer renderer;
  HorizontalPageTextSource source;
  ASSERT_EQ(source.build(page, renderer, 1, 0, 0), DictionaryStatus::Found);

  source.clear();

  EXPECT_EQ(source.view().glyphs, nullptr);
  EXPECT_EQ(source.view().glyphCount, 0);
  EXPECT_EQ(source.view().contentHash, 0U);
  EXPECT_FALSE(source.truncated());
}

struct ScannerProbeRule {
  std::string prefix;
  DictionaryStatus status = DictionaryStatus::Found;
  size_t matchedBytes = SIZE_MAX;
  bool transformed = false;
  uint8_t sourceMask = DictIndex::DICT_JMDICT;
  uint8_t priority = 200;
  uint8_t posFlags = DictIndexRecord::POS_OTHER;
};

struct ScannerProbeRecorder {
  ScannerProbeRecorder() = default;
  ScannerProbeRecorder(std::initializer_list<ScannerProbeRule> initialRules) : rules(initialRules) {}

  std::vector<ScannerProbeRule> rules;
  std::vector<std::string> texts;
  std::vector<DictionaryLookupMode> modes;

  static DictionaryStatus call(void* context, const DictionaryQuery& query, DictionaryProbeResult& out) {
    auto& self = *static_cast<ScannerProbeRecorder*>(context);
    self.texts.emplace_back(query.text);
    self.modes.push_back(query.mode);
    out = {};
    const ScannerProbeRule* best = nullptr;
    for (const auto& rule : self.rules) {
      if (!query.text.starts_with(rule.prefix)) continue;
      if (!best || rule.prefix.size() > best->prefix.size()) best = &rule;
    }
    if (!best) return out.status = DictionaryStatus::NotFound;
    out.status = best->status;
    if (best->status == DictionaryStatus::Found) {
      out.matchedBytes = best->matchedBytes == SIZE_MAX ? best->prefix.size() : best->matchedBytes;
      out.transformed = best->transformed;
      out.sourceMask = best->sourceMask;
      out.priority = best->priority;
      out.posFlags = best->posFlags;
    }
    return best->status;
  }
};

std::vector<PageTextGlyph> makeScannerGlyphs(std::u32string_view codepoints, uint16_t paragraph = 0) {
  std::vector<PageTextGlyph> glyphs;
  glyphs.reserve(codepoints.size());
  for (size_t index = 0; index < codepoints.size(); ++index) {
    PageTextGlyph glyph;
    glyph.codepoint = static_cast<uint32_t>(codepoints[index]);
    glyph.paragraph = paragraph;
    glyph.pageWord = static_cast<uint16_t>(index);
    glyph.x = static_cast<int16_t>(index * 8);
    glyph.width = 8;
    glyph.height = 16;
    glyphs.push_back(glyph);
  }
  return glyphs;
}

DictionaryStatus scanToEnd(PageWordScanner& scanner) {
  DictionaryStatus latest = DictionaryStatus::NotFound;
  while (!scanner.done()) {
    latest = scanner.stepOne();
    if (latest != DictionaryStatus::Found && latest != DictionaryStatus::NotFound) break;
  }
  return latest;
}

DictionaryStatus completeScan(PageWordScanner& scanner, const std::vector<PageTextGlyph>& glyphs,
                              ScannerProbeRecorder& recorder, const uint32_t glyphHash = 0x11223344U,
                              const DictionaryBackendKind backend = DictionaryBackendKind::Japanese) {
  const DictionaryStatus begin = scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), glyphHash},
                                               backend, {&recorder, ScannerProbeRecorder::call});
  return begin == DictionaryStatus::Found ? scanToEnd(scanner) : begin;
}

PageWordScanCacheIdentity scanCacheIdentity(const uint16_t glyphCount,
                                            const DictionaryBackendKind backend = DictionaryBackendKind::Japanese) {
  return {backend, 3, 7, 0x11223344U, UINT64_C(0x1122334455667788), glyphCount};
}

uint32_t testFnv1a(const uint8_t* bytes, const size_t count) {
  uint32_t hash = UINT32_C(2166136261);
  for (size_t index = 0; index < count; ++index) {
    hash ^= bytes[index];
    hash *= UINT32_C(16777619);
  }
  return hash;
}

void putTestLe32(uint8_t* destination, const uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

void repairScanCachePayloadChecksum(std::vector<uint8_t>& bytes) {
  ASSERT_GE(bytes.size(), PageWordScanCache::kHeaderSize);
  putTestLe32(bytes.data() + 28,
              testFnv1a(bytes.data() + PageWordScanCache::kHeaderSize, bytes.size() - PageWordScanCache::kHeaderSize));
}

TEST(PageWordScannerTest, StarDictProbesAndPublishesOneExactSourceToken) {
  std::array<PageTextGlyph, 3> glyphs{{{'c', 0, 4}, {'a', 0, 4}, {'t', 0, 4}}};
  ScannerProbeRecorder recorder{{{"cat"}}};
  PageWordScanner scanner;

  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 7}, DictionaryBackendKind::StarDict,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(recorder.texts, (std::vector<std::string>{"cat"}));
  ASSERT_EQ(scanner.candidateCount(), 1);
  const PageWordCandidate* candidate = scanner.candidate(0);
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(candidate->firstGlyph, 0);
  EXPECT_EQ(candidate->glyphCount, 3);
  EXPECT_EQ(candidate->matchedBytes, 3);
  EXPECT_EQ(candidate->firstPageWord, 4);
  EXPECT_EQ(candidate->lastPageWord, 4);
  EXPECT_TRUE(scanner.done());
}

TEST(PageWordScannerTest, ReportsWhichGlyphsTheProgressiveCursorHasProcessed) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder;
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);

  EXPECT_FALSE(scanner.hasProcessedGlyph(0));
  EXPECT_FALSE(scanner.hasProcessedGlyph(1));
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_TRUE(scanner.hasProcessedGlyph(0));
  EXPECT_FALSE(scanner.hasProcessedGlyph(1));
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_TRUE(scanner.hasProcessedGlyph(1));
}

TEST(PageWordScannerTest, StarDictUsesPageWordsAndEnEmDashesAsExactTokenBoundaries) {
  std::array<PageTextGlyph, 14> glyphs{};
  const std::array<uint32_t, 14> codepoints{'c', 'a', 't', 0x2013, 'd', 'o', 'g', '!', 'b', 'i', 'r', 'd', 0x2014, '?'};
  for (uint16_t i = 0; i < glyphs.size(); ++i) {
    glyphs[i].codepoint = codepoints[i];
    glyphs[i].pageWord = i <= 7 ? 0 : 1;
  }
  ScannerProbeRecorder recorder{{{"cat"}, {"bird"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::StarDict,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);

  EXPECT_EQ(scanToEnd(scanner), DictionaryStatus::NotFound);
  ASSERT_EQ(recorder.texts, (std::vector<std::string>{"cat", "dog!", "bird"}));
  ASSERT_EQ(scanner.candidateCount(), 3);
  EXPECT_EQ(scanner.candidate(0)->firstGlyph, 0);
  EXPECT_EQ(scanner.candidate(1)->firstGlyph, 4);
  EXPECT_EQ(scanner.candidate(2)->firstGlyph, 8);
  EXPECT_EQ(scanner.candidate(1)->glyphCount, 4);
  EXPECT_EQ(scanner.candidate(2)->lastPageWord, 1);
}

TEST_F(JapaneseDictionaryTest, StarDictUnknownRawTokenRemainsSelectableForFullLookupAndSuggestions) {
  writeStarDict({{"cat", "feline"}, {"cot", "bed"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  auto glyphs = makeScannerGlyphs(U"“cut!”");
  for (auto& glyph : glyphs) glyph.pageWord = 7;
  const DictionaryProbeFn probe{&engine, [](void* context, const DictionaryQuery& query, DictionaryProbeResult& out) {
                                  return static_cast<DictionaryEngine*>(context)->probe(query, out);
                                }};
  PageWordScanner scanner;
  ASSERT_EQ(
      scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::StarDict, probe),
      DictionaryStatus::Found);

  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(scanner.candidateCount(), 1);
  ASSERT_NE(scanner.candidate(0), nullptr);
  EXPECT_EQ(scanner.candidate(0)->firstGlyph, 0);
  EXPECT_EQ(scanner.candidate(0)->glyphCount, glyphs.size());
  EXPECT_EQ(scanner.candidate(0)->firstPageWord, 7);
  EXPECT_EQ(scanner.candidate(0)->lastPageWord, 7);

  DictionaryLookupFlow flow;
  flow.beginPage(0, 0, false, 0);
  flow.onScanProgress(scanner.candidateCount(), scanner.completedSuccessfully(), DictionaryStatus::Found);
  const DictionaryLookupFlowCommand lookup = flow.takeCommand();
  ASSERT_EQ(lookup.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(lookup.candidateIndex, 0);

  const std::string raw =
      "\xE2\x80\x9C"
      "cut!"
      "\xE2\x80\x9D";
  DictionaryResult result;
  EXPECT_EQ(engine.lookup({raw}, result), DictionaryStatus::NotFound);
  DictionarySuggestions suggestions;
  ASSERT_EQ(engine.suggest(raw, suggestions), DictionaryStatus::Found);
  EXPECT_TRUE(std::any_of(suggestions.items.begin(), suggestions.items.begin() + suggestions.count,
                          [](const DictionaryOwnedText& item) { return item.view() == "cat"; }));
  engine.close();
}

TEST(PageWordScannerTest, StarDictSkipsSyntheticSeparatorsAndPunctuationOnlyUnits) {
  std::array<PageTextGlyph, 6> glyphs{{{'!', 0, 0},
                                       {' ', 0, PageTextGlyph::kSyntheticPageWord},
                                       {'c', 0, 1},
                                       {'a', 0, 1},
                                       {'t', 0, 1},
                                       {0x3001, 0, 2}}};
  ScannerProbeRecorder recorder{{{"!"}, {"cat"}, {"、"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::StarDict,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);

  scanToEnd(scanner);
  EXPECT_EQ(recorder.texts, (std::vector<std::string>{"cat"}));
  ASSERT_EQ(scanner.candidateCount(), 1);
  EXPECT_EQ(scanner.candidate(0)->firstGlyph, 2);
}

TEST(PageWordScannerTest, JapaneseUsesAnEightCodepointLongestWindowAndSkipsMatchInteriors) {
  auto glyphs = makeScannerGlyphs(U"日本語辞書猫犬鳥魚");
  ScannerProbeRecorder recorder{
      {{"日本語辞書", DictionaryStatus::Found, SIZE_MAX, false, DictIndex::DICT_GRAMMAR, 220}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);

  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(recorder.texts.size(), 1);
  EXPECT_EQ(recorder.texts[0], "日本語辞書猫犬鳥");
  EXPECT_EQ(recorder.modes[0], DictionaryLookupMode::LongestAtOffset);
  ASSERT_EQ(scanner.candidateCount(), 1);
  EXPECT_EQ(scanner.candidate(0)->firstGlyph, 0);
  EXPECT_EQ(scanner.candidate(0)->glyphCount, 5);
  EXPECT_EQ(scanner.candidate(0)->matchedBytes, std::string("日本語辞書").size());
  for (int i = 0; i < 4; ++i) EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_EQ(recorder.texts.size(), 1);
}

TEST(PageWordScannerTest, JapaneseProbeWindowsNeverCrossParagraphBoundaries) {
  auto glyphs = makeScannerGlyphs(U"本猫");
  glyphs[1].paragraph = 1;
  ScannerProbeRecorder recorder{{{"本猫"}, {"本"}, {"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(recorder.texts.size(), 1);
  EXPECT_EQ(recorder.texts[0], "本");
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(recorder.texts.size(), 2);
  EXPECT_EQ(recorder.texts[1], "猫");
}

TEST(PageWordScannerTest, JapaneseFiltersShortAllNoiseExactFragmentsAndConjugationPrefixes) {
  struct Case {
    std::u32string text;
    std::string match;
    bool selected;
  };
  const std::array<Case, 17> cases{{{U"には", "には", false},
                                    {U"とても", "とても", true},
                                    {U"ちゃ", "ちゃ", false},
                                    {U"じゃ", "じゃ", false},
                                    {U"ちゃう", "ちゃう", false},
                                    {U"って", "って", false},
                                    {U"そうに", "そうに", false},
                                    {U"そうな", "そうな", false},
                                    {U"そうだ", "そうだ", false},
                                    {U"ました猫", "ました", false},
                                    {U"ません猫", "ません", false},
                                    {U"でした猫", "でした", false},
                                    {U"です猫", "です", false},
                                    {U"ます猫", "ます", false},
                                    {U"ふんふん", "ふんふん", true},
                                    {U"ところ", "ところ", true},
                                    {U"猫", "猫", true}}};
  for (const auto& test : cases) {
    SCOPED_TRACE(test.match);
    auto glyphs = makeScannerGlyphs(test.text);
    ScannerProbeRecorder recorder{{{test.match}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);
    EXPECT_EQ(scanner.stepOne(), test.selected ? DictionaryStatus::Found : DictionaryStatus::NotFound);
    EXPECT_EQ(scanner.candidateCount(), test.selected ? 1 : 0);
  }
}

TEST(PageWordScannerTest, JapaneseStripsAKanjiFinalCaseParticleOnlyWhenTheStemMatchesFully) {
  const std::array<char32_t, 8> particles{U'の', U'は', U'が', U'を', U'に', U'へ', U'も', U'と'};
  for (const char32_t particle : particles) {
    const std::u32string text = std::u32string(U"東") + particle + U"猫";
    auto glyphs = makeScannerGlyphs(text);
    const std::string matched = particle == U'の'   ? "東の"
                                : particle == U'は' ? "東は"
                                : particle == U'が' ? "東が"
                                : particle == U'を' ? "東を"
                                : particle == U'に' ? "東に"
                                : particle == U'へ' ? "東へ"
                                : particle == U'も' ? "東も"
                                                    : "東と";
    ScannerProbeRecorder recorder{{{matched}, {"東"}, {"猫"}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);

    ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
    ASSERT_EQ(scanner.candidateCount(), 1);
    EXPECT_EQ(scanner.candidate(0)->glyphCount, 1);
    EXPECT_EQ(scanner.candidate(0)->matchedBytes, std::string("東").size());
    ASSERT_GE(recorder.texts.size(), 2);
    EXPECT_EQ(recorder.texts[1], "東");
  }
}

TEST(PageWordScannerTest, JapaneseLetsTheNextPositionWinParticleLedMisSegmentation) {
  auto glyphs = makeScannerGlyphs(U"にどっさり");
  ScannerProbeRecorder recorder{{{"にど"}, {"どっさり"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);

  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_EQ(scanner.candidateCount(), 0);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(scanner.candidateCount(), 1);
  EXPECT_EQ(scanner.candidate(0)->firstGlyph, 1);
  EXPECT_EQ(scanner.candidate(0)->glyphCount, 4);
}

TEST(PageWordScannerTest, JapaneseKeepsLongerGenuineParticleInitialWords) {
  auto glyphs = makeScannerGlyphs(U"とても");
  ScannerProbeRecorder recorder{{{"とても"}, {"ても"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);

  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(scanner.candidateCount(), 1);
  EXPECT_EQ(scanner.candidate(0)->glyphCount, 3);
}

TEST(PageWordScannerTest, JapaneseGroupsAsciiAndFullWidthDigitPrefixesWithCounters) {
  for (const std::u32string& text : {std::u32string(U"2年猫"), std::u32string(U"１５人猫")}) {
    SCOPED_TRACE(testing::PrintToString(text));
    auto glyphs = makeScannerGlyphs(text);
    const std::string counter = text.front() == U'2' ? "年" : "人";
    ScannerProbeRecorder recorder{{{counter}, {"猫"}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);
    ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
    ASSERT_EQ(scanner.candidateCount(), 1);
    EXPECT_EQ(scanner.candidate(0)->firstGlyph, 0);
    EXPECT_EQ(scanner.candidate(0)->glyphCount, text.front() == U'2' ? 2 : 3);
    EXPECT_EQ(scanner.candidate(0)->matchedBytes, counter.size());
    EXPECT_EQ(scanner.candidate(0)->firstPageWord, 0);
    EXPECT_EQ(scanner.candidate(0)->lastPageWord, text.front() == U'2' ? 1 : 2);
  }
}

TEST(PageWordScannerTest, JapaneseDoesNotProbeADigitRunWithoutACounter) {
  auto glyphs = makeScannerGlyphs(U"１５");
  ScannerProbeRecorder recorder{{{"人"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_TRUE(recorder.texts.empty());
}

TEST(PageWordScannerTest, JapaneseDisplayNoiseFiltersStartAtTheOriginalDigitPrefix) {
  auto glyphs = makeScannerGlyphs(U"2ます");
  ScannerProbeRecorder recorder{{{"ます"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(scanner.candidateCount(), 1);
  EXPECT_EQ(scanner.candidate(0)->glyphCount, 3);
}

TEST(PageWordScannerTest, JapaneseSuppressesSmallKanaStartsExceptUntransformedSokuonTe) {
  const std::array<char32_t, 19> suppressed{U'ぁ', U'ぃ', U'ぅ', U'ぇ', U'ぉ', U'ゃ', U'ゅ', U'ょ', U'ゎ', U'ァ',
                                            U'ィ', U'ゥ', U'ェ', U'ォ', U'ッ', U'ャ', U'ュ', U'ョ', U'ヮ'};
  for (const char32_t smallKana : suppressed) {
    auto glyphs = makeScannerGlyphs(std::u32string(1, smallKana) + U"猫");
    ScannerProbeRecorder recorder{{{"ゃ"}, {"猫"}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);
    EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
    EXPECT_TRUE(recorder.texts.empty());
  }
  for (const bool transformed : {false, true}) {
    auto glyphs = makeScannerGlyphs(U"っていう");
    ScannerProbeRecorder recorder{{{"っていう", DictionaryStatus::Found, SIZE_MAX, transformed}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);
    EXPECT_EQ(scanner.stepOne(), transformed ? DictionaryStatus::NotFound : DictionaryStatus::Found);
    EXPECT_EQ(scanner.candidateCount(), transformed ? 0 : 1);
  }
}

TEST(PageWordScannerTest, JapaneseSuppressesOnlyLowPriorityUntransformedReadingCollisions) {
  for (const auto [priority, transformed, posFlags, selected] :
       {std::tuple<uint8_t, bool, uint8_t, bool>{199, false, DictIndexRecord::POS_READING, false},
        {200, false, DictIndexRecord::POS_READING, true},
        {1, true, DictIndexRecord::POS_READING, true},
        {1, false, 0, true}}) {
    auto glyphs = makeScannerGlyphs(U"きょう");
    ScannerProbeRecorder recorder{
        {{"きょう", DictionaryStatus::Found, SIZE_MAX, transformed, DictIndex::DICT_JMDICT, priority, posFlags}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);
    EXPECT_EQ(scanner.stepOne(), selected ? DictionaryStatus::Found : DictionaryStatus::NotFound);
    EXPECT_EQ(scanner.candidateCount(), selected ? 1 : 0);
  }
}

TEST(PageWordScannerTest, JapaneseGroupsFictionalKatakanaNameRunAndHonorific) {
  const std::array<std::u32string, 6> honorifics{U"さん", U"さま", U"くん", U"ちゃん", U"様", U"氏"};
  for (const auto& honorific : honorifics) {
    auto glyphs = makeScannerGlyphs(std::u32string(U"ムーミン") + honorific + U"猫");
    ScannerProbeRecorder recorder{{{"ムー"}, {"猫"}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);

    ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
    ASSERT_EQ(scanner.candidateCount(), 1);
    EXPECT_EQ(scanner.candidate(0)->glyphCount, 4);
    EXPECT_EQ(scanner.candidate(0)->matchedBytes, std::string("ムー").size());
  }
}

TEST(PageWordScannerTest, JapaneseExtendsEveryPinnedHonorificWithoutScanningItsInterior) {
  const std::array<std::u32string, 9> honorifics{U"さん", U"さま", U"くん", U"ちゃん", U"様",
                                                 U"氏",   U"士",   U"師",   U"員"};
  for (const auto& honorific : honorifics) {
    SCOPED_TRACE(testing::PrintToString(honorific));
    const std::u32string text = std::u32string(U"太郎") + honorific + U"猫";
    auto glyphs = makeScannerGlyphs(text);
    ScannerProbeRecorder recorder{{{"太郎"}, {"猫"}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);
    ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
    ASSERT_EQ(scanner.candidateCount(), 1);
    EXPECT_EQ(scanner.candidate(0)->glyphCount, 2);
    EXPECT_EQ(scanner.candidate(0)->lastPageWord, 1);
    for (size_t i = 1; i < 2 + honorific.size(); ++i) EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
    EXPECT_EQ(recorder.texts.size(), 1);
    EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Found);
    EXPECT_EQ(recorder.texts.size(), 2);
  }
}

TEST(PageWordScannerTest, JapaneseDoesNotApplyCjkOccupationHonorificSkippingToKatakanaNames) {
  auto glyphs = makeScannerGlyphs(U"タロウ氏族");
  ScannerProbeRecorder recorder{{{"タロウ"}, {"族"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  // 氏 is a universal honorific and is skipped; this assertion protects the
  // separately CJK-only 士/師/員 rule below by using 士 in a second scan.
  auto occupationGlyphs = makeScannerGlyphs(U"タロウ士族");
  ScannerProbeRecorder occupationRecorder{{{"タロウ"}, {"士"}, {"族"}}};
  ASSERT_EQ(scanner.begin({occupationGlyphs.data(), static_cast<uint16_t>(occupationGlyphs.size()), 1},
                          DictionaryBackendKind::Japanese, {&occupationRecorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  for (int i = 0; i < 2; ++i) EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  EXPECT_EQ(scanner.candidate(1)->firstGlyph, 3);
}

TEST(PageWordScannerTest, FilteredMatchesStillSkipTheirInteriors) {
  auto glyphs = makeScannerGlyphs(U"ちゃ猫");
  ScannerProbeRecorder recorder{{{"ちゃ"}, {"ゃ"}, {"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  ASSERT_EQ(recorder.texts.size(), 1);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_EQ(recorder.texts.size(), 1);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Found);
}

TEST(PageWordScannerTest, ScannerDoesNotAdvanceUntilCallerExplicitlySteps) {
  auto glyphs = makeScannerGlyphs(U"本猫");
  ScannerProbeRecorder recorder{{{"本"}, {"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  // The future activity can withhold stepOne() while the definition worker owns
  // the engine; accessors and begin() perform no hidden scanning.
  EXPECT_FALSE(scanner.done());
  EXPECT_EQ(scanner.candidateCount(), 0);
  EXPECT_EQ(scanner.candidate(0), nullptr);
  EXPECT_TRUE(recorder.texts.empty());
}

TEST(PageWordScannerTest, PublishesPartialAppendOnlyResultsAndSignalsTemporaryEndUntilDone) {
  auto glyphs = makeScannerGlyphs(U"本猫");
  ScannerProbeRecorder recorder{{{"本"}, {"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);

  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  const PageWordCandidate* first = scanner.candidate(0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(scanner.candidate(1), nullptr);
  EXPECT_FALSE(scanner.done());  // Caller must not cycle at this temporary end.
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  EXPECT_TRUE(scanner.done());
  EXPECT_EQ(scanner.candidateCount(), 2);
  EXPECT_EQ(scanner.candidate(0), first);
  EXPECT_EQ(first->firstGlyph, 0);
}

TEST(PageWordScannerTest, CallbackCancellationAndErrorsStopWithoutPublishing) {
  for (const DictionaryBackendKind backend : {DictionaryBackendKind::StarDict, DictionaryBackendKind::Japanese}) {
    for (const DictionaryStatus status : {DictionaryStatus::Cancelled, DictionaryStatus::Unavailable,
                                          DictionaryStatus::ReadError, DictionaryStatus::OutOfMemory}) {
      auto glyphs = makeScannerGlyphs(backend == DictionaryBackendKind::StarDict ? U"cat" : U"猫");
      if (backend == DictionaryBackendKind::StarDict) {
        for (auto& glyph : glyphs) glyph.pageWord = 0;
      }
      ScannerProbeRecorder recorder{{{backend == DictionaryBackendKind::StarDict ? "cat" : "猫", status}}};
      PageWordScanner scanner;
      ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, backend,
                              {&recorder, ScannerProbeRecorder::call}),
                DictionaryStatus::Found);
      EXPECT_EQ(scanner.stepOne(), status);
      EXPECT_EQ(scanner.candidateCount(), 0);
    }
  }
}

TEST(PageWordScannerTest, PropagatesCancellationWhenAtomicProbeLeavesOutputUnpublished) {
  auto cancelledWithoutPublish = [](void*, const DictionaryQuery&, DictionaryProbeResult&) {
    return DictionaryStatus::Cancelled;
  };
  for (const DictionaryBackendKind backend : {DictionaryBackendKind::StarDict, DictionaryBackendKind::Japanese}) {
    auto glyphs = makeScannerGlyphs(backend == DictionaryBackendKind::StarDict ? U"cat" : U"猫");
    if (backend == DictionaryBackendKind::StarDict) {
      for (auto& glyph : glyphs) glyph.pageWord = 0;
    }
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, backend,
                            {nullptr, cancelledWithoutPublish}),
              DictionaryStatus::Found);
    EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Cancelled);
    EXPECT_EQ(scanner.candidateCount(), 0);
  }
}

TEST(PageWordScannerTest, RejectsZeroOutOfRangeAndMidUtf8ProbeLengthsBeforePublishing) {
  const std::array<size_t, 3> invalidLengths{0, 1, 7};
  for (const size_t invalid : invalidLengths) {
    auto glyphs = makeScannerGlyphs(U"日本");
    ScannerProbeRecorder recorder{{{"日本", DictionaryStatus::Found, invalid}}};
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                            {&recorder, ScannerProbeRecorder::call}),
              DictionaryStatus::Found);
    EXPECT_EQ(scanner.stepOne(), DictionaryStatus::ReadError);
    EXPECT_EQ(scanner.candidateCount(), 0);
  }
}

TEST(PageWordScannerTest, RejectsCallbackStatusDisagreementBeforePublishing) {
  auto mismatch = [](void*, const DictionaryQuery&, DictionaryProbeResult& out) {
    out = {};
    out.status = DictionaryStatus::NotFound;
    out.matchedBytes = 3;
    return DictionaryStatus::Found;
  };
  for (const DictionaryBackendKind backend : {DictionaryBackendKind::StarDict, DictionaryBackendKind::Japanese}) {
    auto glyphs = makeScannerGlyphs(backend == DictionaryBackendKind::StarDict ? U"cat" : U"猫");
    if (backend == DictionaryBackendKind::StarDict) {
      for (auto& glyph : glyphs) glyph.pageWord = 0;
    }
    PageWordScanner scanner;
    ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, backend, {nullptr, mismatch}),
              DictionaryStatus::Found);
    EXPECT_EQ(scanner.stepOne(), DictionaryStatus::ReadError);
    EXPECT_EQ(scanner.candidateCount(), 0);
  }
}

TEST(PageWordScannerTest, CandidateFieldOverflowStopsWithoutWritingPastFallbackStorage) {
  std::u32string text(256, U'1');
  text.push_back(U'年');
  auto glyphs = makeScannerGlyphs(text);
  ScannerProbeRecorder recorder{{{"年"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::ReadError);
  EXPECT_EQ(scanner.candidateCount(), 0);
}

TEST(PageWordScannerTest, AllocationFallbackExhaustionAndRestartRemainBounded) {
  auto glyphs = makeScannerGlyphs(std::u32string(300, U'猫'));
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  dict_memory_test::reset();
  dict_memory_test::rejectedRequest = 1;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_TRUE(scanner.truncated());
  ASSERT_EQ(dict_memory_test::requests[0], 300U * sizeof(PageWordCandidate));
  ASSERT_EQ(dict_memory_test::requests[1], 256U * sizeof(PageWordCandidate));
  for (size_t i = 0; i < 256; ++i) ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::OutOfMemory);
  EXPECT_TRUE(scanner.done());
  EXPECT_TRUE(scanner.truncated());
  EXPECT_EQ(scanner.candidateCount(), 256);

  dict_memory_test::reset();
  scanner.restart();
  EXPECT_FALSE(scanner.truncated());
  EXPECT_FALSE(scanner.done());
  ASSERT_EQ(dict_memory_test::requests[0], 300U * sizeof(PageWordCandidate));
  EXPECT_EQ(scanToEnd(scanner), DictionaryStatus::Found);
  EXPECT_EQ(scanner.candidateCount(), 300);
}

TEST(PageWordScannerTest, InitialFallbackInvokesOneRecoveryAndRetriesFullCapacity) {
  auto glyphs = makeScannerGlyphs(std::u32string(300, U'猫'));
  ScannerProbeRecorder recorder{{{"猫"}}};
  struct Recovery {
    uint8_t calls = 0;
    static void release(void* context) { ++static_cast<Recovery*>(context)->calls; }
  } recovery;
  PageWordScanner scanner;
  dict_memory_test::reset();
  dict_memory_test::rejectedRequest = 1;

  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}, {&recovery, Recovery::release}),
            DictionaryStatus::Found);
  EXPECT_EQ(recovery.calls, 1);
  EXPECT_FALSE(scanner.truncated());
  ASSERT_EQ(dict_memory_test::requestCount, 3u);
  EXPECT_EQ(dict_memory_test::requests[2], 300U * sizeof(PageWordCandidate));
}

TEST(PageWordScannerTest, PersistentFallbackEndsAsPartialOutOfMemoryEvenWithoutCandidateExhaustion) {
  auto glyphs = makeScannerGlyphs(std::u32string(300, U'猫'));
  ScannerProbeRecorder recorder;
  struct Recovery {
    uint8_t calls = 0;
    static void release(void* context) { ++static_cast<Recovery*>(context)->calls; }
  } recovery;
  PageWordScanner scanner;
  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = glyphs.size() * sizeof(PageWordCandidate);

  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}, {&recovery, Recovery::release}),
            DictionaryStatus::Found);
  ASSERT_TRUE(scanner.truncated());
  EXPECT_EQ(recovery.calls, 1);
  EXPECT_EQ(scanToEnd(scanner), DictionaryStatus::OutOfMemory);
  EXPECT_TRUE(scanner.done());
  EXPECT_FALSE(scanner.completedSuccessfully());
  EXPECT_EQ(scanner.candidateCount(), 0);
}

TEST(PageWordScannerTest, AllocationFailurePublishesNoBorrowedStateAndClearAllowsRebegin) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;
  EXPECT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::OutOfMemory);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Unavailable);
  EXPECT_EQ(scanner.candidateCount(), 0);

  dict_memory_test::reset();
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  scanner.clear();
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Unavailable);
  EXPECT_EQ(scanner.candidate(0), nullptr);
  EXPECT_FALSE(scanner.done());

  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 1}, DictionaryBackendKind::Japanese,
                          {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::Found);
}

TEST(PageWordScannerTest, EmptySourceBeginsFoundAndAlreadyDone) {
  ScannerProbeRecorder recorder;
  PageWordScanner scanner;
  EXPECT_EQ(scanner.begin({}, DictionaryBackendKind::Japanese, {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  EXPECT_TRUE(scanner.done());
  EXPECT_TRUE(scanner.completedSuccessfully());
  EXPECT_EQ(scanner.stepOne(), DictionaryStatus::NotFound);
  EXPECT_TRUE(recorder.texts.empty());
}

TEST(PageWordScannerTest, LogsBoundedDensePageArrayBudgets) {
  constexpr size_t denseGlyphs = (800 / 8) * (480 / 8);  // Synthetic one 8x8 glyph per display cell.
  constexpr size_t glyphBytes = denseGlyphs * sizeof(PageTextGlyph);
  constexpr size_t candidateBytes = denseGlyphs * sizeof(PageWordCandidate);
  std::cout << "Synthetic dense 800x480 (8x8 cells): glyph-array=" << glyphBytes
            << " bytes, candidate-array=" << candidateBytes
            << " bytes; both are single fallible activity-lifetime allocations (candidate fallback=2048 bytes).\n";
  EXPECT_EQ(denseGlyphs, 6000);
  EXPECT_EQ(glyphBytes, 96000);
  EXPECT_EQ(candidateBytes, 48000);
}

TEST_F(JapaneseDictionaryTest, JapaneseProbePreservesVocabGrammarNamesOrderingAndFilterMetadata) {
  writeVocab({{"共通", "vocab", 170, DictIndexRecord::POS_READING}, {"必須", "required", 1, 0}});
  writeSource("/dictionaries/jp/grammar", {{"共通", "grammar", 250, DictIndexRecord::POS_OTHER},
                                           {"文法", "grammar", 180, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/names", {{"共通", "name", 255, DictIndexRecord::POS_OTHER},
                                         {"文法", "name", 240, DictIndexRecord::POS_OTHER},
                                         {"太郎", "name", 160, DictIndexRecord::POS_OTHER}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);

  DictionaryProbeResult probe;
  ASSERT_EQ(engine.probe({"共通", 0, DictionaryLookupMode::LongestAtOffset}, probe), DictionaryStatus::Found);
  EXPECT_EQ(probe.sourceMask, DictIndex::DICT_JMDICT);
  EXPECT_EQ(probe.priority, 170);
  EXPECT_EQ(probe.posFlags, DictIndexRecord::POS_READING);
  ASSERT_EQ(engine.probe({"文法", 0, DictionaryLookupMode::LongestAtOffset}, probe), DictionaryStatus::Found);
  EXPECT_EQ(probe.sourceMask, DictIndex::DICT_GRAMMAR);
  EXPECT_EQ(probe.priority, 180);
  ASSERT_EQ(engine.probe({"太郎", 0, DictionaryLookupMode::LongestAtOffset}, probe), DictionaryStatus::Found);
  EXPECT_EQ(probe.sourceMask, DictIndex::DICT_NAMES);
  EXPECT_EQ(probe.priority, 160);
}

TEST_F(JapaneseDictionaryTest, StarDictProbeLeavesJapaneseOnlyFilterMetadataNeutral) {
  writeStarDict({{"cat", "feline"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryProbeResult probe;
  ASSERT_EQ(engine.probe({"cat", 0, DictionaryLookupMode::Token}, probe), DictionaryStatus::Found);
  EXPECT_EQ(probe.priority, 0);
  EXPECT_EQ(probe.posFlags, 0);
}

TEST_F(JapaneseDictionaryTest, ScanCacheSaveWritesTheFrozenLittleEndianV3File) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 0x11223344U},
                          DictionaryBackendKind::Japanese, {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_EQ(scanToEnd(scanner), DictionaryStatus::Found);
  ASSERT_TRUE(scanner.done());

  const PageWordScanCacheIdentity identity{
      DictionaryBackendKind::Japanese,     3, 7, 0x11223344U, UINT64_C(0x1122334455667788),
      static_cast<uint16_t>(glyphs.size())};
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 1));

  const std::vector<uint8_t> expected{
      0x43, 0x57, 0x4c, 0x53, 0x03, 0x01, 0x01, 0x00, 0x03, 0x00, 0x07, 0x00, 0x44, 0x33, 0x22, 0x11,
      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x02, 0x00, 0x01, 0x00, 0x8c, 0x18, 0x5c, 0x2b,
      0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x03, 0x01, 0x00, 0x01, 0x00,
  };
  std::ifstream input(resolve("/cache/wlscan.bin"), std::ios::binary);
  const std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  EXPECT_EQ(actual, expected);
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadPublishesTheWholeValidatedCandidateArray) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 1));

  PageWordScanCache loaded;
  ASSERT_TRUE(loaded.load("/cache/wlscan.bin", identity));
  ASSERT_EQ(loaded.candidateCount(), 2);
  EXPECT_EQ(loaded.cursor(), 1);
  ASSERT_NE(loaded.candidate(0), nullptr);
  EXPECT_EQ(loaded.candidate(0)->firstGlyph, 0);
  EXPECT_EQ(loaded.candidate(0)->glyphCount, 1);
  EXPECT_EQ(loaded.candidate(0)->matchedBytes, 3);
  EXPECT_EQ(loaded.candidate(0)->firstPageWord, 0);
  EXPECT_EQ(loaded.candidate(0)->lastPageWord, 0);
  ASSERT_NE(loaded.candidate(1), nullptr);
  EXPECT_EQ(loaded.candidate(1)->firstGlyph, 1);
  EXPECT_EQ(loaded.candidate(1)->firstPageWord, 1);
  EXPECT_EQ(loaded.candidate(2), nullptr);
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadRejectsEverySerializedIdentityMismatch) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 0));

  std::array<PageWordScanCacheIdentity, 4> mismatches{identity, identity, identity, identity};
  mismatches[0].backend = DictionaryBackendKind::StarDict;
  mismatches[1].glyphHash ^= 1;
  mismatches[2].spine++;
  mismatches[3].page++;
  for (size_t index = 0; index < mismatches.size(); ++index) {
    SCOPED_TRACE(index);
    PageWordScanCache loaded;
    EXPECT_FALSE(loaded.load("/cache/wlscan.bin", mismatches[index]));
    EXPECT_EQ(loaded.candidateCount(), 0);
  }
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadRejectsSameSizedDictionaryReplacementBySignature) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 0));

  identity.dictionarySignature ^= UINT64_C(0x8000000000000000);
  PageWordScanCache loaded;
  EXPECT_FALSE(loaded.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(loaded.candidateCount(), 0);
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadRejectsWrongMagicVersionAndFlags) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 0));
  const auto valid = readBytes(resolve("/cache/wlscan.bin"));

  auto versionTwo = valid;
  versionTwo[4] = 2;
  versionTwo[5] = 0;
  writeBytes(resolve("/cache/wlscan.bin"), versionTwo);
  PageWordScanCache oldVersion;
  EXPECT_FALSE(oldVersion.load("/cache/wlscan.bin", identity));

  for (const size_t byte : {size_t{0}, size_t{4}, size_t{6}}) {
    SCOPED_TRACE(byte);
    auto damaged = valid;
    damaged[byte] ^= 0x80;
    writeBytes(resolve("/cache/wlscan.bin"), damaged);
    PageWordScanCache loaded;
    EXPECT_FALSE(loaded.load("/cache/wlscan.bin", identity));
  }
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadRejectsShortHeaderPayloadTrailingBytesAndChecksumDamage) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 0));
  const auto valid = readBytes(resolve("/cache/wlscan.bin"));

  std::vector<std::vector<uint8_t>> damaged;
  damaged.emplace_back(valid.begin(), valid.begin() + 31);
  damaged.emplace_back(valid.begin(), valid.end() - 1);
  damaged.push_back(valid);
  damaged.back().push_back(0);
  damaged.push_back(valid);
  damaged.back()[28] ^= 1;
  for (size_t index = 0; index < damaged.size(); ++index) {
    SCOPED_TRACE(index);
    writeBytes(resolve("/cache/wlscan.bin"), damaged[index]);
    PageWordScanCache loaded;
    EXPECT_FALSE(loaded.load("/cache/wlscan.bin", identity));
    EXPECT_EQ(loaded.candidateCount(), 0);
  }
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadRejectsImpossibleCandidateCountBeforeAllocating) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 0));
  auto damaged = readBytes(resolve("/cache/wlscan.bin"));
  damaged[24] = 3;
  damaged[25] = 0;
  writeBytes(resolve("/cache/wlscan.bin"), damaged);

  const size_t requestsBefore = dict_memory_test::requestCount;
  PageWordScanCache loaded;
  EXPECT_FALSE(loaded.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(dict_memory_test::requestCount, requestsBefore + 1);  // Path scratch only, no candidate array.
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadRejectsInvalidAndNonmonotonicCandidatesWithValidChecksums) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 0));
  const auto valid = readBytes(resolve("/cache/wlscan.bin"));

  struct Mutation {
    size_t byte;
    uint8_t value;
  };
  const std::vector<std::vector<Mutation>> mutations{
      {{34, 0}},                             // Zero glyph count.
      {{35, 0}},                             // Zero matched byte length.
      {{35, 5}},                             // UTF-8 match exceeds one glyph's maximum byte length.
      {{32, 2}},                             // First glyph outside the current source.
      {{40, 0}},                             // Second firstGlyph no longer strictly increases.
      {{36, 0xff}, {37, 0xff}},              // Synthetic first page-word sentinel.
      {{36, 1}},                             // First page-word exceeds last page-word.
      {{36, 1}, {38, 1}, {44, 0}, {46, 0}},  // Second page-word range regresses.
  };
  for (size_t index = 0; index < mutations.size(); ++index) {
    SCOPED_TRACE(index);
    auto damaged = valid;
    for (const auto mutation : mutations[index]) damaged[mutation.byte] = mutation.value;
    repairScanCachePayloadChecksum(damaged);
    writeBytes(resolve("/cache/wlscan.bin"), damaged);
    PageWordScanCache loaded;
    EXPECT_FALSE(loaded.load("/cache/wlscan.bin", identity));
    EXPECT_EQ(loaded.candidateCount(), 0);
  }
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadClampsInvalidCursorButPreservesValidCursor) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 1));

  PageWordScanCache loaded;
  ASSERT_TRUE(loaded.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(loaded.cursor(), 1);

  auto bytes = readBytes(resolve("/cache/wlscan.bin"));
  bytes[26] = 2;
  bytes[27] = 0;
  writeBytes(resolve("/cache/wlscan.bin"), bytes);
  ASSERT_TRUE(loaded.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(loaded.cursor(), 0);
  EXPECT_EQ(loaded.candidateCount(), 2);
}

TEST_F(JapaneseDictionaryTest, LoadedScanCacheCanAtomicallyUpdateItsRestoredCursor) {
  const std::vector<PageTextGlyph> glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder probe{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, probe), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 0));

  PageWordScanCache loaded;
  ASSERT_TRUE(loaded.load("/cache/wlscan.bin", identity));
  ASSERT_EQ(loaded.candidateCount(), 2u);
  ASSERT_TRUE(loaded.saveLoaded("/cache/wlscan.bin", identity, 1));

  PageWordScanCache reopened;
  ASSERT_TRUE(reopened.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(reopened.cursor(), 1u);
}

TEST_F(JapaneseDictionaryTest, ScanCacheSupportsAnEmptyCompleteScanWithoutAllocatingCandidates) {
  ScannerProbeRecorder recorder;
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({}, DictionaryBackendKind::Japanese, {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_TRUE(scanner.done());
  const PageWordScanCacheIdentity identity = scanCacheIdentity(0);
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 9));

  dict_memory_test::reset();
  PageWordScanCache loaded;
  ASSERT_TRUE(loaded.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(loaded.candidateCount(), 0);
  EXPECT_EQ(loaded.cursor(), 0);
  EXPECT_EQ(dict_memory_test::requestCount, 1);  // Path scratch only.
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadOomPublishesNoCandidates) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 1));

  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = 2 * sizeof(PageWordCandidate);
  PageWordScanCache loaded;
  EXPECT_FALSE(loaded.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(loaded.candidateCount(), 0);
  EXPECT_EQ(loaded.cursor(), 0);
  EXPECT_EQ(loaded.candidate(0), nullptr);
}

TEST_F(JapaneseDictionaryTest, FailedScanCacheReloadClearsPreviouslyPublishedState) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(static_cast<uint16_t>(glyphs.size()));
  PageWordScanCache writer;
  ASSERT_TRUE(writer.save("/cache/wlscan.bin", identity, scanner, 1));
  PageWordScanCache loaded;
  ASSERT_TRUE(loaded.load("/cache/wlscan.bin", identity));
  ASSERT_EQ(loaded.candidateCount(), 2);

  auto bytes = readBytes(resolve("/cache/wlscan.bin"));
  bytes.pop_back();
  writeBytes(resolve("/cache/wlscan.bin"), bytes);
  EXPECT_FALSE(loaded.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(loaded.candidateCount(), 0);
  EXPECT_EQ(loaded.cursor(), 0);
  EXPECT_EQ(loaded.candidate(0), nullptr);
}

TEST_F(JapaneseDictionaryTest, ScanCacheSaveRefusesIncompleteAndLowMemoryTruncatedScans) {
  auto incompleteGlyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder incompleteRecorder{{{"猫"}}};
  PageWordScanner incomplete;
  ASSERT_EQ(incomplete.begin({incompleteGlyphs.data(), 1, 0x11223344U}, DictionaryBackendKind::Japanese,
                             {&incompleteRecorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_FALSE(incomplete.done());
  PageWordScanCache cache;
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", scanCacheIdentity(1), incomplete, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));

  auto denseGlyphs = makeScannerGlyphs(std::u32string(300, U'猫'));
  ScannerProbeRecorder denseRecorder{{{"猫"}}};
  PageWordScanner truncated;
  dict_memory_test::rejectedBytes = denseGlyphs.size() * sizeof(PageWordCandidate);
  ASSERT_EQ(truncated.begin({denseGlyphs.data(), static_cast<uint16_t>(denseGlyphs.size()), 0x11223344U},
                            DictionaryBackendKind::Japanese, {&denseRecorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  EXPECT_EQ(scanToEnd(truncated), DictionaryStatus::OutOfMemory);
  EXPECT_TRUE(truncated.done());
  EXPECT_TRUE(truncated.truncated());
  EXPECT_FALSE(
      cache.save("/cache/wlscan.bin", scanCacheIdentity(static_cast<uint16_t>(denseGlyphs.size())), truncated, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
}

TEST_F(JapaneseDictionaryTest, ScanCacheSaveRefusesDoneScannerAfterTerminalProbeError) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬", DictionaryStatus::ReadError}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 0x11223344U},
                          DictionaryBackendKind::Japanese, {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(scanner.candidateCount(), 1);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::ReadError);
  ASSERT_TRUE(scanner.done());
  ASSERT_FALSE(scanner.completedSuccessfully());
  ASSERT_FALSE(scanner.truncated());

  PageWordScanCache cache;
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", scanCacheIdentity(static_cast<uint16_t>(glyphs.size())), scanner, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
}

TEST(PageWordScannerTest, RestartClearsPriorTerminalFailureAndCanCompleteNaturally) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬", DictionaryStatus::ReadError}}};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 0x11223344U},
                          DictionaryBackendKind::Japanese, {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::Found);
  ASSERT_EQ(scanner.stepOne(), DictionaryStatus::ReadError);
  ASSERT_FALSE(scanner.completedSuccessfully());

  recorder.rules = {{"猫"}, {"犬"}};
  scanner.restart();
  EXPECT_FALSE(scanner.done());
  EXPECT_FALSE(scanner.completedSuccessfully());
  EXPECT_FALSE(scanner.truncated());
  EXPECT_EQ(scanner.candidateCount(), 0);
  ASSERT_EQ(scanToEnd(scanner), DictionaryStatus::Found);
  EXPECT_TRUE(scanner.done());
  EXPECT_TRUE(scanner.completedSuccessfully());
  EXPECT_EQ(scanner.candidateCount(), 2);
}

TEST_F(JapaneseDictionaryTest, ScanCacheOpenFailuresAreRecoverableMissesAndNeverPublishTempData) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);

  hal_storage_test::writeOpenFailurePath = "/cache/wlscan.bin.tmp";
  hal_storage_test::writeOpenFailureCreatesFile = true;
  PageWordScanCache cache;
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));

  hal_storage_test::reset();
  hal_storage_test::readOpenFailurePath = "/cache/wlscan.bin.tmp";
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));

  hal_storage_test::reset();
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  hal_storage_test::readOpenFailurePath = "/cache/wlscan.bin";
  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 0);
}

TEST_F(JapaneseDictionaryTest, ScanCacheShortReadReturnsAMissWithoutPartialPublication) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder recorder{{{"猫"}, {"犬"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(2);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 1));

  hal_storage_test::shortReadPath = "/cache/wlscan.bin";
  hal_storage_test::shortReadOffset = PageWordScanCache::kHeaderSize;
  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 0);
  EXPECT_EQ(cache.cursor(), 0);
}

TEST_F(JapaneseDictionaryTest, ScanCacheShortWriteAndCloseFailuresPreserveThePreviousValidCache) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder oldRecorder{{{"猫"}}};
  PageWordScanner oldScanner;
  ASSERT_EQ(completeScan(oldScanner, glyphs, oldRecorder), DictionaryStatus::NotFound);
  ASSERT_TRUE(oldScanner.done());
  ASSERT_EQ(oldScanner.candidateCount(), 1);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(2);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, oldScanner, 0));

  ScannerProbeRecorder newRecorder{{{"猫"}, {"犬"}}};
  PageWordScanner newScanner;
  ASSERT_EQ(completeScan(newScanner, glyphs, newRecorder), DictionaryStatus::Found);

  hal_storage_test::shortWritePath = "/cache/wlscan.bin.tmp";
  hal_storage_test::shortWriteOffset = PageWordScanCache::kHeaderSize;
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, newScanner, 1));
  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);

  hal_storage_test::closeFailurePath = "/cache/wlscan.bin.tmp";
  hal_storage_test::closeFailureOrdinal = 1;
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, newScanner, 1));
  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadIgnoresInterruptedTempAndRecoversPreservedBackup) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 0));

  writeBytes(resolve("/cache/wlscan.bin.tmp"), {0xde, 0xad});
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));

  std::filesystem::rename(resolve("/cache/wlscan.bin"), resolve("/cache/wlscan.bin.bak"));
  writeBytes(resolve("/cache/wlscan.bin.tmp"), {0xbe, 0xef});
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));
}

TEST_F(JapaneseDictionaryTest, ScanCachePromotionFailureRestoresThePreviousValidCache) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder oldRecorder{{{"猫"}}};
  PageWordScanner oldScanner;
  ASSERT_EQ(completeScan(oldScanner, glyphs, oldRecorder), DictionaryStatus::NotFound);
  ASSERT_TRUE(oldScanner.done());
  ASSERT_EQ(oldScanner.candidateCount(), 1);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(2);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, oldScanner, 0));

  ScannerProbeRecorder newRecorder{{{"猫"}, {"犬"}}};
  PageWordScanner newScanner;
  ASSERT_EQ(completeScan(newScanner, glyphs, newRecorder), DictionaryStatus::Found);
  hal_storage_test::renameFailures.push_back({"/cache/wlscan.bin.tmp", "/cache/wlscan.bin"});
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, newScanner, 1));

  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
  EXPECT_EQ(cache.cursor(), 0);
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));
}

TEST_F(JapaneseDictionaryTest, ScanCachePreserveFailureNeverDeletesTheOnlyKnownGoodFinal) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder oldRecorder{{{"猫"}}};
  PageWordScanner oldScanner;
  ASSERT_EQ(completeScan(oldScanner, glyphs, oldRecorder), DictionaryStatus::NotFound);
  ASSERT_TRUE(oldScanner.done());
  ASSERT_EQ(oldScanner.candidateCount(), 1);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(2);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, oldScanner, 0));

  ScannerProbeRecorder newRecorder{{{"猫"}, {"犬"}}};
  PageWordScanner newScanner;
  ASSERT_EQ(completeScan(newScanner, glyphs, newRecorder), DictionaryStatus::Found);
  hal_storage_test::renameFailures.push_back({"/cache/wlscan.bin", "/cache/wlscan.bin.bak"});
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, newScanner, 1));

  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));
}

TEST_F(JapaneseDictionaryTest, ScanCacheStaleTempRemovalFailureIsIgnoredOnLoadButStopsSave) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  writeBytes(resolve("/cache/wlscan.bin.tmp"), {0xde, 0xad});

  hal_storage_test::removeFailurePath = "/cache/wlscan.bin.tmp";
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
}

TEST_F(JapaneseDictionaryTest, ScanCachePathAndCandidateAllocationFailuresAreRecoverable) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);
  PageWordScanCache cache;

  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = 44;  // Two 22-byte sibling paths in one allocation.
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 0);

  dict_memory_test::reset();
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  dict_memory_test::reset();
  dict_memory_test::rejectedBytes = sizeof(PageWordCandidate);
  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 0);
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadCloseFailureClearsOtherwiseValidTemporaryState) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 0));

  hal_storage_test::closeFailurePath = "/cache/wlscan.bin";
  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 0);
  EXPECT_EQ(cache.cursor(), 0);
}

TEST_F(JapaneseDictionaryTest, ScanCacheTempValidationCloseFailureDoesNotReplaceTheOldFinal) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder oldRecorder{{{"猫"}}};
  PageWordScanner oldScanner;
  ASSERT_EQ(completeScan(oldScanner, glyphs, oldRecorder), DictionaryStatus::NotFound);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(2);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, oldScanner, 0));

  ScannerProbeRecorder newRecorder{{{"猫"}, {"犬"}}};
  PageWordScanner newScanner;
  ASSERT_EQ(completeScan(newScanner, glyphs, newRecorder), DictionaryStatus::Found);
  hal_storage_test::closeFailurePath = "/cache/wlscan.bin.tmp";
  hal_storage_test::closeFailureOrdinal = 2;
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, newScanner, 1));

  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));
}

TEST_F(JapaneseDictionaryTest, ScanCacheLoadUsesFinalWhenStaleBackupCleanupFails) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  std::filesystem::copy_file(resolve("/cache/wlscan.bin"), resolve("/cache/wlscan.bin.bak"));

  hal_storage_test::removeFailurePath = "/cache/wlscan.bin.bak";
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));

  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));
}

TEST_F(JapaneseDictionaryTest, ScanCacheBackupCleanupFailureLeavesTheNewFinalLoadable) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder oldRecorder{{{"猫"}}};
  PageWordScanner oldScanner;
  ASSERT_EQ(completeScan(oldScanner, glyphs, oldRecorder), DictionaryStatus::NotFound);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(2);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, oldScanner, 0));

  ScannerProbeRecorder newRecorder{{{"猫"}, {"犬"}}};
  PageWordScanner newScanner;
  ASSERT_EQ(completeScan(newScanner, glyphs, newRecorder), DictionaryStatus::Found);
  hal_storage_test::removeFailurePath = "/cache/wlscan.bin.bak";
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, newScanner, 1));
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 2);
  EXPECT_EQ(cache.cursor(), 1);
}

TEST_F(JapaneseDictionaryTest, ScanCacheRecoveryRenameFailureKeepsTheBackupForANextAttempt) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  std::filesystem::rename(resolve("/cache/wlscan.bin"), resolve("/cache/wlscan.bin.bak"));
  hal_storage_test::renameFailures.push_back({"/cache/wlscan.bin.bak", "/cache/wlscan.bin"});

  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 0);
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));

  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
}

TEST_F(JapaneseDictionaryTest, ScanCacheFailedRestorationLeavesBackupRecoverable) {
  auto glyphs = makeScannerGlyphs(U"猫犬");
  ScannerProbeRecorder oldRecorder{{{"猫"}}};
  PageWordScanner oldScanner;
  ASSERT_EQ(completeScan(oldScanner, glyphs, oldRecorder), DictionaryStatus::NotFound);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(2);
  PageWordScanCache cache;
  ASSERT_TRUE(cache.save("/cache/wlscan.bin", identity, oldScanner, 0));

  ScannerProbeRecorder newRecorder{{{"猫"}, {"犬"}}};
  PageWordScanner newScanner;
  ASSERT_EQ(completeScan(newScanner, glyphs, newRecorder), DictionaryStatus::Found);
  hal_storage_test::renameFailures.push_back({"/cache/wlscan.bin.tmp", "/cache/wlscan.bin"});
  hal_storage_test::renameFailures.push_back({"/cache/wlscan.bin.bak", "/cache/wlscan.bin"});
  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, newScanner, 1));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
  EXPECT_TRUE(std::filesystem::exists(resolve("/cache/wlscan.bin.bak")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));

  hal_storage_test::reset();
  ASSERT_TRUE(cache.load("/cache/wlscan.bin", identity));
  EXPECT_EQ(cache.candidateCount(), 1);
}

TEST_F(JapaneseDictionaryTest, ScanCacheFirstPublicationPromotionFailureLeavesNoPartialFinal) {
  auto glyphs = makeScannerGlyphs(U"猫");
  ScannerProbeRecorder recorder{{{"猫"}}};
  PageWordScanner scanner;
  ASSERT_EQ(completeScan(scanner, glyphs, recorder), DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(1);
  PageWordScanCache cache;
  hal_storage_test::renameFailures.push_back({"/cache/wlscan.bin.tmp", "/cache/wlscan.bin"});

  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin.tmp")));
  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
}

TEST_F(JapaneseDictionaryTest, ScanCacheRejectsNullEmptyAndOverlongPathsBeforeFileIo) {
  ScannerProbeRecorder recorder;
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({}, DictionaryBackendKind::Japanese, {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  const PageWordScanCacheIdentity identity = scanCacheIdentity(0);
  PageWordScanCache cache;

  EXPECT_FALSE(cache.save(nullptr, identity, scanner, 0));
  EXPECT_FALSE(cache.load(nullptr, identity));
  EXPECT_FALSE(cache.save("", identity, scanner, 0));
  EXPECT_FALSE(cache.load("", identity));
  const std::string overlongPath(513, 'x');
  EXPECT_FALSE(cache.save(overlongPath.c_str(), identity, scanner, 0));
  EXPECT_FALSE(cache.load(overlongPath.c_str(), identity));
  EXPECT_EQ(hal_storage_test::openCount, 0);
}

TEST_F(JapaneseDictionaryTest, ScanCacheRejectsAnUnknownBackendValue) {
  ScannerProbeRecorder recorder;
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin({}, DictionaryBackendKind::Japanese, {&recorder, ScannerProbeRecorder::call}),
            DictionaryStatus::Found);
  PageWordScanCacheIdentity identity = scanCacheIdentity(0);
  identity.backend = static_cast<DictionaryBackendKind>(2);
  PageWordScanCache cache;

  EXPECT_FALSE(cache.save("/cache/wlscan.bin", identity, scanner, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/cache/wlscan.bin")));
  EXPECT_FALSE(cache.load("/cache/wlscan.bin", identity));
}

TEST(DictionaryDefinitionModelTest, BeginInvalidatesPublicationAndStartsCodepointCollection) {
  DictionaryEngine engine;
  DictionaryDefinitionModel model;

  EXPECT_EQ(model.state(), DefinitionBuildState::Idle);
  model.begin(engine, {.generation = 1, .epoch = 1}, 0, 120, 3);

  EXPECT_EQ(model.state(), DefinitionBuildState::CollectingCodepoints);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(model.page().lineCount, 0);
}

struct ModelSpan {
  std::string text;
  bool bold = false;
  bool italic = false;
  bool superscript = false;
  bool subscript = false;
  bool ipa = false;
  bool underline = false;
  bool strikethrough = false;
  bool listItem = false;
  bool lineBreak = false;
  uint8_t indentLevel = 0;

  DictionaryDefinitionSpan view() const {
    return {.text = text,
            .bold = bold,
            .italic = italic,
            .superscript = superscript,
            .subscript = subscript,
            .ipa = ipa,
            .underline = underline,
            .strikethrough = strikethrough,
            .listItem = listItem,
            .lineBreak = lineBreak,
            .indentLevel = indentLevel};
  }
};

struct ModelBackend {
  DictionaryStatus styledStatus = DictionaryStatus::Found;
  DictionaryStatus plainStatus = DictionaryStatus::Found;
  std::vector<ModelSpan> styled{{"definition"}};
  std::vector<ModelSpan> plain{{"definition"}};
  std::vector<std::vector<ModelSpan>> scriptedCalls;
  std::vector<DictionaryStatus> scriptedStatuses;
  std::vector<DictionaryDefinitionMode> modes;
  DictionaryDefinitionModel* cancelTarget = nullptr;
  uint32_t cancelOnCall = 0;
  uint32_t cancelAfterSpan = 0;
  uint32_t streamCalls = 0;
  uint32_t cancelCalls = 0;
  void* cancelContext = nullptr;
  void (*cancelHook)(void*) = nullptr;

  static DictionaryStatus open(void*, const DictionaryOpenRequest&) { return DictionaryStatus::Found; }
  static DictionaryStatus probe(void*, const DictionaryQuery&, DictionaryProbeResult&) {
    return DictionaryStatus::NotFound;
  }
  static DictionaryStatus lookup(void*, const DictionaryQuery&, DictionaryResult& out) {
    out = DictionaryResult{};
    out.status = DictionaryStatus::Found;
    out.matchedBytes = 1;
    return DictionaryStatus::Found;
  }
  static bool bookReading(void*, std::string_view, DictionaryOwnedText&) { return false; }
  static DictionaryStatus suggest(void*, std::string_view, DictionarySuggestions&) {
    return DictionaryStatus::NotFound;
  }
  static DictionaryStatus stream(void* raw, const DictionaryDefinitionMode mode, const DictionaryDefinitionSink sink) {
    auto& self = *static_cast<ModelBackend*>(raw);
    const uint32_t call = ++self.streamCalls;
    self.modes.push_back(mode);
    const size_t scriptIndex = call - 1;
    const auto& spans = scriptIndex < self.scriptedCalls.size()
                            ? self.scriptedCalls[scriptIndex]
                            : (mode == DictionaryDefinitionMode::Styled ? self.styled : self.plain);
    const DictionaryStatus result =
        scriptIndex < self.scriptedStatuses.size()
            ? self.scriptedStatuses[scriptIndex]
            : (mode == DictionaryDefinitionMode::Styled ? self.styledStatus : self.plainStatus);
    uint32_t spanIndex = 0;
    for (const auto& owned : spans) {
      if (!sink.onSpan || !sink.onSpan(sink.context, owned.view())) return DictionaryStatus::Cancelled;
      ++spanIndex;
      if (self.cancelTarget && call == self.cancelOnCall && spanIndex == self.cancelAfterSpan) {
        self.cancelTarget->cancel();
        return DictionaryStatus::Cancelled;
      }
    }
    return result;
  }
  static DictionaryCapabilities capabilities(void*) { return {}; }
  static uint64_t signature(void*) { return 1; }
  static void cancel(void* raw) {
    auto& self = *static_cast<ModelBackend*>(raw);
    ++self.cancelCalls;
    if (self.cancelHook) self.cancelHook(self.cancelContext);
  }
  static void close(void*) {}

  DictionaryBackendFunctions functions() {
    return {this, open, probe, lookup, bookReading, suggest, stream, capabilities, signature, cancel, close};
  }
};

struct DefinitionModelHarness {
  ModelBackend backend;
  ModelBackend unusedJapanese;
  DictionaryEngine engine;
  DictionaryResult result;

  DefinitionModelHarness() : engine(unusedJapanese.functions(), backend.functions()) {
    EXPECT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
    EXPECT_EQ(engine.lookup({"x", 0, DictionaryLookupMode::Token}, result), DictionaryStatus::Found);
  }
};

void prepareDefinitionLayout(DefinitionModelHarness& harness, DictionaryDefinitionModel& model, int targetPage,
                             int maxWidth, int linesPerPage, GfxRenderer& renderer) {
  model.begin(harness.engine, harness.result.definition, targetPage, maxWidth, linesPerPage);
  model.collectCodepointsOnWorker();
  ASSERT_EQ(model.state(), DefinitionBuildState::NeedsFontPrewarm);
  const uint32_t workerMeasurements = renderer.measurementCalls;
  RenderLock lock;
  ASSERT_TRUE(model.prewarmOnMain(renderer, 7, lock));
  EXPECT_GT(renderer.measurementCalls, workerMeasurements);
  ASSERT_EQ(model.state(), DefinitionBuildState::LayingOut);
}

std::vector<std::string> definitionPageLines(const DictionaryDefinitionPage& page) {
  std::vector<std::string> lines;
  lines.reserve(page.lineCount);
  for (uint16_t lineIndex = 0; lineIndex < page.lineCount; ++lineIndex) {
    std::string text;
    const auto& line = page.lines[lineIndex];
    for (uint16_t segment = 0; segment < line.segmentCount; ++segment) {
      text.append(page.segmentText(line.firstSegment + segment));
    }
    lines.push_back(std::move(text));
  }
  return lines;
}

TEST(DictionaryDefinitionModelTest, PlainJapanesePublishesExactOwnedLinesWithoutWorkerRendererCalls) {
  DefinitionModelHarness harness;
  harness.backend.styled = {{"猫犬"}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 3, 3, renderer);
  const uint32_t measurementsAfterPrewarm = renderer.measurementCalls;

  model.layoutOnWorker();

  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(renderer.measurementCalls, measurementsAfterPrewarm);
  EXPECT_EQ(model.totalPages(), 1);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"猫", "犬"}));
  ASSERT_EQ(model.page().segmentCount, 2u);
  EXPECT_STREQ(model.page().textPool + model.page().segments[0].textOffset, "猫");
  EXPECT_STREQ(model.page().textPool + model.page().segments[1].textOffset, "犬");
}

TEST(DictionaryDefinitionModelTest, StyledHtmlPreservesRunsListsExplicitAndUnicodeIpaAndScriptMetadata) {
  DefinitionModelHarness harness;
  harness.backend.styled = {
      {.text = "bold", .bold = true},
      {.text = " italic", .italic = true},
      {.text = " ascii", .ipa = true},
      {.text = " ɑ"},
      {.text = "sup", .superscript = true, .underline = true, .listItem = true, .lineBreak = true, .indentLevel = 2},
      {.text = "sub", .subscript = true, .strikethrough = true},
  };
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 100, 4, renderer);
  EXPECT_EQ(renderer.prewarmStyleMask & 0x07U, 0x07U);
  model.layoutOnWorker();

  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  ASSERT_EQ(model.page().lineCount, 2);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"bold italic ascii ɑ", "supsub"}));
  EXPECT_EQ(model.page().lines[1].indentLevel, 2);
  EXPECT_TRUE(model.page().lines[1].isListItem);
  bool explicitIpa = false;
  bool unicodeIpa = false;
  bool superscript = false;
  bool subscript = false;
  for (uint32_t index = 0; index < model.page().segmentCount; ++index) {
    const auto text = model.page().segmentText(index);
    const auto style = model.page().segments[index].style;
    if (text.find("ascii") != std::string_view::npos) explicitIpa = model.page().segments[index].isIpa;
    if (text == "ɑ") unicodeIpa = model.page().segments[index].isIpa;
    if (text == "sup") superscript = (style & EpdFontFamily::SUP) != 0 && (style & EpdFontFamily::UNDERLINE) != 0;
    if (text == "sub") subscript = (style & EpdFontFamily::SUB) != 0 && (style & EpdFontFamily::STRIKETHROUGH) != 0;
  }
  EXPECT_TRUE(explicitIpa);
  EXPECT_TRUE(unicodeIpa);
  EXPECT_TRUE(superscript);
  EXPECT_TRUE(subscript);
}

TEST(DictionaryDefinitionModelTest, MalformedHtmlDiscardsStrictPrefixAndReplaysCompletePlainFallback) {
  DefinitionModelHarness harness;
  harness.backend.styled = {{"strict-prefix"}};
  harness.backend.plain = {{"full plain"}};
  harness.backend.styledStatus = DictionaryStatus::ReadError;
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 100, 3, renderer);
  model.layoutOnWorker();

  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"full plain"}));
  ASSERT_GE(harness.backend.modes.size(), 4u);
  EXPECT_EQ(harness.backend.modes[0], DictionaryDefinitionMode::Styled);
  EXPECT_EQ(harness.backend.modes[1], DictionaryDefinitionMode::PlainFallback);
  EXPECT_EQ(harness.backend.modes[2], DictionaryDefinitionMode::PlainFallback);
  EXPECT_EQ(harness.backend.modes[3], DictionaryDefinitionMode::PlainFallback);
}

TEST(DictionaryDefinitionModelTest, PageCountClampAndForwardBackwardRebuildsAreDeterministic) {
  DefinitionModelHarness harness;
  harness.backend.styled = {{"one two three four"}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;

  prepareDefinitionLayout(harness, model, 99, 5, 2, renderer);
  model.layoutOnWorker();
  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(model.totalPages(), 2);
  EXPECT_EQ(model.publishedPage(), 1);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"three", "four"}));

  prepareDefinitionLayout(harness, model, 0, 5, 2, renderer);
  model.layoutOnWorker();
  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(model.publishedPage(), 0);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"one", "two"}));

  prepareDefinitionLayout(harness, model, 1, 5, 2, renderer);
  model.layoutOnWorker();
  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"three", "four"}));
}

TEST(DictionaryDefinitionModelTest, ChunkBoundariesInsideWordsAndUtf8DoNotChangePublication) {
  const std::string source = "alpha 猫犬 omega";
  DefinitionModelHarness whole;
  whole.backend.styled = {{source}};
  DefinitionModelHarness chunked;
  chunked.backend.styled = {{source.substr(0, 7)}, {source.substr(7, 2)}, {source.substr(9, 3)}, {source.substr(12)}};
  DictionaryDefinitionModel wholeModel;
  DictionaryDefinitionModel chunkedModel;
  GfxRenderer renderer;
  prepareDefinitionLayout(whole, wholeModel, 0, 8, 8, renderer);
  wholeModel.layoutOnWorker();
  prepareDefinitionLayout(chunked, chunkedModel, 0, 8, 8, renderer);
  chunkedModel.layoutOnWorker();

  ASSERT_EQ(wholeModel.state(), DefinitionBuildState::Ready);
  ASSERT_EQ(chunkedModel.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(definitionPageLines(chunkedModel.page()), definitionPageLines(wholeModel.page()));
  EXPECT_EQ(chunkedModel.totalPages(), wholeModel.totalPages());
}

TEST(DictionaryDefinitionModelTest, SourceIdentityIgnoresStyledCallbackRepartitionIncludingSplitUtf8) {
  const std::string regular = "A猫B";
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {
      {{regular}, {.text = "C", .bold = true}},
      {{regular.substr(0, 2)}, {regular.substr(2)}, {.text = "C", .bold = true}},
      {{regular.substr(0, 1)}, {regular.substr(1, 2)}, {regular.substr(3)}, {.text = "C", .bold = true}},
  };
  harness.backend.scriptedStatuses.assign(3, DictionaryStatus::Found);
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 100, 4, renderer);

  model.layoutOnWorker();

  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"A猫BC"}));
}

TEST(DictionaryDefinitionModelTest, SourceIdentityIgnoresPlainFallbackShortReadRepartitionAndSplitUtf8) {
  std::string source(511, 'x');
  source += "猫tail";
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {
      {{"strict prefix"}},
      {{source.substr(0, 512)}, {source.substr(512)}},
      {{source.substr(0, 510)}, {source.substr(510, 3)}, {source.substr(513)}},
      {{source.substr(0, 511)}, {source.substr(511, 1)}, {source.substr(512, 1)}, {source.substr(513)}},
  };
  harness.backend.scriptedStatuses = {DictionaryStatus::ReadError, DictionaryStatus::Found, DictionaryStatus::Found,
                                      DictionaryStatus::Found};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 10000, 4, renderer);

  model.layoutOnWorker();

  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  std::string published;
  for (const auto& line : definitionPageLines(model.page())) published += line;
  EXPECT_EQ(published, source);
  ASSERT_EQ(harness.backend.modes.size(), 4u);
  EXPECT_EQ(harness.backend.modes[0], DictionaryDefinitionMode::Styled);
  EXPECT_EQ(harness.backend.modes[1], DictionaryDefinitionMode::PlainFallback);
  EXPECT_EQ(harness.backend.modes[2], DictionaryDefinitionMode::PlainFallback);
  EXPECT_EQ(harness.backend.modes[3], DictionaryDefinitionMode::PlainFallback);
}

TEST(DictionaryDefinitionModelTest, ExactStarDictChunksRemainEquivalentForAnAdversarialLongToken) {
  const std::string source(1100, 'x');
  DefinitionModelHarness whole;
  whole.backend.styled = {{source}};
  DefinitionModelHarness chunked;
  chunked.backend.styled = {{source.substr(0, 512)}, {source.substr(512, 512)}, {source.substr(1024)}};
  DictionaryDefinitionModel wholeModel;
  DictionaryDefinitionModel chunkedModel;
  GfxRenderer renderer;
  prepareDefinitionLayout(whole, wholeModel, 3, 20, 10, renderer);
  wholeModel.layoutOnWorker();
  prepareDefinitionLayout(chunked, chunkedModel, 3, 20, 10, renderer);
  chunkedModel.layoutOnWorker();

  ASSERT_EQ(wholeModel.state(), DefinitionBuildState::Ready);
  ASSERT_EQ(chunkedModel.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(chunkedModel.totalPages(), wholeModel.totalPages());
  EXPECT_EQ(definitionPageLines(chunkedModel.page()), definitionPageLines(wholeModel.page()));
}

TEST(DictionaryDefinitionModelTest, ListIndentAndBulletConsumeWidthButUnderlineDoesNot) {
  DefinitionModelHarness harness;
  harness.backend.styled = {
      {.text = "abc", .underline = true, .listItem = true, .lineBreak = true, .indentLevel = 1},
  };
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 7, 3, renderer);
  model.layoutOnWorker();

  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  EXPECT_EQ(definitionPageLines(model.page()), (std::vector<std::string>{"ab", "c"}));
  ASSERT_EQ(model.page().lineCount, 2);
  EXPECT_TRUE(model.page().lines[0].isListItem);
  EXPECT_EQ(model.page().lines[0].indentLevel, 1);
  EXPECT_FALSE(model.page().lines[1].isListItem);
  EXPECT_EQ(model.page().lines[1].indentLevel, 1);
  EXPECT_NE(model.page().segments[0].style & EpdFontFamily::UNDERLINE, 0);
}

TEST(DictionaryDefinitionModelTest, StaleAndUnavailableStreamsFailWithoutPublishing) {
  {
    DefinitionModelHarness harness;
    const DictionaryDefinitionHandle stale = harness.result.definition;
    DictionaryResult newer;
    ASSERT_EQ(harness.engine.lookup({"y", 0, DictionaryLookupMode::Token}, newer), DictionaryStatus::Found);
    DictionaryDefinitionModel model;
    model.begin(harness.engine, stale, 0, 20, 2);
    model.collectCodepointsOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
    EXPECT_EQ(model.page().lineCount, 0);
  }
  {
    DefinitionModelHarness harness;
    harness.backend.styledStatus = DictionaryStatus::Unavailable;
    harness.backend.plainStatus = DictionaryStatus::Unavailable;
    DictionaryDefinitionModel model;
    model.begin(harness.engine, harness.result.definition, 0, 20, 2);
    model.collectCodepointsOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
    EXPECT_EQ(model.page().lineCount, 0);
  }
}

TEST(DictionaryDefinitionModelTest, ReadErrorInvalidUtf8AndBackendOomRemainDistinct) {
  {
    DefinitionModelHarness harness;
    harness.backend.styledStatus = DictionaryStatus::ReadError;
    harness.backend.plainStatus = DictionaryStatus::ReadError;
    DictionaryDefinitionModel model;
    model.begin(harness.engine, harness.result.definition, 0, 20, 2);
    model.collectCodepointsOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  }
  {
    DefinitionModelHarness harness;
    const char invalid[] = {static_cast<char>(0xE7), static_cast<char>(0x8C)};
    harness.backend.styled = {{std::string(invalid, sizeof(invalid))}};
    harness.backend.plain = harness.backend.styled;
    DictionaryDefinitionModel model;
    model.begin(harness.engine, harness.result.definition, 0, 20, 2);
    model.collectCodepointsOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  }
  {
    DefinitionModelHarness harness;
    const char invalid[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
    harness.backend.styled = {{std::string(invalid, sizeof(invalid))}};
    harness.backend.plain = harness.backend.styled;
    DictionaryDefinitionModel model;
    model.begin(harness.engine, harness.result.definition, 0, 20, 2);
    model.collectCodepointsOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  }
  {
    DefinitionModelHarness harness;
    harness.backend.styledStatus = DictionaryStatus::OutOfMemory;
    DictionaryDefinitionModel model;
    model.begin(harness.engine, harness.result.definition, 0, 20, 2);
    model.collectCodepointsOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::OutOfMemory);
  }
}

TEST(DictionaryDefinitionModelTest, InvalidStyledUtf8DoesNotReplayValidPlainFallback) {
  const char invalid[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
  DefinitionModelHarness harness;
  harness.backend.styled = {{std::string(invalid, sizeof(invalid))}};
  harness.backend.plain = {{"plain is valid but must not mask styled corruption"}};
  DictionaryDefinitionModel model;
  model.begin(harness.engine, harness.result.definition, 0, 20, 2);

  model.collectCodepointsOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(harness.backend.streamCalls, 1u);
  ASSERT_EQ(harness.backend.modes.size(), 1u);
  EXPECT_EQ(harness.backend.modes.front(), DictionaryDefinitionMode::Styled);
}

TEST(DictionaryDefinitionModelTest, TrailingPartialStyledUtf8ReadErrorDoesNotReplayValidPlainFallback) {
  const char partial[] = {static_cast<char>(0xE7), static_cast<char>(0x8C)};
  DefinitionModelHarness harness;
  harness.backend.styled = {{"valid prefix "}, {std::string(partial, sizeof(partial))}};
  harness.backend.styledStatus = DictionaryStatus::ReadError;
  harness.backend.plain = {{"plain is valid but must not mask styled corruption"}};
  DictionaryDefinitionModel model;
  model.begin(harness.engine, harness.result.definition, 0, 20, 2);

  model.collectCodepointsOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(harness.backend.streamCalls, 1u);
  ASSERT_EQ(harness.backend.modes.size(), 1u);
  EXPECT_EQ(harness.backend.modes.front(), DictionaryDefinitionMode::Styled);
}

TEST(DictionaryDefinitionModelTest, InvalidUtf8DuringSizingIsReadErrorRatherThanCancellation) {
  const char invalid[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {
      {{"strict prefix"}}, {{"plain complete"}}, {{std::string(invalid, sizeof(invalid))}}};
  harness.backend.scriptedStatuses = {DictionaryStatus::ReadError, DictionaryStatus::Found, DictionaryStatus::Found};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
}

TEST(DictionaryDefinitionModelTest, WrapperOverflowDuringFillIsReadErrorRatherThanCancellation) {
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {
      {{"stable"}}, {{"stable"}}, {{.text = "changed", .lineBreak = true, .indentLevel = 2}}};
  harness.backend.scriptedStatuses.assign(3, DictionaryStatus::Found);
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  renderer.indentAdvance = INT_MAX;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
}

TEST(DictionaryDefinitionModelTest, ZeroSpanStyledReadErrorDuringSizingDoesNotEnterPlainFallback) {
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {{{"stable"}}, {}, {{"stable"}}, {{"stable"}}};
  harness.backend.scriptedStatuses = {DictionaryStatus::Found, DictionaryStatus::ReadError, DictionaryStatus::Found,
                                      DictionaryStatus::Found};
  harness.backend.plain = {{"stable"}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(harness.backend.streamCalls, 2u);
  ASSERT_EQ(harness.backend.modes.size(), 2u);
  EXPECT_EQ(harness.backend.modes[0], DictionaryDefinitionMode::Styled);
  EXPECT_EQ(harness.backend.modes[1], DictionaryDefinitionMode::Styled);
}

TEST(DictionaryDefinitionModelTest, ZeroSpanStyledReadErrorDuringFillDoesNotEnterPlainFallback) {
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {{{"stable"}}, {{"stable"}}, {}};
  harness.backend.scriptedStatuses = {DictionaryStatus::Found, DictionaryStatus::Found, DictionaryStatus::ReadError};
  harness.backend.plain = {{"plain must not be replayed during fill"}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(harness.backend.streamCalls, 3u);
  ASSERT_EQ(harness.backend.modes.size(), 3u);
  EXPECT_EQ(harness.backend.modes[2], DictionaryDefinitionMode::Styled);
}

TEST(DictionaryDefinitionModelTest, EveryExactSnapshotAllocationFailurePublishesOnlyOom) {
  for (size_t rejected = 1; rejected <= 3; ++rejected) {
    SCOPED_TRACE(rejected);
    DefinitionModelHarness harness;
    harness.backend.styled = {{"one two"}};
    DictionaryDefinitionModel model;
    GfxRenderer renderer;
    prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
    dict_memory_test::reset();
    dict_memory_test::rejectedRequest = rejected;

    model.layoutOnWorker();

    EXPECT_EQ(model.state(), DefinitionBuildState::OutOfMemory);
    EXPECT_EQ(model.page().lineCount, 0);
    EXPECT_EQ(model.totalPages(), 0);
  }
  dict_memory_test::reset();
}

TEST(DictionaryDefinitionModelTest, ScratchAllocationFailureIsReportedAndClearCanReuseTheModel) {
  DefinitionModelHarness harness;
  DictionaryDefinitionModel model;
  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;
  model.begin(harness.engine, harness.result.definition, 0, 20, 2);
  model.collectCodepointsOnWorker();
  EXPECT_EQ(model.state(), DefinitionBuildState::OutOfMemory);

  dict_memory_test::reset();
  model.clear();
  EXPECT_EQ(model.state(), DefinitionBuildState::Idle);
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
  model.layoutOnWorker();
  EXPECT_EQ(model.state(), DefinitionBuildState::Ready);
}

TEST(DictionaryDefinitionModelTest, CancellationWinsDuringCollectionLayoutAndFill) {
  for (const uint32_t call : {1u, 2u, 3u}) {
    SCOPED_TRACE(call);
    DefinitionModelHarness harness;
    harness.backend.styled = {{"one"}, {" two"}};
    DictionaryDefinitionModel model;
    GfxRenderer renderer;
    harness.backend.cancelTarget = &model;
    harness.backend.cancelOnCall = call;
    harness.backend.cancelAfterSpan = 1;
    model.begin(harness.engine, harness.result.definition, 0, 20, 2);
    model.collectCodepointsOnWorker();
    if (call == 1) {
      EXPECT_EQ(model.state(), DefinitionBuildState::Cancelled);
    } else {
      ASSERT_EQ(model.state(), DefinitionBuildState::NeedsFontPrewarm);
      RenderLock lock;
      ASSERT_TRUE(model.prewarmOnMain(renderer, 7, lock));
      model.layoutOnWorker();
      EXPECT_EQ(model.state(), DefinitionBuildState::Cancelled);
    }
    EXPECT_EQ(model.page().lineCount, 0);
    EXPECT_EQ(model.totalPages(), 0);
    EXPECT_GE(harness.backend.cancelCalls, 1u);
  }
}

TEST(DictionaryDefinitionModelTest, CancellationLinearizesBeforeBackendAndWinsTheFinalReadyBoundary) {
  struct Boundary {
    std::mutex mutex;
    std::condition_variable condition;
    bool workerAtBoundary = false;
    bool allowPublish = false;
    bool workerFinished = false;
    DefinitionBuildState stateDuringBackendCancel = DefinitionBuildState::Idle;
    DictionaryDefinitionModel* model = nullptr;

    static void pauseWorker(void* raw) {
      auto& self = *static_cast<Boundary*>(raw);
      std::unique_lock lock(self.mutex);
      self.workerAtBoundary = true;
      self.condition.notify_all();
      self.condition.wait(lock, [&] { return self.allowPublish; });
    }

    static void releaseFromBackendCancel(void* raw) {
      auto& self = *static_cast<Boundary*>(raw);
      std::unique_lock lock(self.mutex);
      self.stateDuringBackendCancel = self.model->state();
      self.allowPublish = true;
      self.condition.notify_all();
      self.condition.wait(lock, [&] { return self.workerFinished; });
    }
  } boundary;

  DefinitionModelHarness harness;
  harness.backend.styled = {{"stable"}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
  boundary.model = &model;
  harness.backend.cancelContext = &boundary;
  harness.backend.cancelHook = Boundary::releaseFromBackendCancel;
  dictionary_definition_model_test::setReadyPublishHook(Boundary::pauseWorker, &boundary);

  std::thread worker([&] {
    model.layoutOnWorker();
    std::lock_guard lock(boundary.mutex);
    boundary.workerFinished = true;
    boundary.condition.notify_all();
  });
  {
    std::unique_lock lock(boundary.mutex);
    boundary.condition.wait(lock, [&] { return boundary.workerAtBoundary; });
  }
  std::thread canceller([&] { model.cancel(); });
  canceller.join();
  worker.join();
  dictionary_definition_model_test::setReadyPublishHook(nullptr, nullptr);

  EXPECT_EQ(boundary.stateDuringBackendCancel, DefinitionBuildState::Cancelled);
  EXPECT_EQ(model.state(), DefinitionBuildState::Cancelled);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
}

TEST(DictionaryDefinitionModelTest, CancellationStateArbitrationPrecedesFlagAtFinalReadyBoundary) {
  struct Boundary {
    std::mutex mutex;
    std::condition_variable condition;
    bool workerAtBoundary = false;
    bool cancelAfterFlag = false;
    bool allowWorker = false;
    bool workerFinished = false;
    bool allowCancel = false;
    DefinitionBuildState stateAfterCancelFlag = DefinitionBuildState::Idle;
    DictionaryDefinitionModel* model = nullptr;

    static void pauseWorker(void* raw) {
      auto& self = *static_cast<Boundary*>(raw);
      std::unique_lock lock(self.mutex);
      self.workerAtBoundary = true;
      self.condition.notify_all();
      self.condition.wait(lock, [&] { return self.allowWorker; });
    }

    static void pauseCancelAfterFlag(void* raw) {
      auto& self = *static_cast<Boundary*>(raw);
      std::unique_lock lock(self.mutex);
      self.stateAfterCancelFlag = self.model->state();
      self.cancelAfterFlag = true;
      self.condition.notify_all();
      self.condition.wait(lock, [&] { return self.allowCancel; });
    }
  } boundary;

  DefinitionModelHarness harness;
  harness.backend.styled = {{"stable"}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
  boundary.model = &model;
  dictionary_definition_model_test::setReadyPublishHook(Boundary::pauseWorker, &boundary);
  dictionary_definition_model_test::setCancelAfterFlagHook(Boundary::pauseCancelAfterFlag, &boundary);

  std::thread worker([&] {
    model.layoutOnWorker();
    std::lock_guard lock(boundary.mutex);
    boundary.workerFinished = true;
    boundary.condition.notify_all();
  });
  {
    std::unique_lock lock(boundary.mutex);
    boundary.condition.wait(lock, [&] { return boundary.workerAtBoundary; });
  }
  std::thread canceller([&] { model.cancel(); });
  {
    std::unique_lock lock(boundary.mutex);
    boundary.condition.wait(lock, [&] { return boundary.cancelAfterFlag; });
    boundary.allowWorker = true;
    boundary.condition.notify_all();
    boundary.condition.wait(lock, [&] { return boundary.workerFinished; });
    boundary.allowCancel = true;
    boundary.condition.notify_all();
  }
  worker.join();
  canceller.join();
  dictionary_definition_model_test::setReadyPublishHook(nullptr, nullptr);
  dictionary_definition_model_test::setCancelAfterFlagHook(nullptr, nullptr);

  EXPECT_EQ(boundary.stateAfterCancelFlag, DefinitionBuildState::Cancelled);
  EXPECT_EQ(model.state(), DefinitionBuildState::Cancelled);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
}

TEST(DictionaryDefinitionModelTest, CodepointCapUsesBoundedDeterministicFallbackAndReportsTruncation) {
  std::string definition;
  definition.reserve(257 * 3);
  for (uint32_t codepoint = 0x4E00; codepoint < 0x4E00 + 257; ++codepoint) utf8AppendCodepoint(codepoint, definition);
  DefinitionModelHarness harness;
  harness.backend.styled = {{definition}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 10000, 4, renderer);

  EXPECT_EQ(model.advanceTable().count, DefinitionAdvanceTable::kCodepointCapacity);
  EXPECT_TRUE(model.codepointTableTruncated());
  model.layoutOnWorker();
  ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
  std::string published;
  for (const auto& line : definitionPageLines(model.page())) published += line;
  EXPECT_EQ(published, definition);
}

TEST(DictionaryDefinitionModelTest, PrewarmMeasuresOnlyRequiredStylesAndLeavesDefinitionBatchActive) {
  std::string definition;
  definition.reserve(DefinitionAdvanceTable::kCodepointCapacity * 3U);
  for (uint32_t codepoint = 0x4E00; codepoint < 0x4E00 + DefinitionAdvanceTable::kCodepointCapacity; ++codepoint) {
    utf8AppendCodepoint(codepoint, definition);
  }
  DefinitionModelHarness harness;
  harness.backend.styled = {{.text = definition, .bold = true}};
  DictionaryDefinitionModel model;
  GfxRenderer renderer;

  prepareDefinitionLayout(harness, model, 0, 10000, 4, renderer);

  ASSERT_EQ(renderer.prewarmBatches.size(), 2u);
  EXPECT_EQ(renderer.prewarmBatches[0].codepoints.size(), 3u);
  EXPECT_EQ(renderer.prewarmBatches[1].codepoints.size(), DefinitionAdvanceTable::kCodepointCapacity);
  EXPECT_EQ(renderer.prewarmBatches[0].styleMask, 0x03U);
  EXPECT_EQ(renderer.prewarmBatches[1].styleMask, 0x03U);
  EXPECT_EQ(renderer.measurementStyleMask, 0x03U);
  EXPECT_EQ(renderer.uncachedMeasurementCalls, 6u);
  ASSERT_EQ(model.advanceTable().count, DefinitionAdvanceTable::kCodepointCapacity);
  for (uint16_t index = 0; index < model.advanceTable().count; ++index) {
    EXPECT_EQ(model.advanceTable().advances[index][2], model.advanceTable().advances[index][0]);
    EXPECT_EQ(model.advanceTable().advances[index][3], model.advanceTable().advances[index][0]);
  }
}

TEST(DictionaryDefinitionModelTest, PublicationOwnsTextAndRejectsSameSizeReplayMutation) {
  {
    DefinitionModelHarness harness;
    harness.backend.styled = {{"stable"}};
    DictionaryDefinitionModel model;
    GfxRenderer renderer;
    prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
    model.layoutOnWorker();
    ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
    harness.backend.styled[0].text.assign("xxxxxx");
    EXPECT_EQ(model.page().segmentText(0), "stable");
  }
  {
    DefinitionModelHarness harness;
    harness.backend.scriptedCalls = {{{"abc"}}, {{"abc"}}, {{"xyz"}}};
    harness.backend.scriptedStatuses.assign(3, DictionaryStatus::Found);
    DictionaryDefinitionModel model;
    GfxRenderer renderer;
    prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
    model.layoutOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
    EXPECT_EQ(model.page().lineCount, 0);
  }
}

TEST(DictionaryDefinitionModelTest, RejectsNewCodepointBetweenCollectionAndLayoutWithoutTableTruncation) {
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {{{"a"}}, {{"b"}}, {{"b"}}};
  harness.backend.scriptedStatuses.assign(3, DictionaryStatus::Found);
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
  ASSERT_FALSE(model.codepointTableTruncated());

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
}

TEST(DictionaryDefinitionModelTest, RejectsSameCodepointsReorderedBetweenCollectionAndLayout) {
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {{{"ab"}}, {{"ba"}}, {{"ba"}}};
  harness.backend.scriptedStatuses.assign(3, DictionaryStatus::Found);
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(harness.backend.streamCalls, 2u);
}

TEST(DictionaryDefinitionModelTest, RejectsSameCodepointStyleMutationBetweenCollectionAndLayout) {
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {{{.text = "a"}}, {{.text = "a", .bold = true}}, {{.text = "a", .bold = true}}};
  harness.backend.scriptedStatuses.assign(3, DictionaryStatus::Found);
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(harness.backend.streamCalls, 2u);
}

TEST(DictionaryDefinitionModelTest, RejectsSameTextStructuralMutationBetweenCollectionAndLayout) {
  DefinitionModelHarness harness;
  harness.backend.scriptedCalls = {{{"same"}},
                                   {{.text = "same", .listItem = true, .lineBreak = true, .indentLevel = 1}},
                                   {{.text = "same", .listItem = true, .lineBreak = true, .indentLevel = 1}}};
  harness.backend.scriptedStatuses.assign(3, DictionaryStatus::Found);
  DictionaryDefinitionModel model;
  GfxRenderer renderer;
  prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);

  model.layoutOnWorker();

  EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
  EXPECT_EQ(model.page().lineCount, 0);
  EXPECT_EQ(model.totalPages(), 0);
  EXPECT_EQ(harness.backend.streamCalls, 2u);
}

TEST(DictionaryDefinitionModelTest, EmptyDefinitionIsOneReadyPageAndInvalidDimensionsFailClosed) {
  {
    DefinitionModelHarness harness;
    harness.backend.styled.clear();
    DictionaryDefinitionModel model;
    GfxRenderer renderer;
    prepareDefinitionLayout(harness, model, 0, 20, 2, renderer);
    model.layoutOnWorker();
    ASSERT_EQ(model.state(), DefinitionBuildState::Ready);
    EXPECT_EQ(model.totalPages(), 1);
    EXPECT_EQ(model.page().lineCount, 0);
    EXPECT_EQ(model.page().segmentCount, 0u);
    EXPECT_EQ(model.page().textPoolBytes, 0u);
  }
  for (const auto [width, lines] : {std::pair{0, 1}, std::pair{20, 0}, std::pair{20, 70000}}) {
    DefinitionModelHarness harness;
    DictionaryDefinitionModel model;
    model.begin(harness.engine, harness.result.definition, 0, width, lines);
    model.collectCodepointsOnWorker();
    EXPECT_EQ(model.state(), DefinitionBuildState::ReadError);
    EXPECT_EQ(model.page().lineCount, 0);
  }
}

TEST(DictionaryLookupFlowTest, InitialBurstUsesFiftyMillisecondSlicesAndOnePointFiveSecondDeadline) {
  DictionaryLookupFlow flow;
  flow.beginPage(1000, 0, false, 0);

  flow.beginScanSlice(1100);
  EXPECT_TRUE(flow.canStepScan(1149));
  EXPECT_FALSE(flow.canStepScan(1150));
  EXPECT_FALSE(flow.openDeadlineReached(2499));
  EXPECT_TRUE(flow.initialBurstActive(2499));
  EXPECT_TRUE(flow.openDeadlineReached(2500));
  EXPECT_FALSE(flow.initialBurstActive(2500));
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::Loading);
  EXPECT_TRUE(flow.canStillProduceResult());
}

TEST(DictionaryLookupFlowTest, ColdTouchDefersFirstLookupUntilTheTouchedCandidateIsDiscovered) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 0, false, 0, true);

  flow.onScanProgress(1, false, DictionaryStatus::Found);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::None);
  EXPECT_TRUE(flow.initialSelectionDeferred());
  EXPECT_FALSE(flow.hasSelection());
  EXPECT_FALSE(flow.showPositionHeader());
  const auto deferredPresentation =
      dictionaryLookupCandidatePresentation(true, flow.hasSelection(), flow.cursor(), flow.discoveredCount());
  EXPECT_FALSE(deferredPresentation.selectionValid);
  EXPECT_FALSE(deferredPresentation.showPositionHeader);
  EXPECT_FALSE(flow.selectInitialCandidate(2));
  EXPECT_TRUE(flow.selectInitialCandidate(0));

  const auto lookup = flow.takeCommand();
  EXPECT_EQ(lookup.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(lookup.candidateIndex, 0);
  EXPECT_FALSE(flow.initialSelectionDeferred());
  EXPECT_TRUE(flow.hasSelection());
  const auto selectedPresentation =
      dictionaryLookupCandidatePresentation(true, flow.hasSelection(), flow.cursor(), flow.discoveredCount());
  EXPECT_TRUE(selectedPresentation.selectionValid);
  EXPECT_TRUE(selectedPresentation.showPositionHeader);
}

TEST(DictionaryLookupFlowTest, PartialScanFailureNeverCyclesOrWaitsForCandidatesThatCannotArrive) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 2, false, 0);
  const auto first = flow.takeCommand();
  ASSERT_EQ(first.action, DictionaryLookupFlowAction::StartLookup);
  flow.onLookupFinished(first.generation, DictionaryStatus::NotFound);

  flow.onScanProgress(2, false, DictionaryStatus::OutOfMemory);
  EXPECT_TRUE(flow.scanFailed());
  EXPECT_FALSE(flow.scanComplete());
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::OutOfMemory);
  EXPECT_FALSE(flow.canStillProduceResult());
  EXPECT_TRUE(flow.moveCursor(1));
  const auto second = flow.takeCommand();
  ASSERT_EQ(second.action, DictionaryLookupFlowAction::StartLookup);
  flow.onLookupFinished(second.generation, DictionaryStatus::NotFound);
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::OutOfMemory);
  EXPECT_FALSE(flow.moveCursor(1));
  EXPECT_FALSE(flow.waitingForNextCandidate());
  EXPECT_EQ(flow.cursor(), 1);
}

TEST(DictionaryLookupFlowTest, CandidatePublishedAtTerminalMemoryPressureRemainsUsable) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 0, false, 0);

  flow.onScanProgress(1, false, DictionaryStatus::OutOfMemory);

  const auto lookup = flow.takeCommand();
  ASSERT_EQ(lookup.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(lookup.candidateIndex, 0);
  EXPECT_TRUE(flow.hasSelection());
  flow.onLookupFinished(lookup.generation, DictionaryStatus::NotFound);
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::OutOfMemory);
  EXPECT_FALSE(flow.canStillProduceResult());
}

TEST(DictionaryLookupFlowTest, FirstDiscoveredCandidateStartsLookupBeforeScanCompletes) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 0, false, 0);

  flow.onScanProgress(1, false, DictionaryStatus::Found);

  const auto command = flow.takeCommand();
  EXPECT_EQ(command.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(command.candidateIndex, 0);
  EXPECT_EQ(flow.cursor(), 0);
  EXPECT_EQ(flow.discoveredCount(), 1);
  EXPECT_FALSE(flow.scanComplete());
}

TEST(DictionaryLookupFlowTest, LookupAndDefinitionAdvanceThroughPrewarmToReady) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 1, true, 0);
  const auto lookup = flow.takeCommand();
  ASSERT_EQ(lookup.action, DictionaryLookupFlowAction::StartLookup);

  flow.onLookupFinished(lookup.generation, DictionaryStatus::Found);
  const auto collect = flow.takeCommand();
  EXPECT_EQ(collect.action, DictionaryLookupFlowAction::StartDefinitionCollection);
  EXPECT_EQ(collect.definitionPage, 0);

  flow.onDefinitionEvent(collect.generation, DictionaryLookupFlowDefinitionEvent::NeedsFontPrewarm);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::PrewarmDefinition);
  flow.onPrewarmFinished(collect.generation, true);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::StartDefinitionLayout);
  flow.onDefinitionEvent(collect.generation, DictionaryLookupFlowDefinitionEvent::Ready, 2, 0);

  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::Ready);
  EXPECT_EQ(flow.definitionPage(), 0);
  EXPECT_EQ(flow.definitionPageCount(), 2);
}

TEST(DictionaryLookupFlowTest, LookupTerminalOutcomesRemainDistinct) {
  for (const auto [status, expected] : {
           std::pair{DictionaryStatus::NotFound, DictionaryLookupFlowState::NotFound},
           std::pair{DictionaryStatus::ReadError, DictionaryLookupFlowState::ReadError},
           std::pair{DictionaryStatus::OutOfMemory, DictionaryLookupFlowState::OutOfMemory},
           std::pair{DictionaryStatus::Cancelled, DictionaryLookupFlowState::Cancelled},
       }) {
    DictionaryLookupFlow flow;
    flow.beginDirect(10);
    const auto lookup = flow.takeCommand();
    ASSERT_EQ(lookup.action, DictionaryLookupFlowAction::StartLookup);
    flow.onLookupFinished(lookup.generation, status);
    EXPECT_EQ(flow.state(), expected);
  }
}

TEST(DictionaryLookupFlowTest, DefinitionTerminalOutcomesRemainDistinct) {
  for (const auto [event, expected] : {
           std::pair{DictionaryLookupFlowDefinitionEvent::ReadError, DictionaryLookupFlowState::ReadError},
           std::pair{DictionaryLookupFlowDefinitionEvent::OutOfMemory, DictionaryLookupFlowState::OutOfMemory},
           std::pair{DictionaryLookupFlowDefinitionEvent::Cancelled, DictionaryLookupFlowState::Cancelled},
       }) {
    DictionaryLookupFlow flow;
    flow.beginDirect(10);
    const auto lookup = flow.takeCommand();
    flow.onLookupFinished(lookup.generation, DictionaryStatus::Found);
    const auto collect = flow.takeCommand();
    flow.onDefinitionEvent(collect.generation, event);
    EXPECT_EQ(flow.state(), expected);
  }
}

TEST(DictionaryLookupFlowTest, DiscoveredNextCandidateIsUsableWhileScanRemainsIncomplete) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 1, false, 0);
  const auto first = flow.takeCommand();
  flow.onLookupFinished(first.generation, DictionaryStatus::NotFound);
  EXPECT_TRUE(flow.canStillProduceResult());
  flow.onScanProgress(2, false, DictionaryStatus::Found);

  EXPECT_TRUE(flow.moveCursor(1));
  const auto next = flow.takeCommand();
  EXPECT_EQ(next.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(next.candidateIndex, 1);
  EXPECT_EQ(flow.cursor(), 1);
  EXPECT_FALSE(flow.scanComplete());
}

TEST(DictionaryLookupFlowTest, ForwardAtTemporaryEndRequestsOnDemandScanWithoutCycling) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 2, false, 1);
  (void)flow.takeCommand();
  flow.onLookupFinished(flow.generation(), DictionaryStatus::NotFound);

  EXPECT_FALSE(flow.moveCursor(1));
  EXPECT_TRUE(flow.waitingForNextCandidate());
  EXPECT_EQ(flow.cursor(), 1);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::None);

  flow.onScanProgress(3, false, DictionaryStatus::Found);
  const auto next = flow.takeCommand();
  EXPECT_EQ(next.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(next.candidateIndex, 2);
  EXPECT_EQ(flow.cursor(), 2);
}

TEST(DictionaryLookupFlowTest, NavigationClampsUntilNaturalCompletionThenCycles) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 2, false, 0);
  (void)flow.takeCommand();
  flow.onLookupFinished(flow.generation(), DictionaryStatus::NotFound);

  EXPECT_FALSE(flow.moveCursor(-1));
  EXPECT_EQ(flow.cursor(), 0);
  flow.onScanProgress(2, true, DictionaryStatus::Found);
  EXPECT_TRUE(flow.moveCursor(-1));
  EXPECT_EQ(flow.cursor(), 1);
  EXPECT_EQ(flow.takeCommand().candidateIndex, 1);
  flow.onLookupFinished(flow.generation(), DictionaryStatus::NotFound);
  EXPECT_TRUE(flow.moveCursor(1));
  EXPECT_EQ(flow.cursor(), 0);
}

TEST(DictionaryLookupFlowTest, CompletionAtTemporaryEndCyclesOnlyAfterNoNextCandidateExists) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 2, false, 1);
  (void)flow.takeCommand();
  flow.onLookupFinished(flow.generation(), DictionaryStatus::NotFound);
  ASSERT_FALSE(flow.moveCursor(1));

  flow.onScanProgress(2, true, DictionaryStatus::Found);

  const auto wrapped = flow.takeCommand();
  EXPECT_EQ(wrapped.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(wrapped.candidateIndex, 0);
  EXPECT_EQ(flow.cursor(), 0);
}

TEST(DictionaryLookupFlowTest, RestoredCursorIsAcceptedOrClampedDeterministically) {
  DictionaryLookupFlow valid;
  valid.beginPage(0, 3, true, 2);
  EXPECT_EQ(valid.cursor(), 2);
  EXPECT_EQ(valid.takeCommand().candidateIndex, 2);

  DictionaryLookupFlow corrupt;
  corrupt.beginPage(0, 3, true, 99);
  EXPECT_EQ(corrupt.cursor(), 0);
  EXPECT_EQ(corrupt.takeCommand().candidateIndex, 0);

  DictionaryLookupFlow empty;
  empty.beginPage(0, 0, true, 4);
  EXPECT_EQ(empty.cursor(), 0);
  EXPECT_EQ(empty.state(), DictionaryLookupFlowState::NotFound);
}

TEST(DictionaryLookupFlowTest, ReplacingActiveLookupCancelsAndInvalidatesStaleCompletion) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 2, true, 0);
  const auto first = flow.takeCommand();

  EXPECT_TRUE(flow.moveCursor(1));
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::Loading);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::CancelAndJoin);
  flow.onLookupFinished(first.generation, DictionaryStatus::Found);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::None);

  flow.onWorkerReleased();
  const auto replacement = flow.takeCommand();
  EXPECT_EQ(replacement.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(replacement.candidateIndex, 1);
  EXPECT_GT(replacement.generation, first.generation);
}

TEST(DictionaryLookupFlowTest, ReplacingCurrentWordRestartsSameSelectionForSuggestionOrDictionarySwitch) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 2, true, 1);
  const auto first = flow.takeCommand();
  flow.onLookupFinished(first.generation, DictionaryStatus::NotFound);

  EXPECT_TRUE(flow.replaceCurrentLookup());
  const auto replacement = flow.takeCommand();
  EXPECT_EQ(replacement.action, DictionaryLookupFlowAction::StartLookup);
  EXPECT_EQ(replacement.candidateIndex, 1);
  EXPECT_GT(replacement.generation, first.generation);
}

TEST(DictionaryLookupFlowTest, DefinitionPagingRebuildsAndClampsAtBothEnds) {
  DictionaryLookupFlow flow;
  flow.beginDirect(0);
  const auto lookup = flow.takeCommand();
  flow.onLookupFinished(lookup.generation, DictionaryStatus::Found);
  auto collect = flow.takeCommand();
  flow.onDefinitionEvent(collect.generation, DictionaryLookupFlowDefinitionEvent::Ready, 3, 0);

  EXPECT_TRUE(flow.moveDefinitionPage(1));
  collect = flow.takeCommand();
  EXPECT_EQ(collect.action, DictionaryLookupFlowAction::StartDefinitionCollection);
  EXPECT_EQ(collect.definitionPage, 1);
  flow.onDefinitionEvent(collect.generation, DictionaryLookupFlowDefinitionEvent::Ready, 3, 1);
  EXPECT_TRUE(flow.moveDefinitionPage(8));
  collect = flow.takeCommand();
  EXPECT_EQ(collect.definitionPage, 2);
  flow.onDefinitionEvent(collect.generation, DictionaryLookupFlowDefinitionEvent::Ready, 3, 2);
  EXPECT_FALSE(flow.moveDefinitionPage(1));
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::None);

  EXPECT_TRUE(flow.moveDefinitionPage(-8));
  EXPECT_EQ(flow.takeCommand().definitionPage, 0);
}

TEST(DictionaryLookupFlowTest, DirectWordModeNeverRequestsPageScanningOrPositionHeader) {
  DictionaryLookupFlow flow;
  flow.beginDirect(42);

  EXPECT_TRUE(flow.directMode());
  EXPECT_FALSE(flow.canStepScan(42));
  EXPECT_FALSE(flow.showPositionHeader());
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::StartLookup);
}

TEST(DictionaryLookupFlowTest, ExitWhileWorkerOwnedRequiresCancelJoinBeforeRelease) {
  DictionaryLookupFlow flow;
  flow.beginDirect(0);
  (void)flow.takeCommand();

  flow.beginExit();
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::Cancelled);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::CancelAndJoin);
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::None);
  flow.onWorkerReleased();
  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::ReleaseResources);
}

TEST(DictionaryLookupFlowTest, ExitWithoutWorkerCanReleaseImmediately) {
  DictionaryLookupFlow flow;
  flow.beginPage(0, 0, true, 0);

  flow.beginExit();

  EXPECT_EQ(flow.takeCommand().action, DictionaryLookupFlowAction::ReleaseResources);
}

TEST(DictionaryLookupFlowTest, InitializationFailuresPublishTheirExactTerminalState) {
  DictionaryLookupFlow flow;
  flow.beginPage(10, 0, false, 0);
  flow.onInitializationFailed(DictionaryStatus::ReadError);
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::ReadError);
  EXPECT_TRUE(flow.scanComplete());
  EXPECT_FALSE(flow.workerOwned());

  flow.beginPage(20, 0, false, 0);
  flow.onInitializationFailed(DictionaryStatus::OutOfMemory);
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::OutOfMemory);

  flow.beginPage(30, 0, false, 0);
  flow.onInitializationFailed(DictionaryStatus::Unavailable);
  EXPECT_EQ(flow.state(), DictionaryLookupFlowState::Unavailable);
  EXPECT_FALSE(flow.canStillProduceResult());
}

TEST(DictionaryLookupBackChainTest, NestedLookupsRestoreWordsAndDefinitionPagesInLifoOrder) {
  DictionaryLookupBackChain chain;
  ASSERT_TRUE(chain.push("first", 2));
  ASSERT_TRUE(chain.push("second", 4));

  DictionaryOwnedText word;
  uint16_t page = 0;
  ASSERT_TRUE(chain.pop(word, page));
  EXPECT_EQ(word.view(), "second");
  EXPECT_EQ(page, 4);
  ASSERT_TRUE(chain.pop(word, page));
  EXPECT_EQ(word.view(), "first");
  EXPECT_EQ(page, 2);
  EXPECT_FALSE(chain.pop(word, page));
}

TEST(DictionaryLookupBackChainTest, CapacityIsBoundedAndEvictsOnlyTheOldestEntry) {
  DictionaryLookupBackChain chain;
  for (uint16_t index = 0; index <= DictionaryLookupBackChain::kCapacity; ++index) {
    const std::string word = "word-" + std::to_string(index);
    ASSERT_TRUE(chain.push(word, index));
  }
  EXPECT_EQ(chain.depth(), DictionaryLookupBackChain::kCapacity);

  DictionaryOwnedText word;
  uint16_t page = 0;
  for (uint16_t index = DictionaryLookupBackChain::kCapacity; index > 0; --index) {
    ASSERT_TRUE(chain.pop(word, page));
    EXPECT_EQ(page, index);
  }
  EXPECT_FALSE(chain.pop(word, page));
}

TEST(DictionaryLookupFlowTest, HistoryClassificationKeepsDirectStemAlternateAndSuggestionDistinct) {
  DictionaryResult result;
  EXPECT_EQ(dictionaryLookupHistoryKind(result, false), DictionaryLookupHistoryKind::Direct);
  result.transformed = true;
  EXPECT_EQ(dictionaryLookupHistoryKind(result, false), DictionaryLookupHistoryKind::Stem);
  result.alternate = true;
  EXPECT_EQ(dictionaryLookupHistoryKind(result, false), DictionaryLookupHistoryKind::AltForm);
  EXPECT_EQ(dictionaryLookupHistoryKind(result, true), DictionaryLookupHistoryKind::Suggestion);
}

TEST(DictionaryLookupFlowTest, RetainedStarDictQueryIsCleanedForLookupSuggestionsAndHistory) {
  DictionaryOwnedText retained;
  ASSERT_TRUE(
      retained.assign("\xE2\x80\x9C"
                      "cafe\xCC\x81!"
                      "\xE2\x80\x9D"));
  char scratch[256]{};

  ASSERT_EQ(normalizeRetainedDictionaryLookupText(DictionaryBackendKind::StarDict, retained, scratch, sizeof(scratch)),
            DictionaryStatus::Found);
  EXPECT_EQ(retained.view(), "caf\xC3\xA9");

  ASSERT_TRUE(retained.assign("2\xE5\xB9\xB4"));
  EXPECT_EQ(normalizeRetainedDictionaryLookupText(DictionaryBackendKind::Japanese, retained, scratch, sizeof(scratch)),
            DictionaryStatus::Found);
  EXPECT_EQ(retained.view(), "2\xE5\xB9\xB4");
}

TEST(DictionaryLookupFlowTest, SideButtonLookupRequiresBothTheDedicatedPreferenceAndAnEnabledLayout) {
  constexpr uint8_t kEnabledLayout = 0;
  constexpr uint8_t kDisabledLayout = 2;
  EXPECT_FALSE(dictionaryLookupUsesSideButtons(0, kEnabledLayout, kDisabledLayout));
  EXPECT_TRUE(dictionaryLookupUsesSideButtons(1, kEnabledLayout, kDisabledLayout));
  EXPECT_FALSE(dictionaryLookupUsesSideButtons(1, kDisabledLayout, kDisabledLayout));
}

TEST(DictionaryLookupFlowTest, PowerDismissRequiresLookupActionAndRejectsTheScreenshotChordRelease) {
  constexpr uint8_t kIgnore = 0;
  constexpr uint8_t kPageTurn = 2;
  constexpr uint8_t kCreateClipping = 21;
  constexpr uint8_t kLookupWord = 22;

  EXPECT_TRUE(dictionaryLookupPowerReleaseDismisses(kLookupWord, kLookupWord, true, false));
  EXPECT_FALSE(dictionaryLookupPowerReleaseDismisses(kIgnore, kLookupWord, true, false));
  EXPECT_FALSE(dictionaryLookupPowerReleaseDismisses(kPageTurn, kLookupWord, true, false));
  EXPECT_FALSE(dictionaryLookupPowerReleaseDismisses(kCreateClipping, kLookupWord, true, false));
  EXPECT_FALSE(dictionaryLookupPowerReleaseDismisses(kLookupWord, kLookupWord, false, false));
  EXPECT_FALSE(dictionaryLookupPowerReleaseDismisses(kLookupWord, kLookupWord, true, true));
}

TEST(DictionaryLookupFlowTest, SideButtonScrollMappingTracksOrientationAndKeepsVerticalButtonsOtherwise) {
  auto buttons = dictionaryLookupScrollButtons(false, false);
  EXPECT_EQ(buttons.up, DictionaryLookupNavigationButton::Up);
  EXPECT_EQ(buttons.down, DictionaryLookupNavigationButton::Down);

  buttons = dictionaryLookupScrollButtons(true, false);
  EXPECT_EQ(buttons.up, DictionaryLookupNavigationButton::Left);
  EXPECT_EQ(buttons.down, DictionaryLookupNavigationButton::Right);

  buttons = dictionaryLookupScrollButtons(true, true);
  EXPECT_EQ(buttons.up, DictionaryLookupNavigationButton::Right);
  EXPECT_EQ(buttons.down, DictionaryLookupNavigationButton::Left);
}

TEST(DictionaryLookupFlowTest, ExactTouchMissCanFinishWhenItsGlyphWasProcessedButNearestWaitsForCompletion) {
  EXPECT_TRUE(dictionaryLookupInitialTouchMissIsConclusive(true, false, false, false));
  EXPECT_FALSE(dictionaryLookupInitialTouchMissIsConclusive(true, true, false, false));
  EXPECT_TRUE(dictionaryLookupInitialTouchMissIsConclusive(true, true, true, false));
  EXPECT_FALSE(dictionaryLookupInitialTouchMissIsConclusive(false, true, true, false));
  EXPECT_TRUE(dictionaryLookupInitialTouchMissIsConclusive(false, true, false, true));
}

TEST(DictionaryLookupFlowTest, InitialBurstDefersLoadingRefreshButPublishesTheReadyDefinition) {
  EXPECT_FALSE(dictionaryLookupShouldRenderSnapshot(DictionaryLookupFlowState::Loading, true));
  EXPECT_TRUE(dictionaryLookupShouldRenderSnapshot(DictionaryLookupFlowState::Ready, true));
  EXPECT_TRUE(dictionaryLookupShouldRenderSnapshot(DictionaryLookupFlowState::Loading, false));
}

TEST(DictionaryLookupFlowTest, ExistingReaderFramebufferUsesFastInitialPanelRefresh) {
  EXPECT_EQ(dictionaryLookupInitialPanelRefresh(true), DictionaryLookupPanelRefresh::Fast);
  EXPECT_EQ(dictionaryLookupInitialPanelRefresh(false), DictionaryLookupPanelRefresh::Full);
}

TEST(DictionaryLookupFlowTest, MenuPageLookupUsesFastRefreshAfterRedrawingItsBackground) {
  EXPECT_EQ(dictionaryLookupInitialPanelRefresh(false, true), DictionaryLookupPanelRefresh::Fast);
}

TEST_F(JapaneseDictionaryTest, EngineOwnsTemporaryStarDictSwitchBoundary) {
  writeStarDict({{"switched", "selected definition"}});
  Dictionary::clearLookupDictPathOverride();
  DictionaryEngine engine;
  const DictionaryOpenRequest request{"en", "/book-cache"};

  ASSERT_EQ(engine.openStarDictOverride(request, "/dictionaries/en/dict-data"), DictionaryStatus::Found);
  ASSERT_EQ(engine.backendKind(), DictionaryBackendKind::StarDict);
  EXPECT_TRUE(engine.capabilities().dictionarySwitch);
  DictionaryResult result;
  EXPECT_EQ(engine.lookup({"switched", 0, DictionaryLookupMode::Token}, result), DictionaryStatus::Found);
  EXPECT_EQ(result.headword.view(), "switched");
}
}  // namespace

namespace {
std::vector<uint8_t> mangaOcrFixture(const std::vector<std::vector<std::string>>& panels) {
  std::vector<uint8_t> bytes{static_cast<uint8_t>(panels.size()), 0};
  for (const auto& texts : panels) {
    for (int v : {0, 0, 100, 200}) appendLe16(bytes, v);
    bytes.push_back(static_cast<uint8_t>(texts.size()));
    bytes.push_back(0);
    appendLe16(bytes, 0);
    for (const auto& text : texts) {
      for (int v : {10, 20, 30, 40}) appendLe16(bytes, v);
      appendLe16(bytes, text.size());
      bytes.insert(bytes.end(), text.begin(), text.end());
    }
  }
  return bytes;
}
MangaLookupGeometry mangaGeometry() {
  MangaLookupGeometry g;
  g.sourceWidth = 100;
  g.sourceHeight = 200;
  g.views = {{5, 7, 100, 200}, {7, 5, 200, 100}, 120, 220, 0};
  g.layout = {{5, 7, 100, 200}, 120, 220, 0};
  return g;
}
}  // namespace
TEST(MangaPageTextSourceTest, OwnsLexicalRunsAndWholeBlockBounds) {
  auto bytes = mangaOcrFixture({{"hello world", "猫犬"}, {"other"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, 0, mangaGeometry(), source), DictionaryStatus::Found);
  auto view = source.view();
  ASSERT_EQ(view.glyphCount, 15);
  EXPECT_EQ(view.glyphs[0].pageWord, view.glyphs[4].pageWord);
  EXPECT_NE(view.glyphs[0].pageWord, view.glyphs[6].pageWord);
  EXPECT_NE(view.glyphs[0].paragraph, view.glyphs[12].paragraph);
  EXPECT_EQ(view.glyphs[0].x, 15);
  EXPECT_EQ(view.glyphs[0].y, 27);
  EXPECT_EQ(view.glyphs[0].width, 30);
  EXPECT_EQ(view.glyphs[4].width, 30);
  auto moved = std::move(source);
  EXPECT_EQ(source.view().glyphCount, 0);
  bytes.clear();
  EXPECT_EQ(moved.view().glyphs[0].codepoint, 'h');
}

TEST(MangaPageTextSourceTest, RegionPopupPreservesOriginalClippingOrdinals) {
  auto bytes = mangaOcrFixture({{"first words", "猫犬", "last"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  auto geometry = mangaGeometry();
  geometry.textOnly = true;
  geometry.views.base = {3, 5, 80, 60};
  geometry.cellWidth = 10;
  geometry.lineHeight = 20;
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, geometry, source, 1), DictionaryStatus::Found);
  ASSERT_EQ(source.glyphCount, 3);  // Two characters plus the block boundary.
  EXPECT_EQ(source.view().glyphs[0].codepoint, 0x732b);
  EXPECT_EQ(source.view().glyphs[0].pageWord, 2);
  EXPECT_EQ(source.view().glyphs[0].x, 3);
  EXPECT_EQ(source.view().glyphs[1].x, 13);
  EXPECT_NE(source.view().glyphs[0].x, source.view().glyphs[1].x);
  char out[16];
  size_t written = 0;
  ASSERT_TRUE(copyMangaLookupClipping(page, -1, {2, 2, 3, 6}, out, sizeof(out), written));
  EXPECT_EQ(std::string(out, written), "犬");
  EXPECT_EQ(buildMangaLookupTextSource(page, -1, geometry, source, 3), DictionaryStatus::NotFound);
  EXPECT_EQ(source.glyphCount, 0);
}

TEST(MangaPageTextSourceTest, RegionNavigationSkipsEmptyBlocksAndWrapsWithinScope) {
  auto bytes = mangaOcrFixture({{"", "cat", " \n", "dog"}, {"other"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  const auto geometry = mangaGeometry();
  EXPECT_EQ(nextMangaLookupRegion(page, 0, geometry, -1, true), 1);
  EXPECT_EQ(nextMangaLookupRegion(page, 0, geometry, 1, true), 3);
  EXPECT_EQ(nextMangaLookupRegion(page, 0, geometry, 3, true), 1);
  EXPECT_EQ(nextMangaLookupRegion(page, 0, geometry, 1, false), 3);
  EXPECT_EQ(nextMangaLookupRegion(page, 1, geometry, -1, true), 0);
  EXPECT_EQ(nextMangaLookupRegion(page, 2, geometry, -1, true), -1);
  EXPECT_EQ(mangaLookupRegionAtPoint(page, 0, geometry, 15, 27), 1);
  EXPECT_EQ(mangaLookupRegionAtPoint(page, 0, geometry, 45, 27), -1);
  EXPECT_EQ(mangaLookupRegionAtPoint(page, 0, geometry, 0, 0), -1);
}

TEST(MangaPageTextSourceTest, RegionSelectionRejectsInvisibleAndMalformedViews) {
  auto bytes = mangaOcrFixture({{"cat"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  auto geometry = mangaGeometry();
  geometry.views.base = {90, 190, 10, 10};
  EXPECT_EQ(nextMangaLookupRegion(page, -1, geometry, -1, true), -1);
  geometry = mangaGeometry();
  geometry.textOnly = true;
  EXPECT_EQ(nextMangaLookupRegion(page, -1, geometry, -1, true), -1);
  page.panels.bytes = page.panels.bytes.first(2);
  EXPECT_EQ(nextMangaLookupRegion(page, -1, mangaGeometry(), -1, true), -1);
}

TEST(MangaPageTextSourceTest, RegionPopupKeepsTextBeyondTheVisibleRows) {
  auto bytes = mangaOcrFixture({{"cat dog bird fish"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  auto g = mangaGeometry();
  g.textOnly = true;
  g.views.base = {3, 5, 40, 40};
  g.cellWidth = 10;
  g.lineHeight = 20;
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, g, source, 0), DictionaryStatus::Found);
  EXPECT_FALSE(source.truncated);
  EXPECT_EQ(source.glyphCount, 18);
  const uint32_t hash = source.contentHash;
  ASSERT_TRUE(scrollPageTextToSelection(source, {3, 5, 40, 40}, 13, 4));
  EXPECT_EQ(source.glyphs[13].y, 5);
  EXPECT_EQ(source.glyphs[16].y, 25);
  EXPECT_EQ(source.glyphs[0].y, -55);
  EXPECT_EQ(source.glyphs[13].pageWord, 3);
  EXPECT_EQ(source.contentHash, hash);
  ASSERT_TRUE(scrollPageTextToSelection(source, {3, 5, 40, 40}, 0, 3));
  EXPECT_EQ(source.glyphs[0].y, 5);
  EXPECT_EQ(source.glyphs[13].y, 65);
  EXPECT_FALSE(scrollPageTextToSelection(source, {}, 13, 4));
  EXPECT_EQ(source.glyphs[0].y, 5);
  const auto clip = clipPageTextBounds({3, -5, 10, 20}, {3, 5, 40, 40});
  EXPECT_EQ(clip.y, 5);
  EXPECT_EQ(clip.height, 10);
  EXPECT_FALSE(pageTextViewportContains({3, 5, 40, 40}, 43, 10));
  EXPECT_TRUE(pageTextViewportContains({3, 5, 40, 40}, 42, 44));
}

TEST(MangaPageTextSourceTest, WrappedWordHitTestingDoesNotSelectAdjacentWords) {
  auto bytes = mangaOcrFixture({{"a elephant b"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  auto g = mangaGeometry();
  g.textOnly = true;
  g.views.base = {3, 5, 40, 40};
  g.cellWidth = 10;
  g.lineHeight = 20;
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, g, source, 0), DictionaryStatus::Found);
  EXPECT_FALSE(pageTextRangeContains(source.view(), 2, 8, 8, 15));
  EXPECT_TRUE(pageTextRangeContains(source.view(), 2, 8, 28, 15));
  EXPECT_FALSE(pageTextRangeContains(source.view(), 2, 8, 38, 55));
  EXPECT_FALSE(pageTextRangeContains(source.view(), 65000, 8, 28, 15));
}
TEST(MangaPageTextSourceTest, FailsClosedOnBudgetAndAllocationFailure) {
  auto bytes = mangaOcrFixture({{std::string(1025, 'a')}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  EXPECT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::OutOfMemory);
  EXPECT_TRUE(source.truncated);
  EXPECT_EQ(source.view().glyphCount, 0);
  bytes = mangaOcrFixture({{"cat"}});
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;
  EXPECT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::OutOfMemory);
  EXPECT_EQ(source.view().glyphCount, 0);
  dict_memory_test::reset();
}
TEST(MangaPageTextSourceTest, ReconstructsByteOffsetsAfterOwnerTeardown) {
  const std::string text = std::string("bad\0", 4) + "猫犬" + char(0xff) + "hello world";
  auto bytes = mangaOcrFixture({{text}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_NE(source.view().glyphs[0].paragraph, source.view().glyphs[4].paragraph);
  source.clear();
  char out[64];
  size_t written = 0;
  EXPECT_TRUE(copyMangaLookupClipping(page, -1, {1, 1, 3, 6}, out, sizeof(out), written));
  EXPECT_EQ(std::string(out, written), "犬");
  EXPECT_TRUE(copyMangaLookupClipping(page, -1, {2, 3, 1, 3}, out, sizeof(out), written));
  EXPECT_EQ(std::string(out, written), "ello wor");
  EXPECT_FALSE(copyMangaLookupClipping(page, -1, {1, 1, 1, 6}, out, sizeof(out), written));
}

TEST(MangaPageTextSourceTest, KeepsCompletePrefixAndNeverAnIncompleteLatinToken) {
  auto bytes = mangaOcrFixture({{"cat " + std::string(1100, 'x')}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_TRUE(source.truncated);
  EXPECT_EQ(source.glyphCount, 4);
  EXPECT_EQ(source.view().glyphs[3].pageWord, PageTextGlyph::kSyntheticPageWord);
  EXPECT_EQ(dict_memory_test::largestRequest, kMangaLookupMaxGlyphs * sizeof(PageTextGlyph));
}
TEST(MangaPageTextSourceTest, TextFallbackWrapsAndTruncatesAtCompleteRun) {
  auto bytes = mangaOcrFixture({{"cat dog elephant"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  auto g = mangaGeometry();
  g.textOnly = true;
  g.views.base = {3, 5, 40, 40};
  g.cellWidth = 10;
  g.lineHeight = 20;
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, g, source), DictionaryStatus::Found);
  EXPECT_TRUE(source.truncated);
  ASSERT_EQ(source.glyphCount, 8);
  EXPECT_EQ(source.view().glyphs[0].x, 3);
  EXPECT_EQ(source.view().glyphs[0].y, 5);
  EXPECT_EQ(source.view().glyphs[4].x, 3);
  EXPECT_EQ(source.view().glyphs[4].y, 25);
  EXPECT_EQ(source.view().glyphs[4].width, 10);
  EXPECT_EQ(source.view().glyphs[4].height, 20);
  g.cellWidth = 0;
  EXPECT_EQ(buildMangaLookupTextSource(page, -1, g, source), DictionaryStatus::ReadError);
  EXPECT_EQ(source.glyphCount, 0);
}
TEST(MangaPageTextSourceTest, MapsEveryImageAndBaseOrientationWithAsymmetricInsets) {
  // Oracle maps pixel-edge corners through the renderer's physical coordinate
  // equations, independently of the adapter's relative quarter-turn loop.
  auto physical = [](int o, int x, int y) -> std::pair<int, int> {
    switch (o) {
      case 0:
        return {y, 200 - x};
      case 1:
        return {300 - x, 200 - y};
      case 2:
        return {300 - y, x};
      default:
        return {x, y};
    }
  };
  auto logical = [](int o, int x, int y) -> std::pair<int, int> {
    switch (o) {
      case 0:
        return {200 - y, x};
      case 1:
        return {300 - x, 200 - y};
      case 2:
        return {y, 300 - x};
      default:
        return {x, y};
    }
  };
  for (int base = 0; base < 4; ++base)
    for (int image = 0; image < 4; ++image) {
      SCOPED_TRACE(std::to_string(base) + "/" + std::to_string(image));
      auto g = mangaGeometry();
      g.sourceWidth = 100;
      g.sourceHeight = 100;
      g.views.orientation = base;
      g.views.screenWidth = base % 2 ? 300 : 200;
      g.views.screenHeight = base % 2 ? 200 : 300;
      g.views.base = {3, 7, g.views.screenWidth - 14, g.views.screenHeight - 20};
      g.layout = {{13, 17, 100, 100}, image % 2 ? 300 : 200, image % 2 ? 200 : 300, image};
      auto a = physical(image, 23, 37), b = physical(image, 53, 77);
      a = logical(base, a.first, a.second);
      b = logical(base, b.first, b.second);
      int l = std::max(3, std::min(a.first, b.first)), t = std::max(7, std::min(a.second, b.second));
      int r = std::min(g.views.screenWidth - 11, std::max(a.first, b.first));
      int bottom = std::min(g.views.screenHeight - 13, std::max(a.second, b.second));
      PageTextBounds bounds;
      ASSERT_TRUE(mapMangaLookupBlock({10, 20, 30, 40}, g, bounds));
      EXPECT_EQ(bounds.x, l);
      EXPECT_EQ(bounds.y, t);
      EXPECT_EQ(bounds.width, r - l);
      EXPECT_EQ(bounds.height, bottom - t);
    }
}
TEST(MangaPageTextSourceTest, ClipsSourceAndScreenEdgesWithoutOverflowOrInventedCropOrigin) {
  auto g = mangaGeometry();
  PageTextBounds out;
  ASSERT_TRUE(mapMangaLookupBlock({90, 190, 65535, 65535}, g, out));
  EXPECT_EQ(out.x, 95);
  EXPECT_EQ(out.y, 197);
  EXPECT_EQ(out.width, 10);
  EXPECT_EQ(out.height, 10);
  EXPECT_FALSE(mapMangaLookupBlock({100, 0, 20, 20}, g, out));
  EXPECT_FALSE(mapMangaLookupBlock({0, 0, 0, 20}, g, out));
  g.views.base.x = INT_MAX;
  EXPECT_FALSE(mapMangaLookupBlock({0, 0, 20, 20}, g, out));
  g = mangaGeometry();
  g.layout.orientation = 4;
  EXPECT_FALSE(mapMangaLookupBlock({0, 0, 20, 20}, g, out));
  g = mangaGeometry();
  g.layout.geometry = {0, 0, 51, 103};
  ASSERT_TRUE(mapMangaLookupBlock({10, 20, 30, 40}, g, out));
  EXPECT_EQ(out.x, 5);
  EXPECT_EQ(out.y, 10);
  EXPECT_EQ(out.width, 16);
  EXPECT_EQ(out.height, 21);
}
TEST(MangaPageTextSourceTest, ScopeBoundaryHashesAndCacheNamesSeparateAllPagesAndPanels) {
  auto bytes = mangaOcrFixture({{"cat"}, {"cat"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource a, b, c;
  ASSERT_EQ(buildMangaLookupTextSource(page, 0, mangaGeometry(), a), DictionaryStatus::Found);
  ASSERT_EQ(buildMangaLookupTextSource(page, 1, mangaGeometry(), b), DictionaryStatus::Found);
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), c), DictionaryStatus::Found);
  EXPECT_NE(a.contentHash, b.contentHash);
  EXPECT_NE(a.contentHash, c.contentHash);
  EXPECT_EQ(c.glyphCount, 8);
  char name[64];
  std::set<std::string> names;
  for (uint32_t pageIndex : {0u, 1u, 255u, 256u, 9999u})
    for (int panel = -1; panel < 255; ++panel) {
      ASSERT_TRUE(mangaLookupCacheFileName(pageIndex, panel, name, sizeof(name)));
      EXPECT_TRUE(names.insert(name).second);
    }
  EXPECT_FALSE(mangaLookupCacheFileName(UINT32_MAX, 0, name, sizeof(name)));
  EXPECT_FALSE(mangaLookupCacheFileName(0, 255, name, sizeof(name)));
  EXPECT_FALSE(mangaLookupCacheFileName(0, 0, name, 2));
  EXPECT_EQ(name[0], 0);
  auto changed = mangaOcrFixture({{"car"}});
  ASSERT_EQ(manga::format::decodePage(changed, page), manga::format::Error::None);
  ASSERT_EQ(buildMangaLookupTextSource(page, 0, mangaGeometry(), b), DictionaryStatus::Found);
  EXPECT_NE(a.contentHash, b.contentHash);
}
TEST(MangaPageTextSourceTest, RejectsMalformedSequencesAsParagraphSeparatorsAndClippingBoundaries) {
  const std::vector<std::string> malformed{"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"};
  for (const auto& separator : malformed) {
    auto bytes = mangaOcrFixture({{"猫" + separator + "犬"}});
    manga::format::PageView page;
    ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
    OwnedLookupTextSource source;
    ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
    EXPECT_NE(source.view().glyphs[0].paragraph, source.view().glyphs[source.glyphCount - 2].paragraph);
    char out[32];
    size_t written;
    EXPECT_TRUE(copyMangaLookupClipping(page, -1, {1, 1, 0, 3}, out, sizeof(out), written));
    EXPECT_EQ(std::string(out, written), "犬");
    EXPECT_FALSE(copyMangaLookupClipping(page, -1, {0, 1, 0, 3}, out, sizeof(out), written));
    EXPECT_EQ(written, 0u);
  }
}
TEST_F(JapaneseDictionaryTest, MangaOcrUsesRealStarDictScannerWholeWordsIncludingUnknowns) {
  writeStarDict({{"cat", "feline"}, {"dog", "canine"}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  auto bytes = mangaOcrFixture({{"cat unknown—dog"}, {"cat"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, 0, mangaGeometry(), source), DictionaryStatus::Found);
  PageWordScanner scanner;
  ASSERT_EQ(
      scanner.begin(source.view(), engine.backendKind(),
                    {&engine, [](void* p, const DictionaryQuery& q,
                                 DictionaryProbeResult& r) { return static_cast<DictionaryEngine*>(p)->probe(q, r); }}),
      DictionaryStatus::Found);
  scanToEnd(scanner);
  ASSERT_TRUE(scanner.completedSuccessfully());
  ASSERT_EQ(scanner.candidateCount(), 3);
  EXPECT_EQ(scanner.candidate(0)->glyphCount, 3);
  EXPECT_EQ(scanner.candidate(1)->glyphCount, 7);
  EXPECT_EQ(scanner.candidate(2)->glyphCount, 3);
  EXPECT_EQ(scanner.candidate(2)->firstPageWord, 2);
}
TEST_F(JapaneseDictionaryTest, MangaOcrUsesRealJapaneseLongestDeinflectionGrammarNamesAndCounters) {
  writeVocab({{"猫", "cat", 230, 0},
              {"猫犬", "pair", 230, 0},
              {"食べる", "eat", 230, DictIndexRecord::POS_V1},
              {"人", "person", 230, 0},
              {"ムー", "partial name", 230, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/grammar", {{"について", "about", 230, DictIndexRecord::POS_OTHER}});
  writeSource("/dictionaries/jp/names", {{"太郎", "name", 230, DictIndexRecord::POS_OTHER}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  const std::vector<std::string> surfaces{"猫犬", "食べました", "について", "太郎さん", "３人", "ムーミンさん"};
  for (const auto& text : surfaces) {
    SCOPED_TRACE(text);
    auto bytes = mangaOcrFixture({{text}});
    manga::format::PageView page;
    ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
    OwnedLookupTextSource source;
    ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
    PageWordScanner scanner;
    const DictionaryProbeFn probe{&engine, [](void* p, const DictionaryQuery& q, DictionaryProbeResult& r) {
                                    return static_cast<DictionaryEngine*>(p)->probe(q, r);
                                  }};
    ASSERT_EQ(scanner.begin(source.view(), engine.backendKind(), probe), DictionaryStatus::Found);
    scanToEnd(scanner);
    ASSERT_TRUE(scanner.completedSuccessfully());
    ASSERT_GE(scanner.candidateCount(), 1);
    EXPECT_EQ(scanner.candidate(0)->firstGlyph, 0);
    // Compare against the existing scanner's plain glyph fixture to pin all
    // segmentation policy without changing the dictionary/scanner implementation.
    std::u32string codepoints;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text.c_str());
    while (*cursor) codepoints.push_back(utf8NextCodepoint(&cursor));
    auto reference = makeScannerGlyphs(codepoints);
    PageWordScanner baseline;
    ASSERT_EQ(baseline.begin({reference.data(), uint16_t(reference.size()), 1}, engine.backendKind(), probe),
              DictionaryStatus::Found);
    scanToEnd(baseline);
    ASSERT_EQ(scanner.candidateCount(), baseline.candidateCount());
    for (uint16_t i = 0; i < baseline.candidateCount(); ++i) {
      EXPECT_EQ(scanner.candidate(i)->glyphCount, baseline.candidate(i)->glyphCount);
      EXPECT_EQ(scanner.candidate(i)->matchedBytes, baseline.candidate(i)->matchedBytes);
    }
  }
}
TEST(MangaPageTextSourceTest, LongJapaneseRunRetainsGlyphPrefixWhileLatinSuffixIsDiscarded) {
  std::string text;
  for (int i = 0; i < 1100; ++i) text += "猫";
  auto bytes = mangaOcrFixture({{text}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_TRUE(source.truncated);
  EXPECT_EQ(source.glyphCount, kMangaLookupMaxGlyphs);
  text = "猫" + std::string(1100, 'x');
  bytes = mangaOcrFixture({{text}});
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_TRUE(source.truncated);
  EXPECT_EQ(source.glyphCount, 1);
}
TEST(MangaPageTextSourceTest, AllocationFailureRetriesBoundedArrayWithVisibleTruncation) {
  std::string text;
  for (int i = 0; i < 500; ++i) text += "猫";
  auto bytes = mangaOcrFixture({{text}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  dict_memory_test::reset();
  dict_memory_test::rejectedRequest = 1;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_EQ(source.glyphCount, 256);
  EXPECT_TRUE(source.truncated);
  EXPECT_EQ(dict_memory_test::requestCount, 2u);
  dict_memory_test::reset();
}
TEST(MangaPageTextSourceTest, SeparatesUnicodeSpacesControlsLinesAndBlocksWithoutChangingSavedOrder) {
  auto bytes = mangaOcrFixture({{"one\xc2\xa0two\xe3\x80\x80three\tfour\nfive", "six"}, {"seven"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  char out[64];
  size_t written;
  const std::array<std::string, 7> words{"one", "two", "three", "four", "five", "six", "seven"};
  for (uint16_t i = 0; i < words.size(); ++i) {
    ASSERT_TRUE(copyMangaLookupClipping(page, -1, {i, i, 0, uint16_t(words[i].size())}, out, sizeof(out), written));
    EXPECT_EQ(std::string(out, written), words[i]);
  }
  EXPECT_FALSE(copyMangaLookupClipping(page, -1, {2, 3, 0, 4}, out, sizeof(out), written));
  EXPECT_FALSE(copyMangaLookupClipping(page, -1, {4, 5, 0, 3}, out, sizeof(out), written));
  EXPECT_FALSE(copyMangaLookupClipping(page, -1, {0, 0, 0, 3}, out, 3, written));
  EXPECT_EQ(written, 0u);
  EXPECT_EQ(out[0], 0);
}
TEST(MangaPageTextSourceTest, AcceptsFourByteUtf8AndHashesParagraphChanges) {
  const std::string astral = "\xf0\xa0\x80\x80";
  auto bytes = mangaOcrFixture({{astral + "猫"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_EQ(source.view().glyphs[0].codepoint, 0x20000u);
  char out[16];
  size_t written;
  ASSERT_TRUE(copyMangaLookupClipping(page, -1, {0, 0, 4, 7}, out, sizeof(out), written));
  EXPECT_EQ(std::string(out, written), "猫");
  bytes = mangaOcrFixture({{"cat dog"}});
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  const auto spaceHash = source.contentHash;
  bytes = mangaOcrFixture({{"cat\ndog"}});
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_NE(source.contentHash, spaceHash);
  bytes = mangaOcrFixture({{"cat", "dog"}});
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_NE(source.contentHash, spaceHash);
}
TEST(MangaPageTextSourceTest, EmptyInvalidScopeAndInvalidGeometryNeverPublishOldStorage) {
  auto bytes = mangaOcrFixture({{"cat"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, -1, mangaGeometry(), source), DictionaryStatus::Found);
  EXPECT_EQ(buildMangaLookupTextSource(page, 1, mangaGeometry(), source), DictionaryStatus::ReadError);
  EXPECT_EQ(source.glyphCount, 0);
  auto g = mangaGeometry();
  g.layout.screenWidth = 0;
  EXPECT_EQ(buildMangaLookupTextSource(page, -1, g, source), DictionaryStatus::ReadError);
  EXPECT_EQ(source.glyphCount, 0);
  EXPECT_EQ(buildMangaLookupTextSource({}, -1, mangaGeometry(), source), DictionaryStatus::NotFound);
  EXPECT_EQ(source.glyphCount, 0);
  EXPECT_FALSE(source.truncated);
}

TEST_F(JapaneseDictionaryTest, MangaSmokeFixtureNeedsStarDictCaseInsensitiveIndexOrdering) {
  // Exact original generic smoke ordering. "This" precedes "text" in byte
  // order but sorts after it under StarDict's case-insensitive comparator.
  writeStarDict({{"Alignment", "definition"},
                 {"Reader", "definition"},
                 {"This", "definition"},
                 {"paragraph", "definition"},
                 {"text", "definition"},
                 {"the", "definition"}});
  std::filesystem::remove(resolve("/dictionaries/en/dict-data.idx.oft"));
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  DictionaryResult result;
  EXPECT_EQ(engine.lookup({"Reader"}, result), DictionaryStatus::Found);
  EXPECT_EQ(engine.lookup({"text"}, result), DictionaryStatus::NotFound);
  auto bytes = mangaOcrFixture({{"Reader text"}});
  manga::format::PageView page;
  ASSERT_EQ(manga::format::decodePage(bytes, page), manga::format::Error::None);
  OwnedLookupTextSource source;
  ASSERT_EQ(buildMangaLookupTextSource(page, 0, mangaGeometry(), source), DictionaryStatus::Found);
  PageWordScanner scanner;
  ASSERT_EQ(
      scanner.begin(source.view(), engine.backendKind(),
                    {&engine, [](void* p, const DictionaryQuery& q,
                                 DictionaryProbeResult& r) { return static_cast<DictionaryEngine*>(p)->probe(q, r); }}),
      DictionaryStatus::Found);
  scanToEnd(scanner);
  ASSERT_EQ(scanner.candidateCount(), 2);
  const auto* second = scanner.candidate(1);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->firstGlyph, 7);
  EXPECT_EQ(second->glyphCount, 4);
  EXPECT_EQ(second->firstPageWord, 1);
  std::string query;
  for (int i = 0; i < second->glyphCount; ++i)
    query += static_cast<char>(source.view().glyphs[second->firstGlyph + i].codepoint);
  EXPECT_EQ(query, "text");
  engine.close();
  // Rewrite ONLY record order, preserving definition offsets and bytes.
  const std::pair<const char*, int> ordered[] = {{"Alignment", 0}, {"paragraph", 3}, {"Reader", 1},
                                                 {"text", 4},      {"the", 5},       {"This", 2}};
  std::vector<uint8_t> index;
  for (const auto& [word, original] : ordered) {
    index.insert(index.end(), word, word + std::strlen(word) + 1);
    appendBe32(index, original * 10);
    appendBe32(index, 10);
  }
  writeBytes(resolve("/dictionaries/en/dict-data.idx"), index);
  Dictionary::setLookupDictPathOverride("/dictionaries/en/dict-data");
  ASSERT_EQ(engine.open({"en", nullptr}), DictionaryStatus::Found);
  EXPECT_EQ(engine.lookup({query}, result), DictionaryStatus::Found);
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityReadsFullIndexAndNoSparseDefinitionPayload) {
  std::vector<InputRecord> records;
  for (int i = 0; i < 97; ++i) records.push_back({"word" + std::to_string(1000 + i), "body", 1, 0});
  auto bytes = writeSource("/dictionaries/jp/vocab", records);
  std::filesystem::resize_file(resolve("/dictionaries/jp/vocab.dat"), 100 * 1024 * 1024);
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  ASSERT_EQ(engine.beginScanIdentity(state), DictionaryScanIdentityStatus::Pending);
  EXPECT_EQ(state.digest(), 0u);
  while (state.status() == DictionaryScanIdentityStatus::Pending) engine.stepScanIdentity(state, 256);
  ASSERT_EQ(state.status(), DictionaryScanIdentityStatus::Ready);
  const auto original = state.digest();
  engine.close();
  bytes.first[48 * sizeof(DictIndexRecord) + 38] ^= 1;
  writeBytes(resolve("/dictionaries/jp/vocab.idx"), bytes.first);
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  engine.beginScanIdentity(state);
  while (state.status() == DictionaryScanIdentityStatus::Pending) engine.stepScanIdentity(state, 256);
  EXPECT_NE(state.digest(), original);
}

#include "DictionaryScanIdentityPolicy.h"
TEST(DictionaryScanIdentityPolicyTest, PendingWorkYieldsOnlyWhenWorkerAndFirstDefinitionPermit) {
  DictionaryScanIdentityPolicy policy;
  EXPECT_TRUE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, false, false, false, false, false, false));
  policy.progressiveStarted();
  EXPECT_FALSE(policy.canLoad());
  EXPECT_FALSE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, false, false, false, false, false, false));
  EXPECT_TRUE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, false, false, false, true, false, false));
  EXPECT_FALSE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, false, true, false, true, false, false));
  EXPECT_FALSE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, false, false, true, true, false, false));
  EXPECT_FALSE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, false, false, false, true, true, false));
  EXPECT_FALSE(policy.eligible(DictionaryScanIdentityStatus::Ready, true, false, false, false, true, false, false));
}

namespace {
uint64_t finishIdentity(DictionaryEngine& engine, DictionaryScanIdentityState& state) {
  size_t steps = 0;
  while (state.status() == DictionaryScanIdentityStatus::Pending && steps++ < 100000)
    engine.stepScanIdentity(state, 256);
  EXPECT_EQ(state.status(), DictionaryScanIdentityStatus::Ready);
  return state.digest();
}
DictionaryProbeFn identityProbe(DictionaryEngine& engine) {
  return {&engine, [](void* context, const DictionaryQuery& query, DictionaryProbeResult& out) {
            return static_cast<DictionaryEngine*>(context)->probe(query, out);
          }};
}
}  // namespace

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityBoundariesFullReadsAndExistingJapaneseHandles) {
  for (const int count : {48, 49, 64, 96, 97, 4096}) {
    SCOPED_TRACE(count);
    std::vector<InputRecord> records;
    for (int i = 0; i < count; ++i) records.push_back({"word" + std::to_string(10000 + i), "body", 1, 0});
    auto bytes = writeSource("/dictionaries/jp/vocab", records, true);
    std::filesystem::resize_file(resolve("/dictionaries/jp/vocab.dat"), 100 * 1024 * 1024);
    DictionaryEngine engine;
    ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
    hal_storage_test::reset();
    hal_storage_test::rejectDuplicateReaders = true;
    DictionaryScanIdentityState state;
    ASSERT_EQ(engine.beginScanIdentity(state), DictionaryScanIdentityStatus::Pending);
    const auto original = finishIdentity(engine, state);
    EXPECT_EQ(hal_storage_test::readBytes["/dictionaries/jp/vocab.idx"], bytes.first.size());
    EXPECT_EQ(hal_storage_test::readBytes["/dictionaries/jp/vocab.dat"], 0u);
    EXPECT_EQ(hal_storage_test::readBytes["/dictionaries/jp/vocab.spx"], 0u);
    EXPECT_EQ(hal_storage_test::openCount, 0u);
    EXPECT_LE(hal_storage_test::maximumReadBytes, 256u);
    engine.close();
    for (size_t field : {size_t{0}, size_t{38}, size_t{39}}) {
      auto edited = bytes.first;
      edited[(count / 2) * sizeof(DictIndexRecord) + field] ^= 1;
      writeBytes(resolve("/dictionaries/jp/vocab.idx"), edited);
      ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
      engine.beginScanIdentity(state);
      EXPECT_NE(finishIdentity(engine, state), original);
      engine.close();
    }
  }
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentitySurvivesReopenButRehashesNewActivationAndRoutes) {
  writeVocab({{"猫", "cat", 1, 0}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  engine.beginScanIdentity(state);
  engine.stepScanIdentity(state, 8);
  for (unsigned i = 0; i < 3; ++i) {
    engine.cancel();
    engine.close();
    ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
    ASSERT_TRUE(engine.resumeScanIdentity(state));
  }
  hal_storage_test::reset();
  const auto original = finishIdentity(engine, state);
  EXPECT_EQ(hal_storage_test::readBytes["/dictionaries/jp/vocab.idx"], 32u);
  engine.close();
  writeBytes(resolve("/dictionaries/jp/vocab.spx"), {1, 2, 3});
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  hal_storage_test::reset();
  ASSERT_TRUE(engine.resumeScanIdentity(state));
  EXPECT_EQ(finishIdentity(engine, state), original);
  EXPECT_EQ(hal_storage_test::readCount, 0u);
  engine.beginScanIdentity(state);
  EXPECT_EQ(finishIdentity(engine, state), original);
  EXPECT_EQ(hal_storage_test::readBytes["/dictionaries/jp/vocab.idx"], 40u);
  engine.close();
  writeSource("/dictionaries/jp/grammar", {{"猫", "grammar", 2, 0}});
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
  engine.beginScanIdentity(state);
  EXPECT_NE(finishIdentity(engine, state), original);
  engine.close();
  std::filesystem::remove(resolve("/dictionaries/jp/grammar.dat"));
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
  engine.beginScanIdentity(state);
  EXPECT_EQ(finishIdentity(engine, state), original);
  engine.close();
  std::filesystem::resize_file(resolve("/dictionaries/jp/vocab.dat"), 4);
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
  engine.beginScanIdentity(state);
  EXPECT_NE(finishIdentity(engine, state), original);
  engine.close();
  writeSource("/dict/vocab", {{"猫", "cat!", 1, 0}});
  std::filesystem::remove(resolve("/dictionaries/jp/vocab.idx"));
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityCancelsEveryChunkIncludingBeforePublicationAndErrorsNeverTrust) {
  std::vector<InputRecord> records(20, {"word", "body", 1, 0});
  writeVocab(records);
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  for (unsigned boundary = 0; boundary <= 4; ++boundary) {
    DictionaryScanIdentityState state;
    engine.beginScanIdentity(state);
    for (unsigned step = 0; step < boundary; ++step) engine.stepScanIdentity(state, 256);
    EXPECT_EQ(state.status(), DictionaryScanIdentityStatus::Pending);
    state.cancel();
    EXPECT_EQ(engine.stepScanIdentity(state, 256), DictionaryScanIdentityStatus::Cancelled);
    EXPECT_EQ(state.digest(), 0u);
  }
  for (bool seekFailure : {false, true}) {
    DictionaryScanIdentityState state;
    engine.beginScanIdentity(state);
    engine.stepScanIdentity(state, 256);
    if (seekFailure)
      hal_storage_test::seekFailurePath = "/dictionaries/jp/vocab.idx";
    else {
      hal_storage_test::shortReadPath = "/dictionaries/jp/vocab.idx";
      hal_storage_test::shortReadOffset = 256;
    }
    EXPECT_EQ(engine.stepScanIdentity(state, 256), DictionaryScanIdentityStatus::ReadError);
    EXPECT_EQ(state.digest(), 0u);
    hal_storage_test::reset();
    EXPECT_EQ(engine.stepScanIdentity(state, 256), DictionaryScanIdentityStatus::ReadError);
  }
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityStarFullCanonicalFilesNoBodiesOrAccelerators) {
  std::vector<std::pair<std::string, std::string>> records;
  for (int i = 0; i < 300; ++i) records.emplace_back("word" + std::to_string(1000 + i), "body");
  writeStarDict(records);
  const std::string base = "/dictionaries/en/dict-data";
  writeText(resolve(base + ".syn"), std::string(3000, 's'));
  auto ifo = readBytes(resolve(base + ".ifo"));
  ifo.insert(ifo.end(), 3000, '#');
  writeBytes(resolve(base + ".ifo"), ifo);
  std::filesystem::resize_file(resolve(base + ".dict"), 100 * 1024 * 1024);
  DictionaryEngine engine;
  auto open = [&] { return engine.openStarDictOverride({"en", nullptr}, base.c_str()); };
  ASSERT_EQ(open(), DictionaryStatus::Found);
  hal_storage_test::reset();
  hal_storage_test::rejectDuplicateReaders = true;
  DictionaryScanIdentityState state;
  engine.beginScanIdentity(state);
  while (state.status() == DictionaryScanIdentityStatus::Pending) {
    engine.stepScanIdentity(state, 4096);  // The implementation clamps even oversized callers to 256.
    for (const char* ext : {".idx", ".syn", ".ifo", ".dict"})
      EXPECT_EQ(hal_storage_test::activeReaders[base + ext], 0u);
  }
  ASSERT_EQ(state.status(), DictionaryScanIdentityStatus::Ready);
  const auto original = state.digest();
  for (const char* ext : {".idx", ".syn", ".ifo"})
    EXPECT_EQ(hal_storage_test::readBytes[base + ext], std::filesystem::file_size(resolve(base + ext)));
  EXPECT_EQ(hal_storage_test::readBytes[base + ".dict"], 0u);
  EXPECT_LE(hal_storage_test::maximumReadBytes, 256u);
  engine.close();
  for (const char* ext : {".qidx", ".idx.oft", ".syn.oft", ".idx.oft.cspt", ".syn.oft.cspt"})
    writeBytes(resolve(base + ext), {1, 2, 3});
  ASSERT_EQ(open(), DictionaryStatus::Found);
  hal_storage_test::reset();
  EXPECT_TRUE(engine.resumeScanIdentity(state));
  EXPECT_EQ(hal_storage_test::readCount, 0u);
  engine.beginScanIdentity(state);
  EXPECT_EQ(finishIdentity(engine, state), original);
  for (const char* ext : {".qidx", ".idx.oft", ".syn.oft", ".idx.oft.cspt", ".syn.oft.cspt"})
    EXPECT_EQ(hal_storage_test::readBytes[base + ext], 0u);
  engine.close();
  for (const char* ext : {".idx", ".syn", ".ifo"}) {
    auto bytes = readBytes(resolve(base + ext));
    bytes[bytes.size() / 2] ^= 1;
    writeBytes(resolve(base + ext), bytes);
    ASSERT_EQ(open(), DictionaryStatus::Found);
    engine.beginScanIdentity(state);
    EXPECT_NE(finishIdentity(engine, state), original);
    engine.close();
    bytes[bytes.size() / 2] ^= 1;
    writeBytes(resolve(base + ext), bytes);
  }
  ASSERT_EQ(open(), DictionaryStatus::Found);
  dict_memory_test::rejectAll = true;
  EXPECT_EQ(engine.beginScanIdentity(state), DictionaryScanIdentityStatus::OutOfMemory);
  EXPECT_EQ(state.digest(), 0u);
  dict_memory_test::reset();
  hal_storage_test::readOpenFailurePath = base + ".syn";
  EXPECT_EQ(engine.beginScanIdentity(state), DictionaryScanIdentityStatus::ReadError);
  EXPECT_EQ(state.digest(), 0u);
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentitySmallSecondActivationActuallyLoadsCacheWithinInitialSlice) {
  writeVocab({{"猫", "cat", 1, 0}, {"犬", "dog", 1, 0}});
  writeSource("/dictionaries/jp/grammar", {{"猫", "grammar", 2, 0}});
  writeSource("/dictionaries/jp/names", {{"犬", "name", 2, 0}});
  auto glyphs = makeScannerGlyphs(U"猫犬");
  PageTextSourceView source{glyphs.data(), static_cast<uint16_t>(glyphs.size()), 123};
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  engine.beginScanIdentity(state);
  const auto digest = finishIdentity(engine, state);
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin(source, engine.backendKind(), identityProbe(engine)), DictionaryStatus::Found);
  while (!scanner.done()) scanner.stepOne();
  ASSERT_TRUE(scanner.completedSuccessfully());
  ASSERT_EQ(scanner.candidateCount(), 2);
  PageWordScanCache cache;
  PageWordScanCacheIdentity identity{engine.backendKind(), 0, 0, source.contentHash, digest, source.glyphCount};
  ASSERT_TRUE(cache.save("/cache/real.bin", identity, scanner, 1));
  engine.close();
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  engine.beginScanIdentity(state);
  DictionaryScanIdentityPolicy policy;
  unsigned steps = 0;
  for (; steps < policy.kInitialSteps && state.status() == DictionaryScanIdentityStatus::Pending; ++steps)
    engine.stepScanIdentity(state, policy.kChunkBytes);
  ASSERT_EQ(state.status(), DictionaryScanIdentityStatus::Ready);
  EXPECT_LE(steps, 4u);
  policy.initialFinished();
  ASSERT_TRUE(policy.canLoad());
  identity.dictionarySignature = state.digest();
  PageWordScanCache secondActivation;
  ASSERT_TRUE(secondActivation.load("/cache/real.bin", identity));
  EXPECT_EQ(secondActivation.cursor(), 1);
  EXPECT_EQ(secondActivation.candidateCount(), 2);
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityLargeProgressiveScanKeepsCursorWhenVerificationFinishesLate) {
  std::vector<InputRecord> records;
  for (int i = 0; i < 4096; ++i) records.push_back({"word" + std::to_string(10000 + i), "body", 1, 0});
  records.push_back({"猫", "cat", 1, 0});
  records.push_back({"犬", "dog", 1, 0});
  writeVocab(records);
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  engine.beginScanIdentity(state);
  DictionaryScanIdentityPolicy policy;
  for (unsigned i = 0; i < policy.kInitialSteps; ++i) engine.stepScanIdentity(state, policy.kChunkBytes);
  ASSERT_EQ(state.status(), DictionaryScanIdentityStatus::Pending);
  policy.progressiveStarted();
  auto glyphs = makeScannerGlyphs(U"猫犬");
  PageTextSourceView source{glyphs.data(), static_cast<uint16_t>(glyphs.size()), 123};
  PageWordScanner scanner;
  ASSERT_EQ(scanner.begin(source, engine.backendKind(), identityProbe(engine)), DictionaryStatus::Found);
  while (!scanner.done()) scanner.stepOne();
  ASSERT_EQ(scanner.candidateCount(), 2);
  DictionaryLookupFlow flow;
  flow.beginPage(0, 2, true, 1, true);
  const auto beforeCursor = flow.cursor();
  const auto first = *scanner.candidate(0);
  const auto digest = finishIdentity(engine, state);
  EXPECT_FALSE(policy.canLoad());
  EXPECT_EQ(flow.cursor(), beforeCursor);
  EXPECT_EQ(scanner.candidate(0)->firstGlyph, first.firstGlyph);
  PageWordScanCache cache;
  PageWordScanCacheIdentity identity{engine.backendKind(), 0, 0, source.contentHash, digest, source.glyphCount};
  EXPECT_TRUE(cache.save("/cache/large.bin", identity, scanner, flow.cursor()));
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityMoveRevokesMovedFromTrust) {
  writeVocab({{"猫", "cat", 1, 0}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  engine.beginScanIdentity(state);
  const auto digest = finishIdentity(engine, state);
  DictionaryScanIdentityState moved(std::move(state));
  EXPECT_EQ(moved.digest(), digest);
  EXPECT_EQ(state.digest(), 0u);
  EXPECT_NE(state.status(), DictionaryScanIdentityStatus::Ready);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityRouteAllocationFailureDoesNotBreakLookup) {
  writeStarDict({{"cat", "feline"}});
  const std::string base = "/dictionaries/en/dict-data";
  DictionaryEngine engine;
  dict_memory_test::rejectedBytes = base.size() + 1;
  ASSERT_EQ(engine.openStarDictOverride({"en", nullptr}, base.c_str()), DictionaryStatus::Found);
  dict_memory_test::reset();
  DictionaryScanIdentityState state;
  EXPECT_EQ(engine.beginScanIdentity(state), DictionaryScanIdentityStatus::OutOfMemory);
  DictionaryResult result;
  EXPECT_EQ(engine.lookup({"cat", 0}, result), DictionaryStatus::Found);
  engine.close();
  const std::string longBase = "/" + std::string(180, 'a') + "/" + std::string(180, 'b') + "/" + std::string(144, 'c');
  for (const char* ext : {".ifo", ".idx", ".dict", ".idx.oft"})
    writeBytes(resolve(longBase + ext), readBytes(resolve(base + ext)));
  ASSERT_EQ(engine.openStarDictOverride({"en", nullptr}, longBase.c_str()), DictionaryStatus::Found);
  EXPECT_EQ(engine.beginScanIdentity(state), DictionaryScanIdentityStatus::Unavailable);
  EXPECT_EQ(engine.lookup({"cat", 0}, result), DictionaryStatus::Found);
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityUnreadableOptionalJapaneseSourceDisablesOnlyCache) {
  writeVocab({{"猫", "cat", 1, 0}});
  writeSource("/dictionaries/jp/names", {{"犬", "name", 1, 0}});
  hal_storage_test::readOpenFailurePath = "/dictionaries/jp/names.idx";
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  EXPECT_EQ(engine.beginScanIdentity(state), DictionaryScanIdentityStatus::ReadError);
  DictionaryResult result;
  EXPECT_EQ(engine.lookup({"猫", 0}, result), DictionaryStatus::Found);
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityBodyReplacementKeepsCandidatesButChangesSelectedDefinition) {
  for (bool japanese : {true, false}) {
    const std::string base = japanese ? "/dictionaries/jp/vocab" : "/dictionaries/en/dict-data";
    if (japanese)
      writeVocab({{"cat", "one", 1, 0}});
    else
      writeStarDict({{"cat", "one"}});
    DictionaryEngine engine;
    auto open = [&] {
      return japanese ? engine.open({"ja", nullptr}) : engine.openStarDictOverride({"en", nullptr}, base.c_str());
    };
    ASSERT_EQ(open(), DictionaryStatus::Found);
    DictionaryScanIdentityState state;
    engine.beginScanIdentity(state);
    const auto original = finishIdentity(engine, state);
    DictionaryProbeResult candidate;
    EXPECT_EQ(engine.probe({"cat", 0}, candidate), DictionaryStatus::Found);
    EXPECT_EQ(engine.probe({"absent", 0}, candidate), DictionaryStatus::NotFound);
    // StarDict may create .qidx during those probes; it must not change identity.
    EXPECT_TRUE(engine.resumeScanIdentity(state));
    engine.close();
    writeText(resolve(base + (japanese ? ".dat" : ".dict")), "two");
    ASSERT_EQ(open(), DictionaryStatus::Found);
    engine.beginScanIdentity(state);
    EXPECT_EQ(finishIdentity(engine, state), original);
    DictionaryResult result;
    ASSERT_EQ(engine.lookup({"cat", 0}, result), DictionaryStatus::Found);
    std::string definition;
    DictionaryDefinitionSink sink{&definition, [](void* context, const DictionaryDefinitionSpan& span) {
                                    static_cast<std::string*>(context)->append(span.text);
                                    return true;
                                  }};
    EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::PlainFallback, sink),
              DictionaryStatus::Found);
    EXPECT_EQ(definition, "two");
  }
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityStarCancellationReadFailureAndRouteChange) {
  writeStarDict({{"cat", "one"}}, 'm', "kitty", "cat");
  const std::string base = "/dictionaries/en/dict-data";
  DictionaryEngine engine;
  auto open = [&] { return engine.openStarDictOverride({"en", nullptr}, base.c_str()); };
  ASSERT_EQ(open(), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  engine.beginScanIdentity(state);
  engine.stepScanIdentity(state, 4);
  engine.cancel();
  engine.close();
  ASSERT_EQ(open(), DictionaryStatus::Found);
  ASSERT_TRUE(engine.resumeScanIdentity(state));
  hal_storage_test::reset();
  const auto digest = finishIdentity(engine, state);
  EXPECT_EQ(hal_storage_test::readBytes[base + ".idx"], std::filesystem::file_size(resolve(base + ".idx")) - 4);
  engine.close();
  ASSERT_EQ(open(), DictionaryStatus::Found);
  hal_storage_test::reset();
  EXPECT_TRUE(engine.resumeScanIdentity(state));
  EXPECT_EQ(finishIdentity(engine, state), digest);
  EXPECT_EQ(hal_storage_test::readCount, 0u);
  engine.beginScanIdentity(state);
  hal_storage_test::shortReadPath = base + ".idx";
  hal_storage_test::shortReadOffset = 0;
  EXPECT_EQ(engine.stepScanIdentity(state, 256), DictionaryScanIdentityStatus::ReadError);
  EXPECT_EQ(state.digest(), 0u);
  EXPECT_EQ(hal_storage_test::activeReaders[base + ".idx"], 0u);
  hal_storage_test::reset();
  engine.beginScanIdentity(state);
  state.cancel();
  EXPECT_EQ(engine.stepScanIdentity(state, 256), DictionaryScanIdentityStatus::Cancelled);
  EXPECT_EQ(state.digest(), 0u);
  engine.beginScanIdentity(state);
  finishIdentity(engine, state);
  engine.close();
  const std::string other = "/dictionaries/other/dict-data";
  for (const char* ext : {".idx", ".dict", ".ifo", ".syn"})
    writeBytes(resolve(other + ext), readBytes(resolve(base + ext)));
  ASSERT_EQ(engine.openStarDictOverride({"en", nullptr}, other.c_str()), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
  EXPECT_EQ(state.digest(), 0u);
}

TEST(DictionaryScanIdentityPolicyTest, TerminalAndIneligibleStatesNeverAddHashBusySpinReason) {
  DictionaryScanIdentityPolicy policy;
  policy.progressiveStarted();
  for (auto status : {DictionaryScanIdentityStatus::Ready, DictionaryScanIdentityStatus::Cancelled,
                      DictionaryScanIdentityStatus::Unavailable, DictionaryScanIdentityStatus::ReadError,
                      DictionaryScanIdentityStatus::OutOfMemory})
    EXPECT_FALSE(policy.eligible(status, true, false, false, false, true, false, false));
  EXPECT_FALSE(policy.eligible(DictionaryScanIdentityStatus::Pending, false, false, false, false, true, false, false));
  EXPECT_FALSE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, true, false, false, true, false, false));
  EXPECT_TRUE(policy.eligible(DictionaryScanIdentityStatus::Pending, true, false, false, false, false, false, true));
  EXPECT_LE(sizeof(DictionaryScanIdentityState), 192u);
}

namespace {
DictionaryEngine* identityEngineToCancel = nullptr;
void cancelIdentityAfterPhysicalRead() {
  hal_storage_test::afterRead = nullptr;
  identityEngineToCancel->cancel();
}
}  // namespace
TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityCancellationDuringReadNeverPublishesLastChunk) {
  for (bool japanese : {true, false}) {
    if (japanese)
      writeVocab({{"cat", "one", 1, 0}});
    else
      writeStarDict({{"cat", "one"}});
    DictionaryEngine engine;
    ASSERT_EQ(japanese ? engine.open({"ja", nullptr})
                       : engine.openStarDictOverride({"en", nullptr}, "/dictionaries/en/dict-data"),
              DictionaryStatus::Found);
    DictionaryScanIdentityState state;
    engine.beginScanIdentity(state);
    identityEngineToCancel = &engine;
    hal_storage_test::afterRead = cancelIdentityAfterPhysicalRead;
    EXPECT_EQ(engine.stepScanIdentity(state, 256), DictionaryScanIdentityStatus::Cancelled);
    EXPECT_EQ(state.digest(), 0u);
    EXPECT_EQ(hal_storage_test::activeReaders["/dictionaries/en/dict-data.idx"], 0u);
    identityEngineToCancel = nullptr;
  }
}

TEST_F(JapaneseDictionaryTest, VerifiedScanIdentityStarMetadataWidthOptionalSourceAndBodyExtent) {
  writeStarDict({{"cat", "one"}});
  const std::string base = "/dictionaries/en/dict-data";
  DictionaryEngine engine;
  auto open = [&] { return engine.openStarDictOverride({"en", nullptr}, base.c_str()); };
  ASSERT_EQ(open(), DictionaryStatus::Found);
  DictionaryScanIdentityState state;
  engine.beginScanIdentity(state);
  const auto original = finishIdentity(engine, state);
  engine.close();
  auto ifo = readBytes(resolve(base + ".ifo"));
  const std::string metadata = "idxoffsetbits=64\n";
  ifo.insert(ifo.end(), metadata.begin(), metadata.end());
  writeBytes(resolve(base + ".ifo"), ifo);
  ASSERT_EQ(open(), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
  engine.beginScanIdentity(state);
  EXPECT_NE(finishIdentity(engine, state), original);
  engine.close();
  writeBytes(resolve(base + ".syn"), {'k', 'i', 't', 't', 'y', 0, 0, 0, 0, 0});
  ASSERT_EQ(open(), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
  engine.beginScanIdentity(state);
  const auto synonym = finishIdentity(engine, state);
  engine.close();
  std::filesystem::resize_file(resolve(base + ".dict"), 100 * 1024 * 1024);
  ASSERT_EQ(open(), DictionaryStatus::Found);
  EXPECT_FALSE(engine.resumeScanIdentity(state));
  hal_storage_test::reset();
  engine.beginScanIdentity(state);
  EXPECT_NE(finishIdentity(engine, state), synonym);
  EXPECT_EQ(hal_storage_test::readBytes[base + ".dict"], 0u);
}

TEST_F(JapaneseDictionaryTest, KanaMarkerSurvivesLowMergeBudgetAndChunkBoundary) {
  writeVocab({{"かな", "reading", 190, DictIndexRecord::POS_READING},
              {"かな", std::string(126, 'x') + "[kana] usual spelling", 180, DictIndexRecord::POS_READING}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  for (uint32_t largest : {48u * 1024u, 8u * 1024u}) {
    dict_arduino_test::maxAllocHeap = largest;
    bool found = false;
    ASSERT_EQ(index.checkUsuallyKana("かな", found, DictIndex::DICT_JMDICT), JapaneseDictStatus::Found);
    EXPECT_TRUE(found);
  }
}

TEST_F(JapaneseDictionaryTest, KanaMarkerIgnoresSixthSenseAndReportsIncompleteReads) {
  writeVocab({{"かな", "one", 199, DictIndexRecord::POS_READING},
              {"かな", "two", 198, DictIndexRecord::POS_READING},
              {"かな", "three", 197, DictIndexRecord::POS_READING},
              {"かな", "four", 196, DictIndexRecord::POS_READING},
              {"かな", "five", 195, DictIndexRecord::POS_READING},
              {"かな", "[kana] six", 194, DictIndexRecord::POS_READING}});
  DictIndex index;
  ASSERT_EQ(index.open(), JapaneseDictStatus::Found);
  bool found = true;
  EXPECT_EQ(index.checkUsuallyKana("かな", found, DictIndex::DICT_JMDICT), JapaneseDictStatus::Found);
  EXPECT_FALSE(found);
  hal_storage_test::shortReadPath = "/dictionaries/jp/vocab.dat";
  hal_storage_test::shortReadOffset = 0;
  EXPECT_EQ(index.checkUsuallyKana("かな", found, DictIndex::DICT_JMDICT), JapaneseDictStatus::ReadError);
  EXPECT_FALSE(found);
  hal_storage_test::reset();
  dict_memory_test::rejectAll = true;
  EXPECT_EQ(index.checkUsuallyKana("かな", found, DictIndex::DICT_JMDICT), JapaneseDictStatus::OutOfMemory);
  EXPECT_FALSE(found);
}

TEST_F(JapaneseDictionaryTest, KanaProbeCarriesMarkerAndRecoverableFailure) {
  writeVocab({{"かな", "[kana] word", 100, DictIndexRecord::POS_READING}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryProbeResult result;
  ASSERT_EQ(engine.probe({"かな"}, result), DictionaryStatus::Found);
  EXPECT_TRUE(result.usuallyKana);
  EXPECT_FALSE(result.markerCheckFailed);
  dict_memory_test::rejectAll = true;
  ASSERT_EQ(engine.probe({"かな"}, result), DictionaryStatus::Found);
  EXPECT_FALSE(result.usuallyKana);
  EXPECT_TRUE(result.markerCheckFailed);
}

TEST_F(JapaneseDictionaryTest, KanaFailureContinuesScanningWithoutPublishingCache) {
  writeVocab({{"ふわり", "[kana] word", 100, DictIndexRecord::POS_READING}, {"猫", "cat", 200}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  const auto glyphs = makeScannerGlyphs(U"ふわり猫");
  PageWordScanner scanner;
  auto probe = [](void* context, const DictionaryQuery& query, DictionaryProbeResult& out) {
    return static_cast<DictionaryEngine*>(context)->probe(query, out);
  };
  ASSERT_EQ(scanner.begin({glyphs.data(), 4, 0x11223344}, DictionaryBackendKind::Japanese, {&engine, probe}),
            DictionaryStatus::Found);
  hal_storage_test::shortReadPath = "/dictionaries/jp/vocab.dat";
  hal_storage_test::shortReadOffset = 0;
  scanToEnd(scanner);
  EXPECT_TRUE(scanner.completedSuccessfully());
  EXPECT_FALSE(scanner.cacheable());
  ASSERT_EQ(scanner.candidateCount(), 1);
  EXPECT_EQ(scanner.candidate(0)->firstGlyph, 3);
  PageWordScanCache cache;
  const auto identity = scanCacheIdentity(4);
  EXPECT_FALSE(cache.save("/wlscan.bin", identity, scanner, 0));
  EXPECT_FALSE(std::filesystem::exists(resolve("/wlscan.bin")));
  hal_storage_test::reset();
  DictionaryProbeResult recovered;
  ASSERT_EQ(engine.probe({"ふわり"}, recovered), DictionaryStatus::Found);
  EXPECT_TRUE(recovered.usuallyKana);
  ASSERT_EQ(scanner.restart(), DictionaryStatus::Found);
  scanToEnd(scanner);
  EXPECT_TRUE(scanner.cacheable());
  ASSERT_EQ(scanner.candidateCount(), 2);
  EXPECT_EQ(scanner.candidate(0)->glyphCount, 3);
  EXPECT_TRUE(cache.save("/wlscan.bin", identity, scanner, 0));
}

TEST_F(JapaneseDictionaryTest, ShortHiraganaPrefersCanonicalGrammarButNotDigitPrefix) {
  writeVocab({{"こと", "vocabulary", 200}, {"する", "do", 200, DictIndexRecord::POS_VS}});
  writeSource("/dictionaries/jp/grammar", {{"こと", "grammar koto", 200}, {"する", "grammar suru", 200}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  for (const auto& [surface, expected] :
       std::vector<std::pair<std::string, std::string>>{{"こと", "grammar koto"}, {"した", "grammar suru"}}) {
    DictionaryResult result;
    ASSERT_EQ(engine.lookup({surface}, result), DictionaryStatus::Found);
    std::string definition;
    ASSERT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled,
                                      {&definition, acceptDefinitionSpan}),
              DictionaryStatus::Found);
    EXPECT_EQ(definition, expected);
    EXPECT_EQ(result.surface.view(), surface);
  }
  DictionaryQuery prefixed{"こと"};
  prefixed.displayPrefix = "2";
  DictionaryResult result;
  ASSERT_EQ(engine.lookup(prefixed, result), DictionaryStatus::Found);
  std::string definition;
  ASSERT_EQ(
      engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&definition, acceptDefinitionSpan}),
      DictionaryStatus::Found);
  EXPECT_EQ(definition, "vocabulary");
}

TEST_F(JapaneseDictionaryTest, ContextGrammarUsesEarliestTieAndHeapGate) {
  writeVocab({{"猫", "cat", 200}});
  writeSource("/dictionaries/jp/grammar", {{"あいう", "earliest", 200}, {"いう猫", "later", 200}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryQuery query{"猫"};
  query.grammarContext = "あいう猫";
  query.grammarCursorByteOffset = 9;
  query.grammarLabel = "Grammar";
  for (uint32_t heap : {16384u, 16383u}) {
    dict_arduino_test::maxAllocHeap = heap;
    DictionaryResult result;
    ASSERT_EQ(engine.lookup(query, result), DictionaryStatus::Found);
    std::string definition;
    ASSERT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled,
                                      {&definition, acceptDefinitionSpan}),
              DictionaryStatus::Found);
    EXPECT_EQ(definition, heap == 16384u ? "cat\n\n— Grammar: あいう —\nearliest" : "cat");
    EXPECT_EQ(result.headword.view(), "猫");
  }
}

TEST_F(JapaneseDictionaryTest, ContextGrammarStopsAtDuplicateLongestHit) {
  writeVocab({{"こと", "main", 200}});
  writeSource("/dictionaries/jp/grammar", {{"こと", "preferred", 200}, {"ことになる", "context", 200}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryQuery query{"こと"};
  query.grammarContext = "ことになる";
  query.grammarLabel = "Grammar";
  DictionaryResult result;
  ASSERT_EQ(engine.lookup(query, result), DictionaryStatus::Found);
  std::string definition;
  ASSERT_EQ(
      engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&definition, acceptDefinitionSpan}),
      DictionaryStatus::Found);
  EXPECT_EQ(definition, "preferred\n\n— Grammar: ことになる —\ncontext");
  query.grammarContext = "こと";
  ASSERT_EQ(engine.lookup(query, result), DictionaryStatus::Found);
  definition.clear();
  ASSERT_EQ(
      engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&definition, acceptDefinitionSpan}),
      DictionaryStatus::Found);
  EXPECT_EQ(definition, "preferred");
}

TEST(JapaneseLookupContextTest, BoundsContextToParagraphAndUtf8Characters) {
  auto glyphs = makeScannerGlyphs(U"abcことになる終端");
  JapaneseLookupContext context;
  ASSERT_TRUE(buildJapaneseLookupContext({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 0}, 3, context));
  EXPECT_EQ(context.cursorByteOffset, 3);
  EXPECT_EQ(std::string_view(context.text, context.byteCount), "abcことになる終端");
  glyphs[2].paragraph = 1;
  ASSERT_TRUE(buildJapaneseLookupContext({glyphs.data(), static_cast<uint16_t>(glyphs.size()), 0}, 3, context));
  EXPECT_EQ(context.cursorByteOffset, 0);
  EXPECT_EQ(std::string_view(context.text, context.byteCount), "ことになる終端");
  EXPECT_FALSE(buildJapaneseLookupContext({}, 0, context));
  EXPECT_EQ(context.byteCount, 0);
}

TEST_F(JapaneseDictionaryTest, GrammarDefinitionSurvivesFailedLookup) {
  writeVocab({{"猫", "cat", 200}});
  writeSource("/dictionaries/jp/grammar", {{"ことになる", "pattern", 200}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryQuery query{"猫"};
  query.grammarContext = "ことになる";
  query.grammarLabel = "Grammar";
  DictionaryResult retained;
  ASSERT_EQ(engine.lookup(query, retained), DictionaryStatus::Found);
  DictionaryResult failed;
  ASSERT_EQ(engine.lookup({"未知語"}, failed), DictionaryStatus::NotFound);
  std::string text;
  ASSERT_EQ(
      engine.streamDefinition(retained.definition, DictionaryDefinitionMode::Styled, {&text, acceptDefinitionSpan}),
      DictionaryStatus::Found);
  EXPECT_EQ(text, "cat\n\n— Grammar: ことになる —\npattern");
}

TEST_F(JapaneseDictionaryTest, EqualLongestGrammarHitSuppressesShorterAtThatStart) {
  writeVocab({{"あいう", "main", 200}});
  writeSource("/dictionaries/jp/grammar", {{"あいう", "preferred", 200}, {"あい", "must not append", 200}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryQuery query{"あいう"};
  query.grammarContext = "あいう";
  query.grammarLabel = "Grammar";
  DictionaryResult result;
  ASSERT_EQ(engine.lookup(query, result), DictionaryStatus::Found);
  std::string text;
  ASSERT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&text, acceptDefinitionSpan}),
            DictionaryStatus::Found);
  EXPECT_EQ(text, "preferred");
}

TEST_F(JapaneseDictionaryTest, GrammarContextHonorsTenCharacterLimitAndCancellation) {
  writeVocab({{"猫", "cat", 200}});
  writeSource("/dictionaries/jp/grammar",
              {{"あいうえおかきくけa", "ten", 200}, {"あいうえおかきくけab", "eleven", 200}});
  DictionaryEngine engine;
  ASSERT_EQ(engine.open({"ja", nullptr}), DictionaryStatus::Found);
  DictionaryQuery query{"猫"};
  query.grammarContext = "あいうえおかきくけab";
  query.grammarLabel = "Grammar";
  DictionaryResult result;
  ASSERT_EQ(engine.lookup(query, result), DictionaryStatus::Found);
  std::string text;
  ASSERT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&text, acceptDefinitionSpan}),
            DictionaryStatus::Found);
  EXPECT_EQ(text, "cat\n\n— Grammar: あいうえおかきくけa —\nten");
  engine.cancel();
  text.clear();
  EXPECT_EQ(engine.streamDefinition(result.definition, DictionaryDefinitionMode::Styled, {&text, acceptDefinitionSpan}),
            DictionaryStatus::Cancelled);
  EXPECT_TRUE(text.empty());
}
