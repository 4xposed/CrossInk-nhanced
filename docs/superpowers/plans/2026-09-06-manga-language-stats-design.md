# Bounded reading-language attribution design

Date: 2026-09-06. Design only; no firmware changes or builds performed.

## Recommendation and acceptance boundary

Add eight bounded language totals to existing book/global snapshots and stream a
730-day appendix inside their existing versioned files. Send only the compact
summary in Nearby sync. This preserves language/day attribution locally and
supports all-device language totals without a new transfer protocol. It does not
provide all-device per-calendar-day language seconds: existing sync does not
provide per-calendar-day seconds either. Make that boundary explicit in docs and
UI; do not label local day results as all-device results.

This satisfies the handoff's reading-time/language attribution and the followup
plan's bounded persisted day accounting with consistent language-total aggregation.
If acceptance instead requires remote daily-language history, a chunked transport
is additional work; do not claim that parity from this implementation.

The alternatives are a separate sidecar (smaller initial format change but creates
cross-file consistency, reset, move, backup and sync hazards) or transporting the
whole history (requires chunking, reassembly, retry and protocol compatibility).
An in-file appendix plus compact wire summary is the smallest coherent boundary.

## Evidence and integration points

- `BookReadingStats.h:9–40`: compact per-book value, current `stats_v5.bin`, legacy
  reads, save/remove and calendar-bucket helper. Current v5 payload is 73 bytes
  (`BookReadingStats.cpp:242–274`).
- `GlobalReadingStats.h:9–47`: compact values are returned/copied by load and
  aggregation; v3 payload is 159 bytes. Do not embed a day matrix in this struct.
- `GlobalReadingStats.cpp:197–267`: temp/sync/close/backup/rename write sequence;
  extend this streaming writer. `:317–362` aggregates imported files and excludes
  the current MAC. `:372` resets local data while retaining existing backups.
- `NearbyStatsSyncActivity.cpp:95–98,140–165,534–553`: current protocol has a
  14-byte header, byte-sized payload length and fixed summary storage. Simply
  setting CURRENT_FILE_SIZE to a large history file truncates the byte-sized cap.
- `EpubReaderActivity.cpp:2248–2276`, `XtcReaderActivity.cpp:722–738` and
  `MangaReaderActivity.cpp:506–532`: accepted session seconds feed both the total
  and existing calendar span. Manga language comes from optional meta.bin
  metadata; EPUB exposes `Epub.h:126`. XTC has no verified language source.
  TXT currently does not commit these statistics; do not add TXT tracking here.
- `BookStatsActivity.cpp:67–81,291–320,469–495` and `BookStatsView.h:23–33`:
  per-book, this-device and all-device existing screens and completion/date saves.
- `BookCacheUtils.cpp:100–123,134` already discovers versioned `stats_v*.bin` for
  preservation. `HomeActivity.cpp:452` hardcodes v5 in its cache fingerprint.
- `SettingsActivity.cpp:1033` calls local reset; `StatsBackup.cpp:19` copies the
  global file. Keeping history in the same file preserves those ownership rules.

Line numbers describe the inspected working tree and may shift during manga work.

## Fixed language summary and normalization

Use `ReadingLanguageTotals`, exactly eight entries of `{char tag[4]; uint32_t
seconds;}` (64 bytes, verify with static assertions). Slot 0 is `und` (unknown),
slot 1 is `mul` (Other languages), slots 2–7 are six named primary language tags.
Empty named slots have four zero bytes and zero seconds. Slot identities remain
stable for the lifetime of a persisted local file; never relabel past seconds.

Normalize from a bounded input view: trim ASCII whitespace, reject inputs over
63 bytes, map ASCII case to lowercase, accept `_` as `-`, validate nonempty
alphanumeric subtags (1–8 characters) and use only a 2- or 3-letter ASCII primary
subtag. Strip region/script subtags intentionally (`EN_us` -> `en`, `zh-Hant` ->
`zh`). Empty/malformed, `und`, and private-use-only tags become `und`; `mul`
becomes Other. Implement a small static alias table for common three-letter
counterparts (`eng/en`, `spa/es`, `fra/fre/fr`, `deu/ger/de`, `jpn/ja`, `zho/chi/zh`,
`ita/it`, `por/pt`, `rus/ru`, `kor/ko`). Other syntactically valid primary tags
remain distinct; do not imply an exhaustive ISO registry or detect text language.
Never use metadata as a filename. Use bounded comparisons and ASCII logic.

