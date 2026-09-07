// Geometry and grouping below are a Rust translation of routines from
// comic_text_detector (GPL-3.0): inference.py, utils/db_utils.py, and
// utils/textblock.py. Distribution and attribution are handled with the model.
use crate::inference::{Input, Runtime, Session, ort::OrtRuntime};
use anyhow::{Context, Result, ensure};
use image::{Rgb, RgbImage};
#[cfg(test)]
use ndarray::ArrayView3;
use ndarray::{Array2, Array4, ArrayView2, Axis, Ix3, Ix4};
use std::collections::VecDeque;
use std::path::Path;

const INPUT_SIZE: usize = 1024;
const TEXT_HEIGHT: u32 = 64;
const BLOCK_CONFIDENCE: f32 = 0.4;
const BLOCK_NMS_IOU: f64 = 0.35;
const LINE_THRESHOLD: f32 = 0.3;
const LINE_SCORE: f32 = 0.6;

struct PreparedInput {
    tensor: Array4<f32>,
    content_width: usize,
    content_height: usize,
}

fn prepare_input(image: &RgbImage, size: usize) -> PreparedInput {
    let scale =
        (size as f64 / f64::from(image.width())).min(size as f64 / f64::from(image.height()));
    let content_width = ((f64::from(image.width()) * scale).round() as usize).max(1);
    let content_height = ((f64::from(image.height()) * scale).round() as usize).max(1);
    let mut tensor = Array4::zeros((1, 3, size, size));
    for y in 0..content_height {
        let source_y =
            ((y as f64 + 0.5) * f64::from(image.height()) / content_height as f64 - 0.5).max(0.0);
        for x in 0..content_width {
            let source_x =
                ((x as f64 + 0.5) * f64::from(image.width()) / content_width as f64 - 0.5).max(0.0);
            let pixel = bilinear_pixel(image, source_x, source_y);
            for channel in 0..3 {
                // The pinned torch backend receives BGR after upstream's
                // BGR->RGB conversion followed by its final channel reversal.
                tensor[[0, channel, y, x]] = f32::from(pixel[2 - channel]) / 255.0;
            }
        }
    }
    PreparedInput {
        tensor,
        content_width,
        content_height,
    }
}

#[derive(Debug)]
pub struct DetectedLine {
    pub coords: [[f64; 2]; 4],
    pub crops: Vec<RgbImage>,
}

#[derive(Debug)]
pub struct DetectedBlock {
    pub bounds: [f64; 4],
    pub vertical: bool,
    pub font_size: f64,
    pub lines: Vec<DetectedLine>,
}

pub struct Detector {
    session: Box<dyn Session>,
}

impl Detector {
    pub fn load(models: &Path) -> Result<Self> {
        Self::load_with_runtime(models, &OrtRuntime)
    }

    pub fn load_with_runtime(models: &Path, runtime: &dyn Runtime) -> Result<Self> {
        let path = models.join("comictextdetector.onnx");
        ensure!(path.is_file(), "detector model missing: {}", path.display());
        Ok(Self::from_session(runtime.load(&path)?))
    }

    pub fn from_session(session: Box<dyn Session>) -> Self {
        Self { session }
    }

