#include "KeyboardLayoutSet.h"

#include <cstring>

#include "CrossPointSettings.h"

namespace keyboard_layouts {
namespace {

uint8_t indexOf(const freeink::ui::KeyboardLayoutId id) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].id == id) return i;
  }
  return COUNT;
}

freeink::ui::KeyboardLayoutId forLanguage(const Language language) {
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (strcmp(ALL[i].languageCode, LANGUAGE_CODES[static_cast<uint8_t>(language)]) == 0) return ALL[i].id;
  }
  return freeink::ui::KeyboardLayoutId::QwertyEn;
}

constexpr uint16_t ALL_BITS = LATIN_BITS;

uint16_t layoutBit(const freeink::ui::KeyboardLayoutId id) {
  const uint8_t i = indexOf(id);
  return i < COUNT ? bitAt(i) : 0;
}

}  // namespace

uint16_t enabled() {
  const uint16_t configured = static_cast<uint16_t>(SETTINGS.keyboardLayouts & ALL_BITS);
  if (configured != 0) return configured;

  return static_cast<uint16_t>(layoutBit(forLanguage(I18N.getLanguage())) |
                               layoutBit(freeink::ui::KeyboardLayoutId::QwertyEn));
}

freeink::ui::KeyboardLayoutId startingLayout() {
  const auto preferred = forLanguage(I18N.getLanguage());
  if (enabled() & layoutBit(preferred)) return preferred;
  return next(preferred);
}

freeink::ui::KeyboardLayoutId next(const freeink::ui::KeyboardLayoutId current) {
  const uint16_t mask = enabled();
  const uint8_t from = indexOf(current);
  const uint8_t start = from < COUNT ? from : 0;
  for (uint8_t step = 1; step <= COUNT; ++step) {
    const uint8_t i = static_cast<uint8_t>((start + step) % COUNT);
    if (mask & bitAt(i)) return ALL[i].id;
  }
  return current;
}

}  // namespace keyboard_layouts
