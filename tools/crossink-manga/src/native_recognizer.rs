use crate::inference::{Input, Outputs, Runtime, Session, Tensor, ort::OrtRuntime};
use anyhow::{Context, Result, ensure};
use image::{GrayImage, RgbImage};
use serde::Deserialize;
use std::{fmt, fs, path::Path};

#[cfg(test)]
const DEFAULT_INPUT_SIZE: usize = 224;

#[derive(Debug, Deserialize)]
struct RecognizerConfig {
    format_version: u32,
    input_width: usize,
    input_height: usize,
    image_mean: [f32; 3],
    image_std: [f32; 3],
    decoder_start_token_id: i64,
    eos_token_id: i64,
    pad_token_id: i64,
    max_length: usize,
    vocab_size: usize,
    num_beams: usize,
    length_penalty: f32,
    no_repeat_ngram_size: usize,
    early_stopping: bool,
}

/// Native manga-ocr recognizer backed by exported ONNX encoder and decoder graphs.
pub struct Recognizer {
    encoder: Box<dyn Session>,
    decoder_init: Box<dyn Session>,
    decoder: Box<dyn Session>,
    config: RecognizerConfig,
    vocab: Vec<String>,
}

impl fmt::Debug for Recognizer {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Recognizer")
            .field("vocab_size", &self.vocab.len())
            .finish_non_exhaustive()
    }
}

impl Recognizer {
    pub fn load(models: &Path) -> Result<Self> {
        Self::load_with_runtime(models, &OrtRuntime)
    }

    pub fn load_with_runtime(models: &Path, runtime: &dyn Runtime) -> Result<Self> {
        let config_path = models.join("recognizer.json");
        let config_file = fs::File::open(&config_path)
            .with_context(|| format!("open recognizer config {}", config_path.display()))?;
        let config: RecognizerConfig = serde_json::from_reader(config_file)
            .with_context(|| format!("parse recognizer config {}", config_path.display()))?;
        ensure!(
            config.format_version == 1,
            "unsupported recognizer format version {}",
            config.format_version
        );
        ensure!(
            config.input_width > 0 && config.input_height > 0,
            "recognizer input dimensions must be positive"
        );
        ensure!(
            config.max_length >= 2 && config.max_length <= 512,
            "recognizer max_length is outside 2..=512"
        );
        ensure!(
            config.num_beams == 4,
            "recognizer requires the pinned four-beam search configuration"
        );
        ensure!(
            config.length_penalty.is_finite() && config.length_penalty >= 0.0,
            "recognizer length_penalty must be finite and non-negative"
        );
        ensure!(
            config.no_repeat_ngram_size == 3,
            "recognizer requires the pinned trigram repetition limit"
        );
        ensure!(
            config.early_stopping,
            "recognizer requires pinned early stopping"
        );
        ensure!(
            config.image_std.iter().all(|value| *value > 0.0),
            "recognizer image_std must be positive"
        );

        let vocab_path = models.join("vocab.txt");
        let vocab_text = fs::read_to_string(&vocab_path)
            .with_context(|| format!("read recognizer vocabulary {}", vocab_path.display()))?;
        let vocab: Vec<String> = vocab_text.lines().map(str::to_owned).collect();
        ensure!(
            vocab.len() == config.vocab_size,
            "recognizer vocabulary has {} entries, expected {}",
            vocab.len(),
            config.vocab_size
        );

        let encoder_path = models.join("encoder.onnx");
        let decoder_init_path = models.join("decoder_init.onnx");
        let decoder_path = models.join("decoder.onnx");
        ensure!(
            encoder_path.is_file(),
            "recognizer encoder missing: {}",
            encoder_path.display()
        );
        ensure!(
            decoder_init_path.is_file(),
            "recognizer initial decoder missing: {}",
            decoder_init_path.display()
        );
        ensure!(
            decoder_path.is_file(),
            "recognizer decoder missing: {}",
            decoder_path.display()
        );
        let encoder = runtime.load(&encoder_path)?;
        let decoder_init = runtime.load(&decoder_init_path)?;
        let decoder = runtime.load(&decoder_path)?;

        Ok(Self {
            encoder,
            decoder_init,
            decoder,
            config,
            vocab,
        })
    }

