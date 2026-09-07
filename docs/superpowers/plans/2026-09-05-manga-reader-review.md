# Manga reader integration review

Scope: Task 5 foreground reader, navigation, progress, chapter/bookmark selection,
minimal Books/ReaderActivity dispatch and manga bookmark type support. The broader
library, caches, prefetch and OCR tasks remain separate acceptance items.

Independent review found two actionable issues:

1. A failed progress write retried on every loop after the save threshold. The
   reader now retries after 30 seconds, keeps the pending position, and still
   attempts a flush on exit.
2. Repeated TOC lookups rescanned from record zero, making a sequential chapter
   scan quadratic. MangaBook now caches the next ordinal and 64-bit file offset;
   backward reads restart and failures reset the cursor. File handles still close
   before every return. Sequential/backward/failure tests pass under ASan/UBSan.

Scoped re-review found no remaining concrete blocker in those fixes. It also
checked BMP bounds/downscale centering and distinct bookmark fractional anchors.

Controller integration review additionally fixed:

- Manual refresh must not take RenderLock inside the activity because
  ActivityManager::requestManualReaderRefresh already owns it. The manga smoke
  invokes the actual manager API to guard against regression.
- Child-screen return must redraw the manga framebuffer, including cancellation.
- Selection UI mutations and TOC reads run under RenderLock; visible title copies
  end at complete UTF-8 boundaries.
- The bookmark header copies at most 127 UTF-8-safe bytes of title/author rather
  than duplicating potentially 64 KiB portable metadata fields.
- BMP placement accounts for the existing renderer's downsize-only contract.
- Touch page-turn layout reads take RenderLock while panel rendering may rotate
  the renderer temporarily.

The pinned nextPanel implementation explicitly carries panel mode into the next
page when the source overview is absent. A review suggestion to change this was
withdrawn after inspecting the source; it remains intentional Matcha parity.

## Hardware verification still required

On X4/C3, open an arbitrary-name nested converter output folder from Books. Verify
full-page → first crop → next crop/page, reverse navigation, Back to overview/Home,
chapter and percent jumps, per-panel bookmarks, both panel preferences, manual
refresh and exit/reopen. Repeat with panel-only books and missing images. Open a
1,000-chapter book near its end and check the chapter menu's response time.

On Sticky/X4 Pro, repeat using touch and two-finger rotation. Compare wide/tall BMP,
JPEG and PNG placement and verify bezel-safe bounds and e-ink refresh/ghosting.
Inspect internal free/largest heap and task stack high-water marks, plus PSRAM on
S3. Simulate an unwritable state path and confirm bounded retry logs/input response.
No manga render cache reset is needed yet; durable progress lives outside caches.
These checks require a later upload; none has been performed in this work.

The first end-to-end manga smoke run reproduced an immediate chapter-screen exit
in both simulator profiles. Root cause: simulator-injected Confirm releases
returned before MappedInputManager's suppression logic, even though OptionPopup
had selected on press and suppressed the matching release. The simulator-only
injection path now consumes a suppressed Confirm release. Manga and existing
EPUB/dictionary smoke runs pass with this fix; hardware input code is unchanged.
