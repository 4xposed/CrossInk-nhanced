#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  const char* languageCode;
  StrId label;
  uint16_t persistedBit;
};

// Keep the original persisted bits when removing or reordering layouts.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, "EN", StrId::STR_KEYBOARD_LANGUAGE_EN, 1u << 0},
    {freeink::ui::KeyboardLayoutId::QwertzDe, "DE", StrId::STR_KEYBOARD_LANGUAGE_DE, 1u << 2},
    {freeink::ui::KeyboardLayoutId::SpanishEs, "ES", StrId::STR_KEYBOARD_LANGUAGE_ES, 1u << 3},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "keyboard layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return i < COUNT ? ALL[i].persistedBit : 0; }
inline constexpr uint16_t LATIN_BITS = bitAt(0) | bitAt(1) | bitAt(2);

uint16_t enabled();
freeink::ui::KeyboardLayoutId startingLayout();
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