On local insertion use an existing matching slot, then the first empty slot;
when all six are occupied attribute additional named languages to Other. Unknown
remains separate from capacity overflow. The summary is finite even with hostile
metadata, and retains the first six locally encountered languages until reset.

Use saturating uint32 counters for language totals/day cells. Sum with a wider
intermediate or saturating helper. Existing total-time behavior remains unchanged;
in particular do not fix unrelated legacy total overflow in this task. Under
normal unsaturated totals the saturated sum of language totals equals total time.
At numeric saturation expose capped values and test them, without wrapping a
language counter or manufacturing negative Unknown residuals.

## Exact disk and wire layouts

All integers are explicit little-endian fields; no raw C++ struct serialization.

- Book v6: existing v5 offsets 0–72, with version byte 6, followed by the 64-byte
  language summary at 73–136. Fixed summary size: **137 bytes**.
- Global v4: existing v3 offsets 0–158, with version byte 4, followed by the
  64-byte language summary at 159–222. Fixed summary size: **223 bytes**.
- Each summary entry stores tag bytes (NUL-terminated within four bytes), then
  seconds as LE32. A named slot with seconds zero is legal. Reject duplicate tags,
  invalid reserved tags, missing terminators, or occupied holes if the writer
  enforces contiguous insertion. Do not use locale-dependent normalization on disk.
- Optional local appendix immediately follows the summary: ASCII `LDAY` (4),
  appendix version 1 (u8), reserved zero (u8), row count (u16), anchor day (u32).
  Header is **12 bytes**. Each row is day index (u32) plus eight seconds counters
  (8*u32): **36 bytes**. Rows are strictly increasing and unique. Maximum 730 rows,
  range `[max(1, anchor-729), anchor]`; day zero means no valid day and is not a row.
- Full file length is exactly summary size + 12 + 36*rowCount. Maximum local book
  file **26,429 bytes**, global file **26,515 bytes**. Empty local histories have
  a 12-byte appendix with zero rows and anchor. Reject extra/truncated bytes,
  unsupported appendix versions, invalid dates, and out-of-window rows.
- Summary-only v4 global files are valid synced snapshots, explicitly lacking day
  detail. A locally missing appendix is accepted as unavailable history; the next
  save adds an empty appendix. Summary-only book v6 may be accepted for recovery,
  but the normal writer always includes the appendix.

Keep separate constants `SUMMARY_FILE_SIZE` and `MAX_LOCAL_FILE_SIZE`.
`CURRENT_FILE_SIZE` must not become the variable full-file bound in the sync code.
Document versions and offset tables in `docs/file-formats.md` before implementation.

## APIs and streamed update

Proposed shared header `ReadingLanguageStats.h`:

```
struct ReadingLanguageTotals; // fixed 64-byte representation described above
struct ReadingLanguageSpan {
  ReadingStatsDateTime localStart;
  uint32_t seconds;
  char normalizedTag[4];
};
bool normalizeReadingLanguage(std::string_view input, char (&out)[4]);
uint8_t addReadingLanguageSeconds(ReadingLanguageTotals&, const char* tag,
                                 uint32_t seconds);
```

Add a `languageTotals` member to BookReadingStats and GlobalReadingStats. This
adds 64 bytes to each copied value, not 26 KB. Global value size should remain
below 256 bytes; assert that on native and firmware targets. Existing readers
already own these snapshots, so no persistent new allocation is necessary.

Extend save APIs to return bool and accept an optional span:

