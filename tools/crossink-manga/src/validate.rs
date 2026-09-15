use anyhow::{Context, Result, ensure};
use std::{
    fs,
    io::{Read, Seek, SeekFrom},
    path::Path,
};

fn u16_from(reader: &mut impl Read) -> Result<u16> {
    let mut b = [0; 2];
    reader.read_exact(&mut b)?;
    Ok(u16::from_le_bytes(b))
}
fn u32_from(reader: &mut impl Read) -> Result<u32> {
    let mut b = [0; 4];
    reader.read_exact(&mut b)?;
    Ok(u32::from_le_bytes(b))
}

/// Read the exported wire format independently of the writer before publication.
pub fn validate(folder: &Path) -> Result<()> {
    let mut index = fs::File::open(folder.join("book.mki")).context("missing book.mki")?;
    let mut magic = [0; 4];
    index.read_exact(&mut magic)?;
    let version = u32_from(&mut index)?;
    ensure!(
        &magic == b"CMI1" && (1..=3).contains(&version),
        "unsupported book index"
    );
    let count = u32_from(&mut index)?;
    ensure!(
        (1..=10000).contains(&count) && index.metadata()?.len() == 12 + u64::from(count) * 20,
        "invalid index size/count"
    );
    let mut data = fs::File::open(folder.join("book.mkd"))?;
    let data_size = data.metadata()?.len();
    let mut expected_offset = 0u64;
    for panel in 0..count {
        let offset = u64::from(u32_from(&mut index)?);
        let length = u64::from(u32_from(&mut index)?);
        let width = u16_from(&mut index)?;
        let height = u16_from(&mut index)?;
        let _source_page = u32_from(&mut index)?;
        let _panel = u16_from(&mut index)?;
        ensure!(u16_from(&mut index)? == 0, "nonzero index reserved field");
        ensure!(
            width > 0
                && height > 0
                && ((width <= if version == 1 { 480 } else { 528 } && height <= 800)
                    || (version == 3 && width <= 800 && height <= 528)),
            "invalid image dimensions"
        );
        ensure!(
            offset == expected_offset
                && (4..=32754).contains(&length)
                && offset <= data_size
                && length <= data_size - offset,
            "invalid OCR extent"
        );
        data.seek(SeekFrom::Start(offset))?;
        let mut record = vec![0; length as usize];
        data.read_exact(&mut record)?;
        let mut bytes = record.as_slice();
        let blocks = u16_from(&mut bytes)?;
        ensure!(
            blocks <= 255 && u16_from(&mut bytes)? == 0,
            "invalid OCR block count/reserved field"
        );
        for _ in 0..blocks {
            let x = u16_from(&mut bytes)?;
            let y = u16_from(&mut bytes)?;
            let w = u16_from(&mut bytes)?;
            let h = u16_from(&mut bytes)?;
            let n = usize::from(u16_from(&mut bytes)?);
            let flags = u16_from(&mut bytes)?;
            ensure!(
                w > 0
                    && h > 0
                    && u32::from(x) + u32::from(w) <= u32::from(width)
                    && u32::from(y) + u32::from(h) <= u32::from(height),
                "OCR rectangle outside image"
            );
            ensure!(
                flags <= 1 && n <= bytes.len(),
                "invalid OCR flags/text length"
            );
            let text = std::str::from_utf8(&bytes[..n]).context("invalid OCR UTF-8")?;
            ensure!(!text.contains('\0'), "NUL in OCR text");
            bytes = &bytes[n..];
        }
        ensure!(bytes.is_empty(), "unexpected trailing OCR data");
        expected_offset += length;
        let bitmap = folder.join(format!("page_{panel:04}.bmp"));
        ensure!(
            !fs::symlink_metadata(&bitmap)?.file_type().is_symlink(),
            "bitmap cannot be a symlink"
        );
        let mut image = fs::File::open(&bitmap)?;
        let mut header = [0u8; 62];
        image.read_exact(&mut header)?;
        ensure!(
            &header[..2] == b"BM"
                && &header[10..14] == 62u32.to_le_bytes().as_slice()
                && &header[14..18] == 40u32.to_le_bytes().as_slice()
                && &header[26..30] == [1, 0, 1, 0].as_slice()
                && &header[30..34] == [0, 0, 0, 0].as_slice(),
            "unsupported bitmap encoding"
        );
        ensure!(
            &header[18..22] == u32::from(width).to_le_bytes().as_slice()
                && &header[22..26] == u32::from(height).to_le_bytes().as_slice(),
            "bitmap/index dimensions disagree"
        );
        let expected_size = 62 + u32::from(width).div_ceil(32) * 4 * u32::from(height);
        ensure!(
            image.metadata()?.len() == u64::from(expected_size)
                && &header[2..6] == expected_size.to_le_bytes().as_slice(),
            "bitmap size mismatch"
        );
        ensure!(
            &header[54..62] == [0, 0, 0, 0, 255, 255, 255, 0].as_slice(),
            "bitmap palette mismatch"
        );
    }
    ensure!(expected_offset == data_size, "unreferenced OCR data");
    let metadata = fs::read(folder.join("meta.bin"))?;
    let mut bytes = metadata.as_slice();
    ensure!(u32_from(&mut bytes)? == 1, "unsupported metadata");
    let title = usize::from(u16_from(&mut bytes)?);
    let author = usize::from(u16_from(&mut bytes)?);
    ensure!(
        title <= 1024 && author <= 1024 && bytes.len() >= title + author + 2,
        "metadata length invalid"
    );
    ensure!(
        !std::str::from_utf8(&bytes[..title + author])?.contains('\0'),
        "metadata contains NUL"
    );
    bytes = &bytes[title + author..];
    let language = usize::from(u16_from(&mut bytes)?);
    ensure!(
        language <= 16 && bytes.len() == language && !std::str::from_utf8(bytes)?.contains('\0'),
        "invalid language metadata"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn refuses_truncated_index() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("book.mki"), b"CMI1").unwrap();
        assert!(validate(dir.path()).is_err());
    }
}
