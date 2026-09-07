# Manga transport folder mutation design

Date: 2026-09-06. Design only; no implementation or validation claimed.

## Scope and evidence

Implement the transport section of `2026-09-06-manga-library-completion-plan.md` and its completion-gap audit. Support direct manga folder moves and moves of their ancestors through existing WebDAV MOVE and USB serial RENAME. Reuse deletion bookkeeping for HTTP recursive DELETE and USB REMOVE. Keep on-device/HTTP folder rename and move unsupported, WebDAV directory DELETE empty-only, and the SDK pin unchanged. Do not change EPUB move policy or global reading totals. Raw mass-storage/offline renames remain outside firmware observation.

Current entry points and constraints:

- `src/util/BookDeletionSnapshot.h:16` bounds collection at 16 KiB of path bytes, 64 directories and 64 metadata entries. `BookDeletionSnapshot.cpp:43` walks iteratively, recognizes `panels.idx` without parsing a book, and closes handles. It also records ordinary book files, which deletion needs.
- `src/activities/home/FileBrowserActivity.cpp:422` allocates that snapshot fallibly, deletes the directory, then invokes `BookActions::clearMangaMetadata` or `clearFileMetadata`. `BookActions.cpp:109` removes manga cache, progress and bookmarks. Bookmark deletion currently has no checked result.
- `src/network/UsbSerialFileTransfer.cpp:548` and `:569` are REMOVE and RENAME. RENAME already rejects an existing destination. REMOVE has a separate eight-level recursive limit (`:265`). Serial transfers are Home-only (`:249`, `src/main.cpp:1453`).
- `src/network/WebDAVHandler.cpp:491` implements MOVE. Its overwrite branch removes the destination before opening/renaming the source (`:539`), and clears cache before rename (`:549`). `clearBookCache` currently delegates to preserving-user-state cleanup (`src/util/BookCacheUtils.cpp:335`); it preserves stats but still mutates source caches before a failed move.
- `src/network/CrossPointWebServer.cpp:1256` bypasses metadata snapshots for recursive DELETE. WebDAV rejects nonempty directory DELETE (`WebDAVHandler.cpp:421`).
- `MangaProgressStore.cpp:84` identifies progress by CRC32 of the exact folder path under `/.crosspoint/manga-state/<crc>.bin`, with `.bak` and `.tmp` recovery siblings. Its 12-byte v1 record does not store the source path; do not infer ownership from its payload.
- `lib/MangaPanel/MangaCover.cpp:134` identifies cache by the same path CRC under `/.crosspoint/manga_<crc>`. Durable `stats.bin` and `stats_v<digits>.bin` live there; `BookCacheUtils.cpp:100` defines recognition. Preserve bytes, including versions the current reader cannot parse. Pixel/thumbnail data is disposable.
- Bookmarks use CRC and legacy `std::hash` filenames (`BookmarkStore.cpp:31`), and embed a path in the header (`:459`). Existing `migrateForFilePath` merges destination bookmarks and deletes source files (`:625`–`:677`); it is not the migration primitive needed here.
- `RecentBooksStore::updatePath` (`RecentBooksStore.cpp:126`) preserves ordering but returns void and ignores persistence failure. `removeByPath` is also best-effort. `APP_STATE.openEpubPath` is the shared resume identity (`CrossPointState.h:25`). Recovery must precede recent pruning and resume dispatch; boot loads these stores at `main.cpp:1157`.

## Chosen approach

Use one bounded snapshot and one SD-backed transaction journal, owned by an app utility `src/util/BookFolderMutation.{h,cpp}`. Stage all affected durable records before physical rename, publish them only after rename succeeds, and keep source records until all destinations and references are durable. Recover forward after physical success; never roll back a moved book after some metadata has been published.

An in-memory callback loop is smaller but loses recovery evidence on reset. Reusing existing bookmark/EPUB move helpers destroys sources too early and merges unrelated records. A generic filesystem transaction framework is unnecessary: this utility handles one folder mutation at a time and the explicit stores listed here.

## Bounded snapshot and proposed API

Retain the existing class/file name to avoid an unrelated rename. Add a collection mode so move collection records manga only while delete collection retains all current entries:

```cpp
enum class SnapshotMode : uint8_t { DeleteMetadata, MangaMove };
// Existing collect(root) delegates to DeleteMetadata.
bool BookDeletionSnapshot::collect(const std::string& root, SnapshotMode mode);
```

