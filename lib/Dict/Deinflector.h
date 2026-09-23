#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "DictIndex.h"

enum class WordCondition : uint8_t {
  V1 = 0,
  V5 = 1,
  VS = 2,
  VK = 3,
  ADJ_I = 4,
  DICT = 5,
};

struct DeinflectionCandidate {
  char text[DictIndexRecord::HEADWORD_SIZE] = {};
  uint8_t byteLength = 0;
  WordCondition condition = WordCondition::DICT;
};

struct DeinflectionBuffer {
  static constexpr uint8_t kCapacity = 64;
  std::array<DeinflectionCandidate, kCapacity> candidates{};
  uint8_t count = 0;
};

#ifdef CROSSINK_DICT_TESTING
struct DeinflectionRuleForTest {
  const char* from;
  const char* to;
  WordCondition condIn;
  WordCondition condOut;
};
#endif

class Deinflector {
 public:
  static constexpr size_t kRuleCount = 260;

  static void deinflect(std::string_view surface, DeinflectionBuffer& out, CooperativeCancellation cancellation = {});
#ifdef CROSSINK_DICT_TESTING
  static void deinflectForTest(std::string_view surface, const DeinflectionRuleForTest* rules, size_t ruleCount,
                               DeinflectionBuffer& out);
#endif
};
