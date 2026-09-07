#include "MangaNavigation.h"

#include <algorithm>

namespace manga {
namespace {

constexpr uint16_t kMaxPanels = 255;

int16_t firstCrop(const PageAvailability& availability) {
  const uint16_t count = std::min(availability.panelCount, kMaxPanels);
  for (uint16_t panel = 0; panel < count; ++panel) {
    if (availability.hasCrop(panel)) return static_cast<int16_t>(panel);
  }
  return -1;
}

int16_t lastCrop(const PageAvailability& availability) {
  const uint16_t count = std::min(availability.panelCount, kMaxPanels);
  for (uint16_t panel = count; panel > 0; --panel) {
    if (availability.hasCrop(panel - 1)) return static_cast<int16_t>(panel - 1);
  }
  return -1;
}

Move unchanged(const Position position) { return {position, Entry::Overview, false, false}; }

Move changePage(const Position position, const uint32_t page, const Entry entry) {
  return {{page, -1}, entry, true, page != position.page};
}

}  // namespace

bool PageAvailability::hasCrop(const uint16_t panel) const {
  if (panel >= kMaxPanels) return false;
  return (crops[panel / 8] & static_cast<uint8_t>(1U << (panel % 8))) != 0;
}

void PageAvailability::setCrop(const uint16_t panel) {
  if (panel >= kMaxPanels) return;
  crops[panel / 8] |= static_cast<uint8_t>(1U << (panel % 8));
}

Position resolveEntry(const uint32_t page, const Entry entry, const PageAvailability& availability,
                      const bool panelsOnly) {
  if (entry == Entry::Overview && availability.overview && !panelsOnly) return {page, -1};

  const int16_t panel = entry == Entry::LastPanel ? lastCrop(availability) : firstCrop(availability);
  if (panel >= 0) return {page, panel};
  return {page, -1};
}

Position normalizePosition(Position position, const PageAvailability& availability, const bool panelsOnly) {
  if (position.panel >= 0 && availability.hasCrop(static_cast<uint16_t>(position.panel)) &&
      static_cast<uint16_t>(position.panel) < availability.panelCount) {
    return position;
  }
  return resolveEntry(position.page, Entry::Overview, availability, panelsOnly);
}

Move next(Position position, const uint32_t pageCount, const PageAvailability& availability, const bool panelsOnly) {
  if (pageCount == 0 || position.page >= pageCount) return unchanged(position);
  position = normalizePosition(position, availability, panelsOnly);

  const uint16_t count = std::min(availability.panelCount, kMaxPanels);
  const uint16_t start = position.panel < 0 ? 0 : static_cast<uint16_t>(position.panel + 1);
  for (uint16_t panel = start; panel < count; ++panel) {
    if (availability.hasCrop(panel))
      return {{position.page, static_cast<int16_t>(panel)}, Entry::FirstPanel, false, true};
  }

  if (position.page + 1 >= pageCount) return unchanged(position);
  const Entry entry = panelsOnly || !availability.overview ? Entry::FirstPanel : Entry::Overview;
  return changePage(position, position.page + 1, entry);
}

Move automaticNext(const Position position, const uint32_t pageCount, const PageAvailability& availability,
                   const bool panelsOnly) {
  if (position.panel >= 0) return next(position, pageCount, availability, panelsOnly);
  if (!pageCount || position.page >= pageCount - 1) return unchanged(position);
  return changePage(position, position.page + 1, Entry::Overview);
}

Move previous(Position position, const uint32_t pageCount, const PageAvailability& availability,
              const bool panelsOnly) {
  if (pageCount == 0 || position.page >= pageCount) return unchanged(position);
  position = normalizePosition(position, availability, panelsOnly);

  if (position.panel >= 0) {
    for (int16_t panel = static_cast<int16_t>(position.panel - 1); panel >= 0; --panel) {
      if (availability.hasCrop(static_cast<uint16_t>(panel))) {
        return {{position.page, panel}, Entry::LastPanel, false, true};
      }
    }
    if (availability.overview && !panelsOnly) return {{position.page, -1}, Entry::Overview, false, true};
    if (position.page == 0) return unchanged(position);
    return changePage(position, position.page - 1, Entry::LastPanel);
  }

  if (position.page == 0) return unchanged(position);
  return changePage(position, position.page - 1, panelsOnly ? Entry::LastPanel : Entry::Overview);
}

}  // namespace manga
