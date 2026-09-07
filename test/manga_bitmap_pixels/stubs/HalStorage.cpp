#include "HalStorage.h"

#include <utility>

namespace storage_test {
bool failWrite = false;
bool failSync = false;
bool failClose = false;
}  // namespace storage_test

HalFile::HalFile(const char* path, int flags) { file_ = fopen(path, (flags & O_WRITE) ? "w+b" : "rb"); }
HalFile::~HalFile() {
  if (file_) fclose(file_);
}
HalFile::HalFile(HalFile&& other) noexcept { *this = std::move(other); }
HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this != &other) {
    if (file_) fclose(file_);
    file_ = std::exchange(other.file_, nullptr);
  }
  return *this;
}
bool HalFile::close() {
  if (!file_) return true;
  const bool closeOk = fclose(file_) == 0;
  file_ = nullptr;
  return closeOk && !storage_test::failClose;
}
uint64_t HalFile::fileSize64() {
  if (!file_) return 0;
  const off_t current = ftello(file_);
  if (current < 0 || fseeko(file_, 0, SEEK_END) != 0) return 0;
  const off_t end = ftello(file_);
  if (fseeko(file_, current, SEEK_SET) != 0 || end < 0) return 0;
  return static_cast<uint64_t>(end);
}
bool HalFile::seek(size_t pos) { return file_ && fseeko(file_, static_cast<off_t>(pos), SEEK_SET) == 0; }
bool HalFile::seek64(uint64_t pos) { return file_ && fseeko(file_, static_cast<off_t>(pos), SEEK_SET) == 0; }
bool HalFile::seekCur(int64_t offset) { return file_ && fseeko(file_, static_cast<off_t>(offset), SEEK_CUR) == 0; }
int HalFile::read(void* data, size_t size) { return file_ ? static_cast<int>(fread(data, 1, size, file_)) : -1; }
int HalFile::read() { return file_ ? fgetc(file_) : -1; }
size_t HalFile::write(const void* data, size_t size) {
  return file_ && !storage_test::failWrite ? fwrite(data, 1, size, file_) : 0;
}
size_t HalFile::write(uint8_t byte) { return write(&byte, 1); }
bool HalFile::sync() { return file_ && !storage_test::failSync && fflush(file_) == 0; }
HalStorage& HalStorage::getInstance() {
  static HalStorage storage;
  return storage;
}
bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  file = HalFile(path);
  return bool(file);
}
bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  file = HalFile(path, O_WRITE | O_CREAT | O_TRUNC);
  return bool(file);
}
bool HalStorage::remove(const char* path) { return std::remove(path) == 0; }