    pub fn recognize(&mut self, image: &RgbImage) -> Result<String> {
        ensure!(
            image.width() > 0 && image.height() > 0,
            "cannot recognize an empty image"
        );
        let pixels = preprocess_with_config(image, &self.config);
        let pixel_values = Tensor::from_array((
            [1, 3, self.config.input_height, self.config.input_width],
            pixels,
        ))
        .context("create recognizer image tensor")?;
        let encoder_outputs = self
            .encoder
            .run(vec![("pixel_values", pixel_values.input())])
            .context("run recognizer encoder")?;
        let hidden_view = encoder_outputs
            .array(0)
            .context("read recognizer encoder output")?;
        ensure!(
            hidden_view.ndim() == 3,
            "recognizer encoder returned a {}-dimensional tensor",
            hidden_view.ndim()
        );
        let hidden_shape = hidden_view.shape().to_vec();
        let hidden_values: Vec<f32> = hidden_view.iter().copied().collect();

        let initial_ids = Tensor::from_array(([1, 1], vec![self.config.decoder_start_token_id]))
            .context("create recognizer initial token tensor")?;
        let initial_outputs = self
            .decoder_init
            .run(vec![
                ("input_ids", initial_ids.input()),
                ("encoder_hidden_states", Input::F32(hidden_view)),
            ])
            .context("run recognizer initial decoder")?;
        let mut state = copy_decoder_outputs(initial_outputs.as_ref(), self.config.vocab_size)?;
        drop(initial_outputs);
        drop(encoder_outputs);

        // Encoder states are identical for every beam and decoding step.
        let mut batched_hidden_shape = hidden_shape;
        batched_hidden_shape[0] = self.config.num_beams;
        let mut batched_hidden = Vec::with_capacity(hidden_values.len() * self.config.num_beams);
        for _ in 0..self.config.num_beams {
            batched_hidden.extend_from_slice(&hidden_values);
        }
        let hidden_states = Tensor::from_array((batched_hidden_shape, batched_hidden))
            .context("create recognizer encoder-state tensor")?;
        drop(hidden_values);
        drop(pixel_values);

        let mut running = vec![Beam {
            tokens: vec![self.config.decoder_start_token_id],
            score: 0.0,
        }];
        let mut finished = Vec::with_capacity(self.config.num_beams);
        while running[0].tokens.len() < self.config.max_length {
            let generated_length = running[0].tokens.len();
            let candidates = top_candidates(
                &state.logits,
                &running,
                self.config.vocab_size,
                self.config.num_beams * 2,
                self.config.no_repeat_ngram_size,
            )?;
            let mut next_running = Vec::with_capacity(self.config.num_beams);
            let mut parent_indices = Vec::with_capacity(self.config.num_beams);
            let mut next_tokens = Vec::with_capacity(self.config.num_beams);
            for (rank, candidate) in candidates.into_iter().enumerate() {
                let reached_limit = generated_length + 1 >= self.config.max_length;
                if candidate.token == self.config.eos_token_id || reached_limit {
                    if rank < self.config.num_beams {
                        let mut tokens = running[candidate.parent].tokens.clone();
                        tokens.push(candidate.token);
                        let normalized = candidate.score
                            / (generated_length as f32).powf(self.config.length_penalty);
                        finished.push(Beam {
                            tokens,
                            score: normalized,
                        });
                        finished.sort_by(|left, right| right.score.total_cmp(&left.score));
                        finished.truncate(self.config.num_beams);
                    }
                } else if next_running.len() < self.config.num_beams {
                    let mut tokens = running[candidate.parent].tokens.clone();
                    tokens.push(candidate.token);
                    next_running.push(Beam {
                        tokens,
                        score: candidate.score,
                    });
                    parent_indices.push(candidate.parent);
                    next_tokens.push(candidate.token);
                }
            }
            if next_running.is_empty()
                || (self.config.early_stopping && finished.len() >= self.config.num_beams)
            {
                break;
            }
            ensure!(
                next_running.len() == self.config.num_beams,
                "recognizer beam search ran out of continuations"
            );

            let input_ids = Tensor::from_array(([self.config.num_beams, 1], next_tokens))
                .context("create recognizer token tensor")?;
            ensure!(
                state.cache.len() == 4,
                "recognizer decoder returned {} cache tensors",
                state.cache.len()
            );
            let mut cache = state.cache.into_iter();
            let key_0 = cache
                .next()
                .context("recognizer cache key 0 missing")?
                .reorder(&parent_indices)?
                .into_tensor()?;
            let value_0 = cache
                .next()
                .context("recognizer cache value 0 missing")?
                .reorder(&parent_indices)?
                .into_tensor()?;
            let key_1 = cache
                .next()
                .context("recognizer cache key 1 missing")?
                .reorder(&parent_indices)?
                .into_tensor()?;
            let value_1 = cache
                .next()
                .context("recognizer cache value 1 missing")?
                .reorder(&parent_indices)?
                .into_tensor()?;
            let decoder_outputs = self
                .decoder
                .run(vec![
                    ("input_ids", input_ids.input()),
                    ("encoder_hidden_states", hidden_states.input()),
                    ("past_key_0", key_0.input()),
                    ("past_value_0", value_0.input()),
                    ("past_key_1", key_1.input()),
                    ("past_value_1", value_1.input()),
                ])
                .context("run recognizer decoder")?;
            state = copy_decoder_outputs(decoder_outputs.as_ref(), self.config.vocab_size)?;
            running = next_running;
        }

        let token_ids = finished
            .first()
            .or_else(|| running.first())
            .context("recognizer beam search produced no hypothesis")?
            .tokens
            .clone();

        let mut decoded = String::new();
        for token_id in token_ids.into_iter().skip(1) {
            if token_id == self.config.pad_token_id
                || token_id == self.config.decoder_start_token_id
                || token_id == self.config.eos_token_id
            {
                continue;
            }
            let token = self.vocab.get(token_id as usize).with_context(|| {
                format!("recognizer produced token outside vocabulary: {token_id}")
            })?;
            if matches!(
                token.as_str(),
                "[UNK]" | "[MASK]" | "[CLS]" | "[SEP]" | "[PAD]"
            ) {
                continue;
            }
            decoded.push_str(token.strip_prefix("##").unwrap_or(token));
        }
        Ok(post_process(&decoded))
    }
}

