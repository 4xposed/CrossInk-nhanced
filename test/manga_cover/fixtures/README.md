# Manga cover pixel baselines

These fixtures are deterministic synthetic test images, created locally on 2026-09-07 before Task 10a production edits.
No downloaded images are used. Baseline capture used the unmodified real JPEG/PNG ToBmp converters and the locally installed
`default/JPEGDEC` dependency, compiled by AppleClang 21.0.0 in `/private/tmp/crossink-cover10a-baseline`.

The 80 × 120 RGB pattern uses `(13*x + 7*y) % 256`, `(3*x + 11*y) % 256`, `(17*x + 5*y) % 256`.
The baseline and progressive JPEGs were encoded with local `cjpeg -quality 88`, adding `-progressive` for the latter.
The PNGs use Python standard-library `struct` and `zlib`, 8-bit samples, no interlace and filter type zero:
RGBA uses the RGB pattern plus `(x+y) % 256` alpha; grayscale and palette indices use `(13*x+7*y) % 256`;
palette entry `i` is `(i, 255-i, (3*i)%256)`. The existing PNG converter ignores alpha; this task deliberately preserves that behavior.

`mono.bmp` is a hand-built, top-down 80 × 120 one-bit BMP with black/white blocks from `(x//5 + y//7) % 2`.
Its rows are padded to 12 bytes. It exercises the existing non-dithered BMP path and cancellation, independently of the codec goldens.

`goldens.txt` columns: filename, requested width, requested height, baseline success, complete BMP byte length,
FNV-1a 64-bit pixel hash, standard CRC32 of pixel bytes (after the 62-byte BMP header/palette).
The 50 CRC values were captured before implementation; `RealCoverTest` compares current converter output with these literals.
Adaptive contain can reduce an output axis: the 200 × 390 request for these sources produces a 200 × 300 BMP.

Covered requested sizes: carousel center 296 × 468, side 200 × 390; normal Home 150 × 226, 246 × 370,
173 × 260; Grid 123 × 180; minimal 350 × 525; dashboard 296 × 444; full-cover aspect fits 320 × 480 and 480 × 720.
No simulator image output is used as a pixel oracle.

SHA-256:

- `baseline.jpg`: `e8fffa02fd0d8416f7afc05380f3787682fcb41765da2cb8d59090ff5fa2cc59`
- `goldens.txt`: `6b65a8f176f6e056c4410f3f3ab35a6a5765b4acbd6a96ac3cdaa238089b373a`
- `gray.png`: `6592395e2296835fb7797300e41d8fe9eb31d85e725865cc1ee5ed9e4b851796`
- `mono.bmp`: `48ac0b9c2851d94b75d08294e6472ff02457b980a8dee1d91d1ecd8b1c27d493`
- `palette.png`: `ac26bd46fe01044c7db45552c30922f44b3630762f5795714d7b6e601dea5d4d`
- `progressive.jpg`: `a908e6f5402cccdb4f8c1d5d1bae8756c277707da30572472b6b794f96b11c1f`
- `rgba.png`: `64cfbed427e65fa3318551c354dbce50aeffa36a2890c4cb590b2eb26a105f80`
