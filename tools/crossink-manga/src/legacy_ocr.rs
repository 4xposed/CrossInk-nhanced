use anyhow::{Context, Result, bail, ensure};
use serde_json::{Value, json};
use std::{
    fs,
    io::Write,
    path::Path,
    process::{Command, Stdio},
    thread,
    time::Duration,
};

const URL: &str =
    "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.6-flash:generateContent";
const PROMPT: &str = r#"This image is a single panel cropped from a Japanese manga page.
List every piece of text/dialogue visible in this panel, in the order a
reader would read them (top-to-bottom, right-to-left for manga). Then give
a single natural English translation of all of it combined, in the same
reading order, as it would read in an English localization of this manga.

Return ONLY a JSON object, no other text:
{"blocks": [{"text": "<the Japanese text, line breaks as \n>",
             "bbox_2d": [ymin, xmin, ymax, xmax]}, ...],
 "translation": "<natural English translation of all the panel's text combined, in reading order>"}

bbox_2d is each text region's bounding box normalized to a 0-1000 scale
(0,0 = top-left of the panel image, 1000,1000 = bottom-right). If you
cannot determine a precise box, omit bbox_2d for that entry.
If there is no text in the panel, return {"blocks": [], "translation": ""}."#;

#[derive(Debug)]
pub struct Text {
    pub bbox: Option<[f64; 4]>,
    pub text: String,
}

#[derive(Debug, Default)]
pub struct Ocr {
    pub blocks: Vec<Text>,
    pub translation: String,
}

fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut result = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let word = (u32::from(chunk[0]) << 16)
            | (u32::from(*chunk.get(1).unwrap_or(&0)) << 8)
            | u32::from(*chunk.get(2).unwrap_or(&0));
        result.push(ALPHABET[((word >> 18) & 63) as usize] as char);
        result.push(ALPHABET[((word >> 12) & 63) as usize] as char);
        result.push(if chunk.len() > 1 {
            ALPHABET[((word >> 6) & 63) as usize] as char
        } else {
            '='
        });
        result.push(if chunk.len() > 2 {
            ALPHABET[(word & 63) as usize] as char
        } else {
            '='
        });
    }
    result
}

pub fn recognize(image: &Path, key: &str) -> Result<Ocr> {
    ensure!(
        !key.is_empty() && !key.contains(['\r', '\n', '\0']),
        "invalid Gemini API key"
    );
    let (mime, bytes) = image_payload(image)?;
    let payload = json!({"contents":[{"parts":[{"text":PROMPT},
        {"inline_data":{"mime_type":mime,"data":base64(&bytes)}}]}],
        "generationConfig":{"responseMimeType":"application/json"}});
    let mut request = tempfile::NamedTempFile::new()?;
    serde_json::to_writer(&mut request, &payload)?;
    request.flush()?;
    let mut headers = tempfile::NamedTempFile::new()?;
    writeln!(
        headers,
        "Content-Type: application/json\nx-goog-api-key: {key}"
    )?;
    headers.flush()?;
    let response = tempfile::NamedTempFile::new()?;
    for attempt in 0..3 {
        if attempt > 0 {
            thread::sleep(Duration::from_secs(1 << (attempt - 1)));
        }
        let output = Command::new("curl")
            .args([
                "--silent",
                "--show-error",
                "--max-time",
                "60",
                "--max-filesize",
                "4194304",
                "--request",
                "POST",
                "--write-out",
                "%{http_code}",
                "--header",
            ])
            .arg(format!("@{}", headers.path().display()))
            .arg("--data-binary")
            .arg(format!("@{}", request.path().display()))
            .arg("--output")
            .arg(response.path())
            .arg(URL)
            .stdin(Stdio::null())
            .output()
            .context("Gemini OCR requires native curl on PATH")?;
        if !output.status.success() {
            continue;
        }
        let status = String::from_utf8_lossy(&output.stdout)
            .trim()
            .parse::<u16>()
            .unwrap_or(0);
        if status == 429 || status == 408 || status >= 500 {
            continue;
        }
        ensure!(
            (200..300).contains(&status),
            "Gemini rejected OCR request (HTTP {status})"
        );
        let value: Value = match serde_json::from_slice(&fs::read(response.path())?) {
            Ok(value) => value,
            Err(_) => continue,
        };
        if let Some(error) = value.get("error") {
            if matches!(
                error.get("status").and_then(Value::as_str),
                Some("UNAVAILABLE" | "RESOURCE_EXHAUSTED" | "DEADLINE_EXCEEDED" | "INTERNAL")
            ) {
                continue;
            }
            bail!("Gemini rejected OCR request");
        }
        return parse_response(&value);
    }
    bail!("Gemini OCR failed after three attempts; no text available for panel")
}

