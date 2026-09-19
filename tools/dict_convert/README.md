# CrossInk dictionary converter

`crossink-dict` converts Yomitan ZIP archives and extracted directories into
CrossInk's Japanese dictionary files. It runs offline as one executable.

```sh
./crossink-dict --input dictionary.zip --output-dir output/japanese --name vocab
```

On Windows use `crossink-dict.exe`. Choose `--name vocab` (the default), `names`,
or `grammar`. `--output-dir` defaults to `output`. `--format yomitan` is accepted
as an optional explicit format; the tool only handles Yomitan.
Use `--help` for all options and `--version` for the tool version.

Copy the resulting `.idx`, `.dat`, and `.spx` files to `/dictionaries/jp` on the SD card.
Vocabulary is required; names and grammar are optional.
No dictionary cache reset is needed.

## Build

```sh
cargo build --release --locked --manifest-path tools/dict_convert/Cargo.toml
```

The executable is `tools/dict_convert/target/release/crossink-dict` (or `crossink-dict.exe` on Windows).
Build separately for each operating system and CPU architecture.

## Compatibility

JMdict JSON/archive conversion, automatic downloads, and MDict input are not supported.
Only Yomitan exports are supported.

## Rebuild sparse indexes

Rebuild `.spx` files from existing `.idx` files without the original dictionary
source or `.dat` files:

```sh
./crossink-dict --rebuild-spx /path/to/sdcard/dictionaries/jp
```

This processes `vocab`, `names`, `grammar`, and legacy `jmdict`/`jmnedict` indexes,
reporting and skipping missing indexes. It preserves `.idx` and `.dat` files.
Each sidecar is written to an exclusively created `.spx.tmp` file, synced and
validated before replacing its `.spx` file. Existing temporary files cause an
error; inspect them before removing them and retrying. Publication is per sidecar,
so an error on a later index does not undo earlier successful rebuilds.
Do not combine `--rebuild-spx` with conversion options.

## Tests

```sh
cargo test --locked --manifest-path tools/dict_convert/Cargo.toml
```

Rust tests cover conversion, pinned fixture integrity, sparse-index rebuilding,
corruption rejection, and safe publication.
