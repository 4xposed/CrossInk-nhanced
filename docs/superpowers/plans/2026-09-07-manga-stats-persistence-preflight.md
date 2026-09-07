# Task 10b language-stats persistence preflight

Date: 2026-09-07

Scope: persistence and accounting preflight for the approved
`2026-09-06-manga-language-stats-design.md`. This report does not change the
approved v6 book, v4 global, `LDAY` appendix, normalization, aggregation, or wire
formats.

## Current ownership and call paths

`BookReadingStats` owns one value per book and currently reads `stats_v5.bin`,
then only `stats_v4.bin`, then `stats.bin` (`BookReadingStats.cpp:66-95`). Its
loader reads at most the fixed 73-byte v5 payload and identifies a version from
the number of bytes returned (`:138-198`). Save truncates the final file in place,
does not check write or close, and returns no result (`:242-274`). Remove deletes
only current, immediately previous, and legacy names (`:276-297`). These paths
must become the sole owners of book v6 prefix/appendix migration and publication.

`GlobalReadingStats` owns the local lifetime value, backup, synced-summary
aggregation, and calendar buckets. `load()` tries the primary and then `.bak` and
sets a process-wide destructive-save block for newer formats
(`GlobalReadingStats.cpp:270-303`). The current writer uses temp, checked
write/sync/close, size verification, backup rotation, and rename (`:197-267`).
`loadAggregated()` starts with local stats and merges accepted synced files while
attempting to exclude this device's MAC filename (`:317-361`). `addStats()`
saturates the existing counters and merges history (`:71-90`). `resetLocal()`
writes an empty value without backup rotation (`:364-372`).

The reader-exit accounting sites are `EpubReaderActivity.cpp:2245-2274`,
`XtcReaderActivity.cpp:722-746`, and `MangaReaderActivity.cpp:737-762`. They each
derive accepted seconds once, update book and global snapshots, record the same
calendar span, then save book and global independently. EPUB and Manga reject
reading spans below ten seconds and session counts below sixty seconds; XTC has
the same cadence. The new language summary and optional `ReadingLanguageSpan`
must use those already accepted seconds and that same start time. XTC supplies
Unknown. No second timer or commit is needed.

Completion and date edits are ordinary saves without a new span:
`BookStatsActivity.cpp:73-82`, `BookActions.cpp:226-244`, the reader completion
helpers, and the other save call sites in the three readers. They must preserve a
validated appendix exactly. `BookStatsActivity` currently clears its dirty flag
after two void saves, even if storage failed. `BookActions` also reports the
completion mutation through its later flow without knowing whether either stats
file was published.

Cache and backup callers depend on the whole versioned file. Book cache cleanup
discovers and preserves the newest bounded set of `stats_v*.bin` files
(`BookCacheUtils.cpp:89-180`), while Home's cache fingerprint explicitly names
`stats_v5.bin` (`HomeActivity.cpp:453`). Stats backup reads the complete global
file into `std::array<uint8_t, CURRENT_FILE_SIZE>` and rejects anything larger
(`StatsBackup.cpp:105-130,184-207`). A variable v4 local file therefore requires
a bounded streamed whole-file copy up to `MAX_LOCAL_FILE_SIZE`; changing
`CURRENT_FILE_SIZE` to that bound would incorrectly conflate the 223-byte summary
with the local format and would put roughly 26 KB on the task stack.

## Compatibility and data-loss traps

1. **Book discovery can strand valid history.** Merely bumping the version makes
   discovery try v6, v5, and legacy, skipping a direct v4 file. Enumerate v6,
   v5, v4, then legacy as approved. Validate exact file length rather than using a
   prefix read that accepts trailing or truncated appendix bytes.

2. **Book save currently destroys the recovery source first.** The in-place
   truncating writer cannot preserve an appendix across a short write, close
   failure, or power loss. Adopt checked temp/sync/close/size/rename publication.
   A corrupt or unknown-newer v6 must produce an explicit non-mutable load state;
   returning a default value and then overwriting the same path loses data.

3. **Global backup recovery is unsafe on the next save.** If primary is corrupt
   and `load()` succeeds from `.bak`, the current writer removes the valid backup,
   rotates the corrupt primary into `.bak`, and only then promotes temp
   (`GlobalReadingStats.cpp:241-264,279-294`). If promotion fails, recovery leaves
   the corrupt file as primary. Retain load provenance and repair/promote the
   validated backup before mutation, or publish without rotating the known-invalid
   primary over it. Tests must inject failure at each rename.