    pub fn detect(&mut self, image: &RgbImage) -> Result<Vec<DetectedBlock>> {
        ensure!(
            image.width() > 0 && image.height() > 0,
            "cannot detect an empty image"
        );
        let prepared = prepare_input(image, INPUT_SIZE);
        let outputs = self.session.run(vec![(
            "images",
            Input::F32(prepared.tensor.view().into_dyn()),
        )])?;
        let block_output = outputs
            .named_array("blocks")?
            .into_dimensionality::<Ix3>()
            .context("detector blocks must be rank 3")?;
        let mask_output = outputs
            .named_array("mask")?
            .into_dimensionality::<Ix4>()
            .context("detector mask must be rank 4")?;
        let line_output = outputs
            .named_array("lines")?
            .into_dimensionality::<Ix4>()
            .context("detector lines must be rank 4")?;
        ensure!(
            block_output.shape()[0] == 1
                && block_output.shape()[2] == 7
                && mask_output.shape() == [1, 1, INPUT_SIZE, INPUT_SIZE]
                && line_output.shape() == [1, 2, INPUT_SIZE, INPUT_SIZE],
            "unexpected comic-text detector output shapes"
        );

        let scale_x = f64::from(image.width()) / prepared.content_width as f64;
        let scale_y = f64::from(image.height()) / prepared.content_height as f64;
        let candidates = postprocess_block_view(
            block_output.index_axis(Axis(0), 0),
            scale_x,
            scale_y,
            image.width(),
            image.height(),
        );
        let lines = extract_lines(
            line_output.index_axis(Axis(0), 0).index_axis(Axis(0), 0),
            scale_x,
            scale_y,
            image.width(),
            image.height(),
        );
        let mask = resize_mask(
            mask_output.index_axis(Axis(0), 0).index_axis(Axis(0), 0),
            image.width() as usize,
            image.height() as usize,
            prepared.content_width,
            prepared.content_height,
        );
        let grouped = group_lines(
            candidates,
            lines,
            mask.view(),
            image.width(),
            image.height(),
        );
        Ok(grouped
            .into_iter()
            .map(|block| DetectedBlock {
                bounds: block.bounds,
                vertical: block.vertical,
                font_size: block.font_size,
                lines: block
                    .lines
                    .into_iter()
                    .map(|line| DetectedLine {
                        coords: line.coords,
                        crops: make_line_crops(
                            image,
                            mask.view(),
                            line.coords,
                            block.vertical,
                            block.language,
                            block.font_size,
                        ),
                    })
                    .collect(),
            })
            .collect())
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Language {
    English,
    Japanese,
    Unknown,
}

#[derive(Clone, Debug)]
struct RawLine {
    coords: [[f64; 2]; 4],
}

impl RawLine {
    fn from_coords(coords: [[f64; 2]; 4]) -> Self {
        Self { coords }
    }

    fn bounds(&self) -> [f64; 4] {
        let mut bounds = [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ];
        for point in self.coords {
            bounds[0] = bounds[0].min(point[0]);
            bounds[1] = bounds[1].min(point[1]);
            bounds[2] = bounds[2].max(point[0]);
            bounds[3] = bounds[3].max(point[1]);
        }
        bounds
    }
}

#[derive(Clone, Debug)]
struct RawBlock {
    bounds: [f64; 4],
    language: Language,
    confidence: f32,
    lines: Vec<RawLine>,
    vertical: bool,
    font_size: f64,
    distances: Vec<f64>,
    primary: [f64; 2],
    primary_norm: f64,
}

impl RawBlock {
    fn new(bounds: [f64; 4], language: Language) -> Self {
        Self {
            bounds,
            language,
            confidence: 1.0,
            lines: Vec::new(),
            vertical: false,
            font_size: 0.0,
            distances: Vec::new(),
            primary: [0.0; 2],
            primary_norm: 0.0,
        }
    }
}

#[cfg(test)]
fn postprocess_blocks(
    predictions: &[[f32; 7]],
    scale_x: f64,
    scale_y: f64,
    width: u32,
    height: u32,
) -> Vec<RawBlock> {
    let flattened: Vec<f32> = predictions.iter().flatten().copied().collect();
    let view = ArrayView3::from_shape((1, predictions.len(), 7), &flattened)
        .expect("fixed-width test predictions");
    postprocess_block_view(view.index_axis(Axis(0), 0), scale_x, scale_y, width, height)
}

fn postprocess_block_view(
    predictions: ArrayView2<'_, f32>,
    scale_x: f64,
    scale_y: f64,
    width: u32,
    height: u32,
) -> Vec<RawBlock> {
    let mut candidates = Vec::new();
    for prediction in predictions.rows() {
        let objectness = prediction[4];
        if objectness <= BLOCK_CONFIDENCE || !prediction.iter().all(|v| v.is_finite()) {
            continue;
        }
        let (class, class_score) = if prediction[5] >= prediction[6] {
            (Language::English, prediction[5])
        } else {
            (Language::Japanese, prediction[6])
        };
        let confidence = objectness * class_score;
        if confidence <= BLOCK_CONFIDENCE {
            continue;
        }
        let cx = f64::from(prediction[0]);
        let cy = f64::from(prediction[1]);
        let w = f64::from(prediction[2]);
        let h = f64::from(prediction[3]);
        let bounds = [
            ((cx - w / 2.0) * scale_x)
                .trunc()
                .clamp(0.0, f64::from(width)),
            ((cy - h / 2.0) * scale_y)
                .trunc()
                .clamp(0.0, f64::from(height)),
            ((cx + w / 2.0) * scale_x)
                .trunc()
                .clamp(0.0, f64::from(width)),
            ((cy + h / 2.0) * scale_y)
                .trunc()
                .clamp(0.0, f64::from(height)),
        ];
        if bounds[2] <= bounds[0] || bounds[3] <= bounds[1] {
            continue;
        }
        let mut block = RawBlock::new(bounds, class);
        block.confidence = confidence;
        candidates.push(block);
    }
    candidates.sort_by(|a, b| b.confidence.total_cmp(&a.confidence));
    let mut kept: Vec<RawBlock> = Vec::with_capacity(candidates.len().min(300));
    'candidate: for candidate in candidates {
        for prior in &kept {
            if candidate.language == prior.language
                && rectangle_iou(candidate.bounds, prior.bounds) > BLOCK_NMS_IOU
            {
                continue 'candidate;
            }
        }
        kept.push(candidate);
        if kept.len() == 300 {
            break;
        }
    }
    kept
}

fn rectangle_iou(a: [f64; 4], b: [f64; 4]) -> f64 {
    let intersection =
        (a[2].min(b[2]) - a[0].max(b[0])).max(0.0) * (a[3].min(b[3]) - a[1].max(b[1])).max(0.0);
    let area_a = (a[2] - a[0]) * (a[3] - a[1]);
    let area_b = (b[2] - b[0]) * (b[3] - b[1]);
    intersection / (area_a + area_b - intersection)
}

fn extract_lines(
    prediction: ArrayView2<'_, f32>,
    scale_x: f64,
    scale_y: f64,
    width: u32,
    height: u32,
) -> Vec<RawLine> {
    let (rows, columns) = prediction.dim();
    let mut visited = vec![false; rows * columns];
    let mut lines = Vec::new();
    for y in 0..rows {
        for x in 0..columns {
            let index = y * columns + x;
            if visited[index] || prediction[[y, x]] <= LINE_THRESHOLD {
                continue;
            }
            let mut queue = VecDeque::from([(x, y)]);
            visited[index] = true;
            let mut points = Vec::new();
            let mut count = 0usize;
            let mut score_sum = 0.0f64;
            while let Some((px, py)) = queue.pop_front() {
                count += 1;
                score_sum += f64::from(prediction[[py, px]]);
                let mut boundary = false;
                for dy in -1isize..=1 {
                    for dx in -1isize..=1 {
                        if dx == 0 && dy == 0 {
                            continue;
                        }
                        let nx = px as isize + dx;
                        let ny = py as isize + dy;
                        if nx < 0 || ny < 0 || nx >= columns as isize || ny >= rows as isize {
                            boundary = true;
                            continue;
                        }
                        let ni = ny as usize * columns + nx as usize;
                        if prediction[[ny as usize, nx as usize]] <= LINE_THRESHOLD {
                            boundary = true;
                        } else if !visited[ni] {
                            visited[ni] = true;
                            queue.push_back((nx as usize, ny as usize));
                        }
                    }
                }
                if boundary {
                    points.push([px as f64, py as f64]);
                }
            }
            if count == 0 {
                continue;
            }
            let hull = convex_hull(&points);
            if score_sum / count as f64 <= f64::from(LINE_SCORE) {
                continue;
            }
            let Some((quad, short_side)) = minimum_area_rectangle(&hull) else {
                continue;
            };
            if short_side < 2.0 {
                continue;
            }
            let expanded = order_quad(expand_rectangle(quad, 1.5));
            let coords = expanded.map(|[x, y]| {
                [
                    (x.round().clamp(0.0, columns as f64) * scale_x)
                        .trunc()
                        .clamp(0.0, f64::from(width)),
                    (y.round().clamp(0.0, rows as f64) * scale_y)
                        .trunc()
                        .clamp(0.0, f64::from(height)),
                ]
            });
            lines.push(RawLine::from_coords(coords));
            if lines.len() == 1000 {
                return lines;
            }
        }
    }
    lines
}

fn convex_hull(points: &[[f64; 2]]) -> Vec<[f64; 2]> {
    let mut points = points.to_vec();
    points.sort_by(|a, b| a[0].total_cmp(&b[0]).then(a[1].total_cmp(&b[1])));
    points.dedup();
    if points.len() <= 2 {
        return points;
    }
    fn cross(o: [f64; 2], a: [f64; 2], b: [f64; 2]) -> f64 {
        (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    }
    let mut lower = Vec::new();
    for &point in &points {
        while lower.len() >= 2
            && cross(lower[lower.len() - 2], lower[lower.len() - 1], point) <= 0.0
        {
            lower.pop();
        }
        lower.push(point);
    }
    let mut upper = Vec::new();
    for &point in points.iter().rev() {
        while upper.len() >= 2
            && cross(upper[upper.len() - 2], upper[upper.len() - 1], point) <= 0.0
        {
            upper.pop();
        }
        upper.push(point);
    }
    lower.pop();
    upper.pop();
    lower.extend(upper);
    lower
}

fn minimum_area_rectangle(points: &[[f64; 2]]) -> Option<([[f64; 2]; 4], f64)> {
    let hull = convex_hull(points);
    if hull.len() < 3 {
        return None;
    }
    let mut best = None;
    for index in 0..hull.len() {
        let next = hull[(index + 1) % hull.len()];
        let edge = [next[0] - hull[index][0], next[1] - hull[index][1]];
        let norm = edge[0].hypot(edge[1]);
        if norm == 0.0 {
            continue;
        }
        let u = [edge[0] / norm, edge[1] / norm];
        let v = [-u[1], u[0]];
        let mut range = [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ];
        for point in &hull {
            let pu = point[0] * u[0] + point[1] * u[1];
            let pv = point[0] * v[0] + point[1] * v[1];
            range[0] = range[0].min(pu);
            range[1] = range[1].min(pv);
            range[2] = range[2].max(pu);
            range[3] = range[3].max(pv);
        }
        let area = (range[2] - range[0]) * (range[3] - range[1]);
        if best
            .as_ref()
            .is_none_or(|(best_area, _, _)| area < *best_area)
        {
            best = Some((area, range, [u, v]));
        }
    }
    let (_, range, [u, v]) = best?;
    let point = |pu: f64, pv: f64| [pu * u[0] + pv * v[0], pu * u[1] + pv * v[1]];
    let quad = order_quad([
        point(range[0], range[1]),
        point(range[2], range[1]),
        point(range[2], range[3]),
        point(range[0], range[3]),
    ]);
    Some((quad, (range[2] - range[0]).min(range[3] - range[1])))
}

fn order_quad(points: [[f64; 2]; 4]) -> [[f64; 2]; 4] {
    let mut points = points;
    points.sort_by(|a, b| a[0].total_cmp(&b[0]).then(a[1].total_cmp(&b[1])));
    let (top_left, bottom_left) = if points[1][1] > points[0][1] {
        (points[0], points[1])
    } else {
        (points[1], points[0])
    };
    let (top_right, bottom_right) = if points[3][1] > points[2][1] {
        (points[2], points[3])
    } else {
        (points[3], points[2])
    };
    [top_left, top_right, bottom_right, bottom_left]
}

fn expand_rectangle(quad: [[f64; 2]; 4], ratio: f64) -> [[f64; 2]; 4] {
    let width = distance(quad[0], quad[1]);
    let height = distance(quad[0], quad[3]);
    let perimeter = 2.0 * (width + height);
    if width == 0.0 || height == 0.0 || perimeter == 0.0 {
        return quad;
    }
    let padding = width * height * ratio / perimeter;
    let center = [
        quad.iter().map(|point| point[0]).sum::<f64>() / 4.0,
        quad.iter().map(|point| point[1]).sum::<f64>() / 4.0,
    ];
    let scale_x = (width + 2.0 * padding) / width;
    let scale_y = (height + 2.0 * padding) / height;
    let ux = [
        (quad[1][0] - quad[0][0]) / width,
        (quad[1][1] - quad[0][1]) / width,
    ];
    let uy = [
        (quad[3][0] - quad[0][0]) / height,
        (quad[3][1] - quad[0][1]) / height,
    ];
    quad.map(|point| {
        let delta = [point[0] - center[0], point[1] - center[1]];
        let x = delta[0] * ux[0] + delta[1] * ux[1];
        let y = delta[0] * uy[0] + delta[1] * uy[1];
        [
            center[0] + x * scale_x * ux[0] + y * scale_y * uy[0],
            center[1] + x * scale_x * ux[1] + y * scale_y * uy[1],
        ]
    })
}

fn resize_mask(
    mask: ArrayView2<'_, f32>,
    width: usize,
    height: usize,
    content_width: usize,
    content_height: usize,
) -> Array2<f32> {
    let mut resized = Array2::zeros((height, width));
    for y in 0..height {
        let source_y = ((y as f64 + 0.5) * content_height as f64 / height as f64 - 0.5)
            .clamp(0.0, (content_height - 1) as f64);
        for x in 0..width {
            let source_x = ((x as f64 + 0.5) * content_width as f64 / width as f64 - 0.5)
                .clamp(0.0, (content_width - 1) as f64);
            resized[[y, x]] = bilinear_array(mask, source_x, source_y);
        }
    }
    resized
}

fn group_lines(
    mut blocks: Vec<RawBlock>,
    lines: Vec<RawLine>,
    mask: ArrayView2<'_, f32>,
    image_width: u32,
    image_height: u32,
) -> Vec<RawBlock> {
    let mut scattered = Vec::new();
    for line in lines {
        let line_bounds = line.bounds();
        let line_area = (line_bounds[2] - line_bounds[0]) * (line_bounds[3] - line_bounds[1]);
        let mut best = (0.4, None);
        for (index, block) in blocks.iter().enumerate() {
            let covered =
                intersection_area(block.bounds, line_bounds) / line_area.max(f64::EPSILON);
            if covered > best.0 {
                best = (covered, Some(index));
            }
        }
        if let Some(index) = best.1 {
            blocks[index].lines.push(line);
        } else if mask_average(mask, line_bounds) >= 0.1 {
            let mut block = RawBlock::new(line_bounds, Language::Unknown);
            block.lines.push(line);
            scattered.push(block);
        }
    }

    let mut final_blocks = Vec::new();
    for mut block in blocks {
        if block.lines.is_empty() {
            if mask_average(mask, block.bounds) < 0.1 {
                continue;
            }
            let [left, top, right, bottom] = block.bounds;
            block.lines.push(RawLine::from_coords([
                [left, top],
                [right, top],
                [right, bottom],
                [left, bottom],
            ]));
        }
        examine_block(&mut block, image_width, true);
        if block.lines.len() > 1 && (block.language == Language::Japanese || block.vertical) {
            final_blocks.extend(split_text_block(block));
        } else {
            final_blocks.push(block);
        }
    }
    let mut horizontal_scattered = Vec::new();
    let mut vertical_scattered = Vec::new();
    for mut block in scattered {
        examine_block(&mut block, image_width, false);
        if block.vertical {
            vertical_scattered.push(block);
        } else {
            horizontal_scattered.push(block);
        }
    }
    final_blocks.extend(merge_scattered(horizontal_scattered));
    final_blocks.extend(merge_scattered(vertical_scattered));
    sort_blocks(&mut final_blocks, image_width, image_height);
    for block in &mut final_blocks {
        expand_english_lines(block, image_width, image_height);
    }
    final_blocks
}

fn merge_scattered(mut blocks: Vec<RawBlock>) -> Vec<RawBlock> {
    if blocks.len() < 2 {
        return blocks;
    }
    blocks.sort_by(|a, b| a.distances[0].total_cmp(&b.distances[0]));
    let mut merged = vec![false; blocks.len()];
    let mut output = Vec::new();
    for current_index in 0..blocks.len() {
        if merged[current_index] {
            continue;
        }
        let mut current = blocks[current_index].clone();
        for next_index in current_index + 1..blocks.len() {
            if merged[next_index] || !can_merge_lines(&current, &blocks[next_index]) {
                continue;
            }
            let next = &blocks[next_index];
            let current_count = current.lines.len() as f64;
            let next_count = next.lines.len() as f64;
            current.font_size = (current.font_size * current_count + next.font_size * next_count)
                / (current_count + next_count);
            current.primary[0] += next.primary[0];
            current.primary[1] += next.primary[1];
            current.primary_norm = vector_norm(current.primary);
            current.lines.extend(next.lines.iter().cloned());
            current.distances.extend(next.distances.iter().copied());
            merged[next_index] = true;
        }
        current.bounds = [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ];
        for line in &current.lines {
            let bounds = line.bounds();
            current.bounds[0] = current.bounds[0].min(bounds[0]);
            current.bounds[1] = current.bounds[1].min(bounds[1]);
            current.bounds[2] = current.bounds[2].max(bounds[2]);
            current.bounds[3] = current.bounds[3].max(bounds[3]);
        }
        output.push(current);
    }
    output
}

fn can_merge_lines(first: &RawBlock, second: &RawBlock) -> bool {
    let Some(first_line) = first.lines.last() else {
        return false;
    };
    let Some(second_line) = second.lines.last() else {
        return false;
    };
    if convex_polygons_intersect(&first_line.coords, &second_line.coords) {
        return true;
    }
    let ratio = first.font_size / second.font_size;
    if ratio > 1.3 || 1.0 / ratio > 1.3 {
        return false;
    }
    let cosine = (first.primary[0] * second.primary[0] + first.primary[1] * second.primary[1])
        / first.primary_norm
        / second.primary_norm;
    if cosine.abs() < 0.866 {
        return false;
    }
    let first_count = first.lines.len() as f64;
    let second_count = second.lines.len() as f64;
    let average_font = (first.font_size * first_count + second.font_size * second_count)
        / (first_count + second_count);
    let line_distance = second.distances.last().copied().unwrap_or(0.0)
        - first.distances.last().copied().unwrap_or(0.0);
    line_distance <= 2.0 * average_font
        && distance(first_line.coords[0], second_line.coords[0]) <= 2.5 * average_font
}

fn convex_polygons_intersect(first: &[[f64; 2]; 4], second: &[[f64; 2]; 4]) -> bool {
    convex_polygons_intersect_with_tolerance(first, second, 0.0)
}

fn convex_polygons_intersect_with_tolerance(
    first: &[[f64; 2]; 4],
    second: &[[f64; 2]; 4],
    tolerance: f64,
) -> bool {
    for polygon in [first, second] {
        for index in 0..4 {
            let next = (index + 1) % 4;
            let axis = [
                -(polygon[next][1] - polygon[index][1]),
                polygon[next][0] - polygon[index][0],
            ];
            let project = |points: &[[f64; 2]; 4]| {
                points.iter().fold(
                    (f64::INFINITY, f64::NEG_INFINITY),
                    |(minimum, maximum), point| {
                        let value = point[0] * axis[0] + point[1] * axis[1];
                        (minimum.min(value), maximum.max(value))
                    },
                )
            };
            let (first_min, first_max) = project(first);
            let (second_min, second_max) = project(second);
            let projected_tolerance = tolerance * vector_norm(axis);
            if first_max + projected_tolerance < second_min
                || second_max + projected_tolerance < first_min
            {
                return false;
            }
        }
    }
    true
}

fn examine_block(block: &mut RawBlock, image_width: u32, preserve_detector_bounds: bool) {
    let mut vertical_vector = [0.0, 0.0];
    let mut horizontal_vector = [0.0, 0.0];
    let mut centers = Vec::with_capacity(block.lines.len());
    for line in &block.lines {
        let points = line.coords;
        let middle = [
            midpoint(points[0], points[1]),
            midpoint(points[1], points[2]),
            midpoint(points[2], points[3]),
            midpoint(points[3], points[0]),
        ];
        vertical_vector[0] += middle[2][0] - middle[0][0];
        vertical_vector[1] += middle[2][1] - middle[0][1];
        horizontal_vector[0] += middle[1][0] - middle[3][0];
        horizontal_vector[1] += middle[1][1] - middle[3][1];
        centers.push(midpoint(points[0], points[2]));
    }
    let vertical_norm = vector_norm(vertical_vector);
    let horizontal_norm = vector_norm(horizontal_vector);
    block.vertical = match block.language {
        Language::Japanese => vertical_norm > horizontal_norm,
        _ => vertical_norm > horizontal_norm * 2.0,
    };
    block.primary = if block.vertical {
        vertical_vector
    } else {
        horizontal_vector
    };
    block.primary_norm = vector_norm(block.primary).max(f64::EPSILON);
    block.font_size = if block.vertical {
        horizontal_norm / block.lines.len() as f64
    } else {
        vertical_norm / block.lines.len() as f64
    }
    .round()
    .max(1.0);
    let origin = if block.vertical {
        [f64::from(image_width), 0.0]
    } else {
        [0.0, 0.0]
    };
    block.distances = centers
        .iter()
        .map(|center| {
            let delta = [center[0] - origin[0], center[1] - origin[1]];
            (delta[0] * block.primary[1] - delta[1] * block.primary[0]).abs() / block.primary_norm
        })
        .collect();
    let mut order: Vec<usize> = (0..block.lines.len()).collect();
    order.sort_by(|&a, &b| block.distances[a].total_cmp(&block.distances[b]));
    block.lines = order
        .iter()
        .map(|&index| block.lines[index].clone())
        .collect();
    block.distances = order.iter().map(|&index| block.distances[index]).collect();
    if !preserve_detector_bounds {
        block.bounds = [
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        ];
    }
    for line in &block.lines {
        let bounds = line.bounds();
        block.bounds[0] = block.bounds[0].min(bounds[0]);
        block.bounds[1] = block.bounds[1].min(bounds[1]);
        block.bounds[2] = block.bounds[2].max(bounds[2]);
        block.bounds[3] = block.bounds[3].max(bounds[3]);
    }
}

fn split_text_block(mut block: RawBlock) -> Vec<RawBlock> {
    let anchor = block.lines[0].coords[0];
    block.lines.sort_by(|left, right| {
        distance(left.coords[0], anchor).total_cmp(&distance(right.coords[0], anchor))
    });

    let distance_tolerance = block.font_size * 2.0;
    let angle = block.primary[1].atan2(block.primary[0]).to_degrees()
        - if block.vertical { 90.0 } else { 0.0 };
    let mut groups = vec![vec![block.lines[0].clone()]];
    for index in 1..block.lines.len() {
        let previous = &block.lines[index - 1];
        let line = &block.lines[index];
        let distance_between = (block.distances[index] - block.distances[index - 1]).abs();
        let current_group_len = groups.last().map_or(0, Vec::len);
        // Detector contours are integer raster boundaries. Treat the one-pixel
        // extent on each side as touching so a two-pixel rounding gap does not
        // spuriously split fragments of the same line.
        let split = !convex_polygons_intersect_with_tolerance(&previous.coords, &line.coords, 2.0)
            && (distance_between > distance_tolerance
                || (block.vertical
                    && angle.abs() < 15.0
                    && (current_group_len > 1 || distance_between > block.font_size)
                    && (previous.coords[0][1] - line.coords[0][1]).abs() > block.font_size));
        if split {
            groups.push(vec![line.clone()]);
        } else if let Some(group) = groups.last_mut() {
            group.push(line.clone());
        }
    }

    if groups.len() == 1 {
        return vec![block];
    }
    groups
        .into_iter()
        .map(|lines| {
            let mut split = block.clone();
            split.lines = lines;
            split.bounds = [
                f64::INFINITY,
                f64::INFINITY,
                f64::NEG_INFINITY,
                f64::NEG_INFINITY,
            ];
            for line in &split.lines {
                let bounds = line.bounds();
                split.bounds[0] = split.bounds[0].min(bounds[0]);
                split.bounds[1] = split.bounds[1].min(bounds[1]);
                split.bounds[2] = split.bounds[2].max(bounds[2]);
                split.bounds[3] = split.bounds[3].max(bounds[3]);
            }
            split
        })
        .collect()
}

fn expand_english_lines(block: &mut RawBlock, image_width: u32, image_height: u32) {
    if block.language != Language::English || block.vertical || block.lines.is_empty() {
        return;
    }
    let expansion = (block.font_size * 0.1).trunc().max(2.0);
    let mut angle = block.primary[1]
        .atan2(block.primary[0])
        .to_degrees()
        .trunc();
    if angle.abs() < 3.0 {
        angle = 0.0;
    }
    let (sin, cos) = angle.to_radians().sin_cos();
    let shifts = [[-sin, -cos], [sin, -cos], [sin, cos], [-sin, cos]];
    let maximum_x = f64::from(image_width.saturating_sub(1));
    let maximum_y = f64::from(image_height.saturating_sub(1));
    for line in &mut block.lines {
        for (point, shift) in line.coords.iter_mut().zip(shifts) {
            point[0] = (point[0] + shift[0] * expansion)
                .trunc()
                .clamp(0.0, maximum_x);
            point[1] = (point[1] + shift[1] * expansion)
                .trunc()
                .clamp(0.0, maximum_y);
        }
    }
    block.font_size += expansion;
}

fn sort_blocks(blocks: &mut [RawBlock], image_width: u32, image_height: u32) {
    let japanese = blocks
        .iter()
        .filter(|block| block.language == Language::Japanese)
        .count();
    let flip = japanese * 2 > blocks.len();
    let original_width = f64::from(image_width);
    let grid_width = if image_width > image_height {
        original_width / 2.0
    } else {
        original_width
    };
    let image_area = f64::from(image_height) * grid_width;
    blocks.sort_by(|a, b| {
        let weight = |block: &RawBlock| {
            let mut center_x = (block.bounds[0] + block.bounds[2]) / 2.0;
            if flip {
                center_x = original_width - center_x;
            }
            let center_y = (block.bounds[1] + block.bounds[3]) / 2.0;
            let grid_x = (center_x / grid_width * 3.0).floor();
            let grid_y = (center_y / f64::from(image_height) * 4.0).floor();
            let mut result = (grid_y * 3.0 + grid_x) * image_area
                + 1.2 * (center_x - grid_x * grid_width / 3.0)
                + (center_y - grid_y * f64::from(image_height) / 4.0);
            if image_width > image_height && grid_x >= 3.0 {
                result += image_area * 12.0;
            }
            result
        };
        weight(a).total_cmp(&weight(b))
    });
}

fn intersection_area(a: [f64; 4], b: [f64; 4]) -> f64 {
    (a[2].min(b[2]) - a[0].max(b[0])).max(0.0) * (a[3].min(b[3]) - a[1].max(b[1])).max(0.0)
}

fn mask_average(mask: ArrayView2<'_, f32>, bounds: [f64; 4]) -> f64 {
    let (height, width) = mask.dim();
    let left = bounds[0].floor().clamp(0.0, width as f64) as usize;
    let top = bounds[1].floor().clamp(0.0, height as f64) as usize;
    let right = bounds[2].ceil().clamp(0.0, width as f64) as usize;
    let bottom = bounds[3].ceil().clamp(0.0, height as f64) as usize;
    if left >= right || top >= bottom {
        return 0.0;
    }
    let mut sum = 0.0f64;
    for y in top..bottom {
        for x in left..right {
            sum += f64::from(mask[[y, x]]);
        }
    }
    sum / ((right - left) * (bottom - top)) as f64
}

fn make_line_crops(
    image: &RgbImage,
    mask: ArrayView2<'_, f32>,
    mut coords: [[f64; 2]; 4],
    vertical: bool,
    language: Language,
    font_size: f64,
) -> Vec<RgbImage> {
    if language == Language::English || (language == Language::Unknown && !vertical) {
        let expansion = font_size / 3.0;
        for (point, [dx, dy]) in
            coords
                .iter_mut()
                .zip([[-1.0, -1.0], [1.0, -1.0], [1.0, 1.0], [-1.0, 1.0]])
        {
            point[0] = (point[0] + dx * expansion).clamp(0.0, f64::from(image.width()));
            point[1] = (point[1] + dy * expansion).clamp(0.0, f64::from(image.height()));
        }
    }
    let middle = [
        midpoint(coords[0], coords[1]),
        midpoint(coords[1], coords[2]),
        midpoint(coords[2], coords[3]),
        midpoint(coords[3], coords[0]),
    ];
    let vertical_length = distance(middle[0], middle[2]).max(1.0);
    let horizontal_length = distance(middle[3], middle[1]).max(1.0);
    let ratio = vertical_length / horizontal_length;
    let (width, height) = if vertical {
        (
            TEXT_HEIGHT,
            (f64::from(TEXT_HEIGHT) * ratio).round().max(1.0) as u32,
        )
    } else {
        (
            (f64::from(TEXT_HEIGHT) / ratio).round().max(1.0) as u32,
            TEXT_HEIGHT,
        )
    };
    let crop = crate::native_warp::warp(image, coords, width, height);
    let crop_mask = crate::native_warp::warp_mask(mask, coords, width, height);

    let (working_crop, working_mask) = if vertical {
        (
            image::imageops::rotate270(&crop),
            rotate_array_counterclockwise(&crop_mask),
        )
    } else {
        (crop, crop_mask)
    };
    let maximum_ratio = if vertical { 16.0 } else { 8.0 };
    let ratio = f64::from(working_crop.width()) / f64::from(working_crop.height());
    let chunk_count = (ratio / maximum_ratio).ceil().max(1.0) as u32;
    let cuts = low_density_cuts(working_mask.view(), chunk_count);
    let mut chunks = Vec::with_capacity(chunk_count as usize);
    for pair in cuts.windows(2) {
        let chunk = image::imageops::crop_imm(
            &working_crop,
            pair[0],
            0,
            pair[1] - pair[0],
            working_crop.height(),
        )
        .to_image();
        chunks.push(if vertical {
            image::imageops::rotate90(&chunk)
        } else {
            chunk
        });
    }
    chunks
}

fn rotate_array_counterclockwise(source: &Array2<f32>) -> Array2<f32> {
    let (height, width) = source.dim();
    Array2::from_shape_fn((width, height), |(y, x)| source[[x, width - 1 - y]])
}

fn low_density_cuts(mask: ArrayView2<'_, f32>, chunk_count: u32) -> Vec<u32> {
    let (_, width) = mask.dim();
    if chunk_count <= 1 || width <= 1 {
        return vec![0, width as u32];
    }
    let density: Vec<f64> = mask
        .axis_iter(Axis(1))
        .map(|column| column.iter().map(|&value| f64::from(value)).sum())
        .collect();
    let sigma = 8.0_f64;
    let kernel: Vec<f64> = (-64..64)
        .map(|offset| (-(f64::from(offset).powi(2)) / (2.0 * sigma.powi(2))).exp())
        .collect();
    let smoothed: Vec<f64> = (0..width)
        .map(|x| {
            kernel
                .iter()
                .enumerate()
                .filter_map(|(index, &weight)| {
                    let source_x = x as isize + index as isize - 64;
                    (0..width as isize)
                        .contains(&source_x)
                        .then(|| density[source_x as usize] * weight)
                })
                .sum()
        })
        .collect();
    let mut cuts = Vec::with_capacity(chunk_count as usize + 1);
    cuts.push(0);
    let mut previous_cut = 0;
    for index in 1..chunk_count {
        let anchor = width * index as usize / chunk_count as usize;
        let start = anchor.saturating_sub(64).max(previous_cut + 1);
        let end = (anchor + 64).min(width - 1);
        if start > end {
            continue;
        }
        let Some(cut) =
            (start..=end).min_by(|&left, &right| smoothed[left].total_cmp(&smoothed[right]))
        else {
            continue;
        };
        cuts.push(cut as u32);
        previous_cut = cut;
    }
    cuts.push(width as u32);
    cuts
}

fn bilinear_pixel(image: &RgbImage, x: f64, y: f64) -> Rgb<u8> {
    let x = x.clamp(0.0, f64::from(image.width().saturating_sub(1)));
    let y = y.clamp(0.0, f64::from(image.height().saturating_sub(1)));
    let x0 = x.floor() as u32;
    let y0 = y.floor() as u32;
    let x1 = (x0 + 1).min(image.width() - 1);
    let y1 = (y0 + 1).min(image.height() - 1);
    let fx = x - f64::from(x0);
    let fy = y - f64::from(y0);
    let mut out = [0u8; 3];
    for (channel, value) in out.iter_mut().enumerate() {
        let top = f64::from(image.get_pixel(x0, y0)[channel]) * (1.0 - fx)
            + f64::from(image.get_pixel(x1, y0)[channel]) * fx;
        let bottom = f64::from(image.get_pixel(x0, y1)[channel]) * (1.0 - fx)
            + f64::from(image.get_pixel(x1, y1)[channel]) * fx;
        *value = (top * (1.0 - fy) + bottom * fy).round() as u8;
    }
    Rgb(out)
}

fn bilinear_array(array: ArrayView2<'_, f32>, x: f64, y: f64) -> f32 {
    let (height, width) = array.dim();
    let x0 = x.floor() as usize;
    let y0 = y.floor() as usize;
    let x1 = (x0 + 1).min(width - 1);
    let y1 = (y0 + 1).min(height - 1);
    let fx = (x - x0 as f64) as f32;
    let fy = (y - y0 as f64) as f32;
    let top = array[[y0, x0]] * (1.0 - fx) + array[[y0, x1]] * fx;
    let bottom = array[[y1, x0]] * (1.0 - fx) + array[[y1, x1]] * fx;
    top * (1.0 - fy) + bottom * fy
}

fn midpoint(a: [f64; 2], b: [f64; 2]) -> [f64; 2] {
    [(a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0]
}

fn distance(a: [f64; 2], b: [f64; 2]) -> f64 {
    (a[0] - b[0]).hypot(a[1] - b[1])
}

fn vector_norm(vector: [f64; 2]) -> f64 {
    vector[0].hypot(vector[1])
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::Rgb;

    #[test]
    fn preprocessing_matches_upstream_torch_bgr_channels_and_bottom_right_padding() {
        let image = RgbImage::from_pixel(2, 1, Rgb([255, 64, 16]));
        let prepared = prepare_input(&image, 4);

        assert_eq!(prepared.content_width, 4);
        assert_eq!(prepared.content_height, 2);
        assert_eq!(prepared.tensor.shape(), &[1, 3, 4, 4]);
        assert_eq!(prepared.tensor[[0, 0, 0, 0]], 16.0 / 255.0);
        assert_eq!(prepared.tensor[[0, 1, 0, 0]], 64.0 / 255.0);
        assert_eq!(prepared.tensor[[0, 2, 0, 0]], 1.0);
        assert_eq!(prepared.tensor[[0, 0, 2, 0]], 0.0);
        assert_eq!(prepared.tensor[[0, 1, 0, 3]], 64.0 / 255.0);
    }

    #[test]
    fn preprocessing_matches_opencv_linear_downscale_sampling() {
        let image = RgbImage::from_fn(6, 6, |x, y| {
            Rgb([if (x + y) % 2 == 0 { 0 } else { 255 }, 0, 0])
        });

        let prepared = prepare_input(&image, 2);

        assert_eq!(prepared.tensor[[0, 2, 0, 0]], 0.0);
        assert_eq!(prepared.tensor[[0, 2, 0, 1]], 1.0);
        assert_eq!(prepared.tensor[[0, 2, 1, 0]], 1.0);
        assert_eq!(prepared.tensor[[0, 2, 1, 1]], 0.0);
    }

    #[test]
    fn preprocessing_keeps_extreme_aspect_ratio_content_nonzero() {
        let image = RgbImage::from_pixel(1, 3000, Rgb([1, 2, 3]));

        let prepared = prepare_input(&image, 64);

        assert_eq!(prepared.content_width, 1);
        assert_eq!(prepared.content_height, 64);
        assert!(prepared.tensor.iter().all(|value| value.is_finite()));
    }

    #[test]
    fn yolo_postprocessing_multiplies_confidence_and_applies_class_aware_nms() {
        let predictions = vec![
            [50., 50., 20., 20., 0.9, 0.1, 0.9],
            [51., 50., 20., 20., 0.8, 0.1, 0.9],
            [50., 50., 20., 20., 0.8, 0.8, 0.2],
            [80., 80., 10., 10., 0.3, 0.1, 0.9],
        ];

        let blocks = postprocess_blocks(&predictions, 1.0, 1.0, 100, 100);

        assert_eq!(blocks.len(), 2);
        assert_eq!(blocks[0].bounds, [40., 40., 60., 60.]);
        assert_eq!(blocks[0].language, Language::Japanese);
        assert_eq!(blocks[1].language, Language::English);
    }

    #[test]
    fn concave_line_score_uses_only_the_thresholded_component() {
        let mut prediction = ndarray::Array2::zeros((16, 16));
        for y in 2..14 {
            prediction[[y, 2]] = 0.7;
            prediction[[y, 13]] = 0.7;
        }
        for x in 2..=13 {
            prediction[[13, x]] = 0.7;
        }

        let lines = extract_lines(prediction.view(), 1.0, 1.0, 16, 16);

        assert_eq!(lines.len(), 1);
    }

    #[test]
    fn line_crop_rectifies_to_upstream_text_height_without_losing_rgb() {
        let mut image = RgbImage::new(8, 6);
        for (x, y, pixel) in image.enumerate_pixels_mut() {
            *pixel = Rgb([x as u8 * 20, y as u8 * 30, 7]);
        }
        let coords = [[1., 1.], [5., 1.], [5., 3.], [1., 3.]];

        let mask = ndarray::Array2::ones((6, 8));
        let crops = make_line_crops(&image, mask.view(), coords, false, Language::Japanese, 2.0);

        assert_eq!(crops.len(), 1);
        assert_eq!(crops[0].dimensions(), (128, 64));
        assert_eq!(*crops[0].get_pixel(0, 0), Rgb([20, 30, 7]));
        assert_eq!(*crops[0].get_pixel(127, 63), Rgb([100, 90, 7]));
    }

    #[test]
    fn long_horizontal_line_splits_at_low_mask_density_and_keeps_one_line() {
        let image = RgbImage::from_pixel(640, 64, Rgb([255, 255, 255]));
        let mut mask = ndarray::Array2::ones((64, 640));
        for y in 0..64 {
            for x in 300..340 {
                mask[[y, x]] = 0.0;
            }
        }
        let coords = [[0., 0.], [640., 0.], [640., 64.], [0., 64.]];

        let crops = make_line_crops(&image, mask.view(), coords, false, Language::Japanese, 64.0);

        assert_eq!(crops.len(), 2);
        assert_eq!(crops[0].height(), 64);
        assert_eq!(crops[1].height(), 64);
        assert!((300..=340).contains(&crops[0].width()));
        assert_eq!(crops[0].width() + crops[1].width(), 640);
    }

    #[test]
    fn rotated_quadrilateral_starts_at_the_upstream_left_top_corner() {
        let ordered = order_quad([[279., 61.], [437., 361.], [233., 468.], [76., 168.]]);

        assert_eq!(
            ordered,
            [[76., 168.], [279., 61.], [437., 361.], [233., 468.]]
        );
    }

    #[test]
    fn japanese_vertical_lines_are_grouped_and_read_right_to_left() {
        let candidates = vec![RawBlock::new([10., 5., 90., 95.], Language::Japanese)];
        let right = RawLine::from_coords([[70., 10.], [80., 10.], [80., 90.], [70., 90.]]);
        let left = RawLine::from_coords([[30., 10.], [40., 10.], [40., 90.], [30., 90.]]);
        let mask = ndarray::Array2::ones((100, 100));

        let blocks = group_lines(candidates, vec![left, right], mask.view(), 100, 100);

        assert_eq!(blocks.len(), 2);
        assert!(blocks.iter().all(|block| block.vertical));
        assert_eq!(blocks[0].font_size, 10.0);
        assert_eq!(blocks[0].lines[0].coords[0], [70., 10.]);
        assert_eq!(blocks[1].lines[0].coords[0], [30., 10.]);
    }

    #[test]
    fn nearby_scattered_horizontal_lines_merge_into_one_text_block() {
        let first = RawLine::from_coords([[10., 10.], [60., 10.], [60., 20.], [10., 20.]]);
        let second = RawLine::from_coords([[10., 25.], [60., 25.], [60., 35.], [10., 35.]]);
        let mask = ndarray::Array2::ones((80, 80));

        let blocks = group_lines(Vec::new(), vec![first, second], mask.view(), 80, 80);

        assert_eq!(blocks.len(), 1);
        assert_eq!(blocks[0].lines.len(), 2);
        assert_eq!(blocks[0].bounds, [10., 10., 60., 35.]);
    }

    #[test]
    fn japanese_split_pass_keeps_upstream_distance_from_first_line_order() {
        let first = RawLine::from_coords([[90., 0.], [100., 0.], [100., 20.], [90., 20.]]);
        let distant_down =
            RawLine::from_coords([[80., 100.], [90., 100.], [90., 120.], [80., 120.]]);
        let nearby_left = RawLine::from_coords([[70., 0.], [80., 0.], [80., 20.], [70., 20.]]);
        let mut block = RawBlock::new([70., 0., 100., 120.], Language::Japanese);
        block.lines = vec![first, distant_down, nearby_left];
        block.vertical = true;
        block.font_size = 100.0;
        block.distances = vec![10.0, 20.0, 30.0];
        block.primary = [0.0, 60.0];
        block.primary_norm = 60.0;

        let blocks = split_text_block(block);

        assert_eq!(blocks.len(), 1);
        assert_eq!(blocks[0].lines[0].coords[0], [90., 0.]);
        assert_eq!(blocks[0].lines[1].coords[0], [70., 0.]);
        assert_eq!(blocks[0].lines[2].coords[0], [80., 100.]);
    }

    #[test]
    fn vertical_split_treats_two_pixel_raster_gap_as_touching() {
        let first = RawLine::from_coords([[80., 0.], [90., 0.], [90., 100.], [80., 100.]]);
        let upper = RawLine::from_coords([[60., 0.], [70., 0.], [70., 50.], [60., 50.]]);
        let lower = RawLine::from_coords([[60., 52.], [70., 52.], [70., 80.], [60., 80.]]);
        let mut block = RawBlock::new([60., 0., 90., 100.], Language::Japanese);
        block.lines = vec![first, upper, lower];
        block.vertical = true;
        block.font_size = 10.0;
        block.distances = vec![10.0, 20.0, 20.0];
        block.primary = [0.0, 100.0];
        block.primary_norm = 100.0;

        let blocks = split_text_block(block);

        assert_eq!(blocks.len(), 1);
    }

    #[test]
    fn horizontal_english_lines_receive_upstream_final_expansion() {
        let line = RawLine::from_coords([[10., 10.], [60., 10.], [60., 20.], [10., 20.]]);
        let mut block = RawBlock::new([10., 10., 60., 20.], Language::English);
        block.lines.push(line);
        block.vertical = false;
        block.font_size = 10.0;
        block.primary = [50.0, 0.0];
        block.primary_norm = 50.0;

        expand_english_lines(&mut block, 100, 100);

        assert_eq!(block.font_size, 12.0);
        assert_eq!(
            block.lines[0].coords,
            [[10., 8.], [60., 8.], [60., 22.], [10., 22.]]
        );
    }
}
