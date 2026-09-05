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
