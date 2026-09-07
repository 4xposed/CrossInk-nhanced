# Task 10c transport persistence preflight

Date: 2026-09-07

Scope: implementation preflight for the approved
`2026-09-06-manga-transport-design.md` and Task 10c brief. This report maps the
current WebDAV MOVE, USB serial RENAME/REMOVE, HTTP recursive DELETE, persistence
APIs, and boot order. It does not change the approved transaction format,
operation ordering, collision policy, or bounded snapshot.

## Actual transport paths and ownership

Web requests execute synchronously from the network activity's main-task loop:
`CrossPointWebServerActivity::loop()` repeatedly calls `handleClient()`
(`CrossPointWebServerActivity.cpp:422-449`). USB serial commands are also polled
from the main loop and mutations are accepted only when Home is current
(`main.cpp:1444,1462-1463`; `UsbSerialFileTransfer.cpp:249-253,713`). These paths
can call one main-owned mutation gate. They do not need a worker, but the gate
must reject Busy until foreground reader/prefetch/dictionary/cover owners have
been cancelled and drained through their lifecycle contracts.

WebDAV normalizes and URL-decodes both request and Destination paths, extracts a
path from an absolute Destination URL, adds a leading slash, and removes a
trailing slash (`WebDAVHandler.cpp:697-744`). USB accepts `/sdcard` aliases, then
uses the same `FsHelpers::normalisePath`, leading-slash, and trailing-slash rules,
with a fixed 256-byte result bound (`UsbSerialFileTransfer.cpp:158-185,244-247`).
The HTML API uses the same normalization shape (`CrossPointWebServer.cpp:219-235`).
The shared transaction entry should accept only these already normalized absolute
paths and recheck root/protected/ancestor relationships and length; it should not
silently apply a fourth normalization policy.

USB RENAME rejects an existing destination and requires its parent to exist
(`UsbSerialFileTransfer.cpp:569-603`). The HTML move/rename endpoints reject
directory sources and existing targets and therefore remain outside folder-move
UI scope (`CrossPointWebServer.cpp:1060-1196`). WebDAV MOVE supports directory
sources, but `Overwrite: T` currently removes an existing destination without
checking the remove result before renaming (`WebDAVHandler.cpp:491-560`). For an
affected directory move, both Overwrite values must use the approved no-clobber
transaction: `F` keeps 412, while `T` rejects the occupied destination rather
than deleting unrelated content or metadata.

WebDAV DELETE deliberately rejects nonempty directories and remains unchanged
(`WebDAVHandler.cpp:394-447`). HTTP DELETE recursively calls `Storage.removeDir`
and accepts multiple independently normalized roots (`CrossPointWebServer.cpp:
1198-1283`). USB REMOVE performs its own depth-eight recursive walk, clearing
file caches as each file disappears and returning on the first failure
(`UsbSerialFileTransfer.cpp:263-303,548-567`). Task 10c must preflight USB depth
and collect/journal metadata before either recursive implementation mutates
content. HTTP roots need ancestor/duplicate deduplication before collection so a
second requested root cannot act on an already removed subtree.

## Persistence interfaces that cannot acknowledge the transaction

- `RecentBooksStore::updatePath()` mutates in memory, rewrites a cover prefix by
  raw string prefix, calls `saveToFile()`, and returns void
  (`RecentBooksStore.cpp:126-140`). It can rewrite `/cache/1` inside `/cache/10`,
  and callers cannot distinguish durable publication from failure.
  `removeByPath()` returns whether an in-memory row existed even when its save
  fails (`:111-123`). Task 10c needs a frozen snapshot/serialize API or transaction
  adapter that constructs the complete replacement, rewrites only exact book
  paths and cache prefixes at slash boundaries, publishes with checked result,
  and reloads only after both shared JSON files commit.
- `CrossPointState::saveToFile()` returns bool and holds its private state/store
  locks while serializing (`CrossPointState.cpp:39-45`), but there is no API to
  stage a full replacement, validate its length/CRC, or freeze it together with
  Recent Books. Directly assigning `openEpubPath` and calling save, as
  `BookMoveUtils.cpp:69-72` does, cannot support the approved two-file recovery.
  Add a narrow snapshot/serialization boundary used by the transaction, retaining
  every unrelated field and rewriting only exact resume/favorite/preferred paths.
- `BookmarkStore::deleteForFilePath()` is void and reports neither current nor
  legacy deletion failure (`BookmarkStore.cpp:555-569`). Its migration helper is
  bool but merges current/legacy source and destination records, publishes the
  destination, then deletes source files (`:571-683`). That is incompatible with
  collision preflight, preserving current and legacy files separately, and replay
  after partial publication. Task 10c needs bounded read/validate/stage helpers
  and checked per-path deletion; it must not call the merge helper.
- `BookActions::clearFileMetadata()` is void and delegates to void bookmark and
  clipping deletion (`BookActions.cpp:93-107`). `clearMangaMetadata()` returns
  bool for cache/progress but includes void bookmark deletion and removes the
  whole manga cache (`:109-118`). These cannot mark journal cleanup complete.
  Add a checked cleanup result per snapshotted identity and keep durable cache
  members out of blanket directory removal.
- `ClippingStore::deleteForFilePath()` is void and ignores `Storage.remove`
  (`ClippingStore.cpp:494-499`). EPUB moves currently use a bool migration helper,
  but it merges/replaces destination state with backup side effects
  (`:501-545`). If the approved snapshot includes affected EPUBs, give clippings
  the same staged, no-clobber, checked delete boundary as bookmarks.