MangaMove still visits all directories (including manga directories), so ancestor and nested indexed books are covered. Keep 16 KiB/64 directories/64 entries. Fail closed on exhaustion, truncated names, open/close/allocation failures; never act on a partial snapshot. A folder without manga follows the existing non-manga move path after successful complete collection. Do not reinterpret a failed scan as an empty result.

Proposed public interface, with the implementation owning the actual rename to prevent callers from violating ordering:

```cpp
namespace BookFolderMutation {
enum class Result : uint8_t {
  Complete, Busy, InvalidPath, SnapshotLimit, Collision, StorageError,
  MutationFailed, RecoveryPending
};
Result move(const char* oldRoot, const char* newRoot);
Result remove(const char* root);  // recursive HTTP/USB deletion only
Result recoverPending();
bool hasPending();
}
```

The utility acquires a nonblocking app-owned mutation gate. Callers map results to their transport response, not UI dialogs. `RecoveryPending` explicitly means a physical mutation may already have succeeded; USB emits `ERR:metadata_recovery_pending`, HTTP/WebDAV return 503 with that explanation. Do not report an ordinary rename failure or success while bookkeeping is pending.

Allocate one snapshot with `makeUniqueNoThrow`; its roughly 17 KiB footprint cannot fit in a 2–4 KiB task stack and should not permanently consume C3 RAM. Allocate one reusable, checked workspace of at most 4 KiB for source/destination paths, directory names and a 256-byte copy buffer. Limit normalized paths to 1023 bytes plus terminator; reject longer derived paths. No growing vectors of records or decoded MangaBook instances. Process bookmarks one header/record at a time and retain their exact existing records.

Journal bounds: one operation, at most 64 books, at most 16 durable files per book (progress family, two bookmark identities, and stats files). Abort preflight on excess; do not drop files. Stream journal and staged payloads to SD; never load the whole journal in RAM. Journal header is versioned, length-bounded and CRC-protected, with operation kind, normalized roots and snapshot paths. Each file record stores source/stage/destination relative identity, source length and CRC, output length and CRC. Phase records are append-only with sequence and CRC; ignore only a torn final append. A corrupt header/interior record blocks mutation and retains all data. Document this new transaction format in `docs/file-formats.md`; no manga binary book/progress format change is required.

## Path and collision policy

Normalize separators, absolute roots and trailing slashes consistently with transport rules before snapshotting. Reject root/protected paths, dot components, moves into the source subtree, and case-only aliases on FAT. Use component-boundary comparison: a book equal to oldRoot maps to newRoot; a book beginning `oldRoot + '/'` maps to `newRoot + unchanged suffix`. `/A` must never match `/AB`.

For a manga-bearing folder move, reject any existing physical destination even with WebDAV Overwrite:T (409; retain 412 for Overwrite:F). Branch before the existing destination-removal code. This deliberately narrows destructive WebDAV overwrite for manga-bearing folders; preserving unrelated destination data takes precedence. Existing file moves retain their behavior outside this branch.

Before mutation, reject any existing destination progress primary/backup/temp, either bookmark filename, destination recent entry, or resume reference pointing at a different destination identity. Conservatively reject an existing destination manga cache directory, even if it looks disposable: its hash-only identity cannot establish ownership. Reject old/new CRC equality and any alias between source and destination storage keys across snapshot entries, including legacy bookmark hashes. Unknown/corrupt bookmark headers are errors, not permission to overwrite. Existing transaction-owned published files are accepted only during recovery and only after exact length/CRC and embedded-path validation. Recheck collision conditions under the mutation gate immediately before physical mutation/publication.

Legacy hash-only progress and stats cannot prove that a preexisting source record belongs to this book rather than an unrelated book with the same CRC. This is an existing format limitation; the new operation must never invent an ownership claim. Detect aliases within the snapshot and all occupied destination keys. Strong global source ownership would require a separate path-bearing format migration, outside this bounded change.

## Move ordering and recovery

