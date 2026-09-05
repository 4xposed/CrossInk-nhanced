#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
};

struct BlockStyle {
  bool isRtl = false;
};

class TextBlock {
 public:
  TextBlock(std::vector<std::string> words, std::vector<int16_t> positions,
            std::vector<EpdFontFamily::Style> styles = {}, std::vector<uint8_t> bionicBoundaries = {},
            std::vector<uint16_t> bionicOffsets = {}, std::vector<uint8_t> flags = {}, bool rtl = false,
            bool ruby = false)
      : words_(std::move(words)),
        positions_(std::move(positions)),
        styles_(std::move(styles)),
        bionicBoundaries_(std::move(bionicBoundaries)),
        bionicOffsets_(std::move(bionicOffsets)),
        flags_(std::move(flags)),
        ruby_(ruby) {
    style_.isRtl = rtl;
    if (styles_.empty()) styles_.resize(words_.size(), EpdFontFamily::REGULAR);
    if (bionicBoundaries_.empty()) bionicBoundaries_.resize(words_.size(), 0);
    if (bionicOffsets_.empty()) bionicOffsets_.resize(words_.size(), 0);
    if (flags_.empty()) flags_.resize(words_.size(), 0);
  }

  uint16_t wordCount() const { return static_cast<uint16_t>(words_.size()); }
  const char* wordText(const uint16_t index) const { return words_[index].c_str(); }
  uint16_t wordTextLen(const uint16_t index) const { return static_cast<uint16_t>(words_[index].size()); }
  int16_t wordXpos(const uint16_t index) const { return positions_[index]; }
  EpdFontFamily::Style wordStyle(const uint16_t index) const { return styles_[index]; }
  uint8_t bionicBoundary(const uint16_t index) const { return bionicBoundaries_[index]; }
  uint16_t bionicRunOffset(const uint16_t index) const { return bionicOffsets_[index]; }
  bool wordEndsWithInsertedHyphen(const uint16_t index) const { return (flags_[index] & 0x02U) != 0; }
  const BlockStyle& getBlockStyle() const { return style_; }
  int getRubyShift(const int ascender) const { return ruby_ ? ascender / 2 : 0; }

 private:
  std::vector<std::string> words_;
  std::vector<int16_t> positions_;
  std::vector<EpdFontFamily::Style> styles_;
  std::vector<uint8_t> bionicBoundaries_;
  std::vector<uint16_t> bionicOffsets_;
  std::vector<uint8_t> flags_;
  BlockStyle style_{};
  bool ruby_ = false;
};

class PageElement {
 public:
  PageElement(const int16_t x, const int16_t y) : xPos(x), yPos(y) {}
  virtual ~PageElement() = default;
  virtual PageElementTag getTag() const = 0;

  int16_t xPos = 0;
  int16_t yPos = 0;
};

class PageLine final : public PageElement {
 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t x, const int16_t y)
      : PageElement(x, y), block_(std::move(block)) {}

  PageElementTag getTag() const override { return TAG_PageLine; }
  const std::shared_ptr<TextBlock>& getBlock() const { return block_; }

 private:
  std::shared_ptr<TextBlock> block_;
};

class PageImage final : public PageElement {
 public:
  PageImage(const int16_t x, const int16_t y) : PageElement(x, y) {}
  PageElementTag getTag() const override { return TAG_PageImage; }
};

class Page {
 public:
  std::vector<std::unique_ptr<PageElement>> elements;
};
