#include "MangaProgressStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <uzlib.h>

#include <array>

namespace manga {
namespace {
constexpr char TAG[] = "MGPR";
constexpr char ROOT_DIR[] = "/.crosspoint";
constexpr char STATE_DIR[] = "/.crosspoint/manga-state";
constexpr size_t RECORD_SIZE = 12;
constexpr uint8_t VERSION = 1;
constexpr uint8_t PANELS_ONLY_FLAG = 1U << 0;
constexpr uint8_t ROTATE_PANELS_FLAG = 1U << 1;
constexpr uint8_t VALID_FLAGS = PANELS_ONLY_FLAG | ROTATE_PANELS_FLAG;
constexpr uint32_t MAX_PAGE = 9999;
constexpr int16_t MIN_PANEL = -1;
constexpr int16_t MAX_PANEL = 254;

bool valid(const Progress& progress) {
  return progress.page <= MAX_PAGE && progress.panel >= MIN_PANEL && progress.panel <= MAX_PANEL;
}

void encode(const Progress& progress, std::array<uint8_t, RECORD_SIZE>& bytes) {
  const uint16_t panel = static_cast<uint16_t>(progress.panel);
  bytes = {'M',
           'G',
           'P',
           'R',
           VERSION,
           static_cast<uint8_t>((progress.panelsOnly ? PANELS_ONLY_FLAG : 0U) |
                                (progress.rotatePanels ? ROTATE_PANELS_FLAG : 0U)),
           static_cast<uint8_t>(panel),
           static_cast<uint8_t>(panel >> 8),
           static_cast<uint8_t>(progress.page),
           static_cast<uint8_t>(progress.page >> 8),
           static_cast<uint8_t>(progress.page >> 16),
           static_cast<uint8_t>(progress.page >> 24)};
}

bool decode(const std::array<uint8_t, RECORD_SIZE>& bytes, Progress& progress) {
  if (bytes[0] != 'M' || bytes[1] != 'G' || bytes[2] != 'P' || bytes[3] != 'R' || bytes[4] != VERSION ||
      (bytes[5] & ~VALID_FLAGS) != 0) {
    return false;
  }

  Progress decoded;
  decoded.panelsOnly = (bytes[5] & PANELS_ONLY_FLAG) != 0;
  decoded.rotatePanels = (bytes[5] & ROTATE_PANELS_FLAG) != 0;
  decoded.panel = static_cast<int16_t>(static_cast<uint16_t>(bytes[6]) | (static_cast<uint16_t>(bytes[7]) << 8));
  decoded.page = static_cast<uint32_t>(bytes[8]) | (static_cast<uint32_t>(bytes[9]) << 8) |
                 (static_cast<uint32_t>(bytes[10]) << 16) | (static_cast<uint32_t>(bytes[11]) << 24);
  if (!valid(decoded)) return false;
  progress = decoded;
  return true;
}

bool closeChecked(FsFile& file, const char* path) {
  if (file.close()) return true;
  LOG_ERR(TAG, "Failed to close manga state file: %s", path);
  return false;
}

bool removeChecked(const std::string& path, const char* description) {
  if (!Storage.exists(path.c_str()) || Storage.remove(path.c_str())) return true;
  LOG_ERR(TAG, "Failed to remove %s: %s", description, path.c_str());
  return false;
}

bool recoverBackup(const std::string& statePath) {
  const std::string backupPath = statePath + ".bak";
  if (Storage.exists(statePath.c_str()) || !Storage.exists(backupPath.c_str())) return true;
  if (Storage.rename(backupPath.c_str(), statePath.c_str())) return true;
  LOG_ERR(TAG, "Failed to recover manga state backup: %s", backupPath.c_str());
  return false;
}
}  // namespace

MangaProgressStore::MangaProgressStore(const std::string& bookPath) { statePath_ = statePath(bookPath); }

std::string MangaProgressStore::statePath(const std::string& bookPath) {
  const uint32_t crc = uzlib_crc32(bookPath.data(), static_cast<unsigned int>(bookPath.size()), 0);
  return std::string(STATE_DIR) + "/" + std::to_string(crc) + ".bin";
}

bool MangaProgressStore::load(Progress& progress) {
  progress = Progress{};
  if (!recoverBackup(statePath_) || !Storage.exists(statePath_.c_str())) return false;

  FsFile file;
  if (!Storage.openFileForRead(TAG, statePath_, file)) {
    LOG_ERR(TAG, "Failed to open manga state for reading: %s", statePath_.c_str());
    return false;
  }
  if (file.fileSize64() != RECORD_SIZE) {
    LOG_ERR(TAG, "Invalid manga state length: %s", statePath_.c_str());
    closeChecked(file, statePath_.c_str());
    return false;
  }

  std::array<uint8_t, RECORD_SIZE> bytes{};
  const bool readOk = file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
  const bool closeOk = closeChecked(file, statePath_.c_str());
  if (!readOk) {
    LOG_ERR(TAG, "Failed to read manga state: %s", statePath_.c_str());
    return false;
  }
  if (!closeOk) return false;
  if (!decode(bytes, progress)) {
    progress = Progress{};
    LOG_ERR(TAG, "Invalid manga state record: %s", statePath_.c_str());
    return false;
  }
  return true;
}

bool MangaProgressStore::save(const Progress& progress) {
  if (!valid(progress)) {
    LOG_ERR(TAG, "Refusing invalid manga progress (page=%u panel=%d)", static_cast<unsigned>(progress.page),
            static_cast<int>(progress.panel));
    return false;
  }
  if (!Storage.ensureDirectoryExists(ROOT_DIR) || !Storage.ensureDirectoryExists(STATE_DIR)) {
    LOG_ERR(TAG, "Failed to create manga state directory");
    return false;
  }
  if (!recoverBackup(statePath_)) return false;

  const std::string temporaryPath = statePath_ + ".tmp";
  const std::string backupPath = statePath_ + ".bak";
  if (!removeChecked(temporaryPath, "stale manga state temp")) return false;

  FsFile temporary;
  if (!Storage.openFileForWrite(TAG, temporaryPath, temporary)) {
    LOG_ERR(TAG, "Failed to open manga state temp: %s", temporaryPath.c_str());
    return false;
  }
  std::array<uint8_t, RECORD_SIZE> bytes{};
  encode(progress, bytes);
  const bool writeOk = temporary.write(bytes.data(), bytes.size()) == bytes.size();
  const bool syncOk = writeOk && temporary.sync();
  const bool closeOk = closeChecked(temporary, temporaryPath.c_str());
  if (!writeOk || !syncOk || !closeOk) {
    if (!writeOk) LOG_ERR(TAG, "Failed to write manga state temp: %s", temporaryPath.c_str());
    if (writeOk && !syncOk) LOG_ERR(TAG, "Failed to sync manga state temp: %s", temporaryPath.c_str());
    removeChecked(temporaryPath, "failed manga state temp");
    return false;
  }

  const bool hadState = Storage.exists(statePath_.c_str());
  if (hadState) {
    if (!removeChecked(backupPath, "old manga state backup")) {
      removeChecked(temporaryPath, "unpromoted manga state temp");
      return false;
    }
    if (!Storage.rename(statePath_.c_str(), backupPath.c_str())) {
      LOG_ERR(TAG, "Failed to preserve manga state before replacement: %s", statePath_.c_str());
      removeChecked(temporaryPath, "unpromoted manga state temp");
      return false;
    }
  }
  if (!Storage.rename(temporaryPath.c_str(), statePath_.c_str())) {
    LOG_ERR(TAG, "Failed to replace manga state: %s", statePath_.c_str());
    if (hadState && !Storage.rename(backupPath.c_str(), statePath_.c_str())) {
      LOG_ERR(TAG, "Failed to restore manga state backup: %s", backupPath.c_str());
    }
    removeChecked(temporaryPath, "unpromoted manga state temp");
    return false;
  }
  if (hadState && !removeChecked(backupPath, "manga state backup")) return false;
  return true;
}

bool MangaProgressStore::remove() {
  const bool primaryOk = removeChecked(statePath_, "manga state");
  const bool backupOk = removeChecked(statePath_ + ".bak", "manga state backup");
  const bool temporaryOk = removeChecked(statePath_ + ".tmp", "manga state temp");
  return primaryOk && backupOk && temporaryOk;
}

}  // namespace manga