1. Quiesce relevant owners, acquire the mutation gate, recover an existing journal or return Busy/RecoveryPending. Validate roots and close probe handles. Collect the complete snapshot and run all collisions before touching source records or physical folders.
2. Create exclusively `/.crosspoint/manga-mutation/`; an existing directory is recovery input, never stale data to delete blindly. Write and verify the immutable journal. Stage every source durable file, closing each reader before reopening that path. Preserve progress primary/backup/temp bytes independently; do not call `load()` because it can recover/rename the source backup. Stage all recognized stats files byte-for-byte. If the concurrent language-stats implementation introduces a per-book appendix/sidecar, register it in this same durable-file enumeration and bound; global day/language aggregates are path-independent and remain untouched. Stage both bookmark identity files with only their embedded book path rewritten; keep title/author and records unchanged. Validate bounded lengths and supported bookmark layouts before staging. Do not merge the two bookmark files: preserve current and legacy behavior at their corresponding new keys.
3. Stage full recent/state JSON replacements using checked store serialization, retaining all unrelated fields. Repoint only matching manga entries, preserving list position, metadata and cover state; rewrite a cover cache prefix only at a slash boundary. Repoint exact `openEpubPath` matches. Do not add books to recents or alter global/completed counts. Back up the original shared JSON files in the transaction directory. The stores must be frozen against writers through publication.
4. Append and verify PREPARED. Put an exclusively-created small transaction token in the source root using reserved `.crossink-move-token`; refuse an existing token. It contains journal operation ID/CRC. Close it before rename. This token moves with the folder and distinguishes successful rename from unrelated destination creation after reset. Failure to create/verify it aborts without physical rename.
5. Call `Storage.rename(oldRoot, newRoot)` with no open source handle. On false, inspect token location: if still at source, remove only the owned token and staging/journal after checked cleanup, then return MutationFailed. Source progress/stats/bookmarks/recents/resume and destination data remain unchanged. If token location proves movement despite a false return, recover forward. If ambiguous, retain journal and both roots untouched and return RecoveryPending.
6. With token verified at destination and source absent, append MOVED. Publish each staged durable record with no-clobber rename only after checking destination absence. On retry, an already published file must match the journal. Keep original source records intact. Append progress only after close/readback checks. A failure leaves token, journal and remaining stage files for retry. No cache clearing occurs here.
7. Publish staged recent/state JSON using transaction-owned backup and rename, retaining originals until both files are verified. After a reset, recognize original/staged/final JSON by stored length/CRC; never overwrite a third, unexplained version. Reload stores only after both publications succeed. Mark REFERENCES_DONE durably. This avoids best-effort `updatePath`/`removeByPath` being mistaken for persistence success.
8. Only now remove original per-book durable files explicitly listed in the journal, checking each result. Leave old pixel/thumbnail cache debris for ordinary cache cleanup; deleting entire old cache directories is unnecessary and risks unknown files. Remove the owned destination token, mark DONE, then remove transaction-owned stage/backups/journal last. Cleanup failure remains pending and idempotently retryable.

Boot recovery runs after storage is mounted but before loading/pruning recents or selecting resume; it also runs before accepting subsequent mutations. Source token + source present + destination absent means pre-move abort: discard owned staging and token, leave source state. Destination token + source absent means resume publication. Both/neither roots, a missing/mismatched token before DONE, unexpected destination data, or corrupt journal means retain evidence and block affected reading/mutation until resolved. Do not guess that destination existence proves success. An incomplete staging transaction without PREPARED cannot have invoked rename and can discard only its owned files.

After REFERENCES_DONE, cleanup replay does not require the token if its removal was recorded or all final records and references are verified; this covers power loss between token deletion and DONE. An unresolved transaction must block opening affected books, bookmark enumeration of them, cache clear, stats reset and mutations that could rewrite their stores. Permit unrelated browsing only where it cannot prune or persist shared recents/state. The simplest first implementation keeps the firmware in a recoverable transfer/Home state and retries on explicit operation/boot, without creating a second background worker or a retry loop that hammers SD.

## Deletion ordering

Use `BookDeletionSnapshot::collect(DeleteMetadata)` before HTTP/USB recursive removal, with the same bounds as the browser. Preserve USB's existing depth-eight ceiling; preflight it before removal. Deduplicate overlapping HTTP roots so one request does not operate twice on an already removed subtree.

Store the delete snapshot in the same durable journal before physical deletion. No durable metadata is deleted before backing content is removed. Keep existing recursive removal behavior; it is not an atomic operation and cannot restore already deleted book bytes after partial failure.

