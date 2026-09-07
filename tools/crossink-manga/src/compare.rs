use crate::mokuro::{Page, Volume, visible_bounds};
use anyhow::{Result, ensure};
use serde::Serialize;
use std::collections::BTreeMap;

#[derive(Debug, Serialize)]
pub struct PanelReport {
    pub image: String,
    pub clipped_reference: Vec<usize>,
    pub clipped_candidate: Vec<usize>,
    pub reference_blocks: usize,
    pub candidate_blocks: usize,
    pub matched_blocks: usize,
    pub matched_edits: usize,
    pub matched_reference_characters: usize,
    pub panel_edits: usize,
    pub panel_reference_characters: usize,
    pub mean_matched_iou: f64,
    pub matches: Vec<RegionMatch>,
    pub unmatched_reference: Vec<usize>,
    pub unmatched_candidate: Vec<usize>,
}

#[derive(Debug, Serialize)]
pub struct RegionMatch {
    pub reference: usize,
    pub candidate: usize,
    pub iou: f64,
}

#[derive(Debug, Serialize)]
pub struct Report {
    pub normalization: &'static str,
    pub minimum_region_iou: f64,
    pub minimum_region_precision_recall: f64,
    pub maximum_character_error_rate: f64,
    pub pages: usize,
    pub region_recall: f64,
    pub region_precision: f64,
    pub matched_character_error_rate: f64,
    pub panel_character_error_rate: f64,
    pub comparable: bool,
    pub panels: Vec<PanelReport>,
}

fn normalized(text: &str) -> Vec<char> {
    text.chars().filter(|c| !c.is_whitespace()).collect()
}

fn edit_distance(a: &[char], b: &[char]) -> usize {
    let mut previous: Vec<usize> = (0..=b.len()).collect();
    let mut current = vec![0; b.len() + 1];
    for (i, ca) in a.iter().enumerate() {
        current[0] = i + 1;
        for (j, cb) in b.iter().enumerate() {
            current[j + 1] = (previous[j + 1] + 1)
                .min(current[j] + 1)
                .min(previous[j] + usize::from(ca != cb));
        }
        std::mem::swap(&mut previous, &mut current);
    }
    previous[b.len()]
}

fn iou(a: [f64; 4], b: [f64; 4]) -> f64 {
    let intersection =
        (a[2].min(b[2]) - a[0].max(b[0])).max(0.0) * (a[3].min(b[3]) - a[1].max(b[1])).max(0.0);
    let area = |r: [f64; 4]| (r[2] - r[0]).max(0.0) * (r[3] - r[1]).max(0.0);
    let union = area(a) + area(b) - intersection;
    if union > 0.0 {
        intersection / union
    } else {
        0.0
    }
}

fn page_map(volume: &Volume) -> Result<BTreeMap<&str, &Page>> {
    let mut pages = BTreeMap::new();
    for page in &volume.pages {
        ensure!(
            pages.insert(page.img_path.as_str(), page).is_none(),
            "duplicate page {}",
            page.img_path
        );
        ensure!(
            page.img_width > 0 && page.img_height > 0,
            "invalid page dimensions"
        );
        for block in &page.blocks {
            visible_bounds(block.bounds, page.img_width, page.img_height)?;
        }
    }
    Ok(pages)
}

fn clipped_indices(page: &Page) -> Vec<usize> {
    page.blocks
        .iter()
        .enumerate()
        .filter_map(|(i, block)| {
            (visible_bounds(block.bounds, page.img_width, page.img_height).ok()
                != Some(block.bounds))
            .then_some(i)
        })
        .collect()
}

