use crate::mokuro::TextBlock;
use anyhow::{Context, Result, ensure};
use image::GrayImage;
use std::io::Write;
use std::path::Path;

/// Export processing only; packed books remain standard one-bit BMPs.
#[derive(
    Debug,
    Default,
    Clone,
    Copy,
    PartialEq,
    Eq,
    clap::ValueEnum,
    serde::Serialize,
    serde::Deserialize,
)]
#[serde(rename_all = "kebab-case")]
pub enum Dither {
    #[default]
    FloydSteinberg,
    Bayer,
}

pub const MAX_RECORD: usize = 32754;
pub fn encode_ocr(blocks: &[TextBlock]) -> Result<Vec<u8>> {
    ensure!(blocks.len() <= 255, "panel has more than 255 OCR blocks");
    let size = 4 + blocks.iter().map(|b| 12 + b.text.len()).sum::<usize>();
    ensure!(
        size <= MAX_RECORD,
        "OCR panel needs {size} bytes; device limit is {MAX_RECORD}"
    );
    let mut out = Vec::with_capacity(size);
    out.extend_from_slice(&(blocks.len() as u16).to_le_bytes());
    out.extend_from_slice(&[0, 0]);
    for block in blocks {
        ensure!(
            block.bounds[2] > 0 && block.bounds[3] > 0 && !block.text.contains('\0'),
            "invalid OCR block"
        );
        for coordinate in block.bounds {
            out.extend_from_slice(&coordinate.to_le_bytes());
        }
        out.extend_from_slice(&(block.text.len() as u16).to_le_bytes());
        out.extend_from_slice(&u16::from(block.vertical).to_le_bytes());
        out.extend_from_slice(block.text.as_bytes());
    }
    Ok(out)
}

/// Encode a one-bit BMP using the default Floyd–Steinberg conversion.
pub fn write_bmp(path: &Path, image: &GrayImage) -> Result<()> {
    write_bmp_with_dither(path, image, Dither::default())
}

/// Dither at final resolution and encode bottom-up BMP rows with 4-byte alignment.
pub fn write_bmp_with_dither(path: &Path, image: &GrayImage, dither: Dither) -> Result<()> {
    let (w, h) = image.dimensions();
    ensure!(
        w > 0 && h > 0 && ((w <= 528 && h <= 800) || (w <= 800 && h <= 528)),
        "invalid manga bitmap dimensions"
    );
    encode_bmp(path, image, dither)
}

/// Legacy full-page books may retain source resolution; cap host allocation at 64 MP.
pub fn write_legacy_bmp(path: &Path, image: &GrayImage) -> Result<()> {
    let (w, h) = image.dimensions();
    ensure!(
        w > 0
            && h > 0
            && w <= 65535
            && h <= 65535
            && u64::from(w) * u64::from(h) <= 64 * 1024 * 1024,
        "legacy bitmap exceeds dimension or 64 megapixel limit"
    );
    encode_bmp(path, image, Dither::FloydSteinberg)
}

