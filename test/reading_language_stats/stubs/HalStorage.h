#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Print.h"
namespace storage_test {
extern std::string root;
extern bool failWrite, failSync, failRename;
extern int openFiles;
extern std::string failWritePath;
extern int bookWriteOpens;
extern int growOnReadOpenCall;
extern bool emulate32BitSize;
extern int openReadCalls, failOpenReadCall, readCalls, failReadCall;
extern int writeCalls, syncCalls, closeCalls, failWriteCall, failSyncCall, failCloseCall;
extern int failRenameCall, renameCalls, failRenameFromCall;
std::string mapped(const char*);
}  // namespace storage_test
using oflag_t = int;
constexpr int O_RDONLY = 0, O_WRITE = 1, O_CREAT = 2, O_TRUNC = 4;
class HalFile : public Print {
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
  size_t fileSize() { return storage_test::emulate32BitSize ? static_cast<uint32_t>(fileSize64()) : fileSize64(); }
  bool seekSet(size_t p) { return seek64(p); }
  size_t size() { return fileSize64(); }
  bool seek64(uint64_t);
  bool seek(size_t p) { return seek64(p); }
  bool seekCur(int64_t);
  int read(void*, size_t);
  int read();
  size_t write(const void*, size_t);
  size_t write(uint8_t) override;
  size_t write(const uint8_t* p, size_t n) override { return write(static_cast<const void*>(p), n); }
  size_t position() { return file_ ? ftello(file_) : 0; }
  size_t available() { return size() - position(); }
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
  std::vector<std::string> listFiles(const char*, int maxFiles = 200);
  bool ensureDirectoryExists(const char* p) { return exists(p) || mkdir(p); }
  bool openFileForRead(const char* t, const std::string& p, HalFile& f) { return openFileForRead(t, p.c_str(), f); }
  bool openFileForWrite(const char* t, const std::string& p, HalFile& f) { return openFileForWrite(t, p.c_str(), f); }
  bool remove(const char*);
  bool rename(const char*, const char*);
  bool openFileForRead(const char*, const char*, HalFile&);
  bool openFileForWrite(const char*, const char*, HalFile&);
};
#define Storage HalStorage::getInstance()
