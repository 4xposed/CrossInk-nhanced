#pragma once
#include <cstdio>
#include <string>
namespace storage_test {
extern std::string root;
extern bool failWrite, failSync, failRename;
extern int openFiles;
extern bool failClose, failOpenWrite;
extern size_t writtenBytes, failAfterBytes, readBytes;
extern void (*onSync)();
extern int failRenameCall, renameCalls;
std::string mapped(const char*);
}  // namespace storage_test
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
  explicit operator bool() const { return file_ || dir_; }
  bool isOpen() const { return bool(*this); }
  bool isDirectory() const { return dir_; }
  bool allocationFailed() const { return false; }
  bool close();
  uint64_t fileSize64();
  size_t size() { return fileSize64(); }
  bool seek64(uint64_t);
  bool seek(size_t p) { return seek64(p); }
  bool seekCur(int64_t);
  int read(void*, size_t);
  int read();
  size_t write(const void*, size_t);
  size_t write(uint8_t);
  bool sync();
  size_t getName(char*, size_t);
  HalFile openNextFile();

 private:
  std::string path_;
  FILE* file_ = nullptr;
  void* dir_ = nullptr;
  bool counted_ = false;
};
using FsFile = HalFile;
class HalStorage {
 public:
  static HalStorage& getInstance();
  HalFile open(const char*, oflag_t = O_RDONLY);
  bool exists(const char*);
  bool mkdir(const char*, bool = true);
  bool remove(const char*);
  bool rename(const char*, const char*);
  bool openFileForRead(const char*, const char*, HalFile&);
  bool openFileForWrite(const char*, const char*, HalFile&);
  bool openFileForRead(const char* t, const std::string& p, HalFile& f) { return openFileForRead(t, p.c_str(), f); }
  bool openFileForWrite(const char* t, const std::string& p, HalFile& f) { return openFileForWrite(t, p.c_str(), f); }
};
#define Storage HalStorage::getInstance()
