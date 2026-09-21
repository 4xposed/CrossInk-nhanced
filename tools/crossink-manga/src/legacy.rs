use crate::{
    device::Device,
    panels::{self, Rect},
};
use anyhow::{Context, Result, ensure};
use image::{DynamicImage, GenericImageView};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

#[derive(clap::Args)]
pub struct Options {
    #[arg(long)]
    pub input: PathBuf,
    #[arg(long)]
    pub output_dir: PathBuf,
    #[arg(long)]
    pub no_ocr: bool,
    #[arg(long)]
    pub gemini_key_file: Option<PathBuf>,
    #[arg(long)]
    pub title: Option<String>,
    #[arg(long)]
    pub author: Option<String>,
    #[arg(long)]
    pub language: Option<String>,
    #[arg(long)]
    pub page_order_file: Option<PathBuf>,
    /// Ordered source-coordinate rectangles keyed by relative source filename.
    #[arg(long)]
    pub panel_map: Option<PathBuf>,
    #[arg(long)]
    pub toc_file: Option<PathBuf>,
    #[arg(long)]
    pub max_pages: Option<usize>,
    #[arg(long, default_value_t = 10)]
    pub panel_margin: u32,
    #[arg(long)]
    pub mono: bool,
    #[arg(long, conflicts_with = "x4")]
    pub x3: bool,
    #[arg(long)]
    pub x4: bool,
}

fn fitted(image: &DynamicImage, device: Option<Device>) -> DynamicImage {
    match device {
        Some(device) => {
            let (w, h) = panels::fitted(image.width(), image.height(), device);
            image.resize_exact(w, h, image::imageops::FilterType::Lanczos3)
        }
        None => image.clone(),
    }
}

fn write_image(image: &DynamicImage, path: &Path, mono: bool) -> Result<()> {
    if mono {
        crate::format::write_legacy_bmp(path, &image.to_luma8())
    } else if path.extension().is_some_and(|e| e == "png") {
        image.to_rgb8().save(path).context("write PNG")
    } else {
        let mut out = fs::File::create(path)?;
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, 92)
            .encode_image(&image.to_rgb8())
            .context("write baseline JPEG")
    }
}

fn baseline_jpeg(path: &Path) -> Result<bool> {
    use std::io::{Read, Seek, SeekFrom};
    let mut file = std::io::BufReader::new(fs::File::open(path)?);
    let mut marker = [0; 2];
    file.read_exact(&mut marker)?;
    if marker != [0xff, 0xd8] {
        return Ok(false);
    }
    // JPEG segment lengths bound every skip. Stop before entropy-coded pixels.
    for _ in 0..4096 {
        file.read_exact(&mut marker)?;
        if marker[0] != 0xff {
            return Ok(false);
        }
        while marker[1] == 0xff {
            file.read_exact(&mut marker[1..])?;
        }
        match marker[1] {
            0xc0 => return Ok(true),
            0xc1..=0xc3 | 0xc5..=0xcf | 0xda | 0xd9 => return Ok(false),
            0xd0..=0xd8 | 0x01 => continue,
            _ => {}
        }
        let mut length = [0; 2];
        file.read_exact(&mut length)?;
        let length = u16::from_be_bytes(length);
        ensure!(length >= 2, "invalid JPEG segment length");
        file.seek(SeekFrom::Current(i64::from(length) - 2))?;
    }
    Ok(false)
}

fn string_bytes(value: &str) -> Result<&[u8]> {
    ensure!(
        value.len() <= u16::MAX as usize && !value.contains('\0'),
        "metadata/text exceeds format limit or contains NUL"
    );
    Ok(value.as_bytes())
}

