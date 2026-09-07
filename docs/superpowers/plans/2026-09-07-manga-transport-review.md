# Task 10c transport implementation review

Date: 2026-09-07. Scope: Task 10c brief, approved transport design, accepted journal contract, implementation report, and `/private/tmp/crossink-manga-transport10c-review.patch`. CodeGraph was used before focused call-path checks. The review package was read once; its 65,230-token output exceeded the tool channel and was truncated, so the changed implementation was judged through the package context that was returned plus CodeGraph's verbatim current source for the named recovery, cleanup, ownership, and transport risks.

## Spec compliance

**❌ Issues found.** The implementation establishes the required bounded journal, token-authorized move recovery, checked owner publication, confirmed-absence deletion replay, shared-owner reload gate, transport routing, and boot retry policy. Two cleanup paths violate the approved fail-closed/deletion-parity requirements.

### Important — confirmed manga deletion leaves every disposable cache artifact behind

`removeMangaMetadata()` opens the deleted book's `/.crosspoint/manga_<hash>` cache but removes only names for which `durableCacheName()` returns true (`src/util/BookFolderMutation.cpp:857-897`). Pixel payloads/identities, thumbnails, OCR candidates, temporary cover files, and any other derived children are skipped, and the cache directory is never removed. The existing behavior that Task 10c was required to match removes the entire manga cache (`src/activities/home/BookActions.cpp:112-122`), while the new `clearMangaDisposableCache()` confirms that names outside `durableCacheName()` are precisely the disposable children (`src/util/BookMutationStorage.cpp:194-227`). After a successful physical deletion this therefore leaves potentially large orphaned cache data and breaks browser/HTTP/USB deletion parity.

After the confirmed-absence outcome is durable, remove the complete owned manga cache with checked enumeration/removal semantics, including disposable children and the final directory. Recovery must remain idempotent and must record `DeleteMetadataDone` only after the cache is absent. Add a deletion test containing pixel, thumbnail/OCR, unknown disposable files, durable stats/history/dictionary files, and a nested disposable directory; all cache content must be gone after success and after a cut/replay.

### Important — cleanup accepts and deletes the out-of-range `file-1024.stage`

The transaction namespace cleanup recognizes four-digit `file-NNNN.stage` names, but rejects only `index > 1024` (`src/util/BookFolderMutation.cpp:779-792`). The journal permits at most 1,024 durable records, whose valid global ordinals are `0..1023`; `file-1024.stage` can never be transaction-owned. Deleting it contradicts the same function's rule that unknown names are retained (`:764-765`, `:793-807`) and the binding rule that semantic corruption or unexpected debris must not redirect cleanup.

Change the bound to `index >= 1024` (prefer the shared journal maximum constant) and add cleanup/recovery cases for `file-1023.stage` accepted and `file-1024.stage` retained with `RecoveryPending`.

## Strengths

- Move recovery proves the physical operation using the source/destination token and exact root states, then publishes durable files and shared references before deleting staged sources/token (`src/util/BookFolderMutation.cpp:829-855`, `:1056-1087`). A false rename return cannot independently choose rollback or forward recovery.
- Delete replay recomputes absence without restarting physical removal, freezes it into CMO1, rechecks every selected path before metadata cleanup, and publishes shared references only after every selected cleanup phase is recorded (`BookFolderMutation.cpp:899-939`, `:1090-1131`). Recreated or unreadable content remains pending.
- Terminal recovery fully reloads and verifies a `.finalize` journal plus final output fingerprints before cleanup (`BookFolderMutation.cpp:941-974`). The final marker remains until both shared owners reload successfully; transaction buffers are out of scope before the cold reload (`:1019-1041`).
- The journal parser validates CRC, operation ID, sequence, legal phase order, exact indices, and outcome mask bounds (`src/util/BookMutationJournal.cpp:87-139`). The checked directory API and bounded snapshot expose scan failure separately from clean exhaustion; the snapshot owns a fixed 16 KiB arena and 64-entry/directory limits (`src/util/BookDeletionSnapshot.h:19-49`).
- Shared-owner coordination stays at the app boundary: the mutation core calls `ActivityManager::prepareForFolderMutation()` and reloads the existing stores through `BookMutationOwners`, without owning renderer or activity-stack mutation (`src/util/BookMutationOwners.cpp:21-41`).

## Quality verdict

**Task quality: Needs fixes.** The architecture is deliberately bounded and the recovery phases are separated cleanly, but both findings affect destructive cleanup invariants and should block this task gate until fixed. The first leaks user-visible storage after every manga deletion; the second lets an impossible stage name be treated as owned and deleted.

## Evidence and cannot-verify items

