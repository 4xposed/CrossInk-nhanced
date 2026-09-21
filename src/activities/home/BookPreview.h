#pragma once
#include <CooperativeCancellation.h>

#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
class GfxRenderer;
bool prepareBookPreview(RecentBook& book, int width, int height, GfxRenderer& renderer,
                        CooperativeCancellation cancellation = {});
void drawBookPreview(const GfxRenderer& renderer, const RecentBook& book, Rect rect);
