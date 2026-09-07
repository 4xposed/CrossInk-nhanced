#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hal_storage_test {
inline std::filesystem::path root;
inline std::string shortReadPath;
inline std::string shortWritePath;
inline std::string seekFailurePath;
inline std::string readOpenFailurePath;
inline std::string writeOpenFailurePath;
inline std::string closeFailurePath;
inline std::string syncFailurePath;
inline std::string removeFailurePath;
inline std::string directoryAllocationFailurePath;
inline size_t shortReadOffset = SIZE_MAX;
inline size_t shortWriteOffset = SIZE_MAX;
inline size_t directoryAllocationFailureAfterEntries = SIZE_MAX;
inline uint32_t directoryAllocationFailureOpenOrdinal = 1;
inline uint32_t closeFailureOrdinal = 1;
inline uint32_t matchingDirectoryOpenCount = 0;
inline uint32_t matchingCloseCount = 0;
inline uint32_t directoryAllocationFailureCount = 0;
inline uint32_t openCount = 0;
inline uint32_t closeCount = 0;
inline uint32_t readCount = 0;
inline std::map<std::string, size_t> readBytes;
inline std::map<std::string, size_t> activeReaders;
inline size_t maximumReadBytes = 0;
inline bool rejectDuplicateReaders = false;
inline void (*afterRead)() = nullptr;
inline bool writeOpenFailureCreatesFile = false;
inline std::vector<std::pair<std::string, std::string>> renameFailures;

