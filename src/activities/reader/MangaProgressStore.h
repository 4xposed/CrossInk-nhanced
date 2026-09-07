#pragma once

#include <cstdint>
#include <string>

namespace manga {

struct Progress {
  uint32_t page = 0;
  int16_t panel = -1;
  bool panelsOnly = false;
  bool rotatePanels = true;
};

class MangaProgressStore {
 public:
  explicit MangaProgressStore(const std::string& bookPath);

  static std::string statePath(const std::string& bookPath);
  bool load(Progress& progress);
  bool save(const Progress& progress);
  bool remove();
  const std::string& statePath() const { return statePath_; }

 private:
  std::string statePath_;
};

}  // namespace manga