```
bool BookReadingStats::save(const std::string& cachePath,
                            const ReadingLanguageSpan* committedSpan = nullptr) const;
bool GlobalReadingStats::save(const ReadingLanguageSpan* committedSpan = nullptr) const;
bool visitLocalReadingLanguageDays(const char* statsPath,
    bool (*visitor)(void*, uint32_t day, const ReadingLanguageTotals&), void* context);
```

The commit caller adds exactly its already-accepted session seconds to the fixed
summary and passes the same span to save. Default saves (completion/date changes,
pace changes) preserve the appendix byte-for-byte after validating it. New helpers
must not introduce a second timer, periodic page write, new session threshold or
extra stats commit. Preserve the existing reader's commit cadence. If a caller
retries a failed save, reuse the same snapshot/span; merging from the last durable
file then replacing it does not double-apply the span. Never report success after
short write/sync/close/rename failure.

For a span save, open the old file once and the temp file once (different paths).
Read the prefix, validate and stream rows in order, merge the committed interval's
midnight-split pieces into matching day cells, omit expired rows, emit intervening
new rows, then patch/finalize the row count. Keep one old row and one output row
on stack; no vector/history matrix. Validation may use a separate first pass,
closing the first reader before reopening. A single 223-byte summary buffer plus
nested filesystem objects can exceed the desired stack budget; serialize existing
prefix and eight-byte entries separately and measure maximum frame sizes.

Choose anchor as max(previous anchor, latest valid day of the new span), so moving
the clock backwards does not erase newer history. Spans older than the window
still increment lifetime language totals; omit only daily detail. Invalid RTC
spans increment language totals but generate no dated rows. Language totals are
not recomputed from the rolling history. History expiry never reduces totals.

Use the same localStart + seconds calendar convention already used by each
reader. It compresses paused/idle gaps out of a session; correcting that would
change existing calendar semantics and needs a separate design. Do not claim
wall-clock-exact day attribution across pauses or clock corrections. Midnight
splitting of the supplied committed interval is exact, including leap days.

A new metadata language applies to the new session only; history slots keep old
attribution. All supported inputs lacking metadata use Unknown. Historical bytes
cannot reveal the old language or exact dates, so migration seeds the entire
legacy total into Unknown and starts with no day rows. Do not backfill from today's
metadata, start/finish date, weekday buckets, or the global history bitset.

## Migration, preservation and failures

Keep existing v1–v5 book readers and v1–v3 global readers. Book discovery must
explicitly include v6, v5, v4 and legacy stats.bin: advancing only the automatic
previous-version fallback currently drops direct v4 filename access. Unknown newer
files retain the existing refusal-to-overwrite behavior; introduce the same
protection for book saves rather than overwriting unseen newer attribution.

Book writes adopt checked temp/sync/close/rename publication, because truncating
the old file before copying its appendix loses history. Book removal deletes all
supported version filenames and temp/recovery files. Cache preservation and move
logic carry v6 as one file; completion/date edits preserve its appendix. Update
hardcoded v5 fingerprints and simulator expectations, not generated resources.

Local reset writes empty global v4 summary and empty history together; retain the
existing documented backup retention and synced-device retention. A reset must
not later merge its old appendix merely because the general save helper preserves
history by default: use an explicit fresh/reset mode. Backups contain the complete
file. Recovery from the existing pre-reset backup remains existing behavior, not
an unrequested backup-policy redesign. Book reset never subtracts from lifetime
global stats, matching the current ownership boundary.

Validation failure must log and use the existing recovery policy; do not silently
partially overwrite a corrupt appendix. Read-only UI may still expose validated
summary totals with daily history unavailable, but mutation requires recovery or
explicit reset. Atomicity is per stats file: book and global saves are already
separate, and this design does not claim a cross-file transaction on sudden power
loss. All handles must explicitly close on every path.

## Sync and aggregation

Send the 223-byte v4 summary only. The current 14-byte protocol envelope gives a
237-byte packet, so no widened payload length or history chunk protocol is needed.
Change readSmallFile to validate the local v4 file and extract its summary rather
than rejecting full local files above the wire cap. Validate v1 (13), v2 (17),
v3 (159), and v4 (223) explicitly; using only CURRENT_FILE_VERSION would otherwise
drop v3 support after the bump. Use one common codec in persistence and sync.