fn encode_panel(
    data: &mut Vec<u8>,
    panel: Rect,
    crop: Rect,
    ocr: &crate::legacy_ocr::Ocr,
) -> Result<()> {
    let Rect(x1, y1, x2, y2) = panel;
    let blocks: Vec<_> = ocr
        .blocks
        .iter()
        .filter(|b| !b.text.trim().is_empty())
        .take(255)
        .collect();
    for coordinate in [x1, y1, x2 - x1, y2 - y1] {
        data.extend_from_slice(&(coordinate as u16).to_le_bytes());
    }
    data.extend_from_slice(&[blocks.len() as u8, 0]);
    data.extend_from_slice(&(string_bytes(&ocr.translation)?.len() as u16).to_le_bytes());
    data.extend_from_slice(ocr.translation.as_bytes());
    for block in blocks {
        let bounds = if let Some([top, left, bottom, right]) = block.bbox {
            let Rect(cx, cy, cr, cb) = crop;
            let tx = cx + (left.clamp(0.0, 1000.0) / 1000.0 * f64::from(cr - cx)) as u32;
            let ty = cy + (top.clamp(0.0, 1000.0) / 1000.0 * f64::from(cb - cy)) as u32;
            let right = cx + (right.clamp(0.0, 1000.0) / 1000.0 * f64::from(cr - cx)) as u32;
            let bottom = cy + (bottom.clamp(0.0, 1000.0) / 1000.0 * f64::from(cb - cy)) as u32;
            [tx, ty, right.saturating_sub(tx), bottom.saturating_sub(ty)]
        } else {
            [x1, y1, x2 - x1, y2 - y1]
        };
        for coordinate in bounds {
            data.extend_from_slice(
                &u16::try_from(coordinate)
                    .context("OCR coordinate exceeds legacy format")?
                    .to_le_bytes(),
            );
        }
        let text = string_bytes(block.text.trim())?;
        data.extend_from_slice(&(text.len() as u16).to_le_bytes());
        data.extend_from_slice(text);
    }
    Ok(())
}

pub fn write_meta(path: &Path, title: &str, author: &str, language: &str) -> Result<()> {
    if title.is_empty() && author.is_empty() && language.is_empty() {
        return Ok(());
    }
    let language = language.trim();
    let (primary, suffix) = language.split_once(['-', '_']).unwrap_or((language, ""));
    let primary = primary.to_lowercase();
    let primary = match primary.as_str() {
        "jp" => "ja",
        "cn" => "zh",
        "kr" => "ko",
        other => other,
    };
    let language = if suffix.is_empty() {
        primary.to_owned()
    } else {
        format!("{primary}-{suffix}")
    };
    let mut file = fs::File::create(path.join("meta.bin"))?;
    file.write_all(&1u32.to_le_bytes())?;
    for value in [title, author] {
        file.write_all(&(string_bytes(value)?.len() as u16).to_le_bytes())?;
    }
    file.write_all(title.as_bytes())?;
    file.write_all(author.as_bytes())?;
    if !language.is_empty() {
        file.write_all(&(string_bytes(&language)?.len() as u16).to_le_bytes())?;
        file.write_all(language.as_bytes())?;
    }
    Ok(())
}

fn write_toc(output: &Path, source: &Path) -> Result<()> {
    let text = fs::read_to_string(source)?;
    let mut entries = Vec::new();
    for line in text
        .lines()
        .filter(|line| !line.trim().is_empty() && !line.starts_with('#'))
    {
        let (page, title) = line
            .split_once('\t')
            .context("TOC lines must be PAGE<TAB>TITLE")?;
        entries.push((
            page.trim().parse::<u32>().context("invalid TOC page")?,
            title.trim().to_owned(),
        ));
    }
    write_toc_entries(output, entries)
}

fn write_toc_entries(output: &Path, mut entries: Vec<(u32, String)>) -> Result<()> {
    entries.sort_by_key(|e| e.0);
    if !entries.first().is_some_and(|e| e.0 == 0) {
        entries.insert(0, (0, "Cover".into()));
    }
    let mut file = fs::File::create(output.join("toc.idx"))?;
    file.write_all(&1u32.to_le_bytes())?;
    file.write_all(&(entries.len() as u32).to_le_bytes())?;
    for (page, title) in entries {
        file.write_all(&page.to_le_bytes())?;
        file.write_all(&(string_bytes(&title)?.len() as u16).to_le_bytes())?;
        file.write_all(title.as_bytes())?;
    }
    Ok(())
}

