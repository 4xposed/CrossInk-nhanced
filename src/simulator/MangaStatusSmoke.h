#pragma once
#ifdef SIMULATOR
class GfxRenderer;
// Caller owns RenderLock; tests actual renderer planes/cleanup without a second framebuffer.
bool verifyMangaStatusPlanes(GfxRenderer& renderer);
#endif