fn image_payload(image: &Path) -> Result<(&'static str, Vec<u8>)> {
    let bytes = fs::read(image).context("read OCR crop")?;
    match image::guess_format(&bytes).context("identify OCR crop format")? {
        image::ImageFormat::Png => Ok(("image/png", bytes)),
        image::ImageFormat::Jpeg => Ok(("image/jpeg", bytes)),
        _ => {
            // Device output may be 1-bit BMP; Gemini accepts PNG/JPEG payloads.
            // Re-encode only the request, preserving the saved device artwork.
            let mut encoded = std::io::Cursor::new(Vec::new());
            image::load_from_memory(&bytes)?.write_to(&mut encoded, image::ImageFormat::Png)?;
            Ok(("image/png", encoded.into_inner()))
        }
    }
}

fn parse_response(value: &Value) -> Result<Ocr> {
    let text = value
        .pointer("/candidates/0/content/parts/0/text")
        .and_then(Value::as_str)
        .context("Gemini response has no text candidate")?;
    let parsed: Value = serde_json::from_str(text).context("Gemini candidate is not JSON")?;
    let mut blocks = Vec::new();
    if let Some(values) = parsed.get("blocks").and_then(Value::as_array) {
        for block in values {
            let Some(text) = block.get("text").and_then(Value::as_str) else {
                continue;
            };
            let bbox = block
                .get("bbox_2d")
                .and_then(Value::as_array)
                .and_then(|values| {
                    if values.len() != 4 {
                        return None;
                    }
                    let mut coordinates = [0.; 4];
                    for (index, value) in values.iter().enumerate() {
                        let value = value.as_f64()?;
                        if !value.is_finite() {
                            return None;
                        }
                        coordinates[index] = value.clamp(0., 1000.);
                    }
                    (coordinates[0] < coordinates[2] && coordinates[1] < coordinates[3])
                        .then_some(coordinates)
                });
            blocks.push(Text {
                bbox,
                text: text.to_owned(),
            });
        }
    }
    Ok(Ocr {
        blocks,
        translation: parsed
            .get("translation")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn monochrome_bmp_crops_are_sent_as_supported_png_without_changing_crop() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("panel.bmp");
        image::GrayImage::from_pixel(3, 2, image::Luma([0]))
            .save(&path)
            .unwrap();
        let original = fs::read(&path).unwrap();
        let (mime, bytes) = image_payload(&path).unwrap();
        assert_eq!(mime, "image/png");
        assert_eq!(
            image::guess_format(&bytes).unwrap(),
            image::ImageFormat::Png
        );
        assert_eq!(fs::read(path).unwrap(), original);
    }

    fn envelope(value: serde_json::Value) -> serde_json::Value {
        json!({"candidates":[{"content":{"parts":[{"text":value.to_string()}]}}]})
    }

    #[test]
    fn encodes_binary_images_with_standard_padding() {
        for (bytes, expected) in [
            (b"".as_slice(), ""),
            (b"f", "Zg=="),
            (b"fo", "Zm8="),
            (b"foo", "Zm9v"),
            (&[0, 255, 254], "AP/+"),
        ] {
            assert_eq!(base64(bytes), expected);
        }
    }

    #[test]
    fn rejects_header_injection_before_reading_or_sending_image() {
        let error = recognize(Path::new("missing.png"), "key\r\ninjected: value").unwrap_err();
        assert_eq!(error.to_string(), "invalid Gemini API key");
    }

    #[test]
    fn reads_unicode_translation_and_normalized_boxes() {
        let ocr = parse_response(&envelope(json!({"blocks":[
            {"text":"猫", "bbox_2d":[-2,100,1100,900]},
            {"text":"犬"}], "translation":"Cat and dog"})))
        .unwrap();
        assert_eq!(ocr.blocks.len(), 2);
        assert_eq!(ocr.blocks[0].text, "猫");
        assert_eq!(ocr.blocks[0].bbox, Some([0., 100., 1000., 900.]));
        assert_eq!(ocr.blocks[1].bbox, None);
        assert_eq!(ocr.translation, "Cat and dog");
    }

    #[test]
    fn invalid_block_types_are_ignored_and_bad_boxes_use_panel_fallback() {
        let ocr = parse_response(&envelope(json!({"blocks":[null,{"text":5},
            {"text":"bad box", "bbox_2d":[900,100,200,300]}],"translation":17})))
        .unwrap();
        assert_eq!(ocr.blocks.len(), 1);
        assert_eq!(ocr.blocks[0].bbox, None);
        assert_eq!(ocr.translation, "");
    }

    #[test]
    fn reports_missing_or_malformed_candidate_without_echoing_response() {
        assert!(parse_response(&json!({"error":{"message":"private payload"}})).is_err());
        let error = parse_response(
            &json!({"candidates":[{"content":{"parts":[{"text":"private payload"}]}}]}),
        )
        .unwrap_err();
        assert!(!error.to_string().contains("private payload"));
    }
}