pub fn convert(options: &Options) -> Result<()> {
    let key = if options.no_ocr {
        None
    } else {
        let key = if let Some(path) = &options.gemini_key_file {
            fs::read_to_string(path).context("read Gemini key file")?
        } else {
            std::env::var("GEMINI_API_KEY").unwrap_or_default()
        };
        ensure!(
            !key.trim().is_empty(),
            "provide --gemini-key-file or GEMINI_API_KEY, or use --no-ocr"
        );
        Some(key.trim().to_owned())
    };
    ensure!(
        !options.output_dir.exists(),
        "output already exists; choose a new directory"
    );
    ensure!(options.max_pages != Some(0), "--max-pages must be positive");
    let work = tempfile::tempdir()?;
    let sources = work.path().join("sources");
    let mut pages = crate::input::collect(&options.input, &sources)?;
    // Input collection already natural-sorts paths. Stable priority grouping keeps
    // cover/copyright pages first without interleaving numbered chapter folders.
    if !options
        .input
        .extension()
        .is_some_and(|ext| ext.eq_ignore_ascii_case("epub") || ext.eq_ignore_ascii_case("pdf"))
    {
        pages.sort_by_key(|path| {
            let name = path
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .to_lowercase();
            if name.contains("cover") {
                0
            } else if name.contains("copyright") {
                1
            } else {
                2
            }
        });
    }
    let panel_map: std::collections::BTreeMap<String, Vec<Rect>> =
        if let Some(path) = &options.panel_map {
            ensure!(
                fs::metadata(path)?.len() <= 8 * 1024 * 1024,
                "panel map exceeds 8 MiB"
            );
            serde_json::from_slice(&fs::read(path)?).context("parse panel map")?
        } else {
            Default::default()
        };
    for name in panel_map.keys() {
        ensure!(
            pages.iter().any(|path| path
                .strip_prefix(&sources)
                .is_ok_and(|p| p.to_string_lossy().replace('\\', "/") == *name)),
            "unknown panel-map source: {name}"
        );
    }
    let metadata = crate::legacy_metadata::read(&options.input).unwrap_or_else(|error| {
        eprintln!("Warning: could not read source metadata: {error:#}");
        crate::legacy_metadata::BookMetadata::default()
    });
    let original_pages = pages.clone();
    if let Some(order) = &options.page_order_file {
        let order = fs::read_to_string(order)?;
        let mut selected = Vec::new();
        for name in order.lines().map(str::trim).filter(|s| !s.is_empty()) {
            let matches: Vec<_> = pages
                .iter()
                .filter(|p| {
                    p.strip_prefix(&sources)
                        .is_ok_and(|r| r.to_string_lossy().replace('\\', "/") == name)
                        || (!name.contains('/') && p.file_name().is_some_and(|f| f == name))
                })
                .collect();
            ensure!(
                matches.len() == 1,
                "page order entry missing or ambiguous: {name}"
            );
            if !selected.contains(matches[0]) {
                selected.push(matches[0].clone());
            }
        }
        pages = selected;
    }
    if let Some(count) = options.max_pages {
        pages.truncate(count);
    }
    ensure!(!pages.is_empty(), "no pages selected");
    let parent = options
        .output_dir
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(parent)?;
    let staging = tempfile::tempdir_in(parent)?;
    let output = staging.path();
    let device = if options.x3 {
        Some(Device::X3)
    } else if options.x4 {
        Some(Device::X4)
    } else {
        None
    };
    let mut index = Vec::new();
    index.extend_from_slice(&2u32.to_le_bytes());
    index.extend_from_slice(&(pages.len() as u32).to_le_bytes());
    let mut data = Vec::new();
    for (number, source) in pages.iter().enumerate() {
        let decoded =
            image::open(source).with_context(|| format!("decode {}", source.display()))?;
        let has_alpha = decoded.color().has_alpha();
        let rgb = image::RgbImage::from_fn(decoded.width(), decoded.height(), |x, y| {
            let pixel = decoded.get_pixel(x, y).0;
            let alpha = u32::from(pixel[3]);
            image::Rgb([0, 1, 2].map(|channel| {
                ((u32::from(pixel[channel]) * alpha + 255 * (255 - alpha) + 127) / 255) as u8
            }))
        });
        let original = DynamicImage::ImageRgb8(rgb);
        let page = fitted(&original, device);
        let (w, h) = page.dimensions();
        ensure!(
            w <= u16::MAX as u32 && h <= u16::MAX as u32,
            "page dimensions exceed legacy format"
        );
        let ext = if options.mono {
            "bmp"
        } else if source
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("png"))
        {
            "png"
        } else {
            "jpg"
        };
        let page_output = output.join(format!("page_{number:04}.{ext}"));
        let source_extension = source
            .extension()
            .unwrap_or_default()
            .to_string_lossy()
            .to_ascii_lowercase();
        let copy_original = !options.mono
            && !has_alpha
            && original.dimensions() == page.dimensions()
            && (source_extension == "png"
                || (matches!(source_extension.as_str(), "jpg" | "jpeg") && baseline_jpeg(source)?));
        if copy_original {
            fs::copy(source, &page_output)?;
        } else {
            write_image(&page, &page_output, options.mono)?;
        }
        let source_name = source
            .strip_prefix(&sources)?
            .to_string_lossy()
            .replace('\\', "/");
        let boxes = if let Some(rects) = panel_map.get(&source_name) {
            ensure!(!rects.is_empty(), "empty panel map for {source_name}");
            rects
                .iter()
                .map(|&Rect(left, top, right, bottom)| {
                    ensure!(
                        left < right
                            && top < bottom
                            && right <= original.width()
                            && bottom <= original.height(),
                        "invalid panel rectangle for {source_name}"
                    );
                    let x = |v| {
                        (f64::from(v) * f64::from(w) / f64::from(original.width())).round() as u32
                    };
                    let y = |v| {
                        (f64::from(v) * f64::from(h) / f64::from(original.height())).round() as u32
                    };
                    ensure!(
                        x(left) < x(right) && y(top) < y(bottom),
                        "panel collapses at output resolution"
                    );
                    Ok(Rect(x(left), y(top), x(right), y(bottom)))
                })
                .collect::<Result<Vec<_>>>()?
        } else {
            detect_grid(&page.to_luma8())
        };
        ensure!(boxes.len() <= 255, "more than 255 panels on page");
        let start = u32::try_from(data.len()).context("legacy data exceeds 4 GiB")?;
        data.extend_from_slice(&[boxes.len() as u8, 0]);
        for (panel, Rect(x1, y1, x2, y2)) in boxes.iter().copied().enumerate() {
            let full_page =
                (x2 - x1) as f64 * (y2 - y1) as f64 >= f64::from(w) * f64::from(h) * 0.95;
            let mut ocr = crate::legacy_ocr::Ocr::default();
            let margin = options.panel_margin;
            let crop_bounds = Rect(
                x1.saturating_sub(margin),
                y1.saturating_sub(margin),
                x2.saturating_add(margin).min(w),
                y2.saturating_add(margin).min(h),
            );
            if !full_page {
                let sx = f64::from(original.width()) / f64::from(w);
                let sy = f64::from(original.height()) / f64::from(h);
                let left = (f64::from(x1.saturating_sub(margin)) * sx).round() as u32;
                let top = (f64::from(y1.saturating_sub(margin)) * sy).round() as u32;
                let right = (f64::from(x2.saturating_add(margin).min(w)) * sx).round() as u32;
                let bottom = (f64::from(y2.saturating_add(margin).min(h)) * sy).round() as u32;
                let crop = fitted(
                    &original.crop_imm(left, top, right - left, bottom - top),
                    device,
                );
                fs::create_dir_all(output.join("panels"))?;
                let ext = if options.mono { "bmp" } else { "jpg" };
                let path = output.join(format!("panels/p{number}_{panel}.{ext}"));
                write_image(&crop, &path, options.mono)?;
                if let Some(key) = &key {
                    ocr = crate::legacy_ocr::recognize(&path, key).unwrap_or_else(|error| {
                        eprintln!("Warning: OCR failed for page {number} panel {panel}: {error:#}");
                        crate::legacy_ocr::Ocr::default()
                    });
                }
            }
            encode_panel(&mut data, Rect(x1, y1, x2, y2), crop_bounds, &ocr)?;
        }
        index.extend_from_slice(&start.to_le_bytes());
        index.extend_from_slice(
            &(u32::try_from(data.len()).context("legacy data exceeds 4 GiB")? - start)
                .to_le_bytes(),
        );
        index.extend_from_slice(&(w as u16).to_le_bytes());
        index.extend_from_slice(&(h as u16).to_le_bytes());
    }
    fs::write(output.join("panels.idx"), index)?;
    fs::write(output.join("panels.dat"), data)?;
    write_meta(
        output,
        options.title.as_deref().unwrap_or(&metadata.title),
        options.author.as_deref().unwrap_or(&metadata.author),
        options.language.as_deref().unwrap_or(&metadata.language),
    )?;
    if let Some(toc) = &options.toc_file {
        write_toc(output, toc)?;
    } else if !metadata.chapters.is_empty() {
        let entries = metadata
            .chapters
            .iter()
            .filter_map(|(index, title)| {
                let source = original_pages.iter().find(|path| {
                    path.file_stem()
                        .and_then(|stem| stem.to_str())
                        .and_then(|stem| stem.strip_prefix("spine_"))
                        .and_then(|n| n.parse::<usize>().ok())
                        == Some(*index)
                })?;
                let index = pages.iter().position(|page| page == source)?;
                Some((index as u32, title.clone()))
            })
            .collect();
        write_toc_entries(output, entries)?;
    }
    fs::rename(output, &options.output_dir).context("publish legacy book")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ocr_encoding_matches_literal_legacy_fixture() {
        let ocr = crate::legacy_ocr::Ocr {
            blocks: vec![crate::legacy_ocr::Text {
                bbox: Some([50.0, 100.0, 125.0, 266.6666667]),
                text: "猫".into(),
            }],
            translation: "Hi".into(),
        };
        let mut bytes = Vec::new();
        encode_panel(&mut bytes, Rect(1, 2, 31, 42), Rect(0, 0, 30, 80), &ocr).unwrap();
        assert_eq!(
            bytes,
            [
                1, 0, 2, 0, 30, 0, 40, 0, 1, 0, 2, 0, b'H', b'i', 3, 0, 4, 0, 5, 0, 6, 0, 3, 0,
                0xe7, 0x8c, 0xab
            ]
        );
    }
}

