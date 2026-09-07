use image::RgbImage;
use ndarray::{Array2, ArrayView2};

/// Rectify a quadrilateral using a projective mapping and OpenCV 5's floating-point
/// linear interpolation. Samples outside the source are black.
pub(crate) fn warp(image: &RgbImage, coords: [[f64; 2]; 4], width: u32, height: u32) -> RgbImage {
    let mapping = ProjectiveMap::new(coords);
    RgbImage::from_fn(width, height, |x, y| {
        let [sx, sy] = mapping.point(x, y, width, height);
        let ix = sx.floor() as i64;
        let iy = sy.floor() as i64;
        let fx = sx - ix as f64;
        let fy = sy - iy as f64;
        let mut sum = [0f64; 3];
        for (px, py, weight) in [
            (ix, iy, (1. - fx) * (1. - fy)),
            (ix + 1, iy, fx * (1. - fy)),
            (ix, iy + 1, (1. - fx) * fy),
            (ix + 1, iy + 1, fx * fy),
        ] {
            if px >= 0 && py >= 0 && px < i64::from(image.width()) && py < i64::from(image.height())
            {
                let pixel = image.get_pixel(px as u32, py as u32);
                for (channel, value) in sum.iter_mut().enumerate() {
                    *value += f64::from(pixel[channel]) * weight;
                }
            }
        }
        image::Rgb(sum.map(|value| value.round_ties_even().clamp(0., 255.) as u8))
    })
}

struct ProjectiveMap {
    a: f64,
    b: f64,
    c: f64,
    d: f64,
    e: f64,
    f: f64,
    g: f64,
    h: f64,
}
impl ProjectiveMap {
    fn new(coords: [[f64; 2]; 4]) -> Self {
        let [[x0, y0], [x1, y1], [x2, y2], [x3, y3]] = coords;
        let dx1 = x1 - x2;
        let dx2 = x3 - x2;
        let dx3 = x0 - x1 + x2 - x3;
        let dy1 = y1 - y2;
        let dy2 = y3 - y2;
        let dy3 = y0 - y1 + y2 - y3;
        let determinant = dx1 * dy2 - dx2 * dy1;
        let (g, h) = if determinant.abs() < 1e-12 {
            (0., 0.)
        } else {
            (
                (dx3 * dy2 - dx2 * dy3) / determinant,
                (dx1 * dy3 - dx3 * dy1) / determinant,
            )
        };
        let a = x1 - x0 + g * x1;
        let b = x3 - x0 + h * x3;
        let d = y1 - y0 + g * y1;
        let e = y3 - y0 + h * y3;
        Self {
            a,
            b,
            c: x0,
            d,
            e,
            f: y0,
            g,
            h,
        }
    }
    fn point(&self, x: u32, y: u32, width: u32, height: u32) -> [f64; 2] {
        let u = f64::from(x) / f64::from(width.saturating_sub(1).max(1));
        let v = f64::from(y) / f64::from(height.saturating_sub(1).max(1));
        let denominator = self.g * u + self.h * v + 1.;
        [
            (self.a * u + self.b * v + self.c) / denominator,
            (self.d * u + self.e * v + self.f) / denominator,
        ]
    }
}

pub(crate) fn warp_mask(
    mask: ArrayView2<'_, f32>,
    coords: [[f64; 2]; 4],
    width: u32,
    height: u32,
) -> Array2<f32> {
    let mapping = ProjectiveMap::new(coords);
    Array2::from_shape_fn((height as usize, width as usize), |(y, x)| {
        let [sx, sy] = mapping.point(x as u32, y as u32, width, height);
        let ix = sx.floor() as i64;
        let iy = sy.floor() as i64;
        let fx = sx - ix as f64;
        let fy = sy - iy as f64;
        let mut sum = 0.;
        for (px, py, weight) in [
            (ix, iy, (1. - fx) * (1. - fy)),
            (ix + 1, iy, fx * (1. - fy)),
            (ix, iy + 1, (1. - fx) * fy),
            (ix + 1, iy + 1, fx * fy),
        ] {
            if px >= 0 && py >= 0 && px < mask.ncols() as i64 && py < mask.nrows() as i64 {
                sum += f64::from(mask[[py as usize, px as usize]]) * weight;
            }
        }
        sum as f32
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn image_and_mask_share_projective_mapping_and_black_border() {
        let source = RgbImage::from_fn(5, 4, |x, y| image::Rgb([((x + y) * 25) as u8; 3]));
        let mask = Array2::from_shape_fn((4, 5), |(y, x)| ((x + y) * 25) as f32);
        let coords = [[-0.4, 0.2], [3.7, -0.3], [4.3, 3.4], [0.2, 2.7]];
        let pixels = warp(&source, coords, 7, 6);
        let density = warp_mask(mask.view(), coords, 7, 6);
        for (pixel, value) in pixels.pixels().zip(density.iter()) {
            assert!((f32::from(pixel[0]) - value).abs() <= 0.501);
        }
    }
    #[test]
    fn matches_opencv_perspective_interpolation_and_black_border() {
        // Independent cv2.findHomography + warpPerspective, OpenCV default INTER_LINEAR.
        let source = RgbImage::from_fn(5, 4, |x, y| {
            image::Rgb([(x * 40) as u8, (y * 60) as u8, ((x + y) * 25) as u8])
        });
        let actual = warp(
            &source,
            [[-0.4, 0.2], [3.7, -0.3], [4.3, 3.4], [0.2, 2.7]],
            7,
            6,
        );
        let expected = [
            0, 7, 3, 3, 8, 6, 25, 5, 17, 50, 0, 31, 71, 0, 44, 90, 0, 56, 104, 0, 65, 0, 31, 13, 8,
            41, 23, 30, 40, 35, 55, 37, 50, 83, 35, 66, 115, 32, 86, 153, 29, 108, 0, 62, 26, 13,
            74, 39, 35, 74, 53, 60, 74, 68, 88, 74, 86, 120, 74, 106, 158, 75, 130, 0, 100, 42, 18,
            106, 55, 40, 108, 70, 65, 110, 86, 93, 112, 105, 125, 115, 126, 149, 111, 139, 3, 133,
            58, 23, 137, 71, 45, 140, 87, 70, 145, 104, 98, 150, 124, 130, 155, 146, 130, 132, 136,
            8, 162, 73, 28, 167, 87, 50, 173, 103, 74, 179, 121, 92, 161, 125, 102, 137, 121, 67,
            76, 73,
        ];
        // Closed-form homography differs from OpenCV's floating-point solve at half-byte ties.
        for (actual, expected) in actual.as_raw().iter().zip(expected) {
            assert!(
                actual.abs_diff(expected) <= 1,
                "pixel {actual} differs from OpenCV {expected}"
            );
        }
    }
}