- `MangaProgressStore` already has checked load/save/remove and temp/backup files
  (`MangaProgressStore.cpp:82-181`), but its save returns false when backup cleanup
  fails after the new primary has already been published (`:165-174`). A retry can
  therefore be ambiguous. The transaction should copy and validate the existing
  durable primary/backup/tmp state into its journaled staging and publish by
  journal phase, rather than call `save()` as a migration primitive.
- Current `BookReadingStats::save()` is void and truncates in place. Task 10b is
  expected to introduce checked `stats_v6.bin` publication with a streamed
  language-day appendix. Task 10c must consume that final validated whole-file
  boundary, count v6 plus supported migration files, and treat success only as
  completed publication. It must preserve the complete appendix and must not
  parse/re-encode it during a path move.
- `BookMoveUtils::migrateMovedEpubState()` renames an entire cache best-effort,
  then independently migrates bookmarks/clippings and updates shared stores
  (`BookMoveUtils.cpp:39-74`). Its partial side effects are not replayable and its
  destination cache rename is not no-clobber. It is not a suitable base for the
  folder transaction; reuse only format-specific path derivation.

The durable manga cache manifest must explicitly include all supported stats
files, `dictionary_history.txt`, and `dictionary.bin`. Current cache preservation
lists history but omits the per-book dictionary route
(`BookCacheUtils.cpp:24-37`). Pixel, thumbnail, and OCR candidate data remain
disposable and must not consume durable snapshot slots.

## Recovery placement and ordering

Storage mounts at `main.cpp:1152-1160`. Settings, app state, and recents load at
`:1164-1173`, and resume routing later consumes `APP_STATE.openEpubPath` at
`:1376-1395`. Transaction recovery must run after successful `Storage.begin()`
and before `APP_STATE.loadFromFile()` and `RECENT_BOOKS.loadFromFile()`. Loading
Settings first is acceptable if recovery needs the established protected-path
policy, but no recent pruning, dictionary validation, resume selection, web/USB
mutation, or store writer may run before recovery returns Done/NoTransaction or
an explicit affected-path blocked state.

Network-resume boot currently skips Recent Books loading but still loads app
state (`main.cpp:1164-1178`), so recovery cannot be placed only in Home or normal
boot. The same gate must check/replay pending recovery before every WebDAV/USB/HTTP
mutation, covering a transaction created after boot.

## Collision and ambiguity requirements

Preflight every derived destination before physical rename: manga progress
primary/backup/tmp, every supported stats filename including future v6, both
bookmark identities, optional clipping identity, dictionary history/route, shared
JSON stage/backup names, and the physical destination root. Existing WebDAV
overwrite, bookmark merge, clipping merge, cache-directory rename, and cleanup
helpers must never resolve these collisions automatically.

The existing `BookDeletionSnapshot` is bounded to a 16 KiB arena, 64 directories,
and 64 entries and records a path plus `File` or `Manga` kind
(`BookDeletionSnapshot.h:8-32`). Collection scans breadth-first, classifies manga
from `panels.idx`, and records recognized book files plus the manga directory
before deletion (`BookDeletionSnapshot.cpp:43-109`). This is sufficient to retain
the original manga classification after its marker disappears and to decide
cleanup only when the saved backing path is confirmed absent.

It cannot by itself distinguish an original survivor from content recreated at
the same pathname during deletion, does not record a file identity/size, and does
not represent overlapping request-root ownership. The approved conservative rule
remains valid: existence retains metadata, while absence permits cleanup only
when the volume and parent probe are readable; ambiguity keeps metadata and the
journal. Deduplicate HTTP roots before collection and store the snapshot plus root
operation IDs durably. Do not infer successful deletion from a failed open, a
missing manga marker, or a recursive API's single false result. A snapshot bound
failure, late enumeration/allocation/close error, or USB depth violation must
abort before any content mutation.

After partial recursive failure, replay metadata cleanup only for snapshotted
paths conclusively absent. The journal cannot reconstruct already deleted user
content and must never resume deleting surviving content at boot. A later
explicit delete may collect a new operation.

## Minimal implementation grouping

1. **Bounded journal and snapshot core:** operation/phase codec, path arena,
   length/CRC records, token, exact derived-file manifest, collision preflight,
   crossed/reset recovery, and native failure injection. Include stats v6 whole
   files, `dictionary_history.txt`, and `dictionary.bin`.
2. **Checked store adapters:** stage/validate/publish Recent Books and app-state
   JSON under a shared writer freeze; stage current/legacy bookmarks separately;
   expose checked bookmark/clipping deletion and exact path/cache-prefix rewrite.
   Keep these narrow adapters beside their owning stores.
3. **Move orchestrator and boot recovery:** bounded direct/ancestor mapping,
   source token, physical rename diagnosis, per-file publication, reference
   publication, cleanup, blocked-path query, and the post-mount/pre-store-load
   hook in `main.cpp`.
4. **Transport adapters:** route WebDAV directory MOVE and USB RENAME through the
   same result enum/gate, mapping Busy/Collision/RecoveryPending/MutationFailed to
   existing protocol errors. Preserve HTML file-only UI behavior and WebDAV
   nonempty DELETE behavior.
5. **Recursive deletion integration:** extend the existing bounded deletion
   snapshot with durable encoding/root deduplication and confirmed-absence replay;
   use it from HTTP and USB while preserving USB's depth-eight ceiling, then route
   browser metadata cleanup through the same checked post-delete helper.

Implementation tests should exercise direct and ancestor moves, every derived
collision, false rename with token at source/destination/neither, reset at every
phase, v6 maximum appendix and both dictionary files, shared JSON write failure,
and partial recursive deletion with surviving manga whose marker is gone. Hardware
verification remains the approved disposable C3 and X4 Pro/S3 transport matrix;
no cache reset should be needed for a successful move.
