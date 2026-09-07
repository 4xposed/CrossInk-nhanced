# Manga converter

Host-side converter ported from Matcha Reader
`61ca61ba86e3c5709a24d1b9c4f3cf2d41488012`, under the [MIT license](LICENSE).
It writes the compatible [manga binary format](../../docs/manga-format.md).
Copy the generated folder to the SD card and select it in Books to open the manga
reader. The reader includes validated render caches, library thumbnails, idle
prefetch, saved progress and offline lookup of stored OCR with the shared dictionary.

## Setup and an offline conversion

Use Python 3.9 or newer with Pillow. The script imports Pillow only when handling
images, so `python3 tools/manga_convert/convert_manga.py --help` needs only Python.
For local conversion without OCR or model downloads, use a separate environment
containing **Pillow only**:

```sh
python3 -m venv /tmp/crossink-manga-venv
/tmp/crossink-manga-venv/bin/python -m pip install Pillow
/tmp/crossink-manga-venv/bin/python tools/manga_convert/convert_manga.py \
  --input ./manga-pages --output-dir ./converted-book --no-ocr --x4
```

Installing dependencies needs a package source (or locally cached wheels). The
conversion itself above is offline in that Pillow-only environment. `--no-ocr`
disables Gemini; it does **not** disable the optional model loader if its packages
are installed. Pillow and PyMuPDF were installed in an isolated environment for offline validation; no model weights were downloaded.

Dependencies follow the actual imports in `convert_manga.py`:

| Path | Dependencies and external work |
| --- | --- |
| Folder, CBZ/ZIP, EPUB images; grid detection; output | Pillow plus Python standard library |
| PDF rasterization/metadata | Optional `PyMuPDF` (`import fitz`); pages rasterize at 2x resolution |
| YOLO panel detection | Optional `huggingface_hub` and `ultralytics`; automatically calls `hf_hub_download` for `leoxs22/manga-panel-detector-yolo26n`, file `manga_panel_detector_fp32.pt` |
| OCR and English translation | External `curl` executable and a Gemini API key; sends each saved panel crop to the model named by `GEMINI_MODEL` in the script |

The YOLO path may download model weights and initializes third-party ML code; leave
those optional packages out of an offline environment. If imports or model loading
fail, detection falls back to the local white-gutter heuristic, which is less
suitable for irregular or borderless layouts. Real two-page PDF rasterization,
order and metadata are covered by offline tests. Live YOLO inference remains untested.

OCR is enabled by default, so include `--no-ocr` for local use. To deliberately run
cloud OCR later, provide `--gemini-key-file /path/to/key` or `GEMINI_API_KEY` in your
own environment. This uploads cropped images, can incur API charges, and depends
on the configured service/model being available. No live service or model
availability is claimed here. Up to eight panel requests run concurrently per page;
transient errors retry three times with backoff. Failed OCR becomes empty text and
translation, so successful conversion alone does not establish OCR completeness.
Full-page panels without a saved crop are not sent for OCR. Mono crops are currently
passed through the upstream JPEG MIME fallback; live OCR with `--mono` was not
validated. Prefer `--mono --no-ocr` for local monochrome output.

## Input, page order, and metadata

- Recursive image folders and `.cbz`/`.zip` archives accept JPG/JPEG, PNG, WebP, and
  BMP. Archive chapter directories and repeated basenames are preserved. Unsafe
  absolute/traversal member paths are skipped.
- Natural ordering compares chapter paths and numeric filename segments. Filenames
  containing `cover` and `copyright` are moved to the front, in that order.
- EPUB extraction uses spine entries, finding the first actual embedded image in
  each XHTML wrapper (or a direct image spine entry). The collector subsequently
  applies natural sorting to the generated `spine_NNNN_...` filenames, including
  the cover/copyright priority. Metadata/TOC use the upstream regex-based XML
  extraction and may not cover every EPUB serialization.
- PDF pages are rasterized in document order; optional PyMuPDF is required.
- `--page-order-file order.txt` lists paths relative to the extracted/source root,
  one per line. A bare basename works only when unambiguous. Unlisted images are
  dropped; missing names warn; repeated names in the list repeat that page.
- Title/author/language come from EPUB OPF or CBZ/ZIP `ComicInfo.xml`; PDF supplies
  title/author only. `--title`, `--author`, and `--language` override nonempty values.
  Language aliases `jp`, `cn`, and `kr` normalize to `ja`, `zh`, and `ko`.