struct OwnedTensor {
    shape: Vec<usize>,
    values: Vec<f32>,
}

impl OwnedTensor {
    fn into_tensor(self) -> Result<Tensor<f32>> {
        Tensor::from_array((self.shape, self.values)).context("create recognizer cache tensor")
    }

    fn reorder(self, parents: &[usize]) -> Result<Self> {
        ensure!(
            self.shape.len() == 4 && self.shape[0] > 0,
            "recognizer cache has invalid shape {:?}",
            self.shape
        );
        let source_batch = self.shape[0];
        let item_len: usize = self.shape[1..].iter().product();
        ensure!(
            self.values.len() == source_batch * item_len,
            "recognizer cache data does not match its shape"
        );
        let mut values = Vec::with_capacity(parents.len() * item_len);
        for &parent in parents {
            ensure!(
                parent < source_batch,
                "recognizer cache parent {parent} is outside batch {source_batch}"
            );
            values.extend_from_slice(&self.values[parent * item_len..(parent + 1) * item_len]);
        }
        let mut shape = self.shape;
        shape[0] = parents.len();
        Ok(Self { shape, values })
    }
}

#[derive(Clone)]
struct Beam {
    tokens: Vec<i64>,
    score: f32,
}

struct Candidate {
    parent: usize,
    token: i64,
    score: f32,
}

struct DecoderOutputs {
    logits: Vec<f32>,
    cache: Vec<OwnedTensor>,
}

fn top_candidates(
    logits: &[f32],
    beams: &[Beam],
    vocab_size: usize,
    count: usize,
    no_repeat_ngram_size: usize,
) -> Result<Vec<Candidate>> {
    ensure!(
        logits.len() == beams.len() * vocab_size,
        "recognizer logits batch does not match running beams"
    );
    let mut top = Vec::with_capacity(count + 1);
    // Reuse one mask across beams; history is scanned once per beam rather
    // than once for every vocabulary candidate.
    let mut blocked = vec![false; vocab_size];
    for (parent, beam) in beams.iter().enumerate() {
        mark_repeated_tokens(&beam.tokens, no_repeat_ngram_size, &mut blocked);
        let row = &logits[parent * vocab_size..(parent + 1) * vocab_size];
        let max = row.iter().copied().fold(f32::NEG_INFINITY, f32::max);
        let log_sum_exp = row
            .iter()
            .map(|value| (*value - max).exp())
            .sum::<f32>()
            .ln()
            + max;
        for (token, &logit) in row.iter().enumerate() {
            if blocked[token] {
                continue;
            }
            let candidate = Candidate {
                parent,
                token: token as i64,
                score: beam.score + logit - log_sum_exp,
            };
            let position = top
                .iter()
                .position(|existing: &Candidate| candidate.score.total_cmp(&existing.score).is_gt())
                .unwrap_or(top.len());
            if position < count {
                top.insert(position, candidate);
                top.truncate(count);
            }
        }
    }
    ensure!(
        top.len() == count,
        "recognizer decoder did not provide enough beam candidates"
    );
    Ok(top)
}