inline void reset() {
  shortReadPath.clear();
  shortWritePath.clear();
  seekFailurePath.clear();
  readOpenFailurePath.clear();
  writeOpenFailurePath.clear();
  closeFailurePath.clear();
  syncFailurePath.clear();
  removeFailurePath.clear();
  directoryAllocationFailurePath.clear();
  shortReadOffset = SIZE_MAX;
  shortWriteOffset = SIZE_MAX;
  directoryAllocationFailureAfterEntries = SIZE_MAX;
  directoryAllocationFailureOpenOrdinal = 1;
  closeFailureOrdinal = 1;
  matchingDirectoryOpenCount = 0;
  matchingCloseCount = 0;
  directoryAllocationFailureCount = 0;
  openCount = 0;
  closeCount = 0;
  readCount = 0;
  readBytes.clear();
  maximumReadBytes = 0;
  rejectDuplicateReaders = false;
  afterRead = nullptr;
  writeOpenFailureCreatesFile = false;
  renameFailures.clear();
}
}  // namespace hal_storage_test

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  explicit operator bool() const { return isOpen() || directory_ != nullptr; }

  bool open(const std::filesystem::path& path, const std::string& firmwarePath, bool write = false) {
    close();
    writable_ = write;
    firmwarePath_ = firmwarePath;
    if ((write && firmwarePath_ == hal_storage_test::writeOpenFailurePath) ||
        (!write && firmwarePath_ == hal_storage_test::readOpenFailurePath)) {
      if (write && hal_storage_test::writeOpenFailureCreatesFile) {
        std::ofstream interrupted(path, std::ios::binary | std::ios::trunc);
      }
      return false;
    }
    if (!write && hal_storage_test::rejectDuplicateReaders && hal_storage_test::activeReaders[firmwarePath])
      return false;
    stream_.open(path,
                 write ? (std::ios::binary | std::ios::out | std::ios::trunc) : (std::ios::binary | std::ios::in));
    if (stream_.is_open()) {
      ++hal_storage_test::openCount;
      if (!write) ++hal_storage_test::activeReaders[firmwarePath_];
    }
    return stream_.is_open();
  }

  bool isOpen() const { return stream_.is_open(); }

  bool openDirectory(const std::filesystem::path& path, const std::string& firmwarePath) {
    close();
    if (!std::filesystem::is_directory(path)) return false;
    directory_ = std::make_unique<DirectoryState>();
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      directory_->entries.push_back(entry.path());
    }
    std::sort(directory_->entries.begin(), directory_->entries.end());
    firmwarePath_ = firmwarePath;
    if (firmwarePath_ == hal_storage_test::directoryAllocationFailurePath &&
        ++hal_storage_test::matchingDirectoryOpenCount == hal_storage_test::directoryAllocationFailureOpenOrdinal) {
      directory_->allocationFailureAfterEntries = hal_storage_test::directoryAllocationFailureAfterEntries;
    }
    return true;
  }

  bool isDirectory() const { return directory_ != nullptr; }

  void rewindDirectory() {
    if (directory_) directory_->index = 0;
  }

  HalFile openNextFile() {
    allocationFailed_ = false;
    HalFile result;
    if (!directory_ || directory_->index >= directory_->entries.size()) return result;
    if (directory_->index >= directory_->allocationFailureAfterEntries) {
      allocationFailed_ = true;
      ++hal_storage_test::directoryAllocationFailureCount;
      return result;
    }
    const auto path = directory_->entries[directory_->index++];
    const std::string childFirmwarePath = firmwarePath_ == "/" ? firmwarePath_ + path.filename().string()
                                                               : firmwarePath_ + "/" + path.filename().string();
    if (std::filesystem::is_directory(path)) {
      result.openDirectory(path, childFirmwarePath);
    } else {
      result.open(path, childFirmwarePath);
    }
    result.name_ = path.filename().string();
    return result;
  }

  bool allocationFailed() const { return allocationFailed_; }

  void getName(char* destination, size_t size) const {
    if (!destination || size == 0) return;
    const std::string& source = name_.empty() ? firmwarePath_ : name_;
    const size_t copied = std::min(size - 1, source.size());
    std::memcpy(destination, source.data(), copied);
    destination[copied] = '\0';
  }

  bool close() {
    bool failed = false;
    if (stream_.is_open()) {
      if (firmwarePath_ == hal_storage_test::closeFailurePath &&
          ++hal_storage_test::matchingCloseCount == hal_storage_test::closeFailureOrdinal) {
        failed = true;
      }
      if (!writable_) --hal_storage_test::activeReaders[firmwarePath_];
      stream_.close();
      ++hal_storage_test::closeCount;
    }
    directory_.reset();
    allocationFailed_ = false;
    return !failed;
  }

  size_t size() {
    const auto old = writable_ ? stream_.tellp() : stream_.tellg();
    stream_.clear();
    if (writable_)
      stream_.seekp(0, std::ios::end);
    else
      stream_.seekg(0, std::ios::end);
    const auto result = writable_ ? stream_.tellp() : stream_.tellg();
    if (writable_)
      stream_.seekp(old);
    else
      stream_.seekg(old);
    return result < 0 ? 0 : static_cast<size_t>(result);
  }

  size_t fileSize() { return size(); }
  uint64_t fileSize64() { return size(); }

  bool seek(size_t offset) {
    if (firmwarePath_ == hal_storage_test::seekFailurePath) return false;
    stream_.clear();
    if (writable_)
      stream_.seekp(static_cast<std::streamoff>(offset));
    else
      stream_.seekg(static_cast<std::streamoff>(offset));
    return static_cast<bool>(stream_);
  }

  bool seekSet(size_t offset) { return seek(offset); }

  size_t position() const {
    auto& stream = const_cast<std::fstream&>(stream_);
    const auto result = writable_ ? stream.tellp() : stream.tellg();
    return result < 0 ? 0 : static_cast<size_t>(result);
  }

  int available() const { return isOpen() && position() < const_cast<HalFile*>(this)->size(); }

  int read() {
    char value = 0;
    return read(&value, 1) == 1 ? static_cast<unsigned char>(value) : -1;
  }

  int read(void* destination, size_t count) {
    ++hal_storage_test::readCount;
    hal_storage_test::maximumReadBytes = std::max(hal_storage_test::maximumReadBytes, count);
    const auto position = stream_.tellg();
    if (firmwarePath_ == hal_storage_test::shortReadPath && position >= 0 &&
        static_cast<size_t>(position) >= hal_storage_test::shortReadOffset && count > 0) {
      --count;
    }
    stream_.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
    const int actual = static_cast<int>(stream_.gcount());
    hal_storage_test::readBytes[firmwarePath_] += actual;
    if (hal_storage_test::afterRead) hal_storage_test::afterRead();
    return actual;
  }

  size_t write(const void* source, size_t count) {
    size_t writeCount = count;
    const auto position = stream_.tellp();
    if (firmwarePath_ == hal_storage_test::shortWritePath && position >= 0 &&
        static_cast<size_t>(position) >= hal_storage_test::shortWriteOffset && writeCount > 0) {
      --writeCount;
    }
    stream_.write(static_cast<const char*>(source), static_cast<std::streamsize>(writeCount));
    return stream_ ? writeCount : 0;
  }

  bool sync() {
    if (firmwarePath_ == hal_storage_test::syncFailurePath) return false;
    stream_.flush();
    return static_cast<bool>(stream_);
  }

 private:
  struct DirectoryState {
    std::vector<std::filesystem::path> entries;
    size_t index = 0;
    size_t allocationFailureAfterEntries = SIZE_MAX;
  };

  std::fstream stream_;
  std::string firmwarePath_;
  std::string name_;
  bool writable_ = false;
  bool allocationFailed_ = false;
  std::unique_ptr<DirectoryState> directory_;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  static void setRoot(const std::filesystem::path& root) { hal_storage_test::root = root; }

  bool exists(const char* path) { return std::filesystem::exists(resolve(path)); }

  HalFile open(const char* path) {
    HalFile file;
    const auto resolved = resolve(path);
    if (std::filesystem::is_directory(resolved)) {
      file.openDirectory(resolved, path);
    } else {
      file.open(resolved, path);
    }
    return file;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.open(resolve(path), path); }

  bool openFileForRead(const char* module, const std::string& path, HalFile& file) {
    return openFileForRead(module, path.c_str(), file);
  }

  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    std::filesystem::create_directories(resolve(path).parent_path());
    return file.open(resolve(path), path, true);
  }

  bool openFileForWrite(const char* module, const std::string& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }

  bool remove(const char* path) {
    if (hal_storage_test::removeFailurePath == path) return false;
    return std::filesystem::remove(resolve(path));
  }

  bool rename(const char* oldPath, const char* newPath) {
    if (std::find(hal_storage_test::renameFailures.begin(), hal_storage_test::renameFailures.end(),
                  std::pair<std::string, std::string>{oldPath, newPath}) != hal_storage_test::renameFailures.end()) {
      return false;
    }
    std::error_code error;
    std::filesystem::rename(resolve(oldPath), resolve(newPath), error);
    return !error;
  }

 private:
  static std::filesystem::path resolve(const char* path) {
    return hal_storage_test::root / std::filesystem::path(path).relative_path();
  }
};

#define Storage HalStorage::getInstance()