- EPUB3 navigation and EPUB2 NCX chapters map to the final extracted page sequence.
  `--toc-file chapters.tsv` **replaces** the automatic TOC. Each line is a zero-based
  final page index, TAB, title. Output sorts entries and inserts `Cover` at page zero
  if needed. `--max-pages N` truncates converted pages without pruning later TOC
  entries; use it for experiments, not a complete book.

## Output and geometry

`page_0000.<ext>`, `page_0001.<ext>`, etc. contain canonical page images.
`panels/p0_0.jpg` (or `.bmp`) contains zoom crops. Near-full-page panels omit a crop
and rely on the page image. `panels.idx` and `panels.dat` store panel version **2**,
including UTF-8 OCR and per-panel translation. Optional `meta.bin` is metadata v1
with an optional language trailer; `toc.idx` is TOC v1.

`--x3` fits portrait pages/crops to 528x792 and `--x4` to 480x800. Landscape images
use the swapped box; neither flag enlarges small images. Without either flag,
resolution is retained. Page detection and binary coordinates use the resized page
space. Panel crops come from the original full-resolution image and are then fitted
individually, retaining detail for zoom. `--panel-margin` defaults to ten pixels in
resized page space. Alpha normalization composites onto white when invoked by the
resize/crop path; the unresized mono page path retains the upstream conversion.

JPEG/PNG pages are copied when possible, resized when requested, and progressive
JPEGs are re-encoded as baseline even if already small. Other formats become JPEG.
`--mono` writes Floyd–Steinberg-dithered 1-bit BMP pages and crops.

One intentional behavioral correction from the pinned source: normalized OCR boxes
are mapped from the margin-expanded crop's origin, then encoded as **x, y, width,
height**, rather than encoding bottom-right coordinates as width/height. The binary
layout is unchanged. Tests check independent expected bytes, including clipped
margins on the first and last panel rows, full-resolution crop sizing, and missing
OCR boxes falling back to the panel rectangle. Other port edits correct setup,
format-version, and recovery prose; they do not redesign the pipeline.

## Interruptions and reruns

After each page the converter truncates and rewrites **the entire index first,
then the entire data file**, retaining all processed page data in host memory.
An interruption during either write can leave truncated or inconsistent output;
this is not atomic publication. An interruption between pages may leave a readable
prefix, but there is no checkpoint loader or automatic resumption. A rerun starts
at page zero and can repeat OCR calls. Metadata is written early; TOC is written
only at the end. Rerunning into an existing directory does not remove stale page,
crop, metadata, or TOC files from an older/larger conversion. Use a fresh output
directory and retain the source until a complete conversion has been checked.

The upstream writer also truncates counts/text byte lengths to field limits and
can emit page payloads larger than the reader's 32768-byte limit; successful host
output is not proof that every page is device-readable. This port has not added
input-size validation or changed that behavior.

## Tests and hardware handoff

```sh
python3 -m unittest discover -s test/manga_converter -v
```

The tests block socket connections, subprocess execution, and optional model imports.
The synthetic OCR test substitutes a deterministic response at the actual `curl`
subprocess boundary; image processing, grid detection, OCR parsing/mapping, and
binary writing run normally. No credentials, live cloud calls, or model downloads
are used. Tests include binary literals, natural/explicit order, archive paths,
EPUB spine/metadata/TOC, sizing, baseline JPEG, mono output, and OCR coordinates.
Image tests skip without Pillow; the optional PDF test skips without PyMuPDF.
Fresh validation on 2026-09-07 used Python 3.14.7, Pillow 12.3.0 and PyMuPDF 1.28.2
in `/private/tmp/crossink-manga-pdf-validation`. Ordinary discovery passed all ten
tests, including PDF, without preimporting `fitz`. The fixture restores only its two
optional-model import keys, keeping unrelated native modules loaded safely.

```sh
/private/tmp/crossink-manga-pdf-validation/bin/python -B -m unittest discover -s test/manga_converter -v
```

Final physical acceptance remains pending. Copy a completed disposable book to
X4/C3 and Sticky or X4 Pro/S3. Check canonical, sparse and mixed-extension page
order, chapters, panel zoom, rotations, OCR selection boxes and dictionary lookup
against visible text, then reopen after sleep. Compare fresh and cached rendering;
native codec goldens and simulator flows do not establish e-ink image parity.
