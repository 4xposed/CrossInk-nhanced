#include "HalStorage.h"

#include <utility>

namespace {
manga_progress_test::Failure nextFailure = manga_progress_test::Failure::None;
int handles = 0;
int implicitCloseCount = 0;
int renameFailureCountdown = 0;

bool consume(const manga_progress_test::Failure wanted) {
  if (nextFailure != wanted) return false;
  nextFailure = manga_progress_test::Failure::None;
  return true;
}
}  // namespace

namespace manga_progress_test {
void reset(const std::filesystem::path& root) {
  HalStorage::getInstance().root_ = root;
  nextFailure = Failure::None;
  handles = 0;
  implicitCloseCount = 0;
  renameFailureCountdown = 0;
}
void failNext(const Failure failure) { nextFailure = failure; }
void failRenameOnCall(const int call) { renameFailureCountdown = call; }
int openHandles() { return handles; }
int implicitCloses() { return implicitCloseCount; }
}  // namespace manga_progress_test

HalFile::~HalFile() {
  if (isOpen()) {
    ++implicitCloseCount;
    close();
  }
}

HalFile::HalFile(HalFile&& other) noexcept : stream_(std::move(other.stream_)) {}

HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this != &other) {
    if (isOpen()) {
      ++implicitCloseCount;
      close();
    }
    stream_ = std::move(other.stream_);
  }
  return *this;
}

bool HalFile::open(const std::filesystem::path& path, const bool write) {
  stream_.open(path, write ? (std::ios::binary | std::ios::out | std::ios::trunc) : (std::ios::binary | std::ios::in));
  if (stream_.is_open()) ++handles;
  return stream_.is_open();
}

int HalFile::read(void* data, size_t count) {
  if (consume(manga_progress_test::Failure::Read) && count > 0) --count;
  stream_.read(static_cast<char*>(data), static_cast<std::streamsize>(count));
  return static_cast<int>(stream_.gcount());
}

size_t HalFile::write(const void* data, size_t count) {
  if (consume(manga_progress_test::Failure::Write) && count > 0) --count;
  stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(count));
  return stream_ ? count : 0;
}

uint64_t HalFile::fileSize64() {
  const auto current = stream_.tellg();
  stream_.clear();
  stream_.seekg(0, std::ios::end);
  const auto size = stream_.tellg();
  stream_.seekg(current);
  return size < 0 ? 0 : static_cast<uint64_t>(size);
}

bool HalFile::sync() {
  if (consume(manga_progress_test::Failure::Sync)) return false;
  stream_.flush();
  return static_cast<bool>(stream_);
}

bool HalFile::close() {
  const bool fail = consume(manga_progress_test::Failure::Close);
  if (stream_.is_open()) {
    stream_.close();
    --handles;
  }
  return !fail;
}

HalStorage& HalStorage::getInstance() {
  static HalStorage storage;
  return storage;
}

std::filesystem::path HalStorage::resolve(const char* path) const {
  return root_ / std::filesystem::path(path).relative_path();
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  std::error_code error;
  std::filesystem::create_directories(resolve(path), error);
  return !error;
}

bool HalStorage::exists(const char* path) { return std::filesystem::exists(resolve(path)); }

bool HalStorage::remove(const char* path) {
  if (consume(manga_progress_test::Failure::Remove)) return false;
  std::error_code error;
  return std::filesystem::remove(resolve(path), error) && !error;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  if (consume(manga_progress_test::Failure::Rename)) return false;
  if (renameFailureCountdown > 0 && --renameFailureCountdown == 0) return false;
  std::error_code error;
  std::filesystem::rename(resolve(oldPath), resolve(newPath), error);
  return !error;
}

bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  return !consume(manga_progress_test::Failure::OpenRead) && file.open(resolve(path), false);
}
bool HalStorage::openFileForRead(const char* tag, const std::string& path, HalFile& file) {
  return openFileForRead(tag, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  return !consume(manga_progress_test::Failure::OpenWrite) && file.open(resolve(path), true);
}
bool HalStorage::openFileForWrite(const char* tag, const std::string& path, HalFile& file) {
  return openFileForWrite(tag, path.c_str(), file);
}
