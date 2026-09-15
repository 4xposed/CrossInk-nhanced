# Manga OCR region lookup

Open Lookup from the manga menu or press Confirm while reading a panel. For
pages with an overview image, the reader highlights one nonempty OCR region.
Previous/next buttons or vertical swipes cycle through regions in stored order.
Tap another region to highlight it; tap the highlighted region or Lookup to open
its text. Back cancels without moving the reading position. The image is fitted
above the selector toolbar, including when automatically rotated.

The selected region's text appears above the shared dictionary panel. Tap a word
or use the normal dictionary word-navigation controls. The text scrolls to keep
the selected word visible. Dictionary navigation, history and clipping retain
their existing controls. Leaving lookup restores the original page/panel.
Books without usable overview images retain the existing text-only fallback.

The selector borrows the suspended reader's immutable page data and stores only
its selected region ordinal. It allocates no glyph array or second framebuffer.
The popup uses the existing fallible source allocation, capped at 1024 glyphs
(16 KiB), with the existing smaller allocation fallback and visible truncation
warning. Scrolling changes coordinates in that array, preserving text, content
hashes and original clipping ordinals. Region scan caches use separate filenames
from whole-page/panel caches. No book or cache format version changes.

## Verification

Native dictionary/source tests cover region scope, empty and invisible regions,
wraparound, exact clipping offsets, offscreen text retention, scrolling and
wrapped-word touch targets. The manga OCR smoke exercises region cancellation,
button selection, touch popup selection, dictionary/cache/clipping flows and
restoration of reading position on button and touch simulator profiles.

On X3/X4 and X4 Pro, use a newly named converted book folder to avoid old pixel
caches. Check a page with several bubbles, including one at the bottom, and a
long bubble that needs text scrolling. Repeat in portrait and landscape. Select
words, save a clipping and verify its exact text; then close/reopen the book and
sleep/resume. Record free internal heap, largest allocatable block and task stack
headroom over repeated cycles. Simulator checks do not establish physical e-ink
quality, SD timing or C3 heap stability.
