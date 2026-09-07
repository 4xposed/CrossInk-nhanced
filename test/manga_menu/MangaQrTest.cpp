#include <Memory.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MangaQrPayload.h"
using namespace manga;
std::vector<uint8_t> fixture(const std::vector<std::vector<std::string>>& panels) {
  std::vector<uint8_t> bytes{static_cast<uint8_t>(panels.size()), 0};
  const auto word = [&](size_t v) {
    bytes.push_back(v & 255);
    bytes.push_back(v >> 8);
  };
  for (const auto& texts : panels) {
    for (int v : {0, 0, 100, 200}) word(v);
    bytes.push_back(texts.size());
    bytes.push_back(0);
    word(6);
    for (char c : std::string("SECRET")) bytes.push_back(c);
    for (const auto& text : texts) {
      for (int v : {1, 2, 3, 4}) word(v);
      word(text.size());
      bytes.insert(bytes.end(), text.begin(), text.end());
    }
  }
  return bytes;
}
int main() {
  format::PageView page;
  QrUtils::OwnedPayload out;
  auto b = fixture({{"one", "猫"}, {"two"}});
  assert(format::decodePage(b, page) == format::Error::None);
  assert(buildQrPayload(page, -1, out) == QrPayloadResult::Ready);
  assert(std::string(out.bytes.get(), out.length) == "one\n猫\ntwo");
  assert(buildQrPayload(page, 1, out) == QrPayloadResult::Ready);
  assert(std::string(out.bytes.get(), out.length) == "two");
  assert(buildQrPayload(page, 2, out) == QrPayloadResult::Malformed && !out.bytes);
  for (const std::string tail : {std::string("é"), std::string("猫"), std::string("𠮷")}) {
    b = fixture({{std::string(2952, 'a') + tail + "end"}});
    assert(format::decodePage(b, page) == format::Error::None);
    assert(buildQrPayload(page, -1, out) == QrPayloadResult::Ready && out.truncated && out.length == 2952);
    assert(out.bytes[out.length] == '\0');
  }
  b = fixture({{std::string(2953, 'a')}});
  assert(format::decodePage(b, page) == format::Error::None);
  assert(buildQrPayload(page, -1, out) == QrPayloadResult::Ready && !out.truncated && out.length == 2953);
  b = fixture({{std::string(3000, 'a') + std::string("\xc0\x80", 2)}});
  assert(format::decodePage(b, page) == format::Error::None);
  assert(buildQrPayload(page, -1, out) == QrPayloadResult::Malformed && !out.bytes);
  b = fixture({{""}, {""}});
  assert(format::decodePage(b, page) == format::Error::None);
  assert(buildQrPayload(page, -1, out) == QrPayloadResult::Empty && !out.bytes);
  b = fixture({{"hello"}});
  assert(format::decodePage(b, page) == format::Error::None);
  dict_memory_test::reset();
  dict_memory_test::rejectAll = true;
  assert(buildQrPayload(page, -1, out) == QrPayloadResult::OutOfMemory && !out.bytes);
  dict_memory_test::reset();
  assert(buildQrPayload(page, -1, out) == QrPayloadResult::Ready);
  assert(dict_memory_test::largestRequest <= 2954);
  b.clear();
  assert(std::string(out.bytes.get(), out.length) == "hello");
  puts("Manga QR scope/UTF-8/cap/OOM contracts passed");
}
