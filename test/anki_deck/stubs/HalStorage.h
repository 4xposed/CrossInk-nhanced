#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

class HalFile {
 public:
  inline static size_t readCalls = 0;
  inline static size_t writeCalls = 0;
  inline static size_t failWriteCall = 0;
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const std::filesystem::path& path, bool write) {
    close();
    stream_.open(path,
                 write ? (std::ios::binary | std::ios::out | std::ios::trunc) : (std::ios::binary | std::ios::in));
    return stream_.is_open();
  }
  int read(void* data, size_t count) {
    ++readCalls;
    stream_.read(static_cast<char*>(data), static_cast<std::streamsize>(count));
    return static_cast<int>(stream_.gcount());
  }
  size_t write(const void* data, size_t count) {
    ++writeCalls;
    if (failWriteCall && writeCalls == failWriteCall) return 0;
    stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(count));
    return stream_ ? count : 0;
  }
  bool seek64(uint64_t pos) {
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(pos));
    return static_cast<bool>(stream_);
  }
  bool seekSet(size_t pos) { return seek64(pos); }
  uint64_t fileSize64() {
    const auto old = stream_.tellg();
    stream_.clear();
    stream_.seekg(0, std::ios::end);
    const auto size = stream_.tellg();
    stream_.seekg(old);
    return static_cast<uint64_t>(size);
  }
  bool sync() {
    stream_.flush();
    return static_cast<bool>(stream_);
  }
  void flush() { stream_.flush(); }
  bool close() {
    if (stream_.is_open()) stream_.close();
    return true;
  }
  bool isOpen() const { return stream_.is_open(); }
  explicit operator bool() const { return isOpen(); }

 private:
  std::fstream stream_;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  static void setRoot(const std::filesystem::path& root) { getInstance().root_ = root; }
  static void resetOpenCounts() { getInstance().readOpenCount_ = 0; }
  static uint32_t readOpenCount() { return getInstance().readOpenCount_; }
  bool ensureDirectoryExists(const char* path) {
    std::error_code error;
    std::filesystem::create_directories(resolve(path), error);
    return !error;
  }
  bool exists(const char* path) { return std::filesystem::exists(resolve(path)); }
  bool remove(const char* path) {
    std::error_code error;
    return std::filesystem::remove(resolve(path), error);
  }
  bool rename(const char* oldPath, const char* newPath) {
    std::error_code error;
    std::filesystem::rename(resolve(oldPath), resolve(newPath), error);
    return !error;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    ++readOpenCount_;
    return file.open(resolve(path.c_str()), false);
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    ++readOpenCount_;
    return file.open(resolve(path), false);
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    return file.open(resolve(path.c_str()), true);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) { return file.open(resolve(path), true); }

 private:
  std::filesystem::path resolve(const char* path) const {
    std::filesystem::path input(path);
    return root_ / input.relative_path();
  }
  std::filesystem::path root_;
  uint32_t readOpenCount_ = 0;
};

#define Storage HalStorage::getInstance()
using FsFile = HalFile;
