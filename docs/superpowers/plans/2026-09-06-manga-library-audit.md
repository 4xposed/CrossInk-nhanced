# Manga library/action audit

Scope: read-only audit of the current `matcha_features` checkout, with this file as the only output. CrossInk CodeGraph was consulted first. The local Matcha reference is pinned at `61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`; its Home manga code is useful behavioral evidence, but CrossInk's newer CRC progress store and cover-cache contract are authoritative.

## Findings and narrow implementation boundaries

### 1. Treat a manga book as a directory while it still exists

`MangaBook::isMangaFolder()` recognizes a folder only by successfully setting the folder and finding a non-directory `panels.idx` (`lib/MangaPanel/MangaBook.cpp:305-321`). File Browser already opens such a directory as a book instead of descending into it (`src/activities/home/FileBrowserActivity.cpp:819-824`), but its directory action menu exposes only sleep-folder selection and recursive deletion (`src/activities/home/FileBrowserActivity.cpp:487-529`). There is no on-device folder rename/move action to extend today.

Recursive deletion currently descends through every directory and records metadata only for recognized *files* (`src/activities/home/FileBrowserActivity.cpp:161-179`). After `removeDir()` succeeds it clears those recorded paths (`src/activities/home/FileBrowserActivity.cpp:447-458`). A manga root is therefore never recorded, and recognition cannot be deferred until cleanup because both the directory and marker are gone.

Narrow fix boundary: during the pre-delete traversal, while the tree exists, detect each directory containing `panels.idx` and record a typed manga-book root. Do not infer manga from a missing path after deletion. After `removeDir()` succeeds, clear the recorded root's recent entry, CRC progress file, and `BookmarkStore` file with type `"manga"`. Continue walking below a recognized root because nested manga folders are valid deletion targets; deduplicate exact roots. If the scan cannot open a directory, report the cleanup failure rather than classifying absence as manga—`PathResult` documents that HAL existence errors cannot be distinguished from missing (`lib/MangaPanel/MangaBook.h:19-24`).

The single-file deletion ordering is also unsafe for metadata: it calls `clearFileMetadata()` before `Storage.remove()` (`src/activities/home/FileBrowserActivity.cpp:403-414`), so a failed delete loses state. Directory deletion already uses the safer order. Manga deletion should snapshot identity first, delete the content, then remove metadata.

### 2. Centralize manga path identity and migrate all path-derived state

The current durable position filename is CRC32 of the complete folder path (`src/activities/reader/MangaProgressStore.cpp:82-85`). Bookmark filenames use the same path-derived CRC plus book type (`src/BookmarkStore.cpp:31-34`), and the bookmark header stores the original path (`src/BookmarkStore.cpp:162-210`). The reader loads both using the folder path (`src/activities/reader/MangaReaderActivity.cpp:30-50`). Recents also key equality and updates by exact path (`src/RecentBooksStore.h:18`, `src/RecentBooksStore.cpp:109-137`). Consequently, a rename or move without migration makes progress and bookmarks appear lost and leaves the recent entry stale.

Reuse the existing transaction shape rather than adding migration inside UI handlers: `BookMoveUtils::migrateMovedEpubState()` moves the path-keyed cache, migrates bookmarks/clippings, updates recents, and updates resume state after a successful filesystem rename (`src/util/BookMoveUtils.cpp:39-74`). Add a manga sibling/helper whose inputs are explicit `oldPath`, `newPath`, title/author, and keep-in-recents policy. It should:

1. migrate the progress primary and `.bak` files from the old CRC identity to the new CRC identity without deleting a valid destination;
2. call `BookmarkStore::migrateForFilePath(oldPath, newPath, title, author, "manga")`, whose public contract already supports manga (`src/BookmarkStore.h:60-67`);
3. move `manga::cachePath(oldPath)` to `manga::cachePath(newPath)` when present, so generated thumbnails remain reusable;
4. call `RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)`, which preserves ordering and rewrites a cover path beneath the old cache (`src/RecentBooksStore.cpp:124-137`);
5. update `APP_STATE.openEpubPath` just as the existing EPUB migration does (`src/util/BookMoveUtils.cpp:69-72`). Despite its name, the manga reader currently uses this field for resume (`src/activities/reader/MangaReaderActivity.cpp:55-56`).

For a parent-folder move/rename, enumerate recognized manga roots before the filesystem rename, derive each destination by replacing only the exact old directory prefix plus `/`, perform the directory rename, then migrate every captured book. Never call `RECENT_BOOKS.addOrUpdateBook()` between rename and migration: it calls `pruneMissing()` first (`src/RecentBooksStore.cpp:62-71`) and can erase all old nested paths before they can be repointed. Cache-move failure may be non-fatal because it is derived data; progress/bookmark migration failure must be surfaced and leave recoverable old state.

The HTTP `/rename` and `/move` endpoints currently document file-only behavior (`docs/webserver-endpoints.md:145-179`), and the on-device directory menu has no rename/move commands. Keep folder support scoped to the future operation that actually accepts directories; do not silently broaden network contracts as part of Home integration.

### 3. Separate durable state from disposable manga cache cleanup

`BookActions` currently offers cache/stat actions only for EPUB/Anki/XTC and clears metadata only for EPUB/XTC/TXT (`src/activities/home/BookActions.cpp:30-41`, `:46-99`). Add manga to cache capability and metadata deletion through an explicit folder-aware branch. Do not add Delete Stats or completed-book actions until manga reading statistics are implemented; Home likewise only loads per-book stats for EPUB/XTC (`src/activities/home/HomeActivity.cpp:169-175`).

