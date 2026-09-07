# Task 9 — manga text and unified dictionary lookup

Requirements: manga-port-handoff OCR/lookup checklist, approved manga-port-design,
and `2026-09-06-manga-ocr-audit.md` (including signature followup). Work in place;
no commits/pushes, SDK changes, external OCR requests or dictionary replacement.
Preserve both dictionary backends and the existing lookup/scanner/worker pipeline.

## Task 9a: owned OCR source and coordinate/text mapping

Create a compact movable owned glyph source for the existing PageTextSourceView.
Build length-aware UTF-8 from selected panel blocks or all panels in saved order.
Bound allocations against C3 glyph/candidate requirements; handle OOM/truncation
visibly and never persist partial scans. Separate paragraphs at block/line/control
boundaries; retain stable lexical-run ordinals for StarDict and unchanged Japanese
scanner segmentation/longest/deinflected/grammar/name/counter behavior. Do not use
strlen on format views or assign a distinct Latin word ordinal per glyph.

Map whole OCR block rectangles from metadata page coordinates through exact image
fit and all four image/base orientation transforms. Clip with wide arithmetic and
reject empty/overflowed bounds. Do not invent equal-width character boxes for
vertical or multiline OCR. For lookup entered from a legacy crop, use the full-page
background while retaining that panel's text scope; when no overview exists, lay
out a bounded selectable text area. Restore the prior page/panel/orientation on exit.

Keep a tested ordinal/byte mapping or reconstruct from the immutable parent page
view after child teardown, so returned DictionaryClippingRequest resolves exact
source text. Source hash must change with codepoints/boundaries/scope; cache keys
must separate overview and every physical page/panel without integer collisions.

Tests: real existing Japanese/StarDict fixture scanners, lexical/paragraph edges,
UTF-8/NUL/malformed input, bounded/OOM/truncation behavior, all orientations/insets,
block clipping, text-only fallback and exact clipping reconstruction.

## Task 9b: shared activity entry, reader menus and translation

Add an owned external text-source entry to EpubReaderWordLookupActivity while
preserving both existing constructors, EPUB Page building/reload and direct-word
history lookup. Route all source accesses through a small common accessor; retain
external ownership across dictionary changes and nested definitions until workers
and scans have stopped. Do not copy the definition activity or replace its engine.

Accept a per-source scan-cache path; use the existing versioned candidate format
with page/panel identity, source hash/count and verified dictionary scan identity.
Task 9c supplies the verification operation. During Task 9b, keep external-source
persistent cache load/save disabled until that operation is wired; never treat
the existing sampled engine signature as verified. Preserve current EPUB behavior
until the separate shared identity task updates it.
Persist only complete untruncated successful scans. Preserve progressive discovery,
cursor/definition navigation, suggestions, dictionary switching, history, clipping,
nested back, cancellation and reader/dictionary font restoration.

Add translated Lookup and Translation menu entries with applicable empty/error
states. Lookup consumes selected-panel OCR or whole overview OCR. Translation must
show stored translations even with no OCR or dictionary, offline only, with bounded
paging/scrolling. Consume clipping results in the manga parent via ClippingsManager
with title/author/physical page/panel context; never call EPUB Section clipping APIs.
Quiesce prefetch before children and keep borrowed callback/page lifetimes valid.
Honor the handoff's Confirm semantics: overview Confirm opens the menu; panel
Confirm opens that panel's lookup directly, including translated empty-OCR feedback.
Keep the touch menu gesture a menu. Retain/coalesce lookup intent while speculative
I/O drains so a release is not lost or executed twice; never overlap source readers.

Background integration must preserve the durable reader position even on forced
child teardown: never leave `position.panel` changed to overview throughout the
child lifetime. Use an explicit draw scope or restore any temporary selection
before returning from the locked background callback. The callback draws BW only;
do not call the manga grayscale display/refresh sequence over the lookup overlay.
Any borrowed external glyph view used for text fallback must become unreachable
before child source release, including dictionary switching and forced exit.

Simulator tests on buttons/touch: enter lookup/translation from overview and panel,
word/definition paging, nested back/dictionary switch, saved clipping/history,
no-OCR/unavailable dictionaries, font restoration and returning to the same image.
Existing EPUB lookup tests and smoke flows remain regression gates.
Include direct panel Confirm, overview Confirm and Confirm during prefetch drain.

## Task 9c: verified persistent scan identity

Use `2026-09-06-manga-scan-identity-design.md` for the inspected worker/reopen/HAL
ownership map. Rebuildable accelerator sidecars are excluded from both canonical
identity and route-resume descriptors. Keep eligible pending verification on the
existing yield path through skipLoopDelay, polling input between bounded chunks;
the normal 10/50 ms idle delay would make large-index verification impractical.
Physical read serialization already exists in HAL; do not add a render-deferral
protocol. Worker/scanner backend ownership still strictly excludes identity I/O.
The initial verification opportunity must allow real small multi-file dictionaries
to complete, not just one file's first read. Use a bounded initial slice (at most
16 steps of 256 bytes and a cooperative 5 ms elapsed limit), cancellation between
steps, before falling back to progressive scanning. A single SD read may exceed
the elapsed limit; it is not a hard latency guarantee. Test an actual second
activation cache load using a small dictionary, not only a mocked Ready state.
Large dictionaries may finish progressive scanning before full verification, so
strict content validation can make cached scans unhelpful for those activations;
record this tradeoff and measure it separately from time to first definition.

Preserve the existing inexpensive engine.signature API. Add a distinct incremental
scan identity covering all full Japanese active indexes plus descriptors and data
availability/size; no definition-body payload hashing is needed for candidates.
For StarDict use full index/synonym/metadata identity conservatively. Full candidate
records are source tokens, but dictionary replacements must still invalidate the
requested cache. The audit identifies exact files and index-handle constraints.

Verify once per top-level lookup activation and retain the result across internal
backend reopen/cursor/nested-definition transitions only for the same routed source.
Recompute on new activation or dictionary route change. Active sessions assume
immutable dictionary files; transfer screens must quiesce lookup before mutation.
Do not memoize arbitrary external changes by path/size/mtime across activations.

Hash in bounded cancellable slices outside RenderLock and never concurrently with
dictionary probe/definition worker ownership. Preserve input responsiveness and
time-to-first-definition through progressive scanning when verification is pending.
Unverified/error/cancelled identities cannot load/save persistent scans. Large
indexes may bypass a warm-cache load while verification completes; once verified,
complete scans may be saved for later activation. Do not synchronously hash entire
dictionaries during every backend open or cursor change.

Tests: >512-byte middle-only index/POS/priority changes, source availability/size,
route switching, repeated cursor reopen with one activation digest, new activation
rehash, chunk cancellation/failure with no partial trust, and zero payload reads
from a sparse 100 MiB definition body. Existing sampled-signature tests remain valid.

## Hardware and completion

Test on X4 with both dictionaries and user SD fonts, panel/overview/translation,
long Confirm clipping, nested lookup/back/history, repeated opens and cancellation.
Record time-to-first-definition, index verification, free/largest internal heap,
and worker stack watermarks. Restore the user's dictionary backup safely before
final everyday-use handoff. Repeat S3 physical checks when hardware is available;
all target builds and simulator coverage alone do not establish physical image UI.
