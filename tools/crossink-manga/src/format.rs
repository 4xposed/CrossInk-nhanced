use crate::mokuro::TextBlock;
use anyhow::{Context, Result, ensure};
use image::GrayImage;
use std::io::Write;
use std::path::Path;

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

/// Encode one-bit bottom-up BMP, with ordered dithering and 4-byte row alignment.
pub fn write_bmp(path: &Path, image: &GrayImage) -> Result<()> {
    let (w, h) = image.dimensions();
    ensure!(
        w > 0 && h > 0 && w <= 528 && h <= 800,
        "invalid manga bitmap dimensions"
    );
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
    let mut row = vec![0u8; stride as usize];
    for y in (0..h).rev() {
        row.fill(0);
        for x in 0..w {
            if image.get_pixel(x, y).0[0] > BAYER[(y % 4) as usize][(x % 4) as usize] * 16 + 7 {
                row[(x / 8) as usize] |= 0x80 >> (x % 8);
            }
        }
        file.write_all(&row)?;
    }
    file.flush()?;
    file.get_ref().sync_all()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
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
