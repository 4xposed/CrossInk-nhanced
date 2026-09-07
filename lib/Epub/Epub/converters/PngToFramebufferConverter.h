#pragma once

#include "ImageToFramebufferDecoder.h"

class PngToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
    return getDimensionsStatic(imagePath.c_str(), out);
  }
  static bool getDimensionsStatic(const char* imagePath, ImageDimensions& out);
  bool getDimensionsForCache(const char* imagePath, ImageDimensions& out) const override {
    return getDimensionsStatic(imagePath, out);
  }

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool decodeToCache(const std::string& imagePath, const CacheDecodeConfig& config) override {
    return decodeToCache(imagePath.c_str(), config);
  }
  bool decodeToCache(const char* imagePath, const CacheDecodeConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "PNG"; }

 private:
  bool decode(const char* imagePath, GfxRenderer* renderer, const RenderConfig& config, int screenWidth,
              int screenHeight, CooperativeCancellation cancellation);
};