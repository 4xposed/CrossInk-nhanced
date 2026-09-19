use anyhow::{Context as _, Result};
use serde_json::{Value, json};
use std::{fs, io::Write as _, path::Path, process::Command};
use tempfile::tempdir;
use zip::{ZipWriter, write::SimpleFileOptions};

#[derive(Debug, PartialEq, Eq)]
struct Record {
    key: String,
    definition: Vec<u8>,
    offset: u32,
    priority: u8,
    flags: u8,
}

fn convert(source: &Path, output: &Path) -> Result<Vec<Record>> {
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-dict"))
        .arg("--input")
        .arg(source)
        .arg("--output-dir")
        .arg(output)
        .output()?;
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let idx = fs::read(output.join("vocab.idx"))?;
    let dat = fs::read(output.join("vocab.dat"))?;
    assert_eq!(
        idx.len() % 40,
        0,
        "index must contain complete 40-byte records"
    );
    idx.as_chunks::<40>()
        .0
        .iter()
        .map(|bytes| {
            let nul = bytes[..32]
                .iter()
                .position(|&b| b == 0)
                .context("NUL terminator")?;
            let offset = u32::from_le_bytes(bytes[32..36].try_into()?);
            let length = usize::from(u16::from_le_bytes(bytes[36..38].try_into()?));
            let start = usize::try_from(offset)?;
            Ok(Record {
                key: String::from_utf8(bytes[..nul].to_vec())?,
                definition: dat[start..start + length].to_vec(),
                offset,
                priority: bytes[38],
                flags: bytes[39],
            })
        })
        .collect()
}

fn bank(source: &Path, number: u8, rows: &Value) -> Result<()> {
    fs::create_dir_all(source)?;
    fs::write(
        source.join(format!("term_bank_{number}.json")),
        serde_json::to_vec(rows)?,
    )?;
    Ok(())
}

#[test]
fn redirects_use_highest_priority_and_stable_bank_order_in_zip_and_directory() -> Result<()> {
    let temp = tempdir()?;
    let source = temp.path().join("input");
    bank(
        &source,
        2,
        &json!([
            ["alias", "", "", "n", 0, [{"data":{"content":"redirect-glossary"},"content":"⟶ target"}]],
            ["missing", "", "", "n", 0, [{"data":{"content":"redirect-glossary"},"content":"⟶ absent"}]],
            ["chain", "", "", "n", 0, [{"data":{"content":"redirect-glossary"},"content":"⟶ alias"}]],
            ["target", "", "", "n", 99, ["lower priority"]],
            ["target", "", "", "n", 100, ["later tie"]]
        ]),
    )?;
    bank(
        &source,
        10,
        &json!([["target", "", "", "n", 100, ["first tie"]]]),
    )?;
    let output = temp.path().join("directory");
    let records = convert(&source, &output)?;
    assert_eq!(
        records.iter().map(|r| r.key.as_str()).collect::<Vec<_>>(),
        ["alias", "target", "target", "target"]
    );
    assert_eq!(records[0].definition, b"= target\nfirst tie");
    assert_eq!(records[0].priority, 228);
    assert_eq!(records[1].definition, b"first tie");
    assert_eq!(records[2].definition, b"lower priority");
    assert_eq!(records[3].definition, b"later tie");
    let archive = temp.path().join("input.zip");
    let mut zip = ZipWriter::new(fs::File::create(&archive)?);
    for number in [2, 10] {
        let name = format!("term_bank_{number}.json");
        zip.start_file(
            &name,
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated),
        )?;
        zip.write_all(&fs::read(source.join(name))?)?;
    }
    zip.finish()?;
    let zipped = temp.path().join("zip");
    assert_eq!(convert(&archive, &zipped)?, records);
    for suffix in ["idx", "dat", "spx"] {
        assert_eq!(
            fs::read(output.join(format!("vocab.{suffix}")))?,
            fs::read(zipped.join(format!("vocab.{suffix}")))?
        );
    }
    Ok(())
}

#[test]
fn priorities_truncate_clamp_and_default() -> Result<()> {
    let temp = tempdir()?;
    let source = temp.path().join("input");
    for (score, expected) in [
        (json!(-2.9), 126),
        (json!(1e100), 255),
        (json!(-1e100), 0),
        (json!(true), 129),
        (json!(false), 128),
        (json!("unknown"), 100),
        (json!(null), 100),
    ] {
        bank(
            &source,
            1,
            &json!([["word", "", "", "n", score, ["meaning"]]]),
        )?;
        let records = convert(&source, &temp.path().join("output"))?;
        assert_eq!(records[0].priority, expected, "score {score}");
    }
    Ok(())
}

#[test]
fn readings_preserve_part_of_speech_and_omit_redundant_reading_prefix() -> Result<()> {
    let temp = tempdir()?;
    let source = temp.path().join("input");
    for (rules, expected) in [
        ("v1", 1),
        ("v5", 2),
        ("v4", 2),
        ("iv", 2),
        ("vs", 4),
        ("vk", 8),
        ("adj-i", 16),
        ("v2", 15),
        ("aux-v", 15),
        ("vt vi aux aux-adj exp", 0),
        ("", 32),
        ("n adj-na", 32),
        ("v1 vt", 1),
    ] {
        bank(
            &source,
            1,
            &json!([["word", "reading", "", rules, 0, ["meaning"]]]),
        )?;
        let records = convert(&source, &temp.path().join("output"))?;
        assert_eq!(records[0].key, "reading");
        assert_eq!(records[0].definition, b"meaning");
        assert_eq!(records[0].flags, expected | 0x40, "reading: {rules}");
        assert_eq!(records[1].definition, "【reading】\n\nmeaning".as_bytes());
        assert_eq!(records[1].flags, expected, "headword: {rules}");
    }
    Ok(())
}

