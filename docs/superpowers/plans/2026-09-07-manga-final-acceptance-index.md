# Manga final acceptance evidence index

Updated after Task 10d's fresh software verification. See the
[final verification report](2026-09-07-manga-final-verification-report.md) for exact
results, artifact hashes and remaining physical/restoration limits. The binding checklist remains
[the original handoff](2026-09-05-manga-port-handoff.md); review every individual
checkbox against the final working tree, including untracked files.

| Handoff area | Existing evidence to inspect | Remaining gate |
| --- | --- | --- |
| Indexed format, metadata, TOC, image/crop discovery, malformed inputs | foundation review; final whole-port/correction reviews; native manga_format/manga_book | Software reconciliation complete; physical sparse/mixed page checks remain |
| Converter formats/order/metadata/detection/OCR/checkpoint behavior | foundation review; `tools/manga_convert/README.md`; final ordinary 10/10 converter pass | Offline/PDF software checks complete; no live cloud/model calls claimed |
| Reader navigation, panel modes, rotation, chapters, bookmarks/resume | Task 10e menu report/reviews; final button/touch smokes | Software workflows pass; final physical acceptance remains |
| Fresh/cached pixels, geometry, grayscale, corruption/OOM | pixel audit/review; docs/manga-bitmap-pixels.md and manga-pixel-cache.md; image_cache_decode tests | Real hardware rotation/ghosting/cleanup and source/cache equivalence |
| Prefetch owner, cancellation, foreground coordination | prefetch decode report; worker report/review; stress smoke | Final rapid-navigation/sleep/transfer stress and hardware heap/watermarks |
| Browser/Home/Recent/Continue/actions, thumbnail generation | library audit/review; Task 10a report/review | Task 10a review/builds accepted; final library flow and device cover cancellation/sleep timing |
| Progress/bookmarks/finished state/reading time/language | storage review; manga-progress docs; existing stores | Task 10b review/builds accepted; final session/language/Nearby workflow and physical failure checks |
| Move/rename/delete/cache preservation | Task 10c report/review; final Recent deletion corrections and six actual WS lifecycle tests | Software checks pass; physical transport/failure checks remain |
| OCR order/geometry, common Japanese/StarDict UI, translation/history/clipping | OCR source and UI reports/reviews | Final lookup, font and nested activity regression; hardware SD font cycles |
| Scan identity/source replacement/cache reuse | scan identity report/review | Large real indexes: record first-definition latency separately from strict verification/cache usefulness |
| SDK/fonts/dictionary preservation | baseline HEAD ea03940023c1705fed80ea64cc0de3208f38321a; SDK 1e8ee543edca397f2b8747811f5a88f1bc35d233; font-pool report | Whole uncommitted diff review; preserve dictionary work and user fonts; no staging/commit/push |

## Fresh validation at final gate

Completed: native 811/811, converter 10/10, all 12 logical simulator workflows,
six real WS lifecycle runs, three firmware and two simulator builds. The updated
Python stress-validator tests also pass. Static analysis remains exit 1 with
0 high/6 medium/57 low reports (60 unique), all reviewed with no additional unsafe
defect found. Preserve the initial two stale-assertion stress failures alongside
the successful corrected reruns; see the final report for exact commands/logs.

For reproduction, run the complete native suite sequentially (`ctest -j1`), available offline
converter tests, firmware builds for C3/Sticky/X4 Pro, both button/touch simulator
profiles, normal manga/EPUB/OCR smokes and grayscale-prefetch stress after the last
functional fixes. Select static checks appropriate to the final touched code.
Record commands, exit statuses, exact final firmware sizes/hashes and SDK pin.
Do not report old task counts or pre-fix artifacts as final evidence.

Simulator JPEG/PNG stubs do not validate actual image quality. Real-codec native
tests cover deterministic pixels and faults; physical display/SD timing is a
separate acceptance requirement. Current user-confirmed device behavior only
covers the last flashed Task 7 image. Later saved build artifacts are unflashed.

## Physical and restoration constraints

The C3 reader's serial endpoint was absent at the latest enumeration; no S3
physical acceptance has been recorded. Existing flash authorization persists
when the device becomes available. Match the artifact hash to the actual upload,
then capture boot/status evidence and exercise the final workflow.

The original Japanese dictionary remains at
`/dictionaries/jp.user-backup-20260905`; active `/dictionaries/jp` is a small test
fixture. Automatic review rejected copying the 272,117,145-byte backup to local
temporary storage pending explicit user approval. No copy or device mutation was
performed. Do not bypass this rejection, overwrite/delete the backup, or treat
“continue”/“connected” as approval for the export. The existing approval question
remains pending. Preserve user NotoSansJP/BookerlyJP fonts and all durable state.

Final reporting must distinguish completed software from unavailable physical
acceptance and any restoration blocked by the pending approval.

Known final-gate fixes are indexed in [the findings file](2026-09-07-manga-final-known-findings.md).
Task 10e preparation corrections are in [the implementation preflight](2026-09-07-manga-menu-implementation-preflight.md).
