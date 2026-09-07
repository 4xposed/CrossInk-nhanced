#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "QrCodePolicy.h"
extern "C" {
#include <qrcode.h>
}
int main() {
  const size_t sizes[] = {1, 78, 79, 271, 272, 858, 859, 1732, 1733, 2953, 2954};
  const int versions[] = {4, 4, 10, 10, 20, 20, 30, 30, 40, 40, 0};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
    const int version = QrUtils::versionForBytes(sizes[i]);
    assert(version == versions[i]);
    if (!version) continue;
    // Every capacity transition reaches the real byte-mode encoder with mixed
    // 1/2/3/4-byte UTF-8, not only an independently tested payload builder.
    std::string input;
    constexpr std::string_view mixed = "aé漢😀";
    while (input.size() + mixed.size() <= sizes[i]) input += mixed;
    input.append(sizes[i] - input.size(), 'a');
    const size_t gridSize = qrcode_getBufferSize(version);
    std::vector<uint8_t> grid(gridSize + 16, 0x5a);
    QRCode code{};
    assert(qrcode_initBytes(&code, grid.data(), version, ECC_LOW, reinterpret_cast<uint8_t*>(input.data()),
                            input.size()) == 0);
    assert(code.mode == MODE_BYTE && code.size == 17 + 4 * version);
    std::vector<uint8_t> repeated(grid.size(), 0x5a);
    QRCode second{};
    assert(qrcode_initBytes(&second, repeated.data(), version, ECC_LOW, reinterpret_cast<uint8_t*>(input.data()),
                            input.size()) == 0);
    assert(repeated == grid);
    for (size_t byte = gridSize; byte < grid.size(); ++byte) assert(grid[byte] == 0x5a);
  }
  assert(QrUtils::versionForBytes(0) == 0);
  assert(QrUtils::gridBytesForPayload(2953) == 3917);
  puts("QR byte capacities and pinned encoder passed");
}
