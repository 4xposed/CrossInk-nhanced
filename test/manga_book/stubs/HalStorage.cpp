#include "HalStorage.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
namespace storage_test {
int readCalls = 0, nextCalls = 0;
void reset() {
  readCalls = nextCalls = 0;
  handles = implicitCloses = overlaps = 0;
  readers.clear();
  failOpen.clear();
  failRead.clear();
  failSeek.clear();
  failClose.clear();
  failNext.clear();
  failName.clear();
}
}  // namespace storage_test
HalFile::HalFile(const char* path) : path_(path) {
  if (path_ == storage_test::failOpen) return;
  struct stat st{};
  if (stat(path, &st)) return;
  if (S_ISDIR(st.st_mode))
    dir_ = opendir(path);
  else {
    if (storage_test::readers.contains(path_)) {
      ++storage_test::overlaps;
      return;
    }
    file_ = fopen(path, "rb");
    if (file_) storage_test::readers.insert(path_);
  }
  if (*this) ++storage_test::handles;
}
HalFile::~HalFile() {
  if (*this) {
    ++storage_test::implicitCloses;
    close();
  }
}
HalFile::HalFile(HalFile&& other) noexcept { *this = std::move(other); }
HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this != &other) {
    if (*this) {
      ++storage_test::implicitCloses;
      close();
    }
    path_ = std::move(other.path_);
    file_ = std::exchange(other.file_, nullptr);
    dir_ = std::exchange(other.dir_, nullptr);
    allocationFailed_ = std::exchange(other.allocationFailed_, false);
    enumerationFailed_ = std::exchange(other.enumerationFailed_, false);
  }
  return *this;
}
bool HalFile::close() {
  bool ok = path_ != storage_test::failClose;
  if (file_) {
    ok = fclose(file_) == 0 && ok;
    file_ = nullptr;
    storage_test::readers.erase(path_);
    --storage_test::handles;
  }
  if (dir_) {
    ok = closedir(dir_) == 0 && ok;
    dir_ = nullptr;
    --storage_test::handles;
  }
  allocationFailed_ = false;
  enumerationFailed_ = false;
  return ok;
}
uint64_t HalFile::fileSize64() {
  struct stat st{};
  return file_ && !fstat(fileno(file_), &st) ? st.st_size : 0;
}
bool HalFile::seek64(uint64_t pos) {
  return file_ && path_ != storage_test::failSeek && fseeko(file_, pos, SEEK_SET) == 0;
}
int HalFile::read(void* data, size_t count) {
  ++storage_test::readCalls;
  if (!file_) return -1;
  if (path_ == storage_test::failRead && count) --count;
  return static_cast<int>(fread(data, 1, count, file_));
}
size_t HalFile::getName(char* name, size_t length) {
  if (path_ == storage_test::failName) return 0;
  const auto slash = path_.find_last_of('/');
  const std::string base = path_.substr(slash == std::string::npos ? 0 : slash + 1);
  if (!length) return 0;
  const size_t copied = std::min(length - 1, base.size());
  std::memcpy(name, base.data(), copied);
  name[copied] = 0;
  return copied;
}
HalFile HalFile::openNextFile() {
  ++storage_test::nextCalls;
  if (path_ == storage_test::failNext) {
    allocationFailed_ = true;
    return {};
  }
  while (dir_) {
    const auto* entry = readdir(dir_);
    if (!entry) break;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    return HalFile((path_ + '/' + entry->d_name).c_str());
  }
  return {};
}
HalFile HalFile::openNextFileChecked() {
  if (enumerationFailed_) return {};
  if (!dir_ || path_ == storage_test::failNext) {
    enumerationFailed_ = true;
    return {};
  }
  while (true) {
    errno = 0;
    const auto* entry = readdir(dir_);
    if (!entry) {
      enumerationFailed_ = errno != 0;
      return {};
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    HalFile child((path_ + '/' + entry->d_name).c_str());
    if (!child) enumerationFailed_ = true;
    return child;
  }
}
HalStorage& HalStorage::getInstance() {
  static HalStorage storage;
  return storage;
}
bool HalStorage::exists(const char* path) {
  struct stat st{};
  return stat(path, &st) == 0;
}