fn mark_repeated_tokens(tokens: &[i64], size: usize, blocked: &mut [bool]) {
    blocked.fill(false);
    if size == 0 || size > tokens.len() {
        return;
    }
    let prefix = &tokens[tokens.len() - (size - 1)..];
    for window in tokens.windows(size) {
        if window[..size - 1] == *prefix
            && let Ok(next) = usize::try_from(window[size - 1])
            && let Some(entry) = blocked.get_mut(next)
        {
            *entry = true;
        }
    }
}

// Keep the original scalar rule as an independent parity/benchmark reference.
#[cfg(test)]
fn repeats_ngram(tokens: &[i64], next: i64, size: usize) -> bool {
    if size == 0 || tokens.len() + 1 < size {
        return false;
    }
    let prefix = &tokens[tokens.len() - (size - 1)..];
    tokens
        .windows(size)
        .any(|window| window[..size - 1] == *prefix && window[size - 1] == next)
}

fn copy_decoder_outputs(outputs: &dyn Outputs, vocab_size: usize) -> Result<DecoderOutputs> {
    ensure!(
        outputs.len() == 5,
        "recognizer decoder returned {} outputs, expected 5",
        outputs.len()
    );
    let logits = outputs.array(0).context("read recognizer decoder logits")?;
    ensure!(
        logits.ndim() == 3 && logits.shape()[1..] == [1, vocab_size],
        "recognizer decoder returned unexpected logits shape {:?}",
        logits.shape()
    );
    let mut cache = Vec::with_capacity(4);
    for index in 1..outputs.len() {
        let tensor = outputs
            .array(index)
            .context("read recognizer decoder cache")?;
        ensure!(
            tensor.ndim() == 4,
            "recognizer decoder returned a {}-dimensional cache",
            tensor.ndim()
        );
        cache.push(OwnedTensor {
            shape: tensor.shape().to_vec(),
            values: tensor.iter().copied().collect(),
        });
    }
    Ok(DecoderOutputs {
        logits: logits.iter().copied().collect(),
        cache,
    })
}

#[cfg(test)]
fn preprocess(image: &RgbImage) -> Vec<f32> {
    let config = RecognizerConfig {
        format_version: 1,
        input_width: DEFAULT_INPUT_SIZE,
        input_height: DEFAULT_INPUT_SIZE,
        image_mean: [0.5; 3],
        image_std: [0.5; 3],
        decoder_start_token_id: 2,
        eos_token_id: 3,
        pad_token_id: 0,
        max_length: 300,
        vocab_size: 6144,
        num_beams: 4,
        length_penalty: 2.0,
        no_repeat_ngram_size: 3,
        early_stopping: true,
    };
    preprocess_with_config(image, &config)
}

fn preprocess_with_config(image: &RgbImage, config: &RecognizerConfig) -> Vec<f32> {
    let grayscale = GrayImage::from_fn(image.width(), image.height(), |x, y| {
        let [red, green, blue] = image.get_pixel(x, y).0;
        // These are Pillow's fixed-point ITU-R 601-2 coefficients and rounding.
        let luma = (19_595_u32 * u32::from(red)
            + 38_470_u32 * u32::from(green)
            + 7_471_u32 * u32::from(blue)
            + 32_768)
            >> 16;
        image::Luma([luma as u8])
    });
    let resized = pillow_bilinear_resize(&grayscale, config.input_width, config.input_height);
    let plane = config.input_width * config.input_height;
    let mut pixels = Vec::with_capacity(plane * 3);
    for channel in 0..3 {
        pixels.extend(resized.iter().map(|&pixel| {
            let scaled = f32::from(pixel) / 255.0;
            (scaled - config.image_mean[channel]) / config.image_std[channel]
        }));
    }
    pixels
}

