use crate::{
    binary::{self, Record},
    content,
};
use anyhow::{Context as _, Result, ensure};
use serde_json::Value;
use std::{
    collections::HashMap,
    fs::{self, File},
    io::{BufReader, Read},
    path::Path,
};
use zip::ZipArchive;

struct Entry {
    headword: String,
    reading: String,
    definition: String,
    reading_definition: String,
    redirect: String,
    priority: u8,
    flags: u8,
}

fn bank_name(name: &str) -> bool {
    name.strip_prefix("term_bank_")
        .and_then(|s| s.strip_suffix(".json"))
        .is_some_and(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()))
}

fn open_source(path: &Path) -> Result<BufReader<File>> {
    File::open(path)
        .map(BufReader::new)
        .with_context(|| format!("Cannot open {}", path.display()))
}

fn read_json(mut reader: impl Read, name: &str) -> Result<Value> {
    // Some ZIP exports include a UTF-8 BOM before their JSON content.
    let mut prefix = Vec::with_capacity(3);
    reader
        .by_ref()
        .take(3)
        .read_to_end(&mut prefix)
        .with_context(|| format!("Cannot read Yomitan JSON: {name}"))?;
    let prefix = prefix.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&prefix);
    serde_json::from_reader(prefix.chain(reader))
        .with_context(|| format!("Invalid Yomitan JSON: {name}"))
}

fn parse_bank(bank: &Value, name: &str, entries: &mut Vec<Entry>) -> Result<()> {
    let rows = bank
        .as_array()
        .with_context(|| format!("Invalid Yomitan bank {name}: expected an array"))?;
    entries.reserve(rows.len());
    for row in rows {
        let Some(row) = row.as_array().filter(|row| row.len() >= 6) else {
            continue;
        };
        let Some(headword) = row[0].as_str().filter(|word| !word.is_empty()) else {
            continue;
        };
        let reading = row[1].as_str().unwrap_or("");
        let redirect = content::redirect_target(&row[5]);
        let (definition, reading_definition) = if redirect.is_empty() {
            let senses = content::senses(&row[5]);
            let definition = content::definition(headword, reading, &senses);
            let reading_definition = if !reading.is_empty() && reading != headword {
                content::definition(reading, reading, &senses)
            } else {
                String::new()
            };
            (definition, reading_definition)
        } else {
            (String::new(), String::new())
        };
        let score = row[4]
            .as_f64()
            .or_else(|| row[4].as_bool().map(|b| if b { 1.0 } else { 0.0 }));
        // Preserve truncation toward zero, then clamp to the u8 range before casting.
        let priority = score.map_or(100, |s| (s.trunc() + 128.0).clamp(0.0, 255.0) as u8);
        entries.push(Entry {
            headword: headword.into(),
            reading: reading.into(),
            definition,
            reading_definition,
            redirect,
            priority,
            flags: content::pos_flags(row[3].as_str().unwrap_or("")),
        });
    }
    Ok(())
}

#[expect(
    clippy::print_stdout,
    reason = "The CLI reports dictionary metadata and bank count."
)]
fn load(source: &Path) -> Result<Vec<Entry>> {
    let mut entries = Vec::new();
    let meta;
    let bank_count;
    if source.is_dir() {
        let mut names = Vec::new();
        for item in
            fs::read_dir(source).with_context(|| format!("Cannot list {}", source.display()))?
        {
            let item = item
                .with_context(|| format!("Cannot read directory entry in {}", source.display()))?;
            if item.file_name().to_str().is_some_and(bank_name) {
                names.push(item.path());
            }
        }
        names.sort();
        bank_count = names.len();
        let index = source.join("index.json");
        meta = if index
            .try_exists()
            .with_context(|| format!("Cannot inspect {}", index.display()))?
        {
            read_json(open_source(&index)?, "index.json")?
        } else {
            Value::Null
        };
        for path in names {
            let name = path.display().to_string();
            parse_bank(&read_json(open_source(&path)?, &name)?, &name, &mut entries)?;
        }
    } else {
        let mut archive = ZipArchive::new(open_source(source)?)
            .with_context(|| format!("Invalid Yomitan ZIP: {}", source.display()))?;
        let mut names: Vec<_> = archive
            .file_names()
            .filter(|name| bank_name(name))
            .map(str::to_owned)
            .collect();
        names.sort();
        bank_count = names.len();
        meta = if archive.file_names().any(|name| name == "index.json") {
            read_json(
                archive
                    .by_name("index.json")
                    .with_context(|| format!("Cannot open index.json in {}", source.display()))?,
                "index.json",
            )?
        } else {
            Value::Null
        };

        for name in names {
            parse_bank(
                &read_json(
                    archive
                        .by_name(&name)
                        .with_context(|| format!("Cannot open {name} in {}", source.display()))?,
                    &name,
                )?,
                &name,
                &mut entries,
            )?;
        }
    }
    ensure!(
        bank_count > 0,
        "Invalid Yomitan source: no term_bank_N.json files found"
    );
    ensure!(
        meta.is_null() || meta.is_object(),
        "Invalid Yomitan index.json: expected an object"
    );
    println!(
        "Dictionary: {}\nFound {bank_count} term bank files",
        meta["title"].as_str().unwrap_or("(unknown)")
    );
    Ok(entries)
}