fn detect_grid(image: &image::GrayImage) -> Vec<Rect> {
    fn splits(size: u32, gutter: u32, minimum: u32, white: impl Fn(u32) -> bool) -> Vec<u32> {
        let mut cuts = vec![0];
        let mut start = None;
        for position in 0..size {
            if white(position) {
                if start.is_none() {
                    start = Some(position);
                }
            } else if let Some(begin) = start.take()
                && position - begin >= gutter
            {
                cuts.push((begin + position) / 2);
            }
        }
        cuts.push(size);
        if cuts.len() <= 2 {
            return cuts;
        }
        let mut merged = vec![0];
        for position in cuts.into_iter().skip(1) {
            if position - merged.last().copied().unwrap_or(0) >= minimum {
                merged.push(position);
            }
        }
        if merged.last() != Some(&size)
            && let Some(last) = merged.last_mut()
        {
            *last = size;
        }
        merged
    }
    let (w, h) = image.dimensions();
    let gutter = 6.max((f64::from(h) * 0.013) as u32);
    let rows = splits(h, gutter, 60.max((f64::from(h) * 0.05) as u32), |y| {
        (0..w)
            .step_by(2)
            .filter(|x| image.get_pixel(*x, y).0[0] > 215)
            .count() as f64
            > f64::from(w / 2) * 0.95
    });
    let mut boxes = Vec::new();
    for row in rows.windows(2) {
        let (top, bottom) = (row[0], row[1]);
        let cols = splits(w, gutter, 60.max((f64::from(w) * 0.06) as u32), |x| {
            (top..bottom)
                .step_by(2)
                .filter(|y| image.get_pixel(x, *y).0[0] > 215)
                .count() as f64
                > f64::from((bottom - top) / 2) * 0.95
        });
        for col in cols.windows(2).rev() {
            boxes.push(Rect(col[0], top, col[1], bottom));
        }
    }
    if boxes.is_empty() {
        boxes.push(Rect(0, 0, w, h));
    }
    boxes
}