/// Compare the same complete panel set. IoU matching is one-to-one, highest overlap first.
/// Agreement is relative to upstream, not an OCR accuracy measurement.
pub fn compare(reference: &Volume, candidate: &Volume) -> Result<Report> {
    let reference = page_map(reference)?;
    let candidate = page_map(candidate)?;
    ensure!(
        !reference.is_empty() && reference.keys().eq(candidate.keys()),
        "page coverage differs or is empty"
    );
    let mut panels = Vec::with_capacity(reference.len());
    for (name, a) in &reference {
        let b = candidate[name];
        ensure!(
            (a.img_width, a.img_height) == (b.img_width, b.img_height),
            "dimensions differ for {name}"
        );
        let mut pairs = Vec::new();
        for (i, left) in a.blocks.iter().enumerate() {
            for (j, right) in b.blocks.iter().enumerate() {
                let overlap = iou(
                    visible_bounds(left.bounds, a.img_width, a.img_height)?,
                    visible_bounds(right.bounds, b.img_width, b.img_height)?,
                );
                if overlap >= 0.5 {
                    pairs.push((overlap, i, j));
                }
            }
        }
        pairs.sort_by(|a, b| b.0.total_cmp(&a.0).then(a.1.cmp(&b.1)).then(a.2.cmp(&b.2)));
        let mut used_a = vec![false; a.blocks.len()];
        let mut used_b = vec![false; b.blocks.len()];
        let mut matched_edits = 0;
        let mut matched_reference_characters = 0;
        let mut overlap_sum = 0.0;
        let mut matched_blocks = 0;
        let mut matches = Vec::new();
        for (overlap, i, j) in pairs {
            if used_a[i] || used_b[j] {
                continue;
            }
            used_a[i] = true;
            used_b[j] = true;
            let left = normalized(&a.blocks[i].lines.join(""));
            let right = normalized(&b.blocks[j].lines.join(""));
            matched_edits += edit_distance(&left, &right);
            matched_reference_characters += left.len();
            matched_blocks += 1;
            overlap_sum += overlap;
            matches.push(RegionMatch {
                reference: i,
                candidate: j,
                iou: overlap,
            });
        }
        let left = normalized(
            &a.blocks
                .iter()
                .flat_map(|b| &b.lines)
                .cloned()
                .collect::<Vec<_>>()
                .join(""),
        );
        let right = normalized(
            &b.blocks
                .iter()
                .flat_map(|b| &b.lines)
                .cloned()
                .collect::<Vec<_>>()
                .join(""),
        );
        panels.push(PanelReport {
            image: (*name).into(),
            clipped_reference: clipped_indices(a),
            clipped_candidate: clipped_indices(b),
            reference_blocks: a.blocks.len(),
            candidate_blocks: b.blocks.len(),
            matched_blocks,
            matched_edits,
            matched_reference_characters,
            panel_edits: edit_distance(&left, &right),
            panel_reference_characters: left.len(),
            mean_matched_iou: if matched_blocks == 0 {
                0.0
            } else {
                overlap_sum / matched_blocks as f64
            },
            matches,
            unmatched_reference: used_a
                .iter()
                .enumerate()
                .filter_map(|(i, used)| (!used).then_some(i))
                .collect(),
            unmatched_candidate: used_b
                .iter()
                .enumerate()
                .filter_map(|(i, used)| (!used).then_some(i))
                .collect(),
        });
    }
    let total = |f: fn(&PanelReport) -> usize| panels.iter().map(f).sum::<usize>();
    let ratio = |numerator: usize, denominator: usize| numerator as f64 / denominator.max(1) as f64;
    let matched = total(|p| p.matched_blocks);
    let reference_blocks = total(|p| p.reference_blocks);
    let candidate_blocks = total(|p| p.candidate_blocks);
    let region_recall = if reference_blocks == 0 {
        1.0
    } else {
        ratio(matched, reference_blocks)
    };
    let region_precision = if candidate_blocks == 0 {
        1.0
    } else {
        ratio(matched, candidate_blocks)
    };
    let matched_character_error_rate = ratio(
        total(|p| p.matched_edits),
        total(|p| p.matched_reference_characters),
    );
    let panel_character_error_rate = ratio(
        total(|p| p.panel_edits),
        total(|p| p.panel_reference_characters),
    );
    Ok(Report {
        normalization: "Unicode scalar values; whitespace removed; other characters unchanged; rectangles intersected with source image bounds, clipping indexes reported",
        minimum_region_iou: 0.5,
        minimum_region_precision_recall: 0.9,
        maximum_character_error_rate: 0.1,
        pages: panels.len(),
        region_recall,
        region_precision,
        matched_character_error_rate,
        panel_character_error_rate,
        comparable: region_recall >= 0.9
            && region_precision >= 0.9
            && matched_character_error_rate <= 0.1
            && panel_character_error_rate <= 0.1,
        panels,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mokuro::{Block, Page, Volume};

    fn volume(text: &str, bounds: [f64; 4]) -> Volume {
        Volume {
            version: "0.2.5".into(),
            pages: vec![Page {
                img_path: "panel_0000.png".into(),
                img_width: 100,
                img_height: 100,
                blocks: vec![Block {
                    bounds,
                    vertical: true,
                    font_size: 12.0,
                    lines: vec![text.into()],
                    lines_coords: vec![[[0.0, 0.0], [10.0, 0.0], [10.0, 20.0], [0.0, 20.0]]],
                }],
            }],
        }
    }

    #[test]
    fn identical_text_and_geometry_pass() {
        let report = compare(
            &volume("猫が好き", [0.0, 0.0, 10.0, 20.0]),
            &volume("猫が好き", [0.0, 0.0, 10.0, 20.0]),
        )
        .unwrap();
        assert!(report.comparable);
        assert_eq!(report.matched_character_error_rate, 0.0);
        assert_eq!(report.region_recall, 1.0);
    }
    #[test]
    fn compares_visible_geometry_without_mutating_reference() {
        let reference = volume("猫", [-4., 0., 10., 20.]);
        let report = compare(&reference, &volume("猫", [0., 0., 10., 20.])).unwrap();
        assert!(report.comparable);
        assert_eq!(report.panels[0].matches[0].iou, 1.0);
        assert_eq!(reference.pages[0].blocks[0].bounds[0], -4.);
    }
    #[test]
    fn counts_unicode_characters_instead_of_utf8_bytes() {
        let report = compare(
            &volume("猫が好き", [0.0, 0.0, 10.0, 20.0]),
            &volume("犬が好き", [0.0, 0.0, 10.0, 20.0]),
        )
        .unwrap();
        assert_eq!(report.matched_character_error_rate, 0.25);
        assert!(!report.comparable);
    }
    #[test]
    fn omitted_regions_fail_even_if_remaining_text_agrees() {
        let reference = volume("猫", [0.0, 0.0, 10.0, 20.0]);
        let mut candidate = volume("猫", [0.0, 0.0, 10.0, 20.0]);
        candidate.pages[0].blocks.clear();
        let report = compare(&reference, &candidate).unwrap();
        assert_eq!(report.region_recall, 0.0);
        assert_eq!(report.panel_character_error_rate, 1.0);
        assert!(!report.comparable);
    }
    #[test]
    fn rejects_missing_and_duplicate_pages() {
        let reference = volume("猫", [0.0, 0.0, 10.0, 20.0]);
        let mut candidate = volume("猫", [0.0, 0.0, 10.0, 20.0]);
        candidate.pages.clear();
        assert!(compare(&reference, &candidate).is_err());
        candidate
            .pages
            .extend(volume("猫", [0.0, 0.0, 10.0, 20.0]).pages);
        candidate
            .pages
            .extend(volume("猫", [0.0, 0.0, 10.0, 20.0]).pages);
        assert!(compare(&reference, &candidate).is_err());
    }
}