4. **Local and synced v4 have different valid lengths.** Local parsing accepts
   the 223-byte summary with an optional validated appendix; aggregation accepts
   summary-only v4 plus legacy fixed summaries. Give the shared codec an explicit
   local-versus-synced mode. Otherwise aggregation may accept a manually copied
   full local file, and a fixed-size check may reject every normal local file.

5. **Reset needs an explicit fresh-write mode.** A routine null-span save must
   preserve the old appendix, but reset must write an empty summary and empty
   appendix and must not reopen and merge the prior history. After a successful
   explicit reset, clear the process destructive-save block so later saves do not
   remain permanently refused. A failed reset must retain the block and old data.

6. **The self-device aggregation exclusion fails open.** If MAC lookup fails,
   `localFileName` is empty and the condition at `GlobalReadingStats.cpp:337`
   admits every synced file, including a stale snapshot of this device. That can
   double totals and language seconds. Preserve the approved local-MAC exclusion
   by skipping synced aggregation when the local identity cannot be established,
   and test this error path. Do not guess which file is self-owned.

7. **Backups and cache fingerprints need the full new file.** Stream backup data
   in a small fixed buffer, retain the existing temp publication, and verify the
   copied byte count against the validated source size. Update the Home fingerprint
   to v6 and simulator expectations. Keep `BookCacheUtils`' bounded newest-version
   selection; ensure its maximum retains v6 plus required migration sources and
   that temp/recovery names are not mistaken for published stats.

8. **All handles and validation outcomes must be explicit.** The appendix design
   permits a two-pass stream, so every early validation/write error must close both
   files. Invalid appendix data may still expose a validated summary read-only,
   but a mutation must recover or fail; it must never silently replace history
   with an empty appendix.

## Retry and publication contract

Book and global files remain separate transactions. A commit caller updates each
snapshot's fixed summary once, then passes the same span to each target's save.
The save merges daily rows from that target's last durable file; it does not add
the span to the fixed summary again.

Return `true` only when the new final pathname is durably published. Failure
before publication leaves the old file authoritative, so the identical snapshot
and span may be retried for that failed target. Never retry a target that already
returned success with the same span, because that would merge the daily span
twice. A cleanup failure after successful final rename is a logged warning, not a
failed save result: reporting failure after commit would also invite duplication.

Track book and global results independently. If one succeeds and the other fails,
the existing per-file atomicity allows temporary divergence; retry only the failed
target while its snapshot and span still exist. Do not add a cross-file journal.
For UI edits with no span, retain the dirty state and report/log failure until all
required files publish; only then refresh aggregated results or report the edit as
saved.

## Minimal implementation grouping

1. **Shared codec and native tests:** add `ReadingLanguageStats.{h,cpp}` for
   normalization, fixed 64-byte summary coding, saturation, and streamed `LDAY`
   row validation/merge. Keep buffers bounded and expose explicit local and synced
   parse modes.
2. **Book persistence:** change `BookReadingStats.{h,cpp}` and its focused tests
   for v1-v6 migration, newer-format refusal, null-span preservation, exact-length
   validation, atomic publication, failure injection, and complete removal.
3. **Global persistence and aggregation:** change `GlobalReadingStats.{h,cpp}`
   and focused tests for v1-v4 migration, backup-recovery provenance, explicit
   reset, deterministic two-pass language aggregation, self-MAC failure, and
   summary-only synced parsing.
4. **Accounting callers:** wire EPUB, XTC, and Manga reader exits to normalized
   language and per-target bool results; update completion/date actions to
   null-span saves and honest dirty/result handling. Keep existing thresholds and
   total-counter behavior.
5. **Whole-file ownership:** update `StatsBackup.cpp`, `BookCacheUtils.cpp`, the
   Home fingerprint, file-format documentation, and simulator/native assertions.
   Nearby transport remains in its separately approved Task 10b sync group and
   consumes the shared summary codec rather than the local appendix writer.

This ordering keeps each persistence owner and its tests together and lets caller
wiring depend on final bool/save semantics. Hardware verification should perform
one EPUB, XTC, and Manga session on a C3 device, power-cycle after successful
saves, then repeat with injected SD write/rename failures: old totals/history must
survive, logs must identify book versus global failure, and retry must add exactly
one daily-language span.
