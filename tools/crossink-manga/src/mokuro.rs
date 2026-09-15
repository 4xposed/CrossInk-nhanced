use anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};

#[derive(Debug, Deserialize, Serialize)]
pub struct Volume {
    pub version: String,
    pub pages: Vec<Page>,
}
#[derive(Debug, Deserialize, Serialize)]
pub struct Page {
    pub img_path: String,
    pub img_width: u32,
    pub img_height: u32,
    pub blocks: Vec<Block>,
}
#[derive(Debug, Deserialize, Serialize)]
pub struct Block {
    #[serde(rename = "box")]
    pub bounds: [f64; 4],
    pub vertical: bool,
    pub font_size: f64,
    pub lines: Vec<String>,
    pub lines_coords: Vec<[[f64; 2]; 4]>,
}
#[derive(Debug, PartialEq)]
pub struct TextBlock {
    pub bounds: [u16; 4],
    pub vertical: bool,
    pub text: String,
}
/// Intersect finite, positive-area OCR boxes with the visible source image.
/// Upstream can emit edge-crossing boxes; preserve the master and reject invisible boxes.
pub fn visible_bounds(bounds: [f64; 4], width: u32, height: u32) -> Result<[f64; 4]> {
    let [l, t, r, b] = bounds;
    ensure!(
        bounds.iter().all(|v| v.is_finite()) && r > l && b > t,
        "invalid OCR rectangle"
    );
    let clipped = [
        l.max(0.),
        t.max(0.),
        r.min(f64::from(width)),
        b.min(f64::from(height)),
    ];
    ensure!(
        clipped[2] > clipped[0] && clipped[3] > clipped[1],
        "OCR rectangle outside image"
    );
    Ok(clipped)
}

pub fn transform(page: &Page, width: u32, height: u32) -> Result<Vec<TextBlock>> {
    ensure!(
        page.img_width > 0
            && page.img_height > 0
            && width > 0
            && height > 0
            && ((width <= 528 && height <= 800) || (width <= 800 && height <= 528)),
        "invalid panel dimensions"
    );
    ensure!(
        page.blocks.len() <= 255,
        "more than 255 OCR blocks in {}",
        page.img_path
    );
    let sx = f64::from(width) / f64::from(page.img_width);
    let sy = f64::from(height) / f64::from(page.img_height);
    let mut out = Vec::with_capacity(page.blocks.len());
    for block in &page.blocks {
        let [x, y, r, b] = visible_bounds(block.bounds, page.img_width, page.img_height)?;
        ensure!(
            block.font_size.is_finite() && block.font_size > 0.,
            "invalid OCR font size"
        );
        ensure!(
            block.lines.len() == block.lines_coords.len(),
            "OCR lines and polygons disagree"
        );
        for polygon in &block.lines_coords {
            ensure!(
                polygon
                    .iter()
                    .all(|[px, py]| px.is_finite() && py.is_finite()),
                "invalid OCR line polygon"
            );
        }
        let text = block.lines.join("\n");
        ensure!(!text.contains('\0'), "OCR text contains NUL");
        let left = (x * sx).floor() as u16;
        let top = (y * sy).floor() as u16;
        let right = (r * sx).ceil().min(f64::from(width)) as u16;
        let bottom = (b * sy).ceil().min(f64::from(height)) as u16;
        out.push(TextBlock {
            bounds: [left, top, right - left, bottom - top],
            vertical: block.vertical,
            text,
        });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn page() -> Page {
        Page {
            img_path: "panel_0000.png".into(),
            img_width: 1600,
            img_height: 2400,
            blocks: vec![Block {
                bounds: [100., 200., 300., 600.],
                vertical: true,
                font_size: 40.,
                lines: vec!["こんにちは".into()],
                lines_coords: vec![[[100., 200.], [300., 200.], [300., 600.], [100., 600.]]],
            }],
        }
    }
    #[test]
    fn maps_original_crop_rectangles_to_device_pixels() {
        assert_eq!(
            transform(&page(), 480, 720).unwrap(),
            vec![TextBlock {
                bounds: [30, 60, 60, 120],
                vertical: true,
                text: "こんにちは".into()
            }]
        );
    }
    #[test]
    fn clips_partially_visible_rectangles_without_changing_source() {
        let mut p = page();
        p.blocks[0].bounds[2] = 2000.;
        assert_eq!(
            transform(&p, 480, 720).unwrap()[0].bounds,
            [30, 60, 450, 120]
        );
        assert_eq!(p.blocks[0].bounds[2], 2000.);
    }
    #[test]
    fn rejects_fully_outside_rectangles() {
        let mut p = page();
        p.blocks[0].bounds = [-20., 0., -1., 30.];
        assert!(transform(&p, 480, 720).is_err());
    }
    #[test]
    fn accepts_finite_line_polygons_crossing_image_edge() {
        let mut p = page();
        p.blocks[0].lines_coords[0][0][0] = -4.;
        assert!(transform(&p, 480, 720).is_ok());
    }
    #[test]
    fn rejects_nul_text_in_device_records() {
        let mut p = page();
        p.blocks[0].lines = vec!["a\0b".into()];
        assert!(transform(&p, 480, 720).is_err());
    }
    #[test]
    fn rejects_nonfinite_geometry() {
        let mut p = page();
        p.blocks[0].bounds[0] = f64::NAN;
        assert!(transform(&p, 480, 720).is_err());
    }
}
