use image::GrayImage;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Rect(pub u32, pub u32, pub u32, pub u32);

/// Split continuous white gutters; free-form layouts remain full-page fallbacks.
pub fn detect(image: &GrayImage) -> Vec<Rect> {
    let (w, h) = image.dimensions();
    if w == 0 || h == 0 {
        return Vec::new();
    }
    let rows = bands(h, |y| (0..w).all(|x| image.get_pixel(x, y).0[0] >= 245));
    let mut panels = Vec::new();
    for range in rows.windows(2) {
        let (top, bottom) = (range[0], range[1]);
        let columns = bands(w, |x| {
            (top..bottom).all(|y| image.get_pixel(x, y).0[0] >= 245)
        });
        for col in columns.windows(2).rev() {
            panels.push(Rect(col[0], top, col[1], bottom));
        }
    }
    panels
}

fn bands(size: u32, white: impl Fn(u32) -> bool) -> Vec<u32> {
    let mut cuts = vec![0];
    let min_panel = (size / 10).max(16);
    let min_gutter = (size / 200).max(3);
    let mut i = 0;
    while i < size {
        if !white(i) {
            i += 1;
            continue;
        }
        let start = i;
        while i < size && white(i) {
            i += 1;
        }
        let center = start + (i - start) / 2;
        let last = cuts.last().copied().unwrap_or(0);
        if i - start >= min_gutter
            && start > 0
            && i < size
            && center >= last + min_panel
            && size - center >= min_panel
        {
            cuts.push(center);
        }
    }
    cuts.push(size);
    cuts
}

pub fn fitted(width: u32, height: u32, device: crate::device::Device) -> (u32, u32) {
    if width == 0 || height == 0 {
        return (0, 0);
    }
    let (short_edge, long_edge) = device.dimensions();
    let (max_width, max_height) = if width > height {
        (long_edge, short_edge)
    } else {
        (short_edge, long_edge)
    };
    let scale = (f64::from(max_width) / f64::from(width))
        .min(f64::from(max_height) / f64::from(height))
        .min(1.0);
    (
        (f64::from(width) * scale)
            .round()
            .clamp(1.0, f64::from(max_width)) as u32,
        (f64::from(height) * scale)
            .round()
            .clamp(1.0, f64::from(max_height)) as u32,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn separates_white_gutters_in_right_to_left_order() {
        let mut image = GrayImage::from_pixel(200, 200, image::Luma([255]));
        for y in 5..95 {
            for x in 5..95 {
                image.put_pixel(x, y, image::Luma([0]));
            }
            for x in 105..195 {
                image.put_pixel(x, y, image::Luma([0]));
            }
        }
        for y in 105..195 {
            for x in 5..195 {
                image.put_pixel(x, y, image::Luma([0]));
            }
        }
        assert_eq!(
            detect(&image),
            vec![
                Rect(100, 0, 200, 100),
                Rect(0, 0, 100, 100),
                Rect(0, 100, 200, 200)
            ]
        );
    }
    #[test]
    fn borderless_page_is_retained() {
        let image = GrayImage::from_pixel(80, 120, image::Luma([30]));
        assert_eq!(detect(&image), vec![Rect(0, 0, 80, 120)]);
    }
    #[test]
    fn portrait_fits_without_stretching() {
        assert_eq!(fitted(1200, 1800, crate::device::Device::X4), (480, 720));
    }
    #[test]
    fn square_fits_without_stretching() {
        assert_eq!(fitted(1000, 1000, crate::device::Device::X4), (480, 480));
    }
    #[test]
    fn landscape_uses_long_screen_edge_for_all_profiles() {
        for (device, expected) in [
            (crate::device::Device::X4, (800, 400)),
            (crate::device::Device::X4Pro, (800, 400)),
            (crate::device::Device::X3, (792, 396)),
        ] {
            assert_eq!(fitted(1600, 800, device), expected);
        }
    }
    #[test]
    fn small_image_is_not_enlarged() {
        assert_eq!(fitted(120, 180, crate::device::Device::X4), (120, 180));
    }
}
