#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace library {
enum class Category : uint8_t { All, Manga, Books, Articles };
constexpr size_t PATH_CAPACITY = 512;

inline bool normalizeFolder(std::string_view input, char* output, size_t capacity) {
  if (!output || input.empty() || input.front() != '/' || capacity < 2) return false;
  // Validate before writing, so invalid input cannot partly replace a setting.
  size_t required = 1;
  for (size_t begin = 1; begin < input.size();) {
    while (begin < input.size() && input[begin] == '/') ++begin;
    if (begin == input.size()) break;
    size_t end = input.find('/', begin);
    if (end == std::string_view::npos) end = input.size();
    const auto part = input.substr(begin, end - begin);
    if (part == "." || part == "..") return false;
    for (const unsigned char ch : part)
      if (ch < 32 || ch == 127) return false;
    required += part.size() + (required > 1 ? 1 : 0);
    begin = end;
  }
  if (required >= capacity) return false;
  size_t used = 1;
  output[0] = '/';
  for (size_t begin = 1; begin < input.size();) {
    while (begin < input.size() && input[begin] == '/') ++begin;
    if (begin == input.size()) break;
    size_t end = input.find('/', begin);
    if (end == std::string_view::npos) end = input.size();
    if (used > 1) output[used++] = '/';
    std::memmove(output + used, input.data() + begin, end - begin);
    used += end - begin;
    begin = end;
  }
  output[used] = '\0';
  return true;
}

inline bool containsPath(std::string_view folder, std::string_view path) {
  while (folder.size() > 1 && folder.back() == '/') folder.remove_suffix(1);
  if (folder.empty() || path.empty() || path.front() != '/') return false;
  if (folder == "/") return true;
  return path.substr(0, folder.size()) == folder &&
         (path.size() == folder.size() || (path.size() > folder.size() && path[folder.size()] == '/'));
}
}  // namespace library