#[expect(
    clippy::print_stdout,
    reason = "The CLI reports conversion progress and skipped headwords."
)]
pub(crate) fn convert(source: &Path, output: &Path, name: &str) -> Result<()> {
    println!("Loading {}...", source.display());
    let entries = load(source).with_context(|| format!("Cannot load {}", source.display()))?;
    let mut canonical: HashMap<&str, &Entry> = HashMap::with_capacity(entries.len());
    for entry in &entries {
        if entry.redirect.is_empty() && !entry.definition.is_empty() {
            let previous = canonical.entry(&entry.headword).or_insert(entry);
            if entry.priority > previous.priority {
                *previous = entry;
            }
        }
    }
    let mut records = Vec::with_capacity(entries.len());
    let mut skipped = 0;
    let mut processed = 0;
    for entry in &entries {
        let (definition, priority) = if entry.redirect.is_empty() {
            if entry.definition.is_empty() {
                continue;
            }
            (entry.definition.clone(), entry.priority)
        } else {
            let Some(target) = canonical.get(entry.redirect.as_str()) else {
                continue;
            };
            (
                format!("= {}\n{}", entry.redirect, target.definition),
                target.priority,
            )
        };
        if entry.headword.len() < binary::HEADWORD_SIZE {
            records.push(Record {
                headword: entry.headword.clone(),
                definition: definition.into_bytes(),
                priority,
                flags: entry.flags,
            });
        } else {
            skipped += 1;
        }
        if !entry.reading.is_empty() && entry.reading != entry.headword && entry.redirect.is_empty()
        {
            if entry.reading.len() >= binary::HEADWORD_SIZE {
                skipped += 1;
            } else if !entry.reading_definition.is_empty() {
                records.push(Record {
                    headword: entry.reading.clone(),
                    definition: entry.reading_definition.as_bytes().to_vec(),
                    priority,
                    flags: entry.flags | content::POS_READING,
                });
            }
        }
        processed += 1;
    }
    drop(canonical);
    drop(entries);
    println!(
        "Processed {processed} Yomitan entries -> {} index records",
        records.len()
    );
    binary::write(records, output, name)?;
    if skipped > 0 {
        println!("Skipped {skipped} headwords >= 32 bytes");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn bank_names_require_root_numeric_json_names() {
        for (name, expected) in [
            ("term_bank_1.json", true),
            ("term_bank_10.json", true),
            ("term_bank_.json", false),
            ("term_bank_a.json", false),
            ("nested/term_bank_1.json", false),
            ("term_bank_1.json.bak", false),
        ] {
            assert_eq!(bank_name(name), expected, "bank name: {name}");
        }
    }

    #[test]
    fn parse_bank_builds_headword_and_reading_definitions() -> Result<()> {
        let mut entries = Vec::new();
        parse_bank(
            &json!([["word", "reading", "", "v1", -2.9, ["meaning"]]]),
            "term_bank_1.json",
            &mut entries,
        )?;
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].definition, "【reading】\n\nmeaning");
        assert_eq!(entries[0].reading_definition, "meaning");
        assert_eq!(entries[0].priority, 126);
        assert_eq!(entries[0].flags, 1);
        Ok(())
    }

    #[test]
    fn parse_bank_rejects_invalid_root_with_bank_name() {
        let error = parse_bank(&json!({}), "term_bank_42.json", &mut Vec::new())
            .expect_err("object is not a bank array");
        assert!(error.to_string().contains("term_bank_42.json"));
    }

    #[test]
    fn missing_archive_error_identifies_source() -> Result<()> {
        let temp = tempfile::tempdir()?;
        let missing = temp.path().join("missing.zip");
        let Err(error) = load(&missing) else {
            anyhow::bail!("missing input unexpectedly loaded");
        };
        assert!(
            format!("{error:#}").contains(&missing.display().to_string()),
            "{error:#}"
        );
        Ok(())
    }
}