The implementation report records 38/38 focused tests, including all 2,054 injected I/O positions, the earlier full native 771/771 run, simulator patch 4/4, and a successful final C3 build at 6,498,880 bytes with 54,720 bytes free. I did not rerun covered tests. Root reported the simulator pair still in progress when this review began. Real FAT/card faults, hardware cancellation/drain behavior, actual C3/S3 heap fragmentation, cumulative main-task stack high-water, WebDAV/USB clients, and power cuts at physical rename/publication boundaries remain unverified on hardware. The inherited cold `std::string`/`vector` OOM exposure is documented and was an accepted ruling rather than a new finding.

Focused outside-diff checks were limited to named risks: existing manga deletion/cache-clear behavior for deletion parity; `ActivityManager` owner quiescence through `BookMutationOwners`; and app HAL checked-enumeration semantics. No builds, tests, serial actions, or commits were performed.

## Complete-package review addendum

The initial tool rendering omitted part of the 6,115-line package. I subsequently read every omitted store, lifecycle, boot, route, HAL, codec, JSON, simulator-patch, and test hunk in bounded ranges. The already inspected `BookFolderMutation.cpp` transaction/recovery core was not reread; its full current functions had been supplied verbatim by CodeGraph. I also checked the pre-change USB recursion only for the concrete depth-policy risk below. This completes the task-scoped package review.

### Additional Important — USB's supported depth-eight tree is rejected one level early

`BookDeletionSnapshot::append()` rejects every child encountered when the parent directory has `depths_[index] >= maxDepth`, regardless of whether that child is a directory or a file (`src/util/BookDeletionSnapshot.cpp:73-104`). USB passes its existing limit of eight (`src/network/UsbSerialFileTransfer.cpp:548-567`). The pre-change USB recursion rejects only when the child call's depth is greater than eight, so a file or empty directory at depth eight remains within the supported tree (`/private/tmp/crossink-manga-transport10c-before/src/network/UsbSerialFileTransfer.cpp:265-302`). The replacement snapshot therefore refuses valid depth-eight content before deletion and changes the public USB behavior the brief says to preserve.

Apply the limit when enqueueing a child directory whose next depth would exceed the ceiling; do not reject ordinary files merely because their parent is at the maximum depth. A directory at the maximum depth may be accepted only when its complete checked scan proves that processing it requires no deeper traversal. Add boundary tests for a file and empty directory at depth eight succeeding, and a child below depth eight failing before physical mutation. The existing test covers only the failing depth-nine case (`test/book_folder_mutation/MutationTest.cpp:242-250`).

### Definitive verdict

**Specification: FAIL with three Important findings. Task quality: Needs fixes.** The two original cleanup findings remain unchanged, and the complete-package pass adds the USB depth regression above. No additional Critical or Important issues were found across the remaining store, lifecycle, boot, route, HAL, JSON, packaging, and test hunks. The reported validation and hardware limitations remain as recorded above; no tests or builds were rerun.

## Scoped fix-round 1 review

Date: 2026-09-07. Scope was limited to `/private/tmp/crossink-transport10c-review-fix1.patch` and the fix-round report evidence. The complete 271-line fix was read once; no tests or builds were rerun.

- **Full manga cache deletion parity: ADDRESSED.** `removeMangaMetadata()` now performs a bounded iterative postorder traversal, checks every child close and directory enumeration/allocation result, removes files and nested directories, removes the cache root, and confirms its absence before allowing `DeleteMetadataDone` (`src/util/BookFolderMutation.cpp:862-924`). It reuses the existing path/name workspace and never recurses. A failed child scan leaves recovery pending and shared references unchanged; focused tests cover nested disposable/durable children, a cut before the metadata phase, unrelated-cache retention, and retry after a child access failure (`test/book_folder_mutation/MutationTest.cpp:85-137`).
- **Out-of-range stage ordinal: ADDRESSED.** Cleanup now rejects `index >= 1024`, matching valid ordinals `0..1023` (`src/util/BookFolderMutation.cpp:781-792`). The regression proves 1023 is cleaned under a valid staging marker while 1024 remains untouched with recovery pending (`test/book_folder_mutation/MutationTest.cpp:119-137`).
- **USB depth-eight boundary: ADDRESSED.** Snapshot traversal now applies the ceiling only when a child directory would require descent beyond the current maximum; files within a depth-eight directory remain accepted (`src/util/BookDeletionSnapshot.cpp:95-118`). Tests cover files and an empty directory at the boundary plus rejection of a deeper directory before mutation (`test/book_deletion_snapshot/BookDeletionSnapshotTest.cpp:84-98`; `test/book_folder_mutation/MutationTest.cpp:399-408`).

**Specification: PASS for fix round 1. Quality: APPROVED.** All three Important findings are closed, and no new breakage was found in this fix delta. The implementer reports 43/43 focused tests passing in 13.70 seconds, including the existing 2,054-position fault sweep; I did not independently rerun them. Root's C3 and simulator builds remained in progress at review time.
