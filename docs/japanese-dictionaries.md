# Japanese dictionaries

CrossInk looks up Japanese words and conjugations automatically in horizontal
Japanese EPUBs. You’ll need a vocabulary dictionary and a
[Japanese-capable font](./sd-card-fonts.md). Names and grammar dictionaries are
optional and appear in the same lookup panel.

## Convert your dictionary

Use `crossink-dict` with a Yomitan ZIP or extracted folder. See the
[converter README](../tools/dict_convert/README.md) to download or build it.

```sh
./crossink-dict --input dictionary.zip --output-dir output/japanese --name vocab
```

On Windows, use `crossink-dict.exe`. For a names or grammar dictionary, run the
same command with its source file and `--name names` or `--name grammar`.

The tool runs offline and only accepts Yomitan. For JMdict or MDict sources,
get a Yomitan export first.

## Copy to your SD card

Copy the generated files into `/dictionaries/jp`:

| Dictionary | Files |
| --- | --- |
| Vocabulary (required) | `vocab.idx`, `vocab.dat`, `vocab.spx` |
| Names (optional) | `names.idx`, `names.dat`, `names.spx` |
| Grammar (optional) | `grammar.idx`, `grammar.dat`, `grammar.spx` |

Each `.idx` needs its matching `.dat`. The `.spx` files speed up lookup, so keep
them if you can. Existing Matcha files in `/dict` also work, including the
`jmdict` and `jmnedict` basenames.

Open a Japanese EPUB and try looking up a word. There’s no dictionary setting
to enable and no cache to clear.

## If something isn’t working

- **Missing Japanese characters:** select a Japanese-capable `.cpfont`; the
  built-in fonts don’t include Japanese.
- **No Japanese lookup:** check that the book is marked as Japanese and that
  both `vocab.idx` and `vocab.dat` are installed. Without a valid vocabulary
  dictionary, CrossInk uses its normal StarDict fallback.
- **Conversion stops on `.tmp` or `.bak` files:** these may be from an interrupted
  run. Follow the [recovery instructions](../tools/dict_convert/README.md#compatibility-and-recovery)
  before removing them.

For binary layout details, see [File Formats](./file-formats.md).
