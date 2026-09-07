#include <HalStorage.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>

#include "ScreenshotUtil.h"
int main() {
  char temp[] = "/tmp/crossink-screenshot10e-XXXXXX";
  storage_test::root = mkdtemp(temp);
  std::filesystem::create_directories(storage_test::root + "/.crosspoint");
  std::array<uint8_t, 8> pixels{0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55};
  constexpr const char* path = "/.crosspoint/shot.bmp";
  storage_test::failCloseCall = storage_test::closeCalls + 1;
  assert(!ScreenshotUtil::saveFramebufferAsBmp(path, pixels.data(), 8, 8));
  assert(!Storage.exists(path));
  storage_test::failCloseCall = 0;
  storage_test::failWrite = true;
  assert(!ScreenshotUtil::saveFramebufferAsBmp(path, pixels.data(), 8, 8));
  assert(!Storage.exists(path));
  storage_test::failWrite = false;
  assert(ScreenshotUtil::saveFramebufferAsBmp(path, pixels.data(), 8, 8));
  assert(std::filesystem::file_size(storage_test::mapped(path)) == 94);
  assert(storage_test::openFiles == 0);
  std::filesystem::remove_all(storage_test::root);
  puts("Actual screenshot writer rejects close/write failures and retries");
}
