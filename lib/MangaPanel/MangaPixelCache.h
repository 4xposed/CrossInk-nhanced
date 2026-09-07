#pragma once

#include <CooperativeCancellation.h>
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace manga {

enum PixelPolicyFlags : uint8_t {
  PixelPolicyGrayscale = 1u << 0,
  PixelPolicyDither = 1u << 1,
};

constexpr uint8_t kPixelPolicyAllowedMask = PixelPolicyGrayscale | PixelPolicyDither;

struct PixelIdentity {
  uint32_t sourcePathCrc = 0;
  uint32_t sourceCrc = 0;
  uint64_t sourceSize = 0;
  uint16_t sourceWidth = 0;
  uint16_t sourceHeight = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t screenWidth = 0;
  uint16_t screenHeight = 0;
  uint8_t orientation = 0;
  uint8_t flags = 0;
};

bool fingerprintImage(const char* sourcePath, PixelIdentity& identity, CooperativeCancellation cancellation = {});

class MangaPixelCache {
 public:
  MangaPixelCache() = default;
  ~MangaPixelCache();
  MangaPixelCache(const MangaPixelCache&) = delete;
  MangaPixelCache& operator=(const MangaPixelCache&) = delete;

  // Reads only the validated sidecar identity. The caller must still use
  // configure() and open() before treating the raw payload as renderable.
  static bool sourceIdentity(const std::string& bookFolder, uint32_t page, int16_t panel, PixelIdentity& out,
                             CooperativeCancellation cancellation = {});
  bool configure(const std::string& bookFolder, uint32_t page, int16_t panel, const PixelIdentity& identity,
                 CooperativeCancellation cancellation = {}) {
    return configure(bookFolder.c_str(), page, panel, identity, cancellation);
  }
  bool configure(const char* bookFolder, uint32_t page, int16_t panel, const PixelIdentity& identity,
                 CooperativeCancellation cancellation = {});
  const char* temporaryPath() const { return temporaryPath_; }
  bool open(CooperativeCancellation cancellation = {});
  bool publish(CooperativeCancellation cancellation = {});
  void discardTemporary();
  bool readRow(uint8_t* out, size_t capacity);
  bool rewind();
  void close();

 private:
  bool configured_ = false;
  PixelIdentity identity_{};
  uint16_t nextRow_ = 0;
  char finalPath_[128]{};
  char temporaryPath_[128]{};
  char identityPath_[128]{};
  char identityTemporaryPath_[128]{};
  FsFile reader_;
};

}  // namespace manga
