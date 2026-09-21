#pragma once
#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
namespace storage_test {
inline std::string root;
inline bool failWrite = false, failRead = false, failSync = false;
inline int openFiles = 0;
}  // namespace storage_test
constexpr int O_RDONLY = 0, O_RDWR = 1, O_CREAT = 2, O_TRUNC = 4;
class FsFile {
  FILE* file = nullptr;
  DIR* dir = nullptr;
  std::string path;
  bool failed = false;

 public:
  FsFile() = default;
  explicit FsFile(const std::string& p, int flags = 0) : path(p) {
    struct stat st{};
    if (lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return;
    if (std::filesystem::is_directory(p))
      dir = opendir(p.c_str());
    else
      file = fopen(p.c_str(), (flags & O_TRUNC) ? "w+b" : (flags & O_RDWR) ? "r+b" : "rb");
    if (file || dir) ++storage_test::openFiles;
  }
  ~FsFile() { close(); }
  FsFile(FsFile&& other) { *this = std::move(other); }
  FsFile& operator=(FsFile&& other) {
    if (this != &other) {
      close();
      file = std::exchange(other.file, nullptr);
      dir = std::exchange(other.dir, nullptr);
      path = std::move(other.path);
      failed = other.failed;
    }
    return *this;
  }
  explicit operator bool() const { return file || dir; }
  bool close() {
    bool ok = true;
    if (file || dir) --storage_test::openFiles;
    if (file) ok = fclose(file) == 0;
    if (dir) ok = closedir(dir) == 0;
    file = nullptr;
    dir = nullptr;
    return ok;
  }
  bool isDirectory() const { return dir; }
  bool seek(size_t p) { return file && fseek(file, p, SEEK_SET) == 0; }
  size_t position() { return file ? ftell(file) : 0; }
  size_t write(const void* p, size_t n) { return file && !storage_test::failWrite ? fwrite(p, 1, n, file) : 0; }
  int read(void* p, size_t n) { return file && !storage_test::failRead ? fread(p, 1, n, file) : -1; }
  bool sync() { return file && !storage_test::failSync && fflush(file) == 0; }
  size_t getName(char* p, size_t n) {
    const auto name = std::filesystem::path(path).filename().string();
    if (n) {
      std::strncpy(p, name.c_str(), n - 1);
      p[n - 1] = 0;
    }
    return name.size();
  }
  bool enumerationFailed() const { return failed; }
  bool allocationFailed() const { return false; }
  FsFile openNextFileChecked() {
    while (dir) {
      errno = 0;
      auto* entry = readdir(dir);
      if (!entry) {
        failed = errno != 0;
        return {};
      }
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
      const auto child = path + '/' + entry->d_name;
      struct stat st{};
      if (!lstat(child.c_str(), &st) && S_ISLNK(st.st_mode)) continue;
      FsFile next(child);
      if (!next) failed = true;
      return next;
    }
    return {};
  }
};
class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  FsFile open(const char* p, int flags = 0) { return FsFile(storage_test::root + p, flags); }
  bool ensureDirectoryExists(const char* p) {
    std::error_code ec;
    std::filesystem::create_directories(storage_test::root + p, ec);
    return !ec;
  }
  bool remove(const char* p) { return std::filesystem::remove(storage_test::root + p); }
  bool exists(const char* p) { return std::filesystem::exists(storage_test::root + p); }
};
#define Storage HalStorage::getInstance()