const PILLOW_PRECISION_BITS: u32 = 22;

// Coefficient quantization and horizontal/vertical pass ordering follow
// Pillow's src/libImaging/Resample.c bilinear implementation.
struct PillowCoefficients {
    starts: Vec<usize>,
    lengths: Vec<usize>,
    weights: Vec<i32>,
    stride: usize,
}

fn pillow_bilinear_coefficients(input: usize, output: usize) -> PillowCoefficients {
    let scale = input as f64 / output as f64;
    let filter_scale = scale.max(1.0);
    let support = filter_scale;
    let stride = support.ceil() as usize * 2 + 1;
    let mut starts = Vec::with_capacity(output);
    let mut lengths = Vec::with_capacity(output);
    let mut weights = vec![0; output * stride];

    for destination in 0..output {
        let center = (destination as f64 + 0.5) * scale;
        let start = ((center - support + 0.5) as isize).max(0) as usize;
        let end = ((center + support + 0.5) as usize).min(input);
        let length = end - start;
        let offset = destination * stride;
        let mut kernel = Vec::with_capacity(length);
        let mut total = 0.0;
        for source in 0..length {
            let distance = (source as f64 + start as f64 - center + 0.5) / filter_scale;
            let weight = if distance.abs() < 1.0 {
                1.0 - distance.abs()
            } else {
                0.0
            };
            kernel.push(weight);
            total += weight;
        }
        for (source, weight) in kernel.into_iter().enumerate() {
            let weight = weight / total;
            weights[offset + source] =
                (0.5 + weight * f64::from(1_u32 << PILLOW_PRECISION_BITS)) as i32;
        }
        starts.push(start);
        lengths.push(length);
    }

    PillowCoefficients {
        starts,
        lengths,
        weights,
        stride,
    }
}

fn pillow_bilinear_resize(image: &GrayImage, output_width: usize, output_height: usize) -> Vec<u8> {
    let input_width = image.width() as usize;
    let input_height = image.height() as usize;
    let horizontal = if input_width == output_width {
        image.as_raw().clone()
    } else {
        let coefficients = pillow_bilinear_coefficients(input_width, output_width);
        let mut resized = vec![0; output_width * input_height];
        for y in 0..input_height {
            for x in 0..output_width {
                let mut value = 1_i64 << (PILLOW_PRECISION_BITS - 1);
                let coefficient_offset = x * coefficients.stride;
                for index in 0..coefficients.lengths[x] {
                    value +=
                        i64::from(image.as_raw()[y * input_width + coefficients.starts[x] + index])
                            * i64::from(coefficients.weights[coefficient_offset + index]);
                }
                resized[y * output_width + x] =
                    (value >> PILLOW_PRECISION_BITS).clamp(0, 255) as u8;
            }
        }
        resized
    };

    if input_height == output_height {
        return horizontal;
    }
    let coefficients = pillow_bilinear_coefficients(input_height, output_height);
    let mut resized = vec![0; output_width * output_height];
    for y in 0..output_height {
        let coefficient_offset = y * coefficients.stride;
        for x in 0..output_width {
            let mut value = 1_i64 << (PILLOW_PRECISION_BITS - 1);
            for index in 0..coefficients.lengths[y] {
                value += i64::from(horizontal[(coefficients.starts[y] + index) * output_width + x])
                    * i64::from(coefficients.weights[coefficient_offset + index]);
            }
            resized[y * output_width + x] = (value >> PILLOW_PRECISION_BITS).clamp(0, 255) as u8;
        }
    }
    resized
}

fn post_process(text: &str) -> String {
    let compact: String = text
        .chars()
        .filter(|character| !character.is_whitespace())
        .collect();
    let ellipses = compact.replace('…', "...");
    let characters: Vec<char> = ellipses.chars().collect();
    let mut dotted = String::with_capacity(ellipses.len());
    let mut index = 0;
    while index < characters.len() {
        if matches!(characters[index], '・' | '.') {
            let start = index;
            while index < characters.len() && matches!(characters[index], '・' | '.') {
                index += 1;
            }
            if index - start >= 2 {
                dotted.extend(std::iter::repeat_n('．', index - start));
            } else {
                dotted.push(characters[start]);
            }
        } else {
            dotted.push(characters[index]);
            index += 1;
        }
    }
    half_to_full_width(&dotted)
}

