#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
namespace storage_test {
extern bool failWrite, failSync, failClose;
}
using oflag_t = int;
constexpr int O_RDONLY = 0, O_WRITE = 1, O_CREAT = 2, O_TRUNC = 4;
class HalFile {
 public:
  HalFile() = default;
  HalFile(const char*, int = O_RDONLY);
  ~HalFile();
  HalFile(HalFile&&) noexcept;
  HalFile& operator=(HalFile&&) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  explicit operator bool() const { return file_ != nullptr; }
  bool isOpen() const { return file_ != nullptr; }
  bool close();
  uint64_t fileSize64();
  bool seek(size_t);
  bool seek64(uint64_t);
  bool seekSet(size_t p) { return seek(p); }
  bool seekCur(int64_t);
  int read(void*, size_t);
  int read();
  size_t write(const void*, size_t);
  size_t write(uint8_t);
  bool sync();

 private:
  FILE* file_ = nullptr;
};
using FsFile = HalFile;
class HalStorage {
 public:
  static HalStorage& getInstance();
  bool openFileForRead(const char*, const char*, HalFile&);
  bool openFileForWrite(const char*, const char*, HalFile&);
  bool remove(const char*);
};
#define Storage HalStorage::getInstance()
