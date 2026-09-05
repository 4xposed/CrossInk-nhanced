use serde_json::Value;

const POS_ICHIDAN: u8 = 0x01;
const POS_GODAN: u8 = 0x02;
const POS_SURU: u8 = 0x04;
const POS_KURU: u8 = 0x08;
const POS_I_ADJECTIVE: u8 = 0x10;
const POS_OTHER: u8 = 0x20;
const POS_ANY_VERB: u8 = POS_ICHIDAN | POS_GODAN | POS_SURU | POS_KURU;
pub(crate) const POS_READING: u8 = 0x40;

fn field<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

pub(crate) fn flatten(value: &Value) -> String {
    match value {
        Value::Object(_) => flatten_object(value),
        Value::String(text) => text.clone(),
        Value::Array(items) => items.iter().map(flatten).collect(),
        Value::Null => "None".into(),
        Value::Bool(true) => "True".into(),
        Value::Bool(false) => "False".into(),
        Value::Number(number) => number.to_string(),
    }
}

#[expect(
    clippy::match_same_arms,
    reason = "overlapping tags and content kinds have precedence."
)]
fn flatten_object(value: &Value) -> String {
    match field(value, "type") {
        "text" => return field(value, "text").to_owned(),
        "image" => return String::new(),
        _ => {}
    }
    let text = value.get("content").map(flatten).unwrap_or_default();
    if field(value, "type") == "structured-content" {
        return text;
    }
    let tag = field(value, "tag");
    let data = &value["data"];
    let kind = field(data, "content");
    let class = field(data, "class");
    match (tag, kind, class) {
        ("br", _, _) => "\n".into(),
        ("rt", _, _) => String::new(),
        (
            _,
            "part-of-speech-info" | "field-info" | "misc-info" | "dialect-info" | "language-info",
            "tag",
        ) => format!("[{text}] "),
        (_, "forms-label", "tag") => String::new(),
        ("ul", "glossary", _) => format!("\n{text}"),
        ("li", "", _) => format!("• {}\n", text.trim()),
        ("li" | "div", "sense-group", _) => format!("{text}\n"),
        ("li", "sense", _) => text,
        (_, "sense-note-label", _) => format!("{text}: "),
        (_, "sense-note-content", _) => format!("{text}\n"),
        (_, "sense-note", "extra-box") => format!("  → {text}"),
        (_, "example-sentence-a" | "example-sentence-b", _) => format!("{text}\n"),
        (_, "example-sentence", "extra-box") => format!("  {text}"),
        (_, "xref", "extra-box") => String::new(),
        (_, "reference-label", _) => format!("{text} "),
        (_, "forms" | "attribution-footnote", _) => String::new(),
        _ => text,
    }
}

fn flatten_list(value: &Value) -> String {
    match value {
        Value::String(text) if !text.starts_with("redirected from") => text.clone(),
        Value::Object(_) => flatten(value),
        Value::Array(items) => items
            .iter()
            .map(flatten_list)
            .filter(|s| !s.is_empty())
            .collect::<Vec<_>>()
            .join(" "),
        _ => String::new(),
    }
}

/// Flatten and deduplicate the first six senses once for headword and reading records.
pub(crate) fn senses(definitions: &Value) -> Vec<String> {
    let mut values = Vec::with_capacity(6);
    if let Some(items) = definitions.as_array() {
        for item in items.iter().take(6) {
            let text = flatten_list(item).trim().to_owned();
            if !text.is_empty() && !values.contains(&text) {
                values.push(text);
            }
        }
    }
    values
}

/// Render flattened senses, adding a reading prefix only when it differs from the headword.
pub(crate) fn definition(headword: &str, reading: &str, senses: &[String]) -> String {
    let mut parts = Vec::with_capacity(senses.len() + 1);
    if !reading.is_empty() && reading != headword {
        parts.push(format!("【{reading}】"));
    }
    for (index, text) in senses.iter().enumerate() {
        parts.push(if senses.len() > 1 {
            format!("\n{}. {text}", index + 1)
        } else {
            format!("\n{text}")
        });
    }
    let joined = parts.join("\n");
    let mut result = String::with_capacity(joined.len());
    let mut spaces = false;
    let mut newlines = 0;
    for ch in joined.chars() {
        if ch == ' ' || ch == '\t' {
            if !spaces {
                result.push(' ');
            }
            spaces = true;
            newlines = 0;
        } else {
            spaces = false;
            if ch == '\n' {
                newlines += 1;
            } else {
                newlines = 0;
            }
            if newlines <= 2 {
                result.push(ch);
            }
        }
    }
    result.replace("• • ", "• ").trim().to_owned()
}

