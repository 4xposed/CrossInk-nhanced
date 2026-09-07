#include <GfxRenderer.h>
#include <Memory.h>

#include <cassert>
#include <cstdio>
#include <string>

#include "QrUtils.h"
int main() {
  GfxRenderer renderer;
  const Rect bounds{2, 3, 230, 300};
  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;
  assert(QrUtils::drawQrCode(renderer, bounds, "hello", 5) == QrUtils::DrawResult::OutOfMemory);
  assert(renderer.calls == 0);
  dict_memory_test::reset();
  assert(QrUtils::drawQrCode(renderer, bounds, nullptr, 5) == QrUtils::DrawResult::InvalidPayload);
  assert(QrUtils::drawQrCode(renderer, bounds, "hello", 2954) == QrUtils::DrawResult::InvalidPayload);
  assert(dict_memory_test::requestCount == 0 && renderer.calls == 0);
  assert(QrUtils::drawQrCode(renderer, {0, 0, 10, 10}, "hello", 5) == QrUtils::DrawResult::InvalidBounds);
  uint8_t small[1];
  assert(QrUtils::drawQrCode(renderer, bounds, "hello", 5, small, 1) == QrUtils::DrawResult::OutOfMemory);
  std::string max(2953, 'a');
  assert(QrUtils::drawQrCode(renderer, bounds, max.data(), max.size()) == QrUtils::DrawResult::Drawn);
  assert(dict_memory_test::largestRequest == 3917);
  assert(renderer.pixel(2, 3));  // White quiet zone in the bounded rectangle.
  // Generic QR byte API accepts arbitrary bytes; Manga alone validates UTF-8.
  const char binary[] = {char(0xff), 0, 'a'};
  assert(QrUtils::drawQrCode(renderer, bounds, binary, 3) == QrUtils::DrawResult::Drawn);
  puts("QR checked draw bounds/capacity/OOM/binary contracts passed");
}
