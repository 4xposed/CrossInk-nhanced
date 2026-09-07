#pragma once
#include <dirent.h>
#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <string>
namespace mutation_test {
extern std::string root, failPath;
extern int calls, failAt, handles;
extern bool failAfterRename, offline, cutAfterRename, failReload;
extern int cutPhase;
extern std::string partialDeleteChild;
bool failure(const char*);
std::string mapped(const char*);
void reset(const std::string&);
}  // namespace mutation_test
using oflag_t = int;
class HalFile {
  int fd_ = -1;
  DIR* dir_ = nullptr;
  std::string path_;
  bool error_ = false;

 public:
  HalFile() = default;
  explicit HalFile(const char*, oflag_t = O_RDONLY);
  ~HalFile();
  HalFile(HalFile&&) noexcept;
  HalFile& operator=(HalFile&&) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  explicit operator bool() const { return fd_ >= 0 || dir_; }
  bool isOpen() const { return bool(*this); }
  bool isDirectory() const { return dir_; }
  bool close();
  bool sync();
  int read(void*, size_t);
  int read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  size_t write(const void*, size_t);
  size_t write(uint8_t b) { return write(&b, 1); }
  bool seek64(uint64_t);
  bool seekSet(size_t p) { return seek64(p); }
  bool seek(size_t p) { return seek64(p); }
  bool seekCur(int64_t);
  size_t position();
  uint64_t fileSize64();
  size_t fileSize() { return fileSize64(); }
  size_t size() { return fileSize64(); }
  bool truncate(uint64_t);
  size_t getName(char*, size_t);
  HalFile openNextFileChecked();
  HalFile openNextFile() { return openNextFileChecked(); }
  bool enumerationFailed() const { return error_; }
  bool allocationFailed() const { return false; }
};
using FsFile = HalFile;
class HalStorage {
 public:
  static HalStorage& getInstance();
  bool ready() const { return true; }
  HalFile open(const char* p, oflag_t f = O_RDONLY) { return HalFile(p, f); }
  bool exists(const char*);
  bool mkdir(const char*, bool = true);
  bool rmdir(const char*);
  bool remove(const char*);
  bool rename(const char*, const char*);
  bool removeDir(const char*);
  bool ensureDirectoryExists(const char* p) { return exists(p) || mkdir(p); }
  bool openFileForRead(const char*, const char* p, HalFile& f) {
    f = open(p);
    return bool(f);
  }
  bool openFileForWrite(const char*, const char* p, HalFile& f) {
    f = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    return bool(f);
  }
};
#define Storage HalStorage::getInstance()