/// Find the first nonempty redirect glossary target in nested content.
pub(crate) fn redirect_target(value: &Value) -> String {
    match value {
        Value::Object(_) => {
            if field(&value["data"], "content") == "redirect-glossary" {
                flatten(value).replace('⟶', "").trim().to_owned()
            } else {
                value
                    .get("content")
                    .map(redirect_target)
                    .unwrap_or_default()
            }
        }
        Value::Array(items) => items
            .iter()
            .map(redirect_target)
            .find(|s| !s.is_empty())
            .unwrap_or_default(),
        _ => String::new(),
    }
}

/// Map Yomitan inflection rules to the firmware's part-of-speech bitmask.
pub(crate) fn pos_flags(rules: &str) -> u8 {
    if rules.trim().is_empty() {
        return POS_OTHER;
    }
    rules.split_whitespace().fold(0, |flags, tag| {
        flags
            | match tag {
                "vt" | "vi" | "aux" | "aux-adj" | "exp" => 0,
                _ if tag.starts_with("v1") => POS_ICHIDAN,
                _ if ["v5", "v4", "iv"]
                    .iter()
                    .any(|prefix| tag.starts_with(prefix)) =>
                {
                    POS_GODAN
                }
                _ if tag.starts_with("vs") => POS_SURU,
                _ if tag.starts_with("vk") => POS_KURU,
                _ if tag.starts_with("adj-i") => POS_I_ADJECTIVE,
                _ if tag.starts_with('v') || tag == "aux-v" => POS_ANY_VERB,
                _ => POS_OTHER,
            }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn pos_flags_default_and_verb_prefixes() {
        for (rules, expected) in [
            ("", 0x20),
            ("v5k", 0x02),
            ("vt v1", 0x01),
            ("vs vk", 0x0c),
            ("vt vi aux", 0),
            ("aux-v", 0x0f),
            ("adj-i", 0x10),
        ] {
            assert_eq!(pos_flags(rules), expected, "rules: {rules:?}");
        }
    }

    #[test]
    fn break_and_ruby_tags_take_precedence_over_semantic_labels() {
        for (tag, expected) in [("br", "\n"), ("rt", "")] {
            let node = json!({"tag":tag,"data":{"content":"part-of-speech-info","class":"tag"},"content":"noun"});
            assert_eq!(flatten(&node), expected, "tag: {tag}");
        }
    }

    #[test]
    fn typed_nodes_take_precedence_over_html_tags() {
        assert_eq!(
            flatten(&json!({"type":"text","tag":"rt","text":"visible"})),
            "visible"
        );
        assert_eq!(
            flatten(&json!({"type":"image","tag":"li","content":"hidden"})),
            ""
        );
        assert_eq!(
            flatten(&json!({"type":"structured-content","tag":"rt","content":"visible"})),
            "visible"
        );
    }

    #[test]
    fn scalar_content_retains_legacy_spellings() {
        assert_eq!(
            flatten(&json!([null, true, false, 12, "text"])),
            "NoneTrueFalse12text"
        );
    }

    #[test]
    fn redirect_search_returns_first_nonempty_nested_target() {
        let nodes = json!([{"content":[{"data":{"content":"redirect-glossary"},"content":"⟶ "}]},
            {"content":{"data":{"content":"redirect-glossary"},"content":["⟶ ",{"type":"text","text":"target"}]}},
            {"data":{"content":"redirect-glossary"},"content":"later"}]);
        assert_eq!(redirect_target(&nodes), "target");
    }

    #[test]
    fn redirect_search_ignores_ordinary_definitions() {
        assert_eq!(
            redirect_target(&json!(["⟶ ordinary text", {"content":"definition"}])),
            ""
        );
    }

    #[test]
    fn rendering_deduplicates_and_limits_senses_before_numbering() {
        let input = json!([
            " same ",
            "same",
            ["nested", ["text", "redirected from old"]],
            "four",
            "five",
            "six",
            "ignored"
        ]);
        assert_eq!(
            definition("word", "reading", &senses(&input)),
            "【reading】\n\n1. same\n\n2. nested text\n\n3. four\n\n4. five\n\n5. six"
        );
        assert_eq!(
            definition("reading", "reading", &senses(&input)),
            "1. same\n\n2. nested text\n\n3. four\n\n4. five\n\n5. six"
        );
    }

    #[test]
    fn rendering_normalizes_whitespace_including_reading_prefix() {
        let input = json!([" a\t b\n\n\n c • • d "]);
        assert_eq!(
            definition("word", " a\t b ", &senses(&input)),
            "【 a b 】\n\na b\n\n c • d"
        );
        assert_eq!(
            definition("word", "reading", &senses(&json!([]))),
            "【reading】"
        );
        assert_eq!(definition("word", "word", &senses(&json!([]))), "");
    }
}
