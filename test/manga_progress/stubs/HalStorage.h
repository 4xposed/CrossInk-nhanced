#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace manga_progress_test {
enum class Failure { None, OpenRead, OpenWrite, Read, Write, Sync, Close, Remove, Rename };
void reset(const std::filesystem::path& root);
void failNext(Failure failure);
void failRenameOnCall(int call);
int openHandles();
int implicitCloses();
}  // namespace manga_progress_test

class HalFile {
 public:
  HalFile() = default;
  ~HalFile();
  HalFile(HalFile&& other) noexcept;
  HalFile& operator=(HalFile&& other) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  int read(void* data, size_t count);
  size_t write(const void* data, size_t count);
  uint64_t fileSize64();
  bool sync();
  bool close();
  bool isOpen() const { return stream_.is_open(); }
  explicit operator bool() const { return isOpen(); }

 private:
  friend class HalStorage;
  bool open(const std::filesystem::path& path, bool write);
  std::fstream stream_;
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance();
  bool ensureDirectoryExists(const char* path);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool openFileForRead(const char*, const char* path, HalFile& file);
  bool openFileForRead(const char*, const std::string& path, HalFile& file);
  bool openFileForWrite(const char*, const char* path, HalFile& file);
  bool openFileForWrite(const char*, const std::string& path, HalFile& file);

 private:
  friend void manga_progress_test::reset(const std::filesystem::path& root);
  std::filesystem::path resolve(const char* path) const;
  std::filesystem::path root_;
};

#define Storage HalStorage::getInstance()
