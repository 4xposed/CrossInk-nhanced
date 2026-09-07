# Manga progress state

Manga reading progress and its per-book panel preferences are stored outside the disposable book cache. For a book at `bookPath`, the state file is:

```text
/.crosspoint/manga-state/<crc32(bookPath)>.bin
```

The decimal CRC is produced by the existing `uzlib_crc32(bookPath.data(), bookPath.size(), 0)` convention also used for other per-book stores. Renaming or moving a book therefore gives it a new state identity. Firmware-managed manga folder moves migrate its durable state to that identity; raw external SD or USB mass-storage renames do not.

## Version 1 record

Each file is exactly 12 bytes. Multi-byte integers use little-endian byte order.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic bytes `MGPR` |
| 4 | 1 | Format version, currently `1` |
| 5 | 1 | Flags: bit 0 `panelsOnly`, bit 1 `rotatePanels`; other bits must be zero |
| 6 | 2 | Signed panel index (`-1` through `254`) |
| 8 | 4 | Page index (`0` through `9999`) |

Missing state is a normal load miss. A record with the wrong length, magic, version, flags, or numeric ranges is rejected and the caller receives default progress: page `0`, panel `-1`, `panelsOnly=false`, and `rotatePanels=true`.

## Replacement safety

Saves write and sync a sibling `.tmp` file, close it explicitly, rotate the current file to `.bak`, and then rename the completed temporary file into place. If promotion fails, the backup is restored so a previously valid state remains readable. A backup left by an interrupted replacement is recovered when no primary state file exists.

Callers are responsible for write coalescing through `ReaderProgressSaveDebouncer`; the store itself holds no global book list or pending state.

The manga reader counts physical page changes for the ten-page save cadence. Panel
changes and preferences remain pending until that cadence, the five-minute timer,
or exit. Failed saves retain pending state and retry after 30 seconds; exit still
attempts a flush. Bookmark headers copy bounded UTF-8 display labels; portable
metadata remains unchanged in the book folder.

## Library and statistics

Home and Recent Books show physical-page progress as `(page + 1) / pageCount`,
clamped to 0–100%; missing/invalid state remains unknown. Manga statistics reuse
`BookReadingStats` at `/.crosspoint/manga_<crc>/stats_v6.bin` and the shared v4
`/.crosspoint/global_stats.bin`. Time accumulates across panels, and qualifying
forward physical-page transitions count once, including an explicit advance past
the final page. Repeated advance presses at the end do not count it repeatedly.
Menus, child selectors and Quick Lock pause timing. Existing tracking preferences
and idle/session thresholds apply.

Book and device summaries include eight bounded language buckets and up to 730
local daily rows. Legacy time migrates to Unknown; Nearby exchanges summaries,
not daily history. See [File formats](file-formats.md#reading-language-statistics-book-v6--global-v4).
Each statistics file publishes independently. Completion actions in Books and
Recent Books retain the exact desired status, date and snapshots in a retry
screen until both files publish. A successful target is not saved again; recents
and move-to-Read effects run once after completion. Navigation and sleep keep the
failed edit open for explicit Retry. No cross-file power-loss atomicity is claimed.

Reading-cache clearing preserves statistics, dictionary choice and dictionary
history; progress and bookmarks live outside disposable caches. Firmware WebDAV
and USB-command folder moves migrate durable manga identities. Raw external
filesystem moves remain outside this tracking.

Confirmed manga deletion from Books, either Recent layout, HTTP and USB uses the
shared journalled transaction. It captures identities before deleting content,
cleans metadata only for confirmed absent paths, clears matching resume paths,
and retains recovery information until metadata publication and owner reload
finish. Surviving or recreated paths retain their metadata. The checked bookmark
API contributes to the transaction result; the legacy void API remains only for
older callers.

The deletion snapshot is bounded to 64 directories, 64 metadata-bearing books/manga
roots and 16 KiB of combined path storage. It is allocated fallibly, scans iteratively,
and excludes manga page/panel images from metadata entries. Checked enumeration,
scan failures or exceeded bounds cancel destructive work; use smaller selected
folders for larger trees. See [the mutation journal](file-formats.md#manga-folder-mutation-journal-cmj1-version-1)
for recovery limits and on-disk records.
