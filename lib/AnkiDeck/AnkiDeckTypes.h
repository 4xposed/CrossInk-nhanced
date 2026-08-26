#pragma once

#include <array>
#include <cstdint>

constexpr uint8_t kMaxCardFields = 8;
constexpr uint16_t kMaxCardFieldTextBytes = 2048;
constexpr uint16_t kMaxCardSideTextBytes = 4096;

// Callers provide a separately allocated 2,049-byte buffer for every field
// they intend to read. The deck reader fills length and primary on success.
struct CardField {
  char* text = nullptr;
  uint16_t length = 0;
  bool primary = false;
};

// Fixed caller-owned field slots keep styled cards streamable without an
// in-memory deck catalog or per-read allocation.
struct CardFields {
  std::array<CardField, kMaxCardFields> prompt{};
  uint8_t promptCount = 0;
  std::array<CardField, kMaxCardFields> answer{};
  uint8_t answerCount = 0;
};

enum class ReviewKind : uint8_t {
  New = 0,
  Review = 1,
  Learning = 2,
};

struct ReviewState {
  uint32_t dueDay = 0;
  uint16_t intervalDays = 1;
  ReviewKind kind = ReviewKind::New;
  uint8_t flags = 0;
};