fn encode_bmp(path: &Path, image: &GrayImage, dither: Dither) -> Result<()> {
    let (w, h) = image.dimensions();
    let stride = w.div_ceil(32) * 4;
    let size = 62 + stride * h;
    let mut file = std::io::BufWriter::new(
        std::fs::File::create(path).with_context(|| format!("create {}", path.display()))?,
    );
    file.write_all(b"BM")?;
    file.write_all(&size.to_le_bytes())?;
    file.write_all(&[0; 4])?;
    file.write_all(&62u32.to_le_bytes())?;
    file.write_all(&40u32.to_le_bytes())?;
    file.write_all(&w.to_le_bytes())?;
    file.write_all(&h.to_le_bytes())?;
    file.write_all(&1u16.to_le_bytes())?;
    file.write_all(&1u16.to_le_bytes())?;
    file.write_all(&0u32.to_le_bytes())?;
    file.write_all(&(stride * h).to_le_bytes())?;
    file.write_all(&[0; 8])?;
    file.write_all(&2u32.to_le_bytes())?;
    file.write_all(&0u32.to_le_bytes())?;
    file.write_all(&[0, 0, 0, 0, 255, 255, 255, 0])?;
    const BAYER: [[u8; 4]; 4] = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]];
    // Host-only buffers: keep diffusion top-to-bottom while storing BMP rows
    // bottom-up. At accepted dimensions these use <=55 KiB plus a <=3.2 KiB error row.
    let mut pixels = vec![0u8; (stride * h) as usize];
    let mut errors = if dither == Dither::FloydSteinberg {
        vec![0i32; w as usize + 1]
    } else {
        Vec::new()
    };
    for y in 0..h {
        let start = ((h - 1 - y) * stride) as usize;
        let row = &mut pixels[start..start + stride as usize];
        let (mut carry, mut below, mut previous_error) = (0, 0, 0);
        for x in 0..w {
            let value = image.get_pixel(x, y).0[0];
            let white = match dither {
                Dither::Bayer => value > BAYER[(y % 4) as usize][(x % 4) as usize] * 16 + 7,
                Dither::FloydSteinberg => {
                    // Match Pillow 12.3.0 tobilevel: signed division truncates
                    // toward zero, clip before quantization, and use >128.
                    // Adapted under Pillow's HPND license; see UPSTREAM-NOTICES.md.
                    let adjusted =
                        (i32::from(value) + (carry + errors[x as usize + 1]) / 16).clamp(0, 255);
                    let white = adjusted > 128;
                    let error = adjusted - if white { 255 } else { 0 };
                    errors[x as usize] = 3 * error + below;
                    below = 5 * error + previous_error;
                    previous_error = error;
                    carry = 7 * error;
                    white
                }
            };
            if white {
                row[(x / 8) as usize] |= 0x80 >> (x % 8);
            }
        }
        if dither == Dither::FloydSteinberg {
            errors[w as usize] = below;
        }
    }
    file.write_all(&pixels)?;
    file.flush()?;
    file.get_ref().sync_all()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn default_dither_matches_pillow_floyd_steinberg_fixture() {
        let dir = tempfile::tempdir().unwrap();
        let img = GrayImage::from_raw(
            5,
            4,
            vec![
                0, 64, 128, 192, 255, 128, 128, 128, 128, 128, 20, 240, 100, 150, 60, 200, 30, 180,
                70, 220,
            ],
        )
        .unwrap();
        let path = dir.path().join("fixture.bmp");
        write_bmp(&path, &img).unwrap();
        let decoded = image::open(path).unwrap().to_luma8();
        let expected = include_bytes!("../tests/fixtures/floyd-steinberg-5x4.gray");
        assert_eq!(decoded.as_raw().as_slice(), expected);
    }

    #[test]
    fn bayer_option_preserves_previous_pixels() {
        let dir = tempfile::tempdir().unwrap();
        let img = GrayImage::from_raw(
            5,
            4,
            vec![
                0, 64, 128, 192, 255, 128, 128, 128, 128, 128, 20, 240, 100, 150, 60, 200, 30, 180,
                70, 220,
            ],
        )
        .unwrap();
        let path = dir.path().join("bayer.bmp");
        write_bmp_with_dither(&path, &img, Dither::Bayer).unwrap();
        assert_eq!(
            image::open(path).unwrap().to_luma8().as_raw(),
            &[
                0, 0, 255, 255, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 255, 0, 0, 0, 0, 0,
            ]
        );
    }

    #[test]
    fn writes_independent_little_endian_ocr_fixture() {
        let bytes = encode_ocr(&[TextBlock {
            bounds: [1, 2, 3, 4],
            vertical: true,
            text: "猫".into(),
        }])
        .unwrap();
        assert_eq!(
            bytes,
            vec![
                1, 0, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 3, 0, 1, 0, 0xe7, 0x8c, 0xab
            ]
        );
    }
    #[test]
    fn rejects_a_panel_exceeding_firmware_buffer_limit() {
        assert!(
            encode_ocr(&[TextBlock {
                bounds: [0, 0, 1, 1],
                vertical: false,
                text: "x".repeat(32754)
            }])
            .is_err()
        );
    }
    #[test]
    fn writes_padded_one_bit_bmp_with_correct_polarity() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("image.bmp");
        let mut img = GrayImage::from_pixel(9, 2, image::Luma([255]));
        img.put_pixel(0, 0, image::Luma([0]));
        write_bmp(&path, &img).unwrap();
        let bytes = std::fs::read(&path).unwrap();
        assert_eq!(&bytes[28..30], &[1, 0]);
        let decoded = image::open(path).unwrap().to_luma8();
        assert_eq!(decoded, img);
    }
}
