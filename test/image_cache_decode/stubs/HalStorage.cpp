#include "HalStorage.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <utility>
namespace storage_test {
std::string root;
bool failWrite = false, failSync = false, failRename = false;
int openFiles = 0;
bool failClose = false, failOpenWrite = false;
size_t writtenBytes = 0, failAfterBytes = SIZE_MAX, readBytes = 0;
void (*onSync)() = nullptr;
int failRenameCall = 0, renameCalls = 0;
std::string mapped(const char* p) { return !strncmp(p, "/.crosspoint", 12) ? root + std::string(p) : p; }
}  // namespace storage_test
HalFile::HalFile(const char* p, int flags) : path_(storage_test::mapped(p)) {
  struct stat s{};
  if (!stat(path_.c_str(), &s) && S_ISDIR(s.st_mode)) {
    dir_ = opendir(path_.c_str());
    return;
  }
  file_ = fopen(path_.c_str(), (flags & O_WRITE) ? "wb" : "rb");
  if (file_) {
    counted_ = true;
    ++storage_test::openFiles;
  }
}
HalFile::~HalFile() { close(); }
HalFile::HalFile(HalFile&& o) noexcept { *this = std::move(o); }
HalFile& HalFile::operator=(HalFile&& o) noexcept {
  if (this != &o) {
    close();
    path_ = std::move(o.path_);
    file_ = std::exchange(o.file_, nullptr);
    dir_ = std::exchange(o.dir_, nullptr);
    counted_ = std::exchange(o.counted_, false);
  }
  return *this;
}
bool HalFile::close() {
  bool ok = true;
  if (file_) {
    ok = fclose(file_) == 0 && !storage_test::failClose;
    file_ = nullptr;
  }
  if (counted_) {
    --storage_test::openFiles;
    counted_ = false;
  }
  if (dir_) {
    ok = closedir((DIR*)dir_) == 0;
    dir_ = nullptr;
  }
  return ok;
}
uint64_t HalFile::fileSize64() {
  struct stat s{};
  return file_ && !fstat(fileno(file_), &s) ? s.st_size : 0;
}
bool HalFile::seek64(uint64_t p) { return file_ && fseeko(file_, p, SEEK_SET) == 0; }
bool HalFile::seekCur(int64_t p) { return file_ && fseeko(file_, p, SEEK_CUR) == 0; }
int HalFile::read(void* d, size_t n) {
  const int read = file_ ? int(fread(d, 1, n, file_)) : -1;
  if (read > 0) storage_test::readBytes += read;
  return read;
}
int HalFile::read() { return file_ ? fgetc(file_) : -1; }
size_t HalFile::write(const void* d, size_t n) {
  if (!file_ || storage_test::failWrite || storage_test::writtenBytes >= storage_test::failAfterBytes) return 0;
  const size_t written = fwrite(d, 1, n, file_);
  storage_test::writtenBytes += written;
  return written;
}
size_t HalFile::write(uint8_t b) { return write(&b, 1); }
bool HalFile::sync() {
  if (storage_test::onSync) storage_test::onSync();
  return file_ && !storage_test::failSync && fflush(file_) == 0;
}
size_t HalFile::getName(char* out, size_t n) {
  auto p = path_.find_last_of('/');
  auto s = path_.substr(p == std::string::npos ? 0 : p + 1);
  if (!n) return 0;
  auto z = std::min(n - 1, s.size());
  memcpy(out, s.data(), z);
  out[z] = 0;
  return z;
}
HalFile HalFile::openNextFile() {
  while (dir_) {
    auto* e = readdir((DIR*)dir_);
    if (!e) return {};
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) return HalFile((path_ + '/' + e->d_name).c_str());
  }
  return {};
}
HalStorage& HalStorage::getInstance() {
  static HalStorage s;
  return s;
}
HalFile HalStorage::open(const char* p, oflag_t f) { return HalFile(p, f); }
bool HalStorage::exists(const char* p) {
  auto q = storage_test::mapped(p);
  struct stat s{};
  return !stat(q.c_str(), &s);
}
bool HalStorage::mkdir(const char* p, bool) {
  auto q = storage_test::mapped(p);
  return ::mkdir(q.c_str(), 0777) == 0 || errno == EEXIST;
}
bool HalStorage::remove(const char* p) {
  auto q = storage_test::mapped(p);
  return std::remove(q.c_str()) == 0;
}
bool HalStorage::rename(const char* a, const char* b) {
  ++storage_test::renameCalls;
  if (storage_test::failRename || storage_test::renameCalls == storage_test::failRenameCall) return false;
  auto x = storage_test::mapped(a), y = storage_test::mapped(b);
  return std::rename(x.c_str(), y.c_str()) == 0;
}
bool HalStorage::openFileForRead(const char*, const char* p, HalFile& f) {
  f = open(p);
  return bool(f);
}
bool HalStorage::openFileForWrite(const char*, const char* p, HalFile& f) {
  if (storage_test::failOpenWrite) return false;
  f = open(p, O_WRITE | O_CREAT | O_TRUNC);
  return bool(f);
}
