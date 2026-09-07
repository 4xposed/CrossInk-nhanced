#pragma once

#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"
#include "activities/ActivityResult.h"

class GfxRenderer;
class MappedInputManager;
class Activity;

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);
bool hasClearableBookCache(const std::string& path);
bool canSendNearby(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearMangaMetadata(const std::string& folderPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
bool resetBookReaderSettings(const std::string& fullPath);
std::vector<std::string> epubRenderModeOptions();
uint8_t epubRenderModeDisplayIndex(uint8_t renderMode);
uint8_t epubRenderModeForDisplayIndex(uint8_t displayIndex);
std::string confirmationHeading(StrId actionLabelId);
bool isBookCompleted(const std::string& fullPath);
struct CompletionEdit;
bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed,
                         CompletionEdit& edit);
void startCompletionEdit(Activity& owner, GfxRenderer& renderer, MappedInputManager& input, const std::string& fullPath,
                         const std::string& displayName, ActivityResultHandler handler);
void drawToast(const GfxRenderer& renderer, const char* msg);

}  // namespace BookActions
