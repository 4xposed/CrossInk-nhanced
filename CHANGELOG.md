# Changelog

All notable changes to CrossInknhanced are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

CrossInknhanced is a fork of [CrossInk](https://github.com/uxjulia/CrossInk).
Changes inherited from CrossInk are listed in the
[CrossInk changelog](https://github.com/uxjulia/CrossInk/blob/main/CHANGELOG.md).

## [0.1.0] - 2026-09-23

First release of CrossInknhanced, based on CrossInk v1.6.0.

### Added

#### Manga

- Open indexed manga folders from Books and read by page or by panel, with saved position, panel preferences, chapters, and bookmarks.
- Grayscale manga rendering with validated pixel caches and consistent framing when reopening pages or panels.
- Manga covers, progress, and reading statistics in Home and Recent Books, with cache clearing that preserves reading state.
- Manga pages show oriented page/panel counters. Upcoming panels and pages are prepared during idle time, and this work yields to navigation and screen changes.
- Manga reader menus include input settings, automatic page turning, screenshots, safe cache clearing, and offline OCR QR codes on button and touch devices.
- Look up manga text with the shared dictionary, save selections to clippings, revisit lookup history, and read stored translations offline. Tapping a text bubble opens the dictionary at the estimated word, and double arrows browse terms in the same bubble.
- Manga storage supports nested indexed folders, metadata, chapters, and current or legacy image layouts.
- `crossink-manga`, a desktop converter for CBZ/CBR archives with Mokuro panel OCR, native Rust OCR using ONNX models, resumable results, and conversion profiles sized for Xteink X3, X4, and X4 Pro.

#### Japanese dictionary

- Built-in Japanese dictionary lookup (a "mini Yomitan") for EPUBs and manga, with a converter for Yomitan/JMdict data and an optional search index for faster lookups.
- Furigana from EPUB `<ruby>` markup is collected so lookups can show the book's own readings.
- Built-in Noto Sans JP fallback font for Japanese text.

#### Anki

- Review Anki decks on the device, with deck upload from the web portal. The Anki menu opens the `/decks` folder directly and creates it when needed.
- Dictionary panels can add the current term, reading, definition, and nearby source text to a local Saved terms deck. Repeated saves avoid duplicates, and new cards keep existing review progress.
- Anki front text size can be set to Small, Large, or Extra Large in Reader font settings, with automatic fitting for long prompts.

#### OPDS

- OPDS servers that serve XTC books are supported, and each server can have its own download folder.

#### X4 Pro touch interface

- Touch dashboard, category-based Settings, a shared time/battery row, and compact Light controls with circular frontlight sliders.
- Anki review with a two-column rating layout and session progress, centred dictionary panels, and manga panel selection.
- Holding a word opens EPUB lookup at the touched word. In manga, holds target the touched OCR block, with direct lookup for single words and tappable text for longer blocks.
- Reader menus open with a downward page gesture, and a downward gesture from the top edge opens Light. Manga touch navigation follows right-to-left reading.

#### Reading statistics

- Reading statistics keep language totals for books and devices, with local daily history, migration of existing stats as Unknown, and compatible Nearby summary exchange.

### Changed

- OTA updates install device-specific firmware from the latest stable release of `4xposed/CrossInk-nhanced`.
- Default sleep screens show the boot logo with a translated Sleeping label; custom sleep images and book covers remain available.
- Button actions while browsing OPDS catalogs were reworked.
- Built-in fonts share identical lookup tables, leaving more firmware space without changing their appearance.

### Fixed

- Leaving file transfer or entering sleep safely closes incomplete uploads, so navigation cannot stall.
- Moving folders keeps reading progress, bookmarks, reading statistics, and dictionary choices, with recovery after a restart. Recursive deletion only cleans metadata for books confirmed missing.
- Statistics saves keep valid backups when storage fails and retry failed book and device writes independently.
- Failed image decoding closes source files and discards incomplete pixel caches, so reading recovers cleanly.
- Word-scan caches verify complete dictionary indexes before reuse, so replacing a dictionary with one of the same size cannot restore stale results.

[0.1.0]: https://github.com/4xposed/CrossInk-nhanced/releases/tag/v0.1.0
