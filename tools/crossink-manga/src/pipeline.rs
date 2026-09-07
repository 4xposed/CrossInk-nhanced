use crate::{
    format, input, mokuro,
    panels::{self, Rect},
    runtime,
};
use anyhow::{Context, Result, ensure};
use image::{DynamicImage, GenericImageView};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};

#[derive(Default)]
pub struct Options {
    pub backend: crate::native::Backend,
    pub models: Option<PathBuf>,
    pub device: crate::device::Device,
    pub mokuro: Option<PathBuf>,
    pub panel_map: Option<PathBuf>,
    pub uv: Option<PathBuf>,
    pub work: Option<PathBuf>,
    pub title: Option<String>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Panel {
    pub source: String,
    pub source_sha256: String,
    pub source_width: u32,
    pub source_height: u32,
    pub original_page: u32,
    pub panel: u16,
    pub crop: Rect,
    pub crop_sha256: String,
    pub image: String,
    #[serde(default)]
    pub output_width: u16,
    #[serde(default)]
    pub output_height: u16,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Manifest {
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub ocr: BTreeMap<String, String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub device: Option<crate::device::Device>,
    pub version: u32,
    pub fingerprint: String,
    #[serde(default)]
    pub preparation: BTreeMap<String, String>,
    pub panels: Vec<Panel>,
}

fn hash_file(path: &Path) -> Result<String> {
    let mut file = fs::File::open(path)?;
    let mut hash = Sha256::new();
    let mut buf = [0u8; 65536];
    loop {
        let size = file.read(&mut buf)?;
        if size == 0 {
            break;
        }
        hash.update(&buf[..size]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

fn load_image(path: &Path) -> Result<DynamicImage> {
    let mut reader = image::ImageReader::open(path)?.with_guessed_format()?;
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(16384);
    limits.max_image_height = Some(16384);
    limits.max_alloc = Some(256 * 1024 * 1024);
    reader.limits(limits);
    let source = reader
        .decode()
        .with_context(|| format!("decode {}", path.display()))?;
    ensure!(source.width() > 0 && source.height() > 0, "empty image");
    // White compositing avoids turning transparent page margins black.
    let mut rgba = source.to_rgba8();
    for pixel in rgba.pixels_mut() {
        let a = u16::from(pixel[3]);
        for c in 0..3 {
            pixel[c] = ((u16::from(pixel[c]) * a + 255 * (255 - a) + 127) / 255) as u8;
        }
        pixel[3] = 255;
    }
    Ok(DynamicImage::ImageRgba8(rgba))
}

fn paths(output: &Path, options: &Options) -> Result<(PathBuf, PathBuf)> {
    let parent = output
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(parent)?;
    let output = parent
        .canonicalize()?
        .join(output.file_name().context("output needs a folder name")?);
    let work = options.work.clone().unwrap_or_else(|| {
        let mut name = output.as_os_str().to_os_string();
        name.push(".work");
        PathBuf::from(name)
    });
    let wp = work
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(wp)?;
    let work = wp
        .canonicalize()?
        .join(work.file_name().context("work needs a folder name")?);
    ensure!(
        output != work && !output.starts_with(&work) && !work.starts_with(&output),
        "output and work directories must be separate"
    );
    Ok((output, work))
}

/// Prepare persistent lossless crops; stages are reused only for matching inputs.
pub fn prepare(input_path: &Path, output: &Path, options: &Options) -> Result<PathBuf> {
    let (output, work) = paths(output, options)?;
    let input_path = input_path.canonicalize().context("input does not exist")?;
    if input_path.is_dir() {
        ensure!(
            !output.starts_with(&input_path) && !work.starts_with(&input_path),
            "output/work must be outside the source directory"
        );
    }
    let staging = tempfile::tempdir_in(work.parent().context("work parent missing")?)?;
    let source = staging.path().join("source");
    fs::create_dir(&source)?;
    let files = input::collect(&input_path, &source)?;
    ensure!(!files.is_empty(), "no supported manga images found");
    let manual: BTreeMap<String, Vec<Rect>> = if let Some(path) = &options.panel_map {
        serde_json::from_slice(&fs::read(path)?).context("panel map must map relative source paths to ordered [left,top,right,bottom] rectangles")?
    } else {
        BTreeMap::new()
    };
    let mut hash = Sha256::new();
    hash.update(b"crossink-manga-prepare-v1");
    hash.update(serde_json::to_vec(&manual)?);
    let mut sources = Vec::with_capacity(files.len());
    for path in &files {
        let relative = path
            .strip_prefix(&source)?
            .to_str()
            .context("source path is not UTF-8")?
            .replace('\\', "/");
        let digest = hash_file(path)?;
        hash.update((relative.len() as u64).to_le_bytes());
        hash.update(relative.as_bytes());
        hash.update(digest.as_bytes());
        sources.push((relative, digest));
    }
    for name in manual.keys() {
        ensure!(
            sources.iter().any(|(s, _)| s == name),
            "panel map names missing source {name}"
        );
    }
    let fingerprint = format!("{:x}", hash.finalize());
    if work.exists() {
        ensure!(
            !fs::symlink_metadata(&work)?.file_type().is_symlink(),
            "work directory cannot be a symlink"
        );
        let previous: Manifest =
            serde_json::from_slice(&fs::read(work.join("prepared.json")).context(
                "work directory is not a completed preparation; choose another --work path",
            )?)?;
        ensure!(
            previous.version == 1 && previous.fingerprint == fingerprint,
            "work inputs/settings changed; choose a new --work directory"
        );
        ensure!(
            !previous.panels.is_empty() && previous.panels.len() <= 10000,
            "invalid prepared panel count"
        );
        for (index, panel) in previous.panels.iter().enumerate() {
            ensure!(
                panel.image == format!("panel_{index:04}.png"),
                "invalid cached crop name"
            );
            ensure!(
                hash_file(&work.join("crops").join(&panel.image))? == panel.crop_sha256,
                "cached crop changed; choose a new --work directory"
            );
        }
        eprintln!(
            "Reusing {} full-resolution panel crops",
            previous.panels.len()
        );
        return Ok(work);
    }
    let crops = staging.path().join("crops");
    fs::create_dir(&crops)?;
    let previews = staging.path().join("previews");
    fs::create_dir(&previews)?;
    let mut manifest = Manifest {
        ocr: BTreeMap::new(),
        device: None,
        version: 1,
        fingerprint,
        preparation: BTreeMap::from([
            (
                String::from("tool_version"),
                env!("CARGO_PKG_VERSION").into(),
            ),
            (
                String::from("detector"),
                String::from("gutter-v1 with optional explicit rectangles"),
            ),
            (
                String::from("reading_order"),
                String::from("top-to-bottom, right-to-left; explicit map order wins"),
            ),
            (
                String::from("ocr_images"),
                String::from("lossless original-resolution crops"),
            ),
        ]),
        panels: Vec::new(),
    };
    let mut panel_map = BTreeMap::new();
    for (page_index, (path, (relative, digest))) in files.iter().zip(&sources).enumerate() {
        eprintln!(
            "Preparing page {}/{}: {relative}",
            page_index + 1,
            files.len()
        );
        let image = load_image(path)?;
        let rects = manual
            .get(relative)
            .cloned()
            .unwrap_or_else(|| panels::detect(&image.to_luma8()));
        ensure!(
            !rects.is_empty() && rects.len() <= u16::MAX as usize,
            "invalid panel count for {relative}"
        );
        if !manual.contains_key(relative) && rects.len() == 1 {
            eprintln!(
                "  Gutter detector kept a full-page view; review the preview for irregular layouts."
            );
        }
        ensure!(
            manifest.panels.len() + rects.len() <= 10000,
            "book exceeds 10000 reading units"
        );
        for (panel_index, &Rect(l, t, r, b)) in rects.iter().enumerate() {
            ensure!(
                l < r && t < b && r <= image.width() && b <= image.height(),
                "invalid panel rectangle in {relative}"
            );
            let image_name = format!("panel_{:04}.png", manifest.panels.len());
            let destination = crops.join(&image_name);
            image
                .crop_imm(l, t, r - l, b - t)
                .to_rgb8()
                .save(&destination)?;
            manifest.panels.push(Panel {
                source: relative.clone(),
                source_sha256: digest.clone(),
                source_width: image.width(),
                source_height: image.height(),
                original_page: page_index as u32,
                panel: panel_index as u16,
                crop: Rect(l, t, r, b),
                crop_sha256: hash_file(&destination)?,
                image: image_name,
                output_width: 0,
                output_height: 0,
            });
        }
        write_preview(
            &image,
            &rects,
            &previews.join(format!("page_{page_index:04}.png")),
        )?;
        panel_map.insert(relative.clone(), rects);
    }
    fs::write(
        staging.path().join("panels.json"),
        serde_json::to_vec_pretty(&panel_map)?,
    )?;
    fs::write(
        staging.path().join("prepared.json"),
        serde_json::to_vec_pretty(&manifest)?,
    )?;
    // Sources can be recovered from the immutable input; keep only OCR masters/previews.
    fs::remove_dir_all(&source)?;
    ensure!(!work.exists(), "work directory appeared during preparation");
    fs::rename(staging.path(), &work)?;
    Ok(work)
}

fn write_preview(image: &DynamicImage, rects: &[Rect], path: &Path) -> Result<()> {
    let scale = (1000.0 / f64::from(image.width()))
        .min(1000.0 / f64::from(image.height()))
        .min(1.0);
    let mut preview = image
        .resize(
            (f64::from(image.width()) * scale).max(1.) as u32,
            (f64::from(image.height()) * scale).max(1.) as u32,
            image::imageops::FilterType::Triangle,
        )
        .to_rgb8();
    const DIGITS: [u16; 10] = [
        0b111_101_101_101_111,
        0b010_110_010_010_111,
        0b111_001_111_100_111,
        0b111_001_111_001_111,
        0b101_101_111_001_001,
        0b111_100_111_001_111,
        0b111_100_111_101_111,
        0b111_001_001_001_001,
        0b111_101_111_101_111,
        0b111_101_111_001_111,
    ];
    let (w, h) = preview.dimensions();
    let sx = f64::from(w) / f64::from(image.width());
    let sy = f64::from(h) / f64::from(image.height());
    for (i, &Rect(l, t, r, b)) in rects.iter().enumerate() {
        let (l, t, r, b) = (
            ((f64::from(l) * sx) as u32).min(w - 1),
            ((f64::from(t) * sy) as u32).min(h - 1),
            ((f64::from(r) * sx) as u32).min(w - 1),
            ((f64::from(b) * sy) as u32).min(h - 1),
        );
        let red = image::Rgb([220, 0, 0]);
        for x in l..=r {
            preview.put_pixel(x, t, red);
            preview.put_pixel(x, b, red);
        }
        for y in t..=b {
            preview.put_pixel(l, y, red);
            preview.put_pixel(r, y, red);
        }
        for (digit, ch) in (i + 1).to_string().bytes().enumerate() {
            let mask = DIGITS[(ch - b'0') as usize];
            for row in 0..5 {
                for col in 0..3 {
                    for dy in 0..3 {
                        for dx in 0..3 {
                            let x = l + 4 + digit as u32 * 12 + col * 3 + dx;
                            let y = t + 4 + row * 3 + dy;
                            if x < w && y < h {
                                preview.put_pixel(
                                    x,
                                    y,
                                    if mask & (1 << (14 - row * 3 - col)) != 0 {
                                        red
                                    } else {
                                        image::Rgb([255, 255, 255])
                                    },
                                );
                            }
                        }
                    }
                }
            }
        }
    }
    preview.save(path)?;
    Ok(())
}

pub fn convert(input_path: &Path, output: &Path, options: &Options) -> Result<()> {
    let (output, _) = paths(output, options)?;
    ensure!(
        !output.exists(),
        "output already exists; choose a new output folder"
    );
    let work = prepare(input_path, &output, options)?;
    let mut manifest: Manifest = serde_json::from_slice(&fs::read(work.join("prepared.json"))?)?;
    let ocr_path = if let Some(path) = &options.mokuro {
        path.clone()
    } else {
        match options.backend {
            crate::native::Backend::Native => {
                crate::native::run(&work.join("crops"), options.models.as_deref())?
            }
            crate::native::Backend::Upstream => {
                runtime::run_mokuro(&work.join("crops"), options.uv.as_deref())?
            }
        }
    };
    ensure!(
        fs::metadata(&ocr_path)?.len() <= 64 * 1024 * 1024,
        "Mokuro JSON exceeds 64 MiB limit"
    );
    let volume: mokuro::Volume =
        serde_json::from_slice(&fs::read(&ocr_path)?).context("invalid Mokuro JSON")?;
    manifest.ocr.insert("sha256".into(), hash_file(&ocr_path)?);
    manifest.ocr.insert(
        "backend".into(),
        if options.mokuro.is_some() {
            "imported"
        } else {
            match options.backend {
                crate::native::Backend::Native => "native",
                crate::native::Backend::Upstream => "upstream",
            }
        }
        .into(),
    );
    ensure!(
        volume.version == "0.2.5",
        "unsupported Mokuro version {}; expected 0.2.5",
        volume.version
    );
    ensure!(
        volume.pages.len() == manifest.panels.len(),
        "OCR is incomplete: {} pages for {} panels",
        volume.pages.len(),
        manifest.panels.len()
    );
    let mut pages = BTreeMap::new();
    for page in &volume.pages {
        ensure!(
            pages.insert(page.img_path.as_str(), page).is_none(),
            "duplicate OCR image {}",
            page.img_path
        );
    }
    let staged = tempfile::tempdir_in(output.parent().context("output parent missing")?)?;
    let mut index = fs::File::create(staged.path().join("book.mki"))?;
    let mut data = fs::File::create(staged.path().join("book.mkd"))?;
    index.write_all(b"CMI1")?;
    index.write_all(&2u32.to_le_bytes())?;
    index.write_all(&(manifest.panels.len() as u32).to_le_bytes())?;
    let mut offset = 0u32;
    manifest.device = Some(options.device);
    let total_panels = manifest.panels.len();
    for (ordinal, panel) in manifest.panels.iter_mut().enumerate() {
        eprintln!("Exporting panel {}/{}", ordinal + 1, total_panels);
        let page = pages
            .get(panel.image.as_str())
            .with_context(|| format!("missing OCR for {}", panel.image))?;
        let image = load_image(&work.join("crops").join(&panel.image))?;
        ensure!(
            image.dimensions() == (page.img_width, page.img_height),
            "OCR dimensions do not match {}",
            panel.image
        );
        let (width, height) = panels::fitted(image.width(), image.height(), options.device);
        panel.output_width = width as u16;
        panel.output_height = height as u16;
        let blocks = mokuro::transform(page, width, height)
            .with_context(|| format!("OCR for {}", panel.image))?;
        let bytes = format::encode_ocr(&blocks)?;
        for value in [offset, bytes.len() as u32] {
            index.write_all(&value.to_le_bytes())?;
        }
        index.write_all(&(width as u16).to_le_bytes())?;
        index.write_all(&(height as u16).to_le_bytes())?;
        index.write_all(&panel.original_page.to_le_bytes())?;
        index.write_all(&panel.panel.to_le_bytes())?;
        index.write_all(&[0, 0])?;
        data.write_all(&bytes)?;
        offset = offset
            .checked_add(bytes.len() as u32)
            .context("OCR data exceeds 4 GiB")?;
        let gray = image
            .resize_exact(width, height, image::imageops::FilterType::Lanczos3)
            .to_luma8();
        format::write_bmp(&staged.path().join(format!("page_{ordinal:04}.bmp")), &gray)?;
    }
    index.sync_all()?;
    data.sync_all()?;
    drop(index);
    drop(data);
    let title = options
        .title
        .as_deref()
        .or_else(|| input_path.file_stem().and_then(|s| s.to_str()))
        .unwrap_or("Manga");
    ensure!(
        title.len() <= 1024 && !title.contains('\0'),
        "title exceeds metadata limit or contains NUL"
    );
    let mut meta = fs::File::create(staged.path().join("meta.bin"))?;
    meta.write_all(&1u32.to_le_bytes())?;
    meta.write_all(&(title.len() as u16).to_le_bytes())?;
    meta.write_all(&[0, 0])?;
    meta.write_all(title.as_bytes())?;
    meta.write_all(&[2, 0, b'j', b'a'])?;
    meta.sync_all()?;
    drop(meta);
    fs::write(
        staged.path().join("manifest.json"),
        serde_json::to_vec_pretty(&manifest)?,
    )?;
    crate::validate::validate(staged.path())?;
    ensure!(!output.exists(), "output appeared during conversion");
    fs::rename(staged.path(), &output)?;
    eprintln!(
        "Ready: {} ({} panel views)",
        output.display(),
        manifest.panels.len()
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{GrayImage, Luma};
    use serde_json::json;
    fn source(root: &Path) -> (PathBuf, PathBuf) {
        let input = root.join("source");
        std::fs::create_dir(&input).unwrap();
        GrayImage::from_pixel(1200, 1800, Luma([0]))
            .save(input.join("001.png"))
            .unwrap();
        let ocr = root.join("provided.mokuro");
        std::fs::write(&ocr,serde_json::to_vec(&json!({"version":"0.2.5","pages":[{"img_path":"panel_0000.png","img_width":1200,"img_height":1800,"blocks":[{"box":[100,200,300,600],"font_size":40,"vertical":true,"lines":["猫"],"lines_coords":[[[100,200],[300,200],[300,600],[100,600]]]}]}]})).unwrap()).unwrap();
        (input, ocr)
    }
    #[test]
    fn preview_handles_a_crop_at_the_last_source_row() {
        let dir = tempfile::tempdir().unwrap();
        let image = DynamicImage::ImageLuma8(GrayImage::from_pixel(1200, 1800, Luma([255])));
        write_preview(
            &image,
            &[Rect(0, 1799, 1200, 1800)],
            &dir.path().join("preview.png"),
        )
        .unwrap();
        assert!(dir.path().join("preview.png").is_file());
    }

    #[test]
    fn exports_original_resolution_ocr_into_x4_book() {
        let dir = tempfile::tempdir().unwrap();
        let (input, ocr) = source(dir.path());
        let out = dir.path().join("book");
        convert(
            &input,
            &out,
            &Options {
                mokuro: Some(ocr),
                ..Default::default()
            },
        )
        .unwrap();
        let idx = std::fs::read(out.join("book.mki")).unwrap();
        assert_eq!(&idx[..12], b"CMI1\x02\0\0\0\x01\0\0\0");
        assert_eq!(
            image::image_dimensions(out.join("page_0000.bmp")).unwrap(),
            (480, 720)
        );
        assert_eq!(
            image::image_dimensions(dir.path().join("book.work/crops/panel_0000.png")).unwrap(),
            (1200, 1800)
        );
        let data = std::fs::read(out.join("book.mkd")).unwrap();
        assert_eq!(&data[4..12], &[40, 0, 80, 0, 80, 0, 160, 0]);
    }
    #[test]
    fn exports_all_profiles_from_shared_original_resolution_work() {
        use crate::device::Device;
        let dir = tempfile::tempdir().unwrap();
        let (input, ocr) = source(dir.path());
        let work = dir.path().join("shared.work");
        for (device, dimensions, box_bytes) in [
            (Device::X3, (528, 792), vec![44, 0, 88, 0, 88, 0, 176, 0]),
            (Device::X4, (480, 720), vec![40, 0, 80, 0, 80, 0, 160, 0]),
            (Device::X4Pro, (480, 720), vec![40, 0, 80, 0, 80, 0, 160, 0]),
        ] {
            let out = dir.path().join(device.name());
            convert(
                &input,
                &out,
                &Options {
                    device,
                    mokuro: Some(ocr.clone()),
                    work: Some(work.clone()),
                    ..Default::default()
                },
            )
            .unwrap();
            assert_eq!(
                image::image_dimensions(out.join("page_0000.bmp")).unwrap(),
                dimensions
            );
            assert_eq!(
                &std::fs::read(out.join("book.mkd")).unwrap()[4..12],
                box_bytes
            );
            let manifest: serde_json::Value =
                serde_json::from_slice(&std::fs::read(out.join("manifest.json")).unwrap()).unwrap();
            assert_eq!(manifest["device"], device.name());
            crate::validate::validate(&out).unwrap();
        }
        assert_eq!(
            image::image_dimensions(work.join("crops/panel_0000.png")).unwrap(),
            (1200, 1800)
        );
    }

    #[test]
    fn does_not_publish_incomplete_ocr() {
        let dir = tempfile::tempdir().unwrap();
        let (input, ocr) = source(dir.path());
        let out = dir.path().join("book");
        std::fs::write(&ocr, br#"{"version":"0.2.5","pages":[]}"#).unwrap();
        assert!(
            convert(
                &input,
                &out,
                &Options {
                    mokuro: Some(ocr),
                    ..Default::default()
                }
            )
            .is_err()
        );
        assert!(!out.exists());
    }
    #[test]
    fn refuses_existing_output_without_modifying_it() {
        let dir = tempfile::tempdir().unwrap();
        let (input, ocr) = source(dir.path());
        let out = dir.path().join("book");
        std::fs::create_dir(&out).unwrap();
        std::fs::write(out.join("keep"), b"mine").unwrap();
        assert!(
            convert(
                &input,
                &out,
                &Options {
                    mokuro: Some(ocr),
                    ..Default::default()
                }
            )
            .is_err()
        );
        assert_eq!(std::fs::read(out.join("keep")).unwrap(), b"mine");
    }
}
