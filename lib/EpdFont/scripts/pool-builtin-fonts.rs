//! Deduplicate literal immutable font tables without changing their compiled representation.
use regex::Regex;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    env,
    error::Error,
    fs,
    path::{Path, PathBuf},
    process::ExitCode,
};
type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Debug)]
struct Array {
    name: String,
    kind: String,
    key: String,
    start: usize,
    end: usize,
    declaration: String,
    bytes: usize,
}
fn fields(kind: &str) -> Result<(usize, Vec<(u32, bool)>)> {
    Ok(match kind {
        "uint8_t" => (1, vec![(8, false)]),
        "int8_t" => (1, vec![(8, true)]),
        "uint16_t" => (2, vec![(16, false)]),
        "EpdGlyph" => (
            16,
            vec![
                (8, false),
                (8, false),
                (16, false),
                (16, true),
                (16, true),
                (16, false),
                (32, false),
            ],
        ),
        "EpdFontGroup" => (
            20,
            vec![
                (32, false),
                (32, false),
                (32, false),
                (16, false),
                (32, false),
            ],
        ),
        "EpdUnicodeInterval" => (12, vec![(32, false); 3]),
        "EpdKernClassEntry" => (3, vec![(16, false), (8, false)]),
        "EpdLigaturePair" => (8, vec![(32, false); 2]),
        _ => return Err(format!("Unsupported table type: {kind}").into()),
    })
}
fn parse_arrays(source: &str) -> Result<Vec<Array>> {
    let comments = Regex::new(r"(?s)/\*.*?\*/|//[^\n]*")?;
    let clean = comments.replace_all(source, |c: &regex::Captures<'_>| " ".repeat(c[0].len()));
    let pattern =
        Regex::new(r"(?s)static\s+const\s+(\w+)\s+(\w+)\s*\[\s*(\d*)\s*\]\s*=\s*(\{.*?\})\s*;")?;
    let hex = Regex::new(r"-?0[xX][0-9a-fA-F]+")?;
    let trailing = Regex::new(r",\s*]")?;
    let mut result = Vec::new();
    let mut remainder = clean.to_string();
    for cap in pattern.captures_iter(&clean) {
        let whole = cap.get(0).ok_or("Missing array match")?;
        let kind = &cap[1];
        let name = &cap[2];
        let (size, fields) = fields(kind)?;
        let decimal = hex.replace_all(&cap[4], |c: &regex::Captures<'_>| {
            let s = &c[0];
            let negative = s.starts_with('-');
            let value = i64::from_str_radix(&s[if negative { 3 } else { 2 }..], 16);
            match value {
                Ok(n) => (if negative { -n } else { n }).to_string(),
                Err(_) => "INVALID".into(),
            }
        });
        let body = decimal.replace('{', "[").replace('}', "]");
        let body = trailing.replace_all(&body, "]");
        let values: Vec<Value> = serde_json::from_str(&body)
            .map_err(|e| format!("Nonliteral initializer {name}: {e}"))?;
        if values.is_empty() || (!cap[3].is_empty() && cap[3].parse::<usize>()? != values.len()) {
            return Err(format!("Invalid extent: {name}").into());
        }
        for value in &values {
            let row = if kind.starts_with("Epd") {
                value
                    .as_array()
                    .ok_or("Invalid structured initializer")?
                    .as_slice()
            } else {
                std::slice::from_ref(value)
            };
            if row.len() != fields.len() {
                return Err(format!("Invalid field count: {name}").into());
            }
            for (value, (bits, signed)) in row.iter().zip(&fields) {
                let n = value.as_i64().ok_or("Invalid integer field")?;
                let low = if *signed { -(1i64 << (bits - 1)) } else { 0 };
                let high = (1i64 << (bits - u32::from(*signed))) - 1;
                if n < low || n > high {
                    return Err(format!("Invalid field value: {name}").into());
                }
            }
        }
        result.push(Array {
            name: name.into(),
            kind: kind.into(),
            key: serde_json::to_string(&json!([kind, values]))?,
            start: whole.start(),
            end: whole.end(),
            declaration: source[whole.range()].into(),
            bytes: values.len() * size,
        });
    }
    for a in result.iter().rev() {
        remainder.replace_range(a.start..a.end, "");
    }
    if Regex::new(r"\bstatic\s+const\b")?.is_match(&remainder) {
        return Err("Unsupported static const declaration in font header".into());
    }
    Ok(result)
}
fn font_paths(source: &Path) -> Result<Vec<PathBuf>> {
    let text = fs::read_to_string(source.join("all.h"))?;
    let reading = Regex::new(r"^#include BUILTIN_READING_FONT_HEADER\((\w+)\)$")?;
    let ui = Regex::new(r"^#include <builtinFonts/(\w+\.h)>$")?;
    let mut paths = Vec::new();
    for line in text.lines().filter(|line| line.starts_with("#include")) {
        if let Some(c) = reading.captures(line) {
            paths.push(PathBuf::from(format!("{}.h", &c[1])));
            paths.push(PathBuf::from(format!("noemoji/{}.h", &c[1])));
        } else if let Some(c) = ui.captures(line) {
            paths.push(PathBuf::from(&c[1]));
        } else {
            return Err("Unsupported built-in font manifest".into());
        }
    }
    if paths.is_empty() {
        return Err("Empty built-in font manifest".into());
    }
    paths.sort();
    Ok(paths)
}
fn symbol(key: &str) -> String {
    format!("crossink_font_pool_{:x}", Sha256::digest(key.as_bytes()))
}
fn write_changed(path: &Path, text: &str) -> Result<()> {
    if fs::read_to_string(path).ok().as_deref() != Some(text) {
        fs::create_dir_all(path.parent().ok_or("Missing parent directory")?)?;
        fs::write(path, text)?;
    }
    Ok(())
}
fn resolved(path: &Path) -> Result<PathBuf> {
    if path.exists() {
        return Ok(path.canonicalize()?);
    }
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    Ok(resolved(parent)?.join(path.file_name().ok_or("Invalid output path")?))
}
fn generate(source: &Path, output: &Path) -> Result<Value> {
    let source = source.canonicalize()?;
    let destination = resolved(&output.join("builtinFonts"))?;
    if destination.starts_with(&source) || source.starts_with(&destination) {
        return Err("Generated output must be outside source font directory".into());
    }
    let mut headers = BTreeMap::new();
    for path in font_paths(&source)? {
        let text = fs::read_to_string(source.join(&path))?;
        let arrays = parse_arrays(&text)?;
        headers.insert(path, (text, arrays));
    }
    let mut groups: BTreeMap<&str, Vec<&Array>> = BTreeMap::new();
    for (_, arrays) in headers.values() {
        for a in arrays {
            groups.entry(&a.key).or_default().push(a);
        }
    }
    for (key, arrays) in groups.iter().filter(|(_, a)| a.len() > 1) {
        let a = arrays[0];
        let sym = symbol(key);
        let declaration = a.declaration.replacen(&a.name, &sym, 1);
        write_changed(&destination.join("pool").join(format!("{sym}.h")), &format!("#pragma once\n#include <EpdFontData.h>\nstatic_assert(sizeof({}) == {}, \"Font table ABI changed\");\n{declaration}\n",a.kind,fields(&a.kind)?.0))?;
    }
    for (path, (text, arrays)) in &headers {
        let mut transformed = text.clone();
        for a in arrays.iter().rev() {
            if groups[a.key.as_str()].len() > 1 {
                let sym = symbol(&a.key);
                transformed.replace_range(
                    a.start..a.end,
                    &format!(
                        "#include <builtinFonts/pool/{sym}.h>\n#define {} {sym}",
                        a.name
                    ),
                );
            }
        }
        write_changed(&destination.join(path), &transformed)?;
    }
    write_changed(
        &destination.join("all.h"),
        &fs::read_to_string(source.join("all.h"))?,
    )?;
    let mut report = serde_json::Map::new();
    for variant in ["default", "noemoji"] {
        let mut counts: BTreeMap<&str, usize> = BTreeMap::new();
        let mut count = 0;
        for (path, (_, arrays)) in &headers {
            let name = path.file_name().ok_or("Invalid font path")?;
            if headers.contains_key(&Path::new("noemoji").join(name))
                && path.starts_with("noemoji") != (variant == "noemoji")
            {
                continue;
            }
            for a in arrays {
                *counts.entry(&a.key).or_default() += 1;
                count += 1;
            }
        }
        let mut savings: BTreeMap<&str, usize> = BTreeMap::new();
        for (key, n) in counts {
            let a = groups[key][0];
            *savings.entry(&a.kind).or_default() += (n - 1) * a.bytes;
        }
        report.insert(variant.into(),json!({"array_count":count,"candidate_bytes_saved":savings.values().sum::<usize>(),"savings_by_type":savings}));
    }
    let report = Value::Object(report);
    write_changed(
        &output.join("font-pool-report.json"),
        &(serde_json::to_string_pretty(&report)? + "\n"),
    )?;
    Ok(report)
}
fn run() -> Result<()> {
    let mut source = PathBuf::from("lib/EpdFont/builtinFonts");
    let mut output = None;
    let mut args = env::args_os().skip(1);
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--source") => source = args.next().ok_or("--source requires a directory")?.into(),
            Some("--output") => {
                output = Some(PathBuf::from(
                    args.next().ok_or("--output requires a directory")?,
                ))
            }
            Some("--help" | "-h") => {
                println!("Usage: pool-builtin-fonts [--source DIRECTORY] --output DIRECTORY");
                return Ok(());
            }
            _ => return Err(format!("Unknown argument: {}", arg.to_string_lossy()).into()),
        }
    }
    println!(
        "{}",
        serde_json::to_string_pretty(&generate(&source, &output.ok_or("--output is required")?)?)?
    );
    Ok(())
}
fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("pool-builtin-fonts: {e}");
            ExitCode::FAILURE
        }
    }
}
