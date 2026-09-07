# Known findings for the final manga gate

These are inputs to Task 10d whole-diff review and its single fix wave, not
completed fixes. Preserve the no-commit/no-push rule and all dictionary work.

## Converter test cleanup and validation documentation

`test/manga_converter/test_converter.py:42` patches the entire `sys.modules`
dictionary to block two optional model imports. Its cleanup restores the whole
module table, removing a newly imported PyMuPDF native module and crashing SWIG
cleanup with Pillow12.3.0/PyMuPDF1.28.2/Python3.14.7. Root's ordinary unittest run
reproduced the crash; `/private/tmp/crossink-manga-pdf-crash.log` is the stack trace.
This is a test-lifetime defect, not evidence of converter rasterization failure.

Use narrow optional-import blocking that restores only its own keys and leaves
unrelated native modules resident. Retain socket/subprocess/cloud/model guards.
Do not add a mandatory fitz preimport or silently skip installed PDF support.
Rerun the ordinary unittest discovery command in
`/private/tmp/crossink-manga-pdf-validation/bin/python`, including actual two-page
PDF rasterization/order/metadata. The diagnostic preimport workaround passed all
10 tests (`/private/tmp/crossink-manga-final-converter-pdf-preimport.log`), but is
not the final fixture fix. Update `tools/manga_convert/README.md:27,41,138` to
reflect the actual isolated dependency installation and complete offline coverage;
remove stale future-reader/cache language. YOLO/cloud remain untested live.

## Task 10b deferred Low

Independent review found obsolete mirrored Nearby state and wrapper methods after
extracting the shared protocol Session. See
`2026-09-07-manga-language-stats-review.md`. Triage and remove only unused task10b
state; preserve hardware display behavior, loss/retry handling and protocol tests.

## Final reviewer scope

Use `2026-09-07-manga-final-acceptance-index.md`, the original handoff's individual
checkboxes, and every deferred/parked/Ruling entry in the SDD ledger. Review the
entire uncommitted working tree including untracked production code and binary
fixtures. Task-scoped reports are evidence, not a replacement for this review.
Physical C3/S3 acceptance and dictionary export/restore approval remain separate;
neither may be marked passed from native or simulator results.

## Task 10e outside-delta build warnings

Task review R4 records narrowing warnings at src/main.cpp:524–525 and unhandled
ScreenMargin/ControlsHomeButton values at src/activities/settings/SettingsActivity.cpp:1006.
Evidence /private/tmp/crossink-menu10e-final-c3-sim-builds.log. These were outside
the menu delta and remain for final whole-diff triage; do not claim warning-free
builds or expand menu fixes into unrelated cleanup.
