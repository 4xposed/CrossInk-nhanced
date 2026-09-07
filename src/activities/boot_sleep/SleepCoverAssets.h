#pragma once

#include <CooperativeCancellation.h>
#include <MangaCover.h>

#include <string>

class GfxRenderer;
class Txt;
class Xtc;

namespace SleepCoverAssets {

bool prepareXtc(const Xtc& xtc);
bool prepareTxt(const Txt& txt);
bool prepareFullCoverForPath(const std::string& bookPath, bool cropped, const GfxRenderer* renderer = nullptr,
                             CooperativeCancellation cancellation = {},
                             manga::ThumbnailDiagnostics* diagnostics = nullptr, std::string* preparedPath = nullptr, bool imageLevels = false);
bool prepareMinimalCoverForPath(const std::string& bookPath, const GfxRenderer* renderer = nullptr,
                                CooperativeCancellation cancellation = {},
                                manga::ThumbnailDiagnostics* diagnostics = nullptr);
bool prepareDashboardCoverForPath(const std::string& bookPath, const GfxRenderer* renderer = nullptr,
                                  CooperativeCancellation cancellation = {},
                                  manga::ThumbnailDiagnostics* diagnostics = nullptr);

std::string reusableCoverPathFor(const std::string& bookPath);
std::string cachedCoverPathFor(const std::string& bookPath, bool cropped, const GfxRenderer* renderer = nullptr,
                               CooperativeCancellation cancellation = {}, bool imageLevels = false);
std::string cachedMinimalCoverPathFor(const std::string& bookPath);
std::string cachedDashboardCoverPathFor(const std::string& bookPath);

}  // namespace SleepCoverAssets