The global cache cleaner recognizes only `epub_`, `anki_`, `txt_`, and `xtc_` directory prefixes (`src/util/BookCacheUtils.cpp:309-323`) and invokes its preserve-and-clear routine for recognized directories (`src/activities/settings/ClearCacheActivity.cpp:107-145`). Add `manga_` recognition. Under the specified layout, manga progress remains in `/.crosspoint/manga-state/` and bookmarks in `/.crosspoint/bookmarks/`, so removing `/.crosspoint/manga_<crc>/` deletes only derived thumbnails. Do not move manga progress into that cache or teach generic cache cleanup to preserve unrelated top-level files.

Individual manga cache deletion should resolve exactly `manga::cachePath(folder)` and remove that directory. `clearBookCachePreservingUserState()` currently resolves only extension-based book types and otherwise falls through to generic clearing (`src/util/BookCacheUtils.cpp:327-359`); directory recognition must happen before extension dispatch. The template `thumb_v1_[WIDTH]x[HEIGHT].bmp` belongs beneath that cache and must stay templated in `RecentBook.coverBmpPath`, allowing existing Home/Grid calls to substitute their requested size via `UITheme::getCoverThumbPath()` (`src/activities/home/HomeActivity.cpp:373-404`; `src/activities/home/RecentBooksGridActivity.cpp:239-271`).

### 4. Home and Recent Books need explicit manga dispatch, not repeated probes

Recents accepts directory paths because missing checks only call `Storage.exists()` (`src/RecentBooksStore.cpp:140-147`), and Home skips absent paths before loading covers (`src/activities/home/HomeActivity.cpp:620-633`). That is sufficient for a valid stored manga directory; it does not validate the marker. Validate manga when adding/opening it, then use a cheap, shared classification or the known templated cache path in Home. Avoid repeatedly constructing `MangaBook` merely to recognize a persisted recent.

Progress dispatch currently handles EPUB, XTC, TXT/Markdown and returns unknown for everything else (`src/activities/home/RecentBookProgress.cpp:221-232`). Add a manga branch that loads `MangaProgressStore(folder)` and obtains `pageCount` through the lightweight `MangaBook` index. Match the reader's zero-based page semantics (`src/activities/reader/MangaReaderActivity.cpp:46-49`) and report `(page + 1) / pageCount * 100`, clamped, so page 0 is visibly started and the final page reaches 100%. A corrupt/missing progress record, invalid marker, open failure, or zero page count returns `-1` and remains retryable.

Home cover generation has explicit EPUB/XTC branches in both carousel and non-carousel paths (`src/activities/home/HomeActivity.cpp:684-799`); Grid does the same (`src/activities/home/RecentBooksGridActivity.cpp:239-305`). Add manga generation to both surfaces through one cover helper so cache path, source-page choice, template version, dimensions, and retry behavior cannot diverge. On success persist `thumb_v1_[WIDTH]x[HEIGHT].bmp` with `RECENT_BOOKS.updateBook()`. On allocation/decode/write failure retain `CoverState::Unknown`; do not mark `Missing`, because a valid manga has an index but cover generation can fail transiently. The Matcha implementation provides evidence for this retry rule and for Home generating its own manga thumbnail (`../matcha-reader/src/activities/home/HomeActivity.cpp:182-209`), but its hash/cache format should not be copied.

Home's carousel invalidation key currently hashes progress/stats only when `getRecentBookCachePath()` returns an extension-based cache (`src/activities/home/HomeActivity.cpp:156-175`, `:407-437`). Include manga cache identity for thumbnail file state and include the separate manga progress file state, otherwise a saved page change can reuse a stale carousel snapshot. Highlighted progress already funnels non-EPUB books through `RecentBookProgress::loadPercent()` (`src/activities/home/HomeActivity.cpp:965-990`), so the shared dispatch change covers Home and Grid (`src/activities/home/RecentBooksGridActivity.cpp:235-236`).

## Tests that should exercise the boundaries

- Delete one valid manga folder: content removal succeeds, exact recent entry disappears, manga progress primary/backup and manga bookmarks disappear, and unrelated manga state/cache remains.
- Force `removeDir()` failure: durable progress, bookmarks, recents, and cache remain intact.
- Delete a parent containing ordinary files, nested manga roots, and a directory named like manga but lacking `panels.idx`: clean only captured valid manga identities; prove recognition happened before deletion.
- Rename/move one manga folder and a parent containing two nested manga books: preserve page/panel/settings, all distinct bookmarks (`spine=page`, `paragraph=panel+1`), recent ordering/title/author, resume path, and cached thumbnails under each derived destination.
- Exercise destination collisions and injected failures for progress, bookmark, cache, and recent migration. No valid destination state is overwritten; cache failure regenerates later; durable-state failure is reported and old files remain recoverable.
- Clear one manga cache and run global Clear Reading Cache: delete `manga_<crc>/thumb_v1_*` while preserving `manga-state/*.bin`, `.bak`, bookmarks, recents, and other books' caches.
- Home and grid with cold cache, warm cache, corrupt/zero-byte thumbnail, decode/OOM failure, missing progress, corrupt progress, zero-page index, first page, panel bookmark, and final page. Failures retry later and never become permanent `CoverState::Missing`.
- Remove `panels.idx` while leaving the directory: the stale recent is not opened as manga and no new metadata is created; deleting that directory still must not guess manga after it is gone.
- Hardware: on X3/X4, open Home and both Recent views with several manga entries, generate every required theme size, then page through a manga and return Home; expect correct cover/progress without a second framebuffer or sustained heap loss. Repeat on Sticky/X4 Pro and after moving the parent folder on SD. Clear the relevant `/.crosspoint/manga_<crc>/` only when testing regeneration; retain `/.crosspoint/manga-state/` to verify durability.