#[test]
fn byte_limits_duplicate_data_and_sparse_checkpoints() -> Result<()> {
    let temp = tempdir()?;
    let source = temp.path().join("input");
    let mut rows = vec![
        json!(["a".repeat(31), "", "", "n", 0, ["accepted"]]),
        json!(["b".repeat(32), "", "", "n", 0, ["rejected"]]),
        json!(["語".repeat(11), "short", "", "n", 0, ["reading survives"]]),
        json!(["long", "", "", "n", 0, ["x".repeat(65536)]]),
        json!(["unicode", "", "", "n", 0, ["語".repeat(22000)]]),
        json!(["blank", "", "", "n", 0, []]),
        json!(null),
        json!({}),
        json!(["short row"]),
    ];
    rows.extend((0..110).map(|i| json!([format!("word{i:03}"), "", "", "n", 0, ["same"]])));
    bank(&source, 1, &Value::Array(rows))?;
    let output = temp.path().join("output");
    let records = convert(&source, &output)?;
    assert_eq!(records.len(), 114);
    assert_eq!(records[0].key, "a".repeat(31));
    assert_eq!(records[1].definition, vec![b'x'; 65535]);
    assert_eq!(records[2].key, "short");
    assert_eq!(
        records[3].definition,
        "語".repeat(22000).as_bytes()[..65535]
    );
    assert!(records[4..].iter().all(|r| r.offset == records[4].offset));
    let idx = fs::read(output.join("vocab.idx"))?;
    let spx = fs::read(output.join("vocab.spx"))?;
    assert_eq!(&spx[..8], b"CPSPX1\0\0");
    assert_eq!(spx.len(), 128);
    assert_eq!(u32::from_le_bytes(spx[16..20].try_into()?), 114);
    assert_eq!(u32::from_le_bytes(spx[20..24].try_into()?), 3);
    for (checkpoint, record) in [0, 48, 96].into_iter().enumerate() {
        assert_eq!(
            &spx[32 + checkpoint * 32..64 + checkpoint * 32],
            &idx[record * 40..record * 40 + 32]
        );
    }
    Ok(())
}

#[test]
fn structured_content_preserves_layout_and_filters_annotations() -> Result<()> {
    let temp = tempdir()?;
    let source = temp.path().join("input");
    for (kind, tag, class, expected) in [
        ("part-of-speech-info", "span", "tag", "[ text ]"),
        ("field-info", "span", "tag", "[ text ]"),
        ("misc-info", "span", "tag", "[ text ]"),
        ("dialect-info", "span", "tag", "[ text ]"),
        ("language-info", "span", "tag", "[ text ]"),
        ("forms-label", "span", "tag", ""),
        ("glossary", "ul", "", "text"),
        ("", "li", "", "• text"),
        ("sense-group", "div", "", "text"),
        ("sense", "li", "", "text"),
        ("sense-note-label", "span", "", "text :"),
        ("sense-note-content", "span", "", "text"),
        ("sense-note", "div", "extra-box", "→ text"),
        ("example-sentence-a", "div", "", "text"),
        ("example-sentence-b", "div", "", "text"),
        ("example-sentence", "div", "extra-box", "text"),
        ("xref", "div", "extra-box", ""),
        ("reference-label", "span", "", "text"),
        ("forms", "div", "", ""),
        ("attribution-footnote", "div", "", ""),
        ("", "br", "", ""),
        ("", "rt", "", ""),
    ] {
        bank(
            &source,
            1,
            &json!([["word", "", "", "n", 0,
            [{"tag":tag, "data":{"content":kind,"class":class},"content":" text "}]]]),
        )?;
        let records = convert(&source, &temp.path().join("output"))?;
        if expected.is_empty() {
            assert!(records.is_empty(), "{kind}/{tag}/{class}");
        } else {
            assert_eq!(
                records[0].definition,
                expected.as_bytes(),
                "{kind}/{tag}/{class}"
            );
        }
    }
    bank(
        &source,
        1,
        &json!([[
            "word",
            "",
            "",
            "n",
            0,
            [
                "same",
                "same",
                ["foo", ["bar", "redirected from baz"]],
                "4",
                "5",
                "6",
                "ignored"
            ]
        ]]),
    )?;
    let records = convert(&source, &temp.path().join("output"))?;
    assert_eq!(
        records[0].definition,
        b"1. same\n\n2. foo bar\n\n3. 4\n\n4. 5\n\n5. 6"
    );
    Ok(())
}

#[test]
fn usually_kana_tag_survives_in_reading_record() -> Result<()> {
    let source = tempdir()?;
    let output = tempdir()?;
    let definitions = json!([{
        "type": "structured-content",
        "content": {"tag": "span", "data": {"content": "misc-info", "class": "tag"},
                    "content": "kana"}
    }]);
    bank(
        source.path(),
        1,
        &json!([["仮名", "かな", "", "n", 1, definitions, 1, ""]]),
    )?;
    let records = convert(source.path(), output.path())?;
    let reading = records
        .iter()
        .find(|r| r.key == "かな")
        .context("missing reading record")?;
    assert!(String::from_utf8_lossy(&reading.definition).contains("[kana]"));
    assert_ne!(reading.flags & 0x40, 0);
    Ok(())
}
