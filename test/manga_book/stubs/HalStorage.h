#pragma once
#include <dirent.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
namespace storage_test {
extern int readCalls, nextCalls;
inline int handles = 0, implicitCloses = 0, overlaps = 0;
inline std::unordered_set<std::string> readers;
inline std::string failOpen, failRead, failSeek, failClose, failNext, failName;
void reset();
}  // namespace storage_test
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(const char* path);
  ~HalFile();
  HalFile(HalFile&& other) noexcept;
  HalFile& operator=(HalFile&& other) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  explicit operator bool() const { return file_ || dir_; }
  bool isDirectory() const { return dir_; }
  bool allocationFailed() const { return allocationFailed_; }
  bool enumerationFailed() const { return enumerationFailed_; }
  bool close();
  uint64_t fileSize64();
  bool seek64(uint64_t pos);
  int read(void* data, size_t count);
  size_t getName(char* name, size_t length);
  HalFile openNextFile();
  HalFile openNextFileChecked();

 private:
  std::string path_;
  FILE* file_ = nullptr;
  DIR* dir_ = nullptr;
  bool allocationFailed_ = false;
  bool enumerationFailed_ = false;
};
using FsFile = HalFile;
class HalStorage {
 public:
  static HalStorage& getInstance();
  HalFile open(const char* path) { return HalFile(path); }
  bool exists(const char* path);
};
#define Storage HalStorage::getInstance()
