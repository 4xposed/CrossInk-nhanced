#include "HalStorage.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <set>
#include <utility>
namespace mutation_test {
std::set<std::string> openPaths;
std::string root, failPath;
int calls = 0, failAt = 0, handles = 0;
bool failAfterRename = false, offline = false, cutAfterRename = false, failReload = false;
int cutPhase = 0;
std::string partialDeleteChild;
bool failure(const char* p) { return offline || (++calls == failAt && failAt) || (!failPath.empty() && failPath == p); }
std::string mapped(const char* p) { return root + p; }
void reset(const std::string& p) {
  root = p;
  calls = failAt = 0;
  failPath.clear();
  failAfterRename = offline = cutAfterRename = failReload = false;
  cutPhase = 0;
  partialDeleteChild.clear();
}
}  // namespace mutation_test
HalFile::HalFile(const char* p, oflag_t flags) : path_(p) {
  if (mutation_test::failure(p)) return;
  const auto full = mutation_test::mapped(p);
  struct stat st{};
  if (!stat(full.c_str(), &st) && S_ISDIR(st.st_mode))
    dir_ = opendir(full.c_str());
  else {
    if (mutation_test::openPaths.contains(path_)) return;
    fd_ = ::open(full.c_str(), flags, 0600);
    if (fd_ >= 0) mutation_test::openPaths.insert(path_);
  }
  if (*this) ++mutation_test::handles;
}
HalFile::~HalFile() { close(); }
HalFile::HalFile(HalFile&& o) noexcept { *this = std::move(o); }
HalFile& HalFile::operator=(HalFile&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = std::exchange(o.fd_, -1);
    dir_ = std::exchange(o.dir_, nullptr);
    path_ = std::move(o.path_);
    error_ = std::exchange(o.error_, false);
  }
  return *this;
}
bool HalFile::close() {
  if (!*this) {
    error_ = false;
    return true;
  }
  bool ok = !mutation_test::failure(path_.c_str());
  if (dir_) {
    ok = (closedir(dir_) == 0) && ok;
    dir_ = nullptr;
  }
  if (fd_ >= 0) {
    ok = (::close(fd_) == 0) && ok;
    fd_ = -1;
    mutation_test::openPaths.erase(path_);
  }
  --mutation_test::handles;
  error_ = false;
  return ok;
}
bool HalFile::sync() { return !mutation_test::failure(path_.c_str()) && fd_ >= 0 && !fsync(fd_); }
int HalFile::read(void* p, size_t n) {
  if (mutation_test::failure(path_.c_str())) return -1;
  return fd_ < 0 ? -1 : static_cast<int>(::read(fd_, p, n));
}
size_t HalFile::write(const void* p, size_t n) {
  if (n == 24 && !memcmp(p, "CMP1", 4) && static_cast<const uint8_t*>(p)[8] == mutation_test::cutPhase) {
    mutation_test::offline = true;
    return 0;
  }
  if (mutation_test::failure(path_.c_str())) return 0;
  auto v = fd_ < 0 ? -1 : ::write(fd_, p, n);
  return v < 0 ? 0 : v;
}
bool HalFile::seek64(uint64_t n) {
  return !mutation_test::failure(path_.c_str()) && fd_ >= 0 && lseek(fd_, n, SEEK_SET) >= 0;
}
bool HalFile::seekCur(int64_t n) {
  return !mutation_test::failure(path_.c_str()) && fd_ >= 0 && lseek(fd_, n, SEEK_CUR) >= 0;
}
size_t HalFile::position() { return fd_ < 0 ? 0 : lseek(fd_, 0, SEEK_CUR); }
uint64_t HalFile::fileSize64() {
  struct stat st{};
  return fd_ >= 0 && !fstat(fd_, &st) ? st.st_size : 0;
}
bool HalFile::truncate(uint64_t n) { return !mutation_test::failure(path_.c_str()) && fd_ >= 0 && !ftruncate(fd_, n); }
size_t HalFile::getName(char* p, size_t n) {
  auto name = path_.substr(path_.find_last_of('/') + 1);
  if (n) {
    auto count = std::min(n - 1, name.size());
    memcpy(p, name.data(), count);
    p[count] = 0;
    return count;
  }
  return 0;
}
HalFile HalFile::openNextFileChecked() {
  if (error_) return {};
  if (!dir_ || mutation_test::failure(path_.c_str())) {
    error_ = true;
    return {};
  }
  while (true) {
    errno = 0;
    auto* item = readdir(dir_);
    if (!item) {
      error_ = errno != 0;
      return {};
    }
    if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
    auto path = path_ + (path_ == "/" ? "" : "/") + item->d_name;
    HalFile child(path.c_str());
    if (!child) error_ = true;
    return child;
  }
}
HalStorage& HalStorage::getInstance() {
  static HalStorage instance;
  return instance;
}
bool HalStorage::exists(const char* p) {
  if (mutation_test::failure(p)) return false;
  struct stat st{};
  return !stat(mutation_test::mapped(p).c_str(), &st);
}
bool HalStorage::mkdir(const char* p, bool parents) {
  if (mutation_test::failure(p)) return false;
  std::error_code e;
  return parents ? std::filesystem::create_directories(mutation_test::mapped(p), e) || (!e && exists(p))
                 : (::mkdir(mutation_test::mapped(p).c_str(), 0700) == 0);
}
bool HalStorage::rmdir(const char* p) {
  return !mutation_test::failure(p) && !::rmdir(mutation_test::mapped(p).c_str());
}
bool HalStorage::remove(const char* p) {
  return !mutation_test::failure(p) && !::unlink(mutation_test::mapped(p).c_str());
}
bool HalStorage::rename(const char* a, const char* b) {
  bool fail = mutation_test::failure(a);
  if (fail && !mutation_test::failAfterRename) return false;
  bool ok = !::rename(mutation_test::mapped(a).c_str(), mutation_test::mapped(b).c_str());
  if (ok && mutation_test::cutAfterRename && !strcmp(a, "/Series")) {
    mutation_test::offline = true;
    return false;
  }
  return ok && !fail;
}
bool HalStorage::removeDir(const char* p) {
  if (mutation_test::failure(p)) return false;
  std::error_code ec;
  if (!mutation_test::partialDeleteChild.empty()) {
    std::filesystem::remove_all(mutation_test::mapped(mutation_test::partialDeleteChild.c_str()), ec);
    mutation_test::partialDeleteChild.clear();
    return false;
  }
  return std::filesystem::remove_all(mutation_test::mapped(p), ec) > 0 && !ec;
}