Current old receivers reject v4 payloads and withhold their application ACK, so
they cannot falsely report a successful v4 import. The new sender cannot identify
that rejection with the current protocol; it sees only the same timeout as packet
loss or storage failure. Apply the reserved-byte capability handshake and tests in
`2026-09-07-manga-stats-sync-preflight.md` before claiming explicit old-peer
version-mismatch reporting. New firmware imports old summaries and attributes
their total to Unknown, but the exchange remains incomplete because the old peer
cannot import v4. Do not silently downgrade and discard language totals.

For all-device language totals use the same accepted files and local-MAC exclusion
as GlobalReadingStats::loadAggregated. Merge identical normalized tags, preserving
Unknown and Other. To avoid SD enumeration order changing visible languages when
the union exceeds six names, use two bounded passes: choose the lexicographically
smallest six distinct named tags across accepted summaries, then accumulate those
names and fold every other named total into Other. Keep that selection in the
result's fixed six slots. This deterministic aggregate does not change any local
file's first-six insertion policy. Invalid/newer files are skipped for both total
and language counters, never one dimension alone.

Do not reconstruct global history by summing book histories (deleted books and
local reset make that wrong). Synced summary-only files have no dated detail.
Local daily APIs take an explicit local file path; no implicit aggregation flag.

## Existing UI and validation

Add one language breakdown page within BookStatsActivity for each applicable scope
(book, this device, all devices), following its existing navigation/input patterns.
Show nonzero rows, normalized tag and formatted duration; Unknown and Other labels
use new translation keys. Eight rows are bounded; paginate using runtime usable
height if the existing layout cannot fit. Include a clear scope title. Do not read
SD during render: fixed summaries are already held in the activity. Keep existing
summary/date screens intact. Daily history persistence need not introduce a new
calendar chart. Live uncommitted time must not be assigned wholesale to the current
language if a snapshot already includes earlier committed intervals.

Required native tests:

1. Byte-exact book v6/global v4 round trips with empty and maximum appendices;
   v1–v5/v1–v3 migration yields all Unknown and zero invented dated history.
2. Normalization case/whitespace/underscore/regions/scripts/aliases, malformed
   metadata, overlong input, all six named slots, Other and Unknown distinction.
3. Mixed languages over multiple sessions and changed metadata; a normal
   completion/date save preserves every language/day counter.
4. Midnight, month/year/leap boundaries, no RTC, clock rollback, 730-day expiry,
   very old interval, zero interval, uint32 saturation, no duplicate row keys.
5. Same accepted duration enters existing totals and language totals once;
   subthreshold sessions, pause/idle rejection, menu/lookup and return preserve
   existing behavior. No TXT tracking is introduced.
6. Truncation at header/entry/row boundaries, bad versions/counts/lengths/tags,
   short write/sync/close/rename failures, retry, backup recovery and newer-file
   save refusal. Old durable data remains available after failed publication.
7. Reset empties local summary and appendix together; backup and remote retention,
   book removal, v4/v5/v6 cache clear/move preservation, fingerprint invalidation.
8. New sync sends 223-byte summary from a large local file, accepts old formats,
   rejects malformed/newer formats, skips self, avoids duplicate local count,
   and produces deterministic language totals regardless of directory order.
   Test old-peer rejection reporting. Synced snapshots contain no day appendix.
9. Check native/target sizeof and compiler stack frames: no history-sized arrays,
   no repeated allocation during streaming or UI render. No new SDK APIs needed.

Hardware verification after implementation (not performed for this design): on
X4/C3, read manga with two language metadata values across separate sessions,
include Unknown, reopen stats and check totals; cross a known RTC midnight with a
short active interval; clear book cache and verify preservation, then reset local
stats and verify empty local scope. Pair two updated devices and verify all-device
language totals without counting self. Repeat key flows on an S3 target. Check
heap/largest block and task stack high-water marks during max-history save. No
EPUB content cache reset is needed for a stats-only change.
