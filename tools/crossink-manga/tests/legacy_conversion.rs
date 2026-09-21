#![allow(clippy::unwrap_used)]
use std::process::Command;

#[test]
fn legacy_cli_writes_device_compatible_index_without_python() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("source");
    std::fs::create_dir(&source).unwrap();
    image::GrayImage::from_pixel(40, 60, image::Luma([128]))
        .save(source.join("01.png"))
        .unwrap();
    let output = dir.path().join("book");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga"))
        .arg("legacy-convert")
        .arg("--input")
        .arg(&source)
        .arg("--output-dir")
        .arg(&output)
        .args([
            "--no-ocr",
            "--title",
            "猫",
            "--author",
            "A",
            "--language",
            "JP",
        ])
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(
        std::fs::read(output.join("panels.idx")).unwrap(),
        [
            2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 14, 0, 0, 0, 40, 0, 60, 0
        ]
    );
    assert_eq!(
        std::fs::read(output.join("panels.dat")).unwrap(),
        [1, 0, 0, 0, 0, 0, 40, 0, 60, 0, 0, 0, 0, 0]
    );
    assert_eq!(
        std::fs::read(output.join("meta.bin")).unwrap(),
        [
            1, 0, 0, 0, 3, 0, 1, 0, 0xe7, 0x8c, 0xab, b'A', 2, 0, b'j', b'a'
        ]
    );
    assert!(output.join("page_0000.png").is_file());
}

#[test]
fn legacy_mono_preserves_large_page_dimensions_and_writes_one_bit_bmp() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("source");
    std::fs::create_dir(&source).unwrap();
    image::GrayImage::from_pixel(800, 1200, image::Luma([128]))
        .save(source.join("page.png"))
        .unwrap();
    let output = dir.path().join("book");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga"))
        .args(["legacy-convert", "--input"])
        .arg(&source)
        .arg("--output-dir")
        .arg(&output)
        .args(["--no-ocr", "--mono"])
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let bytes = std::fs::read(output.join("page_0000.bmp")).unwrap();
    assert_eq!(&bytes[28..30], &1u16.to_le_bytes());
    assert_eq!(
        image::open(output.join("page_0000.bmp")).unwrap().height(),
        1200
    );
}

#[test]
fn legacy_transparent_pixels_are_white() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("source");
    std::fs::create_dir(&source).unwrap();
    image::RgbaImage::from_pixel(20, 30, image::Rgba([0, 0, 0, 0]))
        .save(source.join("page.png"))
        .unwrap();
    let output = dir.path().join("book");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga"))
        .args(["legacy-convert", "--input"])
        .arg(&source)
        .arg("--output-dir")
        .arg(&output)
        .arg("--no-ocr")
        .output()
        .unwrap();
    assert!(result.status.success());
    assert_eq!(
        image::open(output.join("page_0000.png"))
            .unwrap()
            .to_rgb8()
            .get_pixel(0, 0)
            .0,
        [255, 255, 255]
    );
}

#[test]
fn legacy_panel_map_overrides_detection_and_scales_source_coordinates() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("source");
    std::fs::create_dir(&source).unwrap();
    image::GrayImage::from_pixel(960, 1600, image::Luma([128]))
        .save(source.join("page.png"))
        .unwrap();
    let map = dir.path().join("panels.json");
    std::fs::write(&map, r#"{"page.png":[[480,0,960,800],[0,0,480,800]]}"#).unwrap();
    let output = dir.path().join("book");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga"))
        .args(["legacy-convert", "--input"])
        .arg(&source)
        .arg("--output-dir")
        .arg(&output)
        .args(["--no-ocr", "--x4", "--panel-map"])
        .arg(&map)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let bytes = std::fs::read(output.join("panels.dat")).unwrap();
    assert_eq!(&bytes[..10], &[2, 0, 240, 0, 0, 0, 240, 0, 144, 1]);
    assert_eq!(
        image::open(output.join("panels/p0_0.jpg")).unwrap().width(),
        480
    );
}

#[test]
fn legacy_epub_chapters_follow_reordered_truncated_image_pages() {
    use std::io::Write;
    let dir = tempfile::tempdir().unwrap();
    let book = dir.path().join("book.epub");
    let mut archive = zip::ZipWriter::new(std::fs::File::create(&book).unwrap());
    let mut image_bytes = std::io::Cursor::new(Vec::new());
    image::DynamicImage::ImageLuma8(image::GrayImage::from_pixel(20, 30, image::Luma([0])))
        .write_to(&mut image_bytes, image::ImageFormat::Png)
        .unwrap();
    for (name,data) in [
        ("META-INF/container.xml",br#"<container><rootfiles><rootfile full-path="book.opf"/></rootfiles></container>"#.as_slice()),
        ("book.opf",br#"<package><metadata><title>Book</title></metadata><manifest><item id="text" href="text.xhtml"/><item id="one" href="one.png"/><item id="two" href="two.png"/><item id="nav" href="nav.xhtml" properties="nav"/></manifest><spine><itemref idref="text"/><itemref idref="one"/><itemref idref="two"/></spine></package>"#),
        ("text.xhtml",b"<html><p>text only</p></html>"),
        ("nav.xhtml",br#"<html xmlns:epub="http://www.idpf.org/2007/ops"><nav epub:type="toc"><ol><li><a href="one.png">One</a></li><li><a href="two.png">Two</a></li></ol></nav></html>"#),
        ("one.png",image_bytes.get_ref()),("two.png",image_bytes.get_ref()),
    ] {archive.start_file(name,zip::write::SimpleFileOptions::default()).unwrap();archive.write_all(data).unwrap();}
    archive.finish().unwrap();
    let order = dir.path().join("order.txt");
    std::fs::write(&order, "spine_00002.png\nspine_00001.png\n").unwrap();
    let output = dir.path().join("book");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga"))
        .args(["legacy-convert", "--input"])
        .arg(&book)
        .arg("--output-dir")
        .arg(&output)
        .args(["--no-ocr", "--max-pages", "1", "--page-order-file"])
        .arg(&order)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(
        std::fs::read(output.join("toc.idx")).unwrap(),
        [1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, b'T', b'w', b'o']
    );
}

#[test]
fn legacy_keeps_unresized_baseline_jpeg_bytes() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("source");
    std::fs::create_dir(&source).unwrap();
    let path = source.join("page.jpg");
    let image = image::RgbImage::from_fn(40, 60, |x, y| {
        image::Rgb([(x * 6) as u8, (y * 4) as u8, 123])
    });
    image::codecs::jpeg::JpegEncoder::new_with_quality(std::fs::File::create(&path).unwrap(), 99)
        .encode_image(&image)
        .unwrap();
    let output = dir.path().join("book");
    let result = Command::new(env!("CARGO_BIN_EXE_crossink-manga"))
        .args(["legacy-convert", "--input"])
        .arg(&source)
        .arg("--output-dir")
        .arg(&output)
        .args(["--no-ocr", "--x4"])
        .output()
        .unwrap();
    assert!(result.status.success());
    assert_eq!(
        std::fs::read(path).unwrap(),
        std::fs::read(output.join("page_0000.jpg")).unwrap()
    );
}
