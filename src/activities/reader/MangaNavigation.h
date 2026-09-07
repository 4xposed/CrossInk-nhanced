#pragma once

#include <array>
#include <cstdint>

namespace manga {

struct Position {
  uint32_t page = 0;
  int16_t panel = -1;
};

struct PageAvailability {
  bool overview = false;
  uint16_t panelCount = 0;
  std::array<uint8_t, 32> crops{};

  bool hasCrop(uint16_t panel) const;
  void setCrop(uint16_t panel);
};

enum class Entry { Overview, FirstPanel, LastPanel };

struct Move {
  Position position;
  Entry entry = Entry::Overview;
  bool changePage = false;
  bool changed = false;
};

Position resolveEntry(uint32_t page, Entry entry, const PageAvailability& availability, bool panelsOnly);
Position normalizePosition(Position position, const PageAvailability& availability, bool panelsOnly);
Move next(Position position, uint32_t pageCount, const PageAvailability& availability, bool panelsOnly);
Move automaticNext(Position position, uint32_t pageCount, const PageAvailability& availability, bool panelsOnly);
Move previous(Position position, uint32_t pageCount, const PageAvailability& availability, bool panelsOnly);

}  // namespace manga