After full success, call checked equivalents of `BookActions::clearMangaMetadata`/`clearFileMetadata` for every saved identity, remove matching recents and clear an exact matching resume path. Apply the browser's favorite/preferred sleep cleanup and invalidate SleepImageIndex. Checked bookmark deletion must include current/legacy files and report I/O failures; a void helper cannot acknowledge transaction completion. Persist shared state with the same checked publication helper as move.

On partial recursive failure, clean metadata only for entries whose backing file or whole manga folder is confirmed absent while the volume remains mounted/readable. A surviving manga directory keeps its state even if its marker was already deleted. Never reclassify it by `isMangaFolder` after removal. A failed/ambiguous existence probe keeps records and journal for retry. Do not automatically resume deleting surviving user files at boot: replay only metadata cleanup for confirmed-absent snapshot entries. A later explicit delete may finish the content deletion. Report partial failure, retaining metadata of survivors.

Apply the shared checked cleanup path to the device browser when implementing to prevent divergence, without adding move UI. Do not route WebDAV directory DELETE through this recursive API.

## Ownership and integration requirements

Home-only serial authorization does not alone prove a future cover worker is idle. Before collection, prevent activity transitions and request cancellation/drain of foreground reader, prefetch, dictionary lookup and cover generation through their existing ownership/lifecycle contracts. No renderer or activity stack mutation from the transport utility. Do not hold RenderLock while waiting for a worker that needs it; drain first, then acquire short locks only for shared state publication. Reject Busy if the caller cannot establish quiescence.

Trace the actual WebServer callback task before implementation and marshal the mutation to its authorized owner if needed. Serialize WebDAV, HTTP, USB and any shared-store writers with the gate. The new utility uses app HAL only; filesystem no-clobber is implemented by checks under this exclusive firmware gate, not an assumed SDK primitive. Offline/raw USB edits during a transaction are unsupported; mismatches must stop recovery safely. FAT/card failure can tear directory operations, so the journal provides checked recovery, not a claim of fully atomic or power-loss-proof storage.

## Implementation acceptance checks (not run for this design)

- Disposable direct `/transport-test/Book` and ancestor `/transport-test/Series` moves through both transports retain page/panel, completion/time stats (all versions), bookmarks, recent order and resume. `/Series2` references remain unchanged.
- Snapshot arena/directory/book limits, long derived paths, low-heap allocation failure and USB depth limit fail before content mutation. Include a 65th manga folder and a late scan error.
- Existing physical destinations with both WebDAV overwrite values, orphan destination progress `.bak`/`.tmp`, cache, current/legacy bookmarks, recent/resume collisions and injected equal hashes preserve every original/destination byte.
- Inject failure before/after each rename, write, close, phase append and cleanup. Reset after physical rename but before MOVED, after each published durable file, between shared JSON files and after token removal. Retry must converge without bookmark duplication, recents reorder, lost stats or blind overwrite.
- Full and partial HTTP/USB recursive deletes cover nested manga plus EPUB/TXT files. Surviving manga retains state; deleted paths clean state even after markers vanish. Recovery never continues content deletion without a new request. WebDAV still rejects a nonempty directory.
- Verify actual C3 and X4 Pro/S3 transport paths using only disposable fixtures, including pending worker cancellation, reader resume after reboot, low internal heap, failed SD writes and recovery logs. No cache reset is required for successful migration; use only disposable cache resets in cold-cache checks. Run relevant native failure-injection tests and matching PlatformIO builds in the implementation task, not this design task.

## Readiness and limits

### Task 9 integration addendum

The durable cache-file manifest also includes `dictionary_history.txt` and
`dictionary.bin`: shared manga lookup uses the existing per-book history and
dictionary route. Preserve both through move, failure/recovery and cache clear;
neither is a derived candidate scan. `BookCacheUtils.cpp:24–37` already preserves
history but omits the route file. Include these two files in the bounded file
count, collision preflight and publication tests. OCR scan files are disposable.

The store mapping, bounded API, collision policy and operation ordering are ready for implementation. Boot recovery and checked shared JSON publication are required parts of the work, not optional follow-up. The callback-owner/quiescence integration must be verified against the cover/prefetch work being integrated concurrently. Legacy hash-only source identity and filesystem I/O ambiguity remain explicit limits; neither permits overwriting an occupied destination or discarding recovery evidence.