fn half_to_full_width(text: &str) -> String {
    const HALF_KANA: &str = "｡｢｣､･ｦｧｨｩｪｫｬｭｮｯｰｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝﾞﾟ";
    const FULL_KANA: &str = "。「」、・ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン゛゜";
    const VOICED: &[(&str, &str)] = &[
        ("ｶﾞ", "ガ"),
        ("ｷﾞ", "ギ"),
        ("ｸﾞ", "グ"),
        ("ｹﾞ", "ゲ"),
        ("ｺﾞ", "ゴ"),
        ("ｻﾞ", "ザ"),
        ("ｼﾞ", "ジ"),
        ("ｽﾞ", "ズ"),
        ("ｾﾞ", "ゼ"),
        ("ｿﾞ", "ゾ"),
        ("ﾀﾞ", "ダ"),
        ("ﾁﾞ", "ヂ"),
        ("ﾂﾞ", "ヅ"),
        ("ﾃﾞ", "デ"),
        ("ﾄﾞ", "ド"),
        ("ﾊﾞ", "バ"),
        ("ﾋﾞ", "ビ"),
        ("ﾌﾞ", "ブ"),
        ("ﾍﾞ", "ベ"),
        ("ﾎﾞ", "ボ"),
        ("ﾊﾟ", "パ"),
        ("ﾋﾟ", "ピ"),
        ("ﾌﾟ", "プ"),
        ("ﾍﾟ", "ペ"),
        ("ﾎﾟ", "ポ"),
        ("ｳﾞ", "ヴ"),
    ];
    let mut converted = text.to_owned();
    for &(half, full) in VOICED {
        converted = converted.replace(half, full);
    }
    converted
        .chars()
        .map(|character| {
            if ('!'..='~').contains(&character) {
                char::from_u32(character as u32 + 0xfee0).unwrap_or(character)
            } else if let Some(position) = HALF_KANA.chars().position(|half| half == character) {
                FULL_KANA.chars().nth(position).unwrap_or(character)
            } else {
                character
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{Rgb, RgbImage};
    use sha2::{Digest, Sha256};

    #[test]
    fn preprocess_matches_manga_ocr_grayscale_normalization() {
        let mut image = RgbImage::new(1, 1);
        image.put_pixel(0, 0, Rgb([255, 0, 0]));

        let pixels = preprocess(&image);

        assert_eq!(pixels.len(), 3 * 224 * 224);
        for &pixel in &pixels {
            assert!((pixel - -0.403_921_57).abs() < 1e-6, "{pixel}");
        }
    }

    #[test]
    fn preprocess_matches_pillow_fixed_point_luma_rounding() {
        let image = RgbImage::from_pixel(1, 1, Rgb([34, 255, 32]));

        let pixels = preprocess(&image);

        let expected = (164.0_f32 / 255.0 - 0.5) / 0.5;
        assert!((pixels[0] - expected).abs() < 1e-6, "{}", pixels[0]);
    }

    #[test]
    fn preprocess_matches_manga_ocr_bilinear_resize() {
        let image = RgbImage::from_fn(2, 2, |x, y| {
            if x == y {
                Rgb([0, 0, 0])
            } else {
                Rgb([255, 255, 255])
            }
        });

        let pixels = preprocess(&image);

        let expected = [
            ((0, 0), -1.0),
            ((111, 0), -0.011_764_706),
            ((112, 0), 0.011_764_706),
            ((55, 55), -1.0),
            ((111, 111), -0.003_921_569),
            ((112, 111), 0.003_921_569),
            ((111, 112), 0.003_921_569),
            ((112, 112), -0.003_921_569),
            ((223, 223), -1.0),
        ];
        for ((x, y), expected) in expected {
            let actual = pixels[y * 224 + x];
            assert!((actual - expected).abs() < 1e-6, "({x}, {y}): {actual}");
        }
    }

    #[test]
    fn resize_matches_pillow_bilinear_fixed_point_rounding() {
        let image = GrayImage::from_fn(317, 61, |x, y| {
            let red = ((x * 17 + y * 29) % 256) as u8;
            let green = ((x * 31 + y * 7) % 256) as u8;
            let blue = ((x * 11 + y * 43) % 256) as u8;
            let luma = (19_595_u32 * u32::from(red)
                + 38_470_u32 * u32::from(green)
                + 7_471_u32 * u32::from(blue)
                + 32_768)
                >> 16;
            image::Luma([luma as u8])
        });

        let resized = pillow_bilinear_resize(&image, 224, 224);

        assert_eq!(
            format!("{:x}", Sha256::digest(&resized)),
            "8dbdc924422f3e163809f83fc29bfae64a24693add78f567c9481d67e43326ea"
        );
    }

    #[test]
    fn post_process_matches_manga_ocr() {
        assert_eq!(post_process("  …・・. A 12 ｶﾞ "), "．．．．．．Ａ１２ガ");
        assert_eq!(post_process("ABC...・・.・"), "ＡＢＣ．．．．．．．");
    }

    #[test]
    fn trigram_filter_rejects_only_an_existing_continuation() {
        let tokens = [2, 7, 8, 7, 8];

        assert!(repeats_ngram(&tokens, 7, 3));
        assert!(!repeats_ngram(&tokens, 9, 3));
        assert!(!repeats_ngram(&tokens, 7, 0));
    }

    #[test]
    fn blocked_token_mask_handles_multiple_continuations_and_resets_between_beams() {
        let mut blocked = vec![false; 10];
        mark_repeated_tokens(&[2, 7, 8, 7, 8, 9, 7, 8], 3, &mut blocked);
        let ids: Vec<_> = blocked
            .iter()
            .enumerate()
            .filter_map(|(id, &value)| value.then_some(id))
            .collect();
        assert_eq!(ids, [7, 9]);
        mark_repeated_tokens(&[2, 3], 3, &mut blocked);
        assert!(blocked.iter().all(|&value| !value));
    }

    #[test]
    fn blocked_token_mask_matches_scalar_rule_for_short_histories() {
        // Exhaust all histories up to six tokens, including IDs outside the
        // vocabulary. These must never become an index into the mask.
        for length in 0..=6 {
            for mut encoded in 0..4_usize.pow(length) {
                let mut tokens = Vec::new();
                for _ in 0..length {
                    tokens.push([-1, 0, 2, 9][encoded % 4]);
                    encoded /= 4;
                }
                for size in 0..=8 {
                    let mut blocked = [true; 5];
                    mark_repeated_tokens(&tokens, size, &mut blocked);
                    for (next, &actual) in blocked.iter().enumerate() {
                        assert_eq!(
                            actual,
                            repeats_ngram(&tokens, next as i64, size),
                            "tokens={tokens:?}, size={size}, next={next}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn candidate_filter_preserves_ties_and_does_not_leak_between_beams() {
        let beams = [
            Beam {
                tokens: vec![0, 1, 2, 0, 1],
                score: 0.0,
            },
            Beam {
                tokens: vec![0, 1, 3, 0, 1],
                score: 0.0,
            },
        ];
        let candidates = top_candidates(&[0.0; 8], &beams, 4, 6, 3).unwrap();
        let ids: Vec<_> = candidates
            .iter()
            .map(|item| (item.parent, item.token))
            .collect();
        assert_eq!(ids, [(0, 0), (0, 1), (0, 3), (1, 0), (1, 1), (1, 2)]);
        for candidate in candidates {
            assert!((candidate.score + 4.0_f32.ln()).abs() < 1e-6);
        }
    }

    #[test]
    fn load_reports_a_missing_model_file() {
        let models = tempfile::tempdir().unwrap();

        let error = Recognizer::load(models.path()).unwrap_err();

        assert!(error.to_string().contains("recognizer.json"), "{error:#}");
    }

    #[test]
    #[ignore = "requires locally exported manga-ocr models and a real line crop"]
    fn real_crop_matches_upstream_manga_ocr() {
        let models = std::env::var("CROSSINK_NATIVE_MODELS").unwrap();
        let crop = std::env::var("CROSSINK_NATIVE_CROP").unwrap();
        let expected = std::env::var("CROSSINK_NATIVE_EXPECTED").unwrap();
        let mut recognizer = Recognizer::load(Path::new(&models)).unwrap();
        let image = image::open(crop).unwrap().to_rgb8();

        assert_eq!(recognizer.recognize(&image).unwrap(), expected);
    }
}