#[cfg(test)]
mod grid_tests {
    use super::*;
    #[test]
    fn legacy_grid_uses_near_white_gutters_in_reading_order() {
        let mut image = image::GrayImage::from_pixel(200, 200, image::Luma([0]));
        for y in 0..200 {
            for x in 90..110 {
                image.put_pixel(x, y, image::Luma([220]));
            }
        }
        assert_eq!(
            detect_grid(&image),
            vec![Rect(100, 0, 200, 200), Rect(0, 0, 100, 200)]
        );
    }
}

#[cfg(test)]
mod boundary_tests {
    use super::*;
    #[test]
    fn metadata_utf8_length_limit_preserves_complete_characters() {
        let dir = tempfile::tempdir().unwrap();
        let exact = "猫".repeat(21845);
        write_meta(dir.path(), &exact, "", "").unwrap();
        let bytes = fs::read(dir.path().join("meta.bin")).unwrap();
        assert_eq!(&bytes[4..6], &u16::MAX.to_le_bytes());
        assert_eq!(std::str::from_utf8(&bytes[8..]).unwrap(), exact);
        assert!(write_meta(dir.path(), &(exact + "猫"), "", "").is_err());
    }
}

#[cfg(test)]
mod jpeg_tests {
    use super::*;
    #[test]
    fn jpeg_copy_check_ignores_marker_like_metadata_and_rejects_progressive_frame() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("page.jpg");
        fs::write(
            &path,
            [0xff, 0xd8, 0xff, 0xe1, 0, 4, 0xff, 0xc0, 0xff, 0xc2],
        )
        .unwrap();
        assert!(!baseline_jpeg(&path).unwrap());
        fs::write(
            &path,
            [0xff, 0xd8, 0xff, 0xe1, 0, 4, 0xff, 0xc2, 0xff, 0xc0],
        )
        .unwrap();
        assert!(baseline_jpeg(&path).unwrap());
    }
}
