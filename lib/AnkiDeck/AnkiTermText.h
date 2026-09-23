#pragma once
#include <algorithm>
#include <cstring>
#include <string_view>

// Caller owns the bounded buffer (capacity + one byte for the terminator).
class AnkiTermText {
 public:
  AnkiTermText(char* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}
  void append(std::string_view text) {
    const size_t count = std::min(text.size(), capacity_ - size_);
    if (count) std::memcpy(buffer_ + size_, text.data(), count);
    size_ += count;
    truncated_ = truncated_ || count != text.size();
  }
  std::string_view finish() {
    if (truncated_ && capacity_ >= 3) {
      size_ = std::min(size_, capacity_ - 3);
      // Preserve UTF-8 even when a streamed definition splits a codepoint.
      while (size_ && (static_cast<unsigned char>(buffer_[size_]) & 0xC0) == 0x80) --size_;
      std::memcpy(buffer_ + size_, "\xE2\x80\xA6", 3);
      size_ += 3;
    }
    buffer_[size_] = '\0';
    return {buffer_, size_};
  }

 private:
  char* buffer_;
  size_t capacity_;
  size_t size_ = 0;
  bool truncated_ = false;
};
