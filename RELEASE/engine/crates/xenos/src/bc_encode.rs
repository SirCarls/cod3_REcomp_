//! Block-compression encoders: RGBA8 to BC1 (DXT1), BC2 (DXT3), BC3 (DXT5)
//! and BC5 (DXN / ATI2), for the HD texture pack.
//!
//! The block layouts are the ones `bc` decodes (Microsoft's "Texture Block
//! Compression in Direct3D 11", see docs/SOURCES.md), and every palette is
//! computed by the decoder's own functions, so the error the encoder
//! minimizes is the error the decoded texture shows.
//!
//! Color blocks (BC1, and the color half of BC2/BC3):
//!
//! 1. The endpoints start at the ends of the block's principal axis, the
//!    line through the colors' mean along which they vary most.
//! 2. Then a few rounds of: pick the nearest palette entry for every texel,
//!    and solve for the endpoints that best fit those picks (least squares).
//! 3. Finally each stored endpoint component is moved by one step (5 or 6
//!    bits) while that lowers the error, which fixes rounding losses.
//!
//! Single-color blocks use a table of the best endpoint pair per 8-bit value
//! instead, so a solid color comes out exact or one step off. Opaque BC1
//! blocks that don't fit well also try the three-color mode. BC1 blocks with
//! transparent texels (alpha below 128) fit only their opaque texels in the
//! three-color mode and give the others the transparent index.
//!
//! BC4-style blocks (BC3 alpha, both BC5 channels) are fitted the same way in
//! one dimension, in the 8-value mode and in the 6-value mode (which has
//! exact 0 and 255 entries); the better one gets the one-step search.
//!
//! On a set of 2x upscaled game textures this comes out slightly better than
//! ImageMagick's cluster fit (BC1 44.3 against 43.8 dB PSNR).

use crate::bc::{color_palette, expand5, expand6, interpolated_palette, lerp, BlockFormat};
use std::sync::OnceLock;

/// Pixels per block side.
const BLOCK: usize = 4;
/// Texels per block.
const TEXELS: usize = BLOCK * BLOCK;
/// BC1 texels with alpha below this become transparent (the usual
/// alpha-test reference value).
pub const BC1_ALPHA_THRESHOLD: u8 = 128;
/// Rounds of "pick the indices, then refit the endpoints to them". The error
/// rarely improves after two or three.
const REFIT_ROUNDS: usize = 4;
/// Passes of the one-step endpoint search after the refit.
const NUDGE_PASSES: usize = 4;
/// Opaque BC1 blocks whose four-color error (summed over texels and RGB) is
/// at most this skip trying the three-color mode: 144 is about 1.7 per
/// channel, where the try rarely wins but doubles the time.
const THREE_COLOR_TRY_ERROR: u32 = 144;

/// The 16 RGBA texels of a block, row by row.
type BlockTexels = [[u8; 4]; TEXELS];

/// Encodes `width` x `height` RGBA8 pixels (rows top to bottom) as linear
/// block data in `format`: blocks left to right, top to bottom, as
/// `bc::decode` reads them. Blocks reaching past the right or bottom edge
/// repeat the edge texels, so small mip levels keep their real colors.
///
/// BC1 blocks with a texel whose alpha is below 128 use the three-color mode
/// with transparent texels; give opaque alpha to get only opaque blocks.
/// BC5 encodes red and green. Panics if `rgba` has the wrong length.
pub fn encode(format: BlockFormat, width: usize, height: usize, rgba: &[u8]) -> Vec<u8> {
    assert_eq!(rgba.len(), width * height * 4, "pixel data size");
    let blocks_wide = width.div_ceil(BLOCK);
    let blocks_high = height.div_ceil(BLOCK);
    let mut out = Vec::with_capacity(blocks_wide * blocks_high * format.block_bytes());
    for by in 0..blocks_high {
        for bx in 0..blocks_wide {
            let block: BlockTexels = std::array::from_fn(|i| {
                let x = (bx * BLOCK + i % BLOCK).min(width - 1);
                let y = (by * BLOCK + i / BLOCK).min(height - 1);
                let at = (y * width + x) * 4;
                [rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3]]
            });
            let channel = |c: usize| -> [u8; TEXELS] { std::array::from_fn(|i| block[i][c]) };
            match format {
                BlockFormat::Bc1 => out.extend_from_slice(&encode_color(&block, ColorKind::Bc1)),
                BlockFormat::Bc2 => {
                    out.extend_from_slice(&encode_explicit_alpha(&channel(3)));
                    out.extend_from_slice(&encode_color(&block, ColorKind::FourColorOnly));
                }
                BlockFormat::Bc3 => {
                    out.extend_from_slice(&encode_interpolated(&channel(3)));
                    out.extend_from_slice(&encode_color(&block, ColorKind::FourColorOnly));
                }
                BlockFormat::Bc5 => {
                    out.extend_from_slice(&encode_interpolated(&channel(0)));
                    out.extend_from_slice(&encode_interpolated(&channel(1)));
                }
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------
// Color blocks

/// Which color block is being encoded.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ColorKind {
    /// A BC1 block: four-color or three-color mode, transparent texels allowed.
    Bc1,
    /// The color half of a BC2/BC3 block, which is always read in four-color
    /// mode (and some old decoders misread c0 <= c1, so it is avoided).
    FourColorOnly,
}

/// The two BC1 palette layouts.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ColorMode {
    /// c0 > c1: c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1.
    Four,
    /// c0 <= c1: c0, c1, (c0 + c1) / 2, and index 3 for transparent texels.
    Three,
}

impl ColorMode {
    /// Palette entries an opaque texel may use.
    fn usable_entries(self) -> usize {
        match self {
            ColorMode::Four => 4,
            ColorMode::Three => 3,
        }
    }

    /// How far palette entry `index` lies from endpoint 0 toward endpoint 1.
    fn fraction(self, index: u8) -> f32 {
        match (self, index) {
            (_, 0) => 0.0,
            (_, 1) => 1.0,
            (ColorMode::Four, 2) => 1.0 / 3.0,
            (ColorMode::Four, _) => 2.0 / 3.0,
            (ColorMode::Three, _) => 0.5,
        }
    }
}

/// An endpoint in its stored precision: red 0-31, green 0-63, blue 0-31.
type Endpoint = [i32; 3];
const ENDPOINT_MAX: Endpoint = [31, 63, 31];

fn pack565(e: Endpoint) -> u16 {
    ((e[0] << 11) | (e[1] << 5) | e[2]) as u16
}

/// The nearest stored endpoint to an 8-bit color (clamped to 0-255).
fn quantize(color: [f32; 3]) -> Endpoint {
    std::array::from_fn(|c| {
        (color[c].clamp(0.0, 255.0) * ENDPOINT_MAX[c] as f32 / 255.0).round() as i32
    })
}

/// The texels a color fit works on.
struct ColorTexels {
    rgb: [[i32; 3]; TEXELS],
    /// Texels that count; the others are transparent and get index 3.
    active: [bool; TEXELS],
}

/// Endpoints, the palette index of every texel, and the summed squared error.
#[derive(Debug, Clone, Copy)]
struct ColorFit {
    ends: [Endpoint; 2],
    indices: [u8; TEXELS],
    error: u32,
}

/// Picks the nearest palette entry for every active texel.
fn evaluate(texels: &ColorTexels, ends: [Endpoint; 2], mode: ColorMode) -> ColorFit {
    evaluate_below(texels, ends, mode, u32::MAX).expect("errors stay far below u32::MAX")
}

/// `evaluate`, but giving up (`None`) as soon as the error reaches `limit`:
/// most candidates of the one-step search lose, and this stops them early.
fn evaluate_below(
    texels: &ColorTexels,
    ends: [Endpoint; 2],
    mode: ColorMode,
    limit: u32,
) -> Option<ColorFit> {
    let palette = color_palette(pack565(ends[0]), pack565(ends[1]), mode == ColorMode::Four);
    let palette: [[i32; 3]; 4] =
        std::array::from_fn(|k| std::array::from_fn(|c| palette[k][c] as i32));
    let usable = &palette[..mode.usable_entries()];
    let mut indices = [3u8; TEXELS];
    let mut error = 0u32;
    for (i, texel) in texels.rgb.iter().enumerate() {
        if !texels.active[i] {
            continue;
        }
        let mut best = (u32::MAX, 0u8);
        for (k, entry) in usable.iter().enumerate() {
            let d: i32 = (0..3).map(|c| (texel[c] - entry[c]).pow(2)).sum();
            if (d as u32) < best.0 {
                best = (d as u32, k as u8);
            }
        }
        indices[i] = best.1;
        error += best.0;
        if error >= limit {
            return None;
        }
    }
    Some(ColorFit {
        ends,
        indices,
        error,
    })
}

/// For every 8-bit value, the endpoint pair (5 or 6 bits each) whose palette
/// entry `num / den` of the way from endpoint 0 to endpoint 1 comes closest.
fn single_color_table(bits: u32, num: u32, den: u32) -> [[u8; 2]; 256] {
    let expand = if bits == 5 { expand5 } else { expand6 };
    let levels = 1u16 << bits;
    std::array::from_fn(|value| {
        let mut best = ((i32::MAX, i32::MAX), [0u8; 2]);
        for a in 0..levels {
            for b in 0..levels {
                let (ea, eb) = (expand(a), expand(b));
                let error = (lerp(ea, eb, num, den) as i32 - value as i32).abs();
                // On a tie, endpoints closer together: other decoders round
                // interpolated entries a little differently, and a narrow
                // pair keeps that difference small.
                let key = (error, (ea as i32 - eb as i32).abs());
                if key < best.0 {
                    best = (key, [a as u8, b as u8]);
                }
            }
        }
        best.1
    })
}

/// Single-color tables: [red/blue (5 bits), green (6 bits)] for the
/// four-color entry 2 (1/3 of the way) and the three-color entry 2 (half way).
struct SingleColorTables {
    four: [[[u8; 2]; 256]; 2],
    three: [[[u8; 2]; 256]; 2],
}

fn single_color_tables() -> &'static SingleColorTables {
    static TABLES: OnceLock<SingleColorTables> = OnceLock::new();
    TABLES.get_or_init(|| SingleColorTables {
        four: [single_color_table(5, 1, 3), single_color_table(6, 1, 3)],
        three: [single_color_table(5, 1, 2), single_color_table(6, 1, 2)],
    })
}

/// The fit for texels that all have one color, aimed at palette entry 2.
fn solid_fit(texels: &ColorTexels, color: [i32; 3], mode: ColorMode) -> ColorFit {
    let tables = single_color_tables();
    let table = match mode {
        ColorMode::Four => &tables.four,
        ColorMode::Three => &tables.three,
    };
    let pair = |c: usize| table[(c == 1) as usize][color[c] as usize];
    let (r, g, b) = (pair(0), pair(1), pair(2));
    let ends = [
        [r[0] as i32, g[0] as i32, b[0] as i32],
        [r[1] as i32, g[1] as i32, b[1] as i32],
    ];
    evaluate(texels, ends, mode)
}

/// The ends of the principal axis of `points` (at least two distinct colors):
/// the mean plus the axis direction times the smallest and largest
/// projection.
fn principal_axis_ends(points: &[[i32; 3]]) -> [[f32; 3]; 2] {
    let n = points.len() as f32;
    let mut mean = [0f32; 3];
    for p in points {
        for c in 0..3 {
            mean[c] += p[c] as f32 / n;
        }
    }
    // Covariance matrix (symmetric).
    let mut cov = [[0f32; 3]; 3];
    for p in points {
        let d: [f32; 3] = std::array::from_fn(|c| p[c] as f32 - mean[c]);
        for i in 0..3 {
            for j in 0..3 {
                cov[i][j] += d[i] * d[j];
            }
        }
    }
    // Power iteration for the dominant eigenvector. It starts from the
    // covariance column of the channel that varies most, which is not zero
    // when the colors differ (a fixed start such as gray can be orthogonal
    // to the axis, e.g. for red against green).
    let widest = (0..3)
        .max_by(|&a, &b| cov[a][a].total_cmp(&cov[b][b]))
        .unwrap_or(0);
    let mut axis = cov[widest];
    for _ in 0..8 {
        let next: [f32; 3] = std::array::from_fn(|i| (0..3).map(|j| cov[i][j] * axis[j]).sum());
        let largest = next.iter().fold(0f32, |m, v| m.max(v.abs()));
        if largest <= f32::EPSILON {
            break;
        }
        axis = next.map(|v| v / largest);
    }
    let length = axis.iter().map(|v| v * v).sum::<f32>().sqrt();
    if length <= f32::EPSILON {
        axis = [1.0, 1.0, 1.0];
    }
    let length = axis.iter().map(|v| v * v).sum::<f32>().sqrt();
    let axis = axis.map(|v| v / length);
    let (mut t_min, mut t_max) = (f32::MAX, f32::MIN);
    for p in points {
        let t: f32 = (0..3).map(|c| (p[c] as f32 - mean[c]) * axis[c]).sum();
        t_min = t_min.min(t);
        t_max = t_max.max(t);
    }
    [
        std::array::from_fn(|c| mean[c] + axis[c] * t_min),
        std::array::from_fn(|c| mean[c] + axis[c] * t_max),
    ]
}

/// The endpoints (as 8-bit colors) that best fit the current indices in the
/// least-squares sense, or `None` if every texel uses one fraction.
fn refit(texels: &ColorTexels, fit: &ColorFit, mode: ColorMode) -> Option<[[f32; 3]; 2]> {
    // Each texel x is modeled as (1 - t) * e0 + t * e1; the normal equations
    // are [aa ab; ab bb] [e0; e1] = [ax; bx] per channel.
    let (mut aa, mut ab, mut bb) = (0f32, 0f32, 0f32);
    let (mut ax, mut bx) = ([0f32; 3], [0f32; 3]);
    for (i, texel) in texels.rgb.iter().enumerate() {
        if !texels.active[i] {
            continue;
        }
        let t = mode.fraction(fit.indices[i]);
        let s = 1.0 - t;
        aa += s * s;
        ab += s * t;
        bb += t * t;
        for c in 0..3 {
            ax[c] += s * texel[c] as f32;
            bx[c] += t * texel[c] as f32;
        }
    }
    let det = aa * bb - ab * ab;
    if det.abs() < 1e-4 {
        return None;
    }
    Some([
        std::array::from_fn(|c| (bb * ax[c] - ab * bx[c]) / det),
        std::array::from_fn(|c| (aa * bx[c] - ab * ax[c]) / det),
    ])
}

/// Moves single endpoint components by one step while that lowers the error.
fn nudge(texels: &ColorTexels, mut best: ColorFit, mode: ColorMode) -> ColorFit {
    for _ in 0..NUDGE_PASSES {
        let error_before = best.error;
        for end in 0..2 {
            for channel in 0..3 {
                for step in [-1, 1] {
                    let value = best.ends[end][channel] + step;
                    if !(0..=ENDPOINT_MAX[channel]).contains(&value) {
                        continue;
                    }
                    let mut ends = best.ends;
                    ends[end][channel] = value;
                    if let Some(better) = evaluate_below(texels, ends, mode, best.error) {
                        best = better;
                    }
                }
            }
        }
        if best.error == error_before || best.error == 0 {
            break;
        }
    }
    best
}

/// The best fit of the active texels (at least one) in `mode`.
fn fit_color(texels: &ColorTexels, mode: ColorMode) -> ColorFit {
    let mut points = [[0i32; 3]; TEXELS];
    let mut count = 0;
    for (i, texel) in texels.rgb.iter().enumerate() {
        if texels.active[i] {
            points[count] = *texel;
            count += 1;
        }
    }
    let points = &points[..count];
    if points.iter().all(|p| *p == points[0]) {
        return solid_fit(texels, points[0], mode);
    }
    let [lo, hi] = principal_axis_ends(points);
    let mut best = evaluate(texels, [quantize(lo), quantize(hi)], mode);
    for _ in 0..REFIT_ROUNDS {
        if best.error == 0 {
            break;
        }
        let Some([e0, e1]) = refit(texels, &best, mode) else {
            break;
        };
        let candidate = evaluate(texels, [quantize(e0), quantize(e1)], mode);
        if candidate.error >= best.error {
            break;
        }
        best = candidate;
    }
    nudge(texels, best, mode)
}

/// The 8 block bytes for `fit`, with the endpoints ordered as `mode` needs.
fn color_block_bytes(fit: &ColorFit, mode: ColorMode) -> [u8; 8] {
    let (mut c0, mut c1) = (pack565(fit.ends[0]), pack565(fit.ends[1]));
    let mut indices = fit.indices;
    match mode {
        ColorMode::Four if c0 < c1 => {
            std::mem::swap(&mut c0, &mut c1);
            // Swapping the endpoints swaps entries 0/1 and 2/3.
            indices.iter_mut().for_each(|i| *i ^= 1);
        }
        ColorMode::Four if c0 == c1 => {
            // Every entry is the same color, and BC1 reads equal endpoints
            // in three-color mode, where index 3 is transparent.
            indices = [0; TEXELS];
        }
        ColorMode::Three if c0 > c1 => {
            std::mem::swap(&mut c0, &mut c1);
            // Entries 0 and 1 swap; the average and transparent stay.
            indices.iter_mut().filter(|i| **i < 2).for_each(|i| *i ^= 1);
        }
        _ => {}
    }
    let bits = indices
        .iter()
        .enumerate()
        .fold(0u32, |bits, (i, &index)| bits | ((index as u32) << (2 * i)));
    let mut out = [0u8; 8];
    out[..2].copy_from_slice(&c0.to_le_bytes());
    out[2..4].copy_from_slice(&c1.to_le_bytes());
    out[4..].copy_from_slice(&bits.to_le_bytes());
    out
}

/// One color block: BC1 (with transparency) or the color half of BC2/BC3.
fn encode_color(block: &BlockTexels, kind: ColorKind) -> [u8; 8] {
    let transparent: [bool; TEXELS] =
        std::array::from_fn(|i| kind == ColorKind::Bc1 && block[i][3] < BC1_ALPHA_THRESHOLD);
    if transparent.iter().all(|&t| t) {
        // c0 = c1 selects three-color mode; every index 3 is transparent.
        return [0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF];
    }
    let texels = ColorTexels {
        rgb: std::array::from_fn(|i| std::array::from_fn(|c| block[i][c] as i32)),
        active: transparent.map(|t| !t),
    };
    if transparent.iter().any(|&t| t) {
        return color_block_bytes(&fit_color(&texels, ColorMode::Three), ColorMode::Three);
    }
    let four = fit_color(&texels, ColorMode::Four);
    if kind == ColorKind::Bc1 && four.error > THREE_COLOR_TRY_ERROR {
        // The three-color mode (without its transparent entry) sometimes
        // fits better, for instance colors that cluster at both ends.
        let three = fit_color(&texels, ColorMode::Three);
        if three.error < four.error {
            return color_block_bytes(&three, ColorMode::Three);
        }
    }
    color_block_bytes(&four, ColorMode::Four)
}

// ---------------------------------------------------------------------------
// Alpha and single-channel blocks

/// BC2 alpha: each texel's nearest 4-bit value (decoded as `nibble * 17`).
fn encode_explicit_alpha(alpha: &[u8; TEXELS]) -> [u8; 8] {
    let mut out = [0u8; 8];
    for (i, &a) in alpha.iter().enumerate() {
        let nibble = ((a as u32 + 8) / 17).min(15) as u8;
        out[i / 2] |= nibble << (4 * (i % 2));
    }
    out
}

/// The two BC4 palette layouts.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum InterpolatedMode {
    /// a0 > a1: the endpoints and six values between them.
    Eight,
    /// a0 <= a1: the endpoints, four values between them, 0 and 255.
    Six,
}

impl InterpolatedMode {
    /// How far entry `index` lies from a0 toward a1, or `None` for the fixed
    /// 0 and 255 entries of the six-value mode.
    fn fraction(self, index: u8) -> Option<f32> {
        match (self, index) {
            (_, 0) => Some(0.0),
            (_, 1) => Some(1.0),
            (InterpolatedMode::Eight, k) => Some((k - 1) as f32 / 7.0),
            (InterpolatedMode::Six, 6 | 7) => None,
            (InterpolatedMode::Six, k) => Some((k - 1) as f32 / 5.0),
        }
    }

    /// True if the endpoint pair selects this mode in the decoder.
    fn accepts(self, a0: i32, a1: i32) -> bool {
        let in_range = (0..=255).contains(&a0) && (0..=255).contains(&a1);
        in_range
            && match self {
                InterpolatedMode::Eight => a0 > a1,
                InterpolatedMode::Six => a0 <= a1,
            }
    }
}

#[derive(Debug, Clone, Copy)]
struct InterpolatedFit {
    a0: u8,
    a1: u8,
    indices: [u8; TEXELS],
    error: u32,
}

/// Picks the nearest palette entry for every value.
fn evaluate_interpolated(values: &[u8; TEXELS], a0: u8, a1: u8) -> InterpolatedFit {
    evaluate_interpolated_below(values, a0, a1, u32::MAX).expect("errors stay far below u32::MAX")
}

/// `evaluate_interpolated`, but giving up as soon as the error reaches
/// `limit` (see `evaluate_below`).
fn evaluate_interpolated_below(
    values: &[u8; TEXELS],
    a0: u8,
    a1: u8,
    limit: u32,
) -> Option<InterpolatedFit> {
    let palette = interpolated_palette(a0, a1);
    let mut indices = [0u8; TEXELS];
    let mut error = 0u32;
    for (i, &v) in values.iter().enumerate() {
        let mut best = (u32::MAX, 0u8);
        for (k, &entry) in palette.iter().enumerate() {
            let d = (v as i32 - entry as i32).pow(2) as u32;
            if d < best.0 {
                best = (d, k as u8);
            }
        }
        indices[i] = best.1;
        error += best.0;
        if error >= limit {
            return None;
        }
    }
    Some(InterpolatedFit {
        a0,
        a1,
        indices,
        error,
    })
}

/// The endpoints for `values` (not all equal) in `mode` from their range
/// and a least-squares refit, before `nudge_interpolated`.
fn fit_interpolated(values: &[u8; TEXELS], mode: InterpolatedMode) -> InterpolatedFit {
    // The six-value mode spends its endpoints on the values that the fixed
    // 0 and 255 entries don't already cover.
    let spanned = values
        .iter()
        .copied()
        .filter(|&v| mode == InterpolatedMode::Eight || (v != 0 && v != 255));
    let (lo, hi) = spanned.fold((255u8, 0u8), |(lo, hi), v| (lo.min(v), hi.max(v)));
    if lo > hi {
        // Only 0s and 255s, which the six-value mode holds exactly.
        return evaluate_interpolated(values, 0, 0);
    }
    let (a0, a1) = match mode {
        InterpolatedMode::Eight => (hi, lo),
        InterpolatedMode::Six => (lo, hi),
    };
    let mut best = evaluate_interpolated(values, a0, a1);

    for _ in 0..REFIT_ROUNDS {
        if best.error == 0 {
            break;
        }
        // Least squares as for colors, over the texels on interpolated entries.
        let (mut aa, mut ab, mut bb, mut ax, mut bx) = (0f32, 0f32, 0f32, 0f32, 0f32);
        for (i, &v) in values.iter().enumerate() {
            if let Some(t) = mode.fraction(best.indices[i]) {
                let s = 1.0 - t;
                aa += s * s;
                ab += s * t;
                bb += t * t;
                ax += s * v as f32;
                bx += t * v as f32;
            }
        }
        let det = aa * bb - ab * ab;
        if det.abs() < 1e-4 {
            break;
        }
        let a0 = ((bb * ax - ab * bx) / det).round() as i32;
        let a1 = ((aa * bx - ab * ax) / det).round() as i32;
        let (a0, a1) = (a0.clamp(0, 255), a1.clamp(0, 255));
        if !mode.accepts(a0, a1) {
            break;
        }
        let candidate = evaluate_interpolated(values, a0 as u8, a1 as u8);
        if candidate.error >= best.error {
            break;
        }
        best = candidate;
    }
    best
}

/// Moves either or both endpoints by one while the error drops.
fn nudge_interpolated(
    values: &[u8; TEXELS],
    mut best: InterpolatedFit,
    mode: InterpolatedMode,
) -> InterpolatedFit {
    const MOVES: [(i32, i32); 8] = [
        (-1, 0),
        (1, 0),
        (0, -1),
        (0, 1),
        (-1, -1),
        (1, 1),
        (-1, 1),
        (1, -1),
    ];
    for _ in 0..NUDGE_PASSES {
        let error_before = best.error;
        for (d0, d1) in MOVES {
            let (a0, a1) = (best.a0 as i32 + d0, best.a1 as i32 + d1);
            if !mode.accepts(a0, a1) {
                continue;
            }
            if let Some(better) =
                evaluate_interpolated_below(values, a0 as u8, a1 as u8, best.error)
            {
                best = better;
            }
        }
        if best.error == error_before || best.error == 0 {
            break;
        }
    }
    best
}

/// One BC4-style block (BC3 alpha, a BC5 channel).
fn encode_interpolated(values: &[u8; TEXELS]) -> [u8; 8] {
    let fit = if values.iter().all(|&v| v == values[0]) {
        // Equal endpoints (six-value mode): entry 0 is the value itself.
        evaluate_interpolated(values, values[0], values[0])
    } else {
        // Only the better mode gets the (slower) one-step search; the other
        // rarely overtakes it.
        let eight = fit_interpolated(values, InterpolatedMode::Eight);
        let six = fit_interpolated(values, InterpolatedMode::Six);
        if six.error < eight.error {
            nudge_interpolated(values, six, InterpolatedMode::Six)
        } else {
            nudge_interpolated(values, eight, InterpolatedMode::Eight)
        }
    };
    let bits = fit
        .indices
        .iter()
        .enumerate()
        .fold(0u64, |bits, (i, &index)| bits | ((index as u64) << (3 * i)));
    let mut out = [0u8; 8];
    out[0] = fit.a0;
    out[1] = fit.a1;
    out[2..].copy_from_slice(&bits.to_le_bytes()[..6]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bc::decode;

    /// A small deterministic pseudo-random generator (64-bit LCG), so the
    /// tests need no crates and always see the same data.
    struct Lcg(u64);
    impl Lcg {
        fn byte(&mut self) -> u8 {
            self.0 = self
                .0
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1_442_695_040_888_963_407);
            (self.0 >> 56) as u8
        }
    }

    /// Root mean square error over the given channels.
    fn rmse(a: &[u8], b: &[u8], channels: &[usize]) -> f64 {
        let mut sum = 0f64;
        let mut count = 0usize;
        for (pa, pb) in a.chunks_exact(4).zip(b.chunks_exact(4)) {
            for &c in channels {
                sum += (pa[c] as f64 - pb[c] as f64).powi(2);
                count += 1;
            }
        }
        (sum / count as f64).sqrt()
    }

    fn round_trip(format: BlockFormat, w: usize, h: usize, rgba: &[u8]) -> Vec<u8> {
        let blocks = encode(format, w, h, rgba);
        assert_eq!(
            blocks.len(),
            w.div_ceil(4) * h.div_ceil(4) * format.block_bytes()
        );
        decode(format, w, h, &blocks).unwrap()
    }

    /// The image with alpha 255 everywhere (what BC1 is given for opaque
    /// textures; lower alpha would make texels transparent).
    fn opaque(rgba: &[u8]) -> Vec<u8> {
        rgba.chunks_exact(4)
            .flat_map(|p| [p[0], p[1], p[2], 255])
            .collect()
    }

    /// The RMSE of `format` on `image`: red and green for BC5, else RGB.
    fn color_rmse(format: BlockFormat, w: usize, h: usize, image: &[u8]) -> f64 {
        let image = if format == BlockFormat::Bc1 {
            opaque(image)
        } else {
            image.to_vec()
        };
        let out = round_trip(format, w, h, &image);
        if format == BlockFormat::Bc5 {
            rmse(&image, &out, &[0, 1])
        } else {
            rmse(&image, &out, &[0, 1, 2])
        }
    }

    /// A 64 x 64 image of smooth gradients in every channel, plus `noise`.
    fn gradient(noise: u8, seed: u64) -> Vec<u8> {
        let mut rng = Lcg(seed);
        let mut out = Vec::new();
        for y in 0..64i32 {
            for x in 0..64i32 {
                let base = [x * 4, y * 4, (x + y) * 2, 255 - x * 3];
                for v in base {
                    let jitter = if noise == 0 {
                        0
                    } else {
                        (rng.byte() % (2 * noise + 1)) as i32 - noise as i32
                    };
                    out.push((v + jitter).clamp(0, 255) as u8);
                }
            }
        }
        out
    }

    const FORMATS: [BlockFormat; 4] = [
        BlockFormat::Bc1,
        BlockFormat::Bc2,
        BlockFormat::Bc3,
        BlockFormat::Bc5,
    ];

    #[test]
    fn solid_colors_are_exact_or_one_step_off() {
        let mut rng = Lcg(1);
        for _ in 0..300 {
            let color = [rng.byte(), rng.byte(), rng.byte(), rng.byte()];
            for format in FORMATS {
                // BC1 would turn alpha below 128 into transparent black.
                let alpha = if format == BlockFormat::Bc1 {
                    255
                } else {
                    color[3]
                };
                let texel = [color[0], color[1], color[2], alpha];
                let rgba: Vec<u8> = texel.iter().copied().cycle().take(64).collect();
                let out = round_trip(format, 4, 4, &rgba);
                for p in out.chunks_exact(4) {
                    let off = |c: usize| (p[c] as i32 - color[c] as i32).abs();
                    match format {
                        BlockFormat::Bc5 => assert_eq!((off(0), off(1)), (0, 0)),
                        _ => assert!(off(0) <= 1 && off(1) <= 1 && off(2) <= 1, "{color:?}"),
                    }
                    match format {
                        BlockFormat::Bc2 => assert!(off(3) <= 8),
                        BlockFormat::Bc3 => assert_eq!(off(3), 0),
                        _ => {}
                    }
                }
            }
        }
        // Colors an endpoint can hold come out exact.
        let exact = [expand5(9), expand6(40), expand5(30), 255];
        let rgba: Vec<u8> = exact.iter().copied().cycle().take(64).collect();
        for format in [BlockFormat::Bc1, BlockFormat::Bc2, BlockFormat::Bc3] {
            assert_eq!(round_trip(format, 4, 4, &rgba), rgba);
        }
    }

    #[test]
    fn smooth_gradients() {
        let image = gradient(0, 0);
        for format in FORMATS {
            let error = color_rmse(format, 64, 64, &image);
            // Red follows x and green y, so a block's colors fill a plane that
            // four colors on a line only approximate: ImageMagick's cluster
            // fit gets 2.98 on this image, this encoder 2.97. BC5 gets 0.70:
            // steps of 4 would fit exactly in a range widened to 14, but
            // searching for that costs more than it gains on real textures.
            let limit = if format == BlockFormat::Bc5 {
                0.75
            } else {
                3.3
            };
            assert!(error < limit, "{format:?} rmse {error}");
        }
        let bc3 = round_trip(BlockFormat::Bc3, 64, 64, &image);
        assert!(rmse(&image, &bc3, &[3]) < 0.6);
    }

    #[test]
    fn noisy_gradients() {
        let image = gradient(10, 7);
        for format in FORMATS {
            let error = color_rmse(format, 64, 64, &image);
            // Measured: 5.19 for BC1-BC3, 0.85 for BC5.
            let limit = if format == BlockFormat::Bc5 { 1.0 } else { 5.6 };
            assert!(error < limit, "{format:?} rmse {error}");
        }
        let bc3 = round_trip(BlockFormat::Bc3, 64, 64, &image);
        assert!(rmse(&image, &bc3, &[3]) < 2.5);
    }

    #[test]
    fn random_noise_blocks() {
        // Uniform noise is the worst case for four colors on a line; the
        // limits are what this encoder reaches with some margin, so a
        // regression in the fit shows up here.
        let mut rng = Lcg(3);
        let image: Vec<u8> = (0..32 * 32 * 4).map(|_| rng.byte()).collect();
        let bc1 = color_rmse(BlockFormat::Bc1, 32, 32, &image);
        let bc5 = color_rmse(BlockFormat::Bc5, 32, 32, &image);
        let bc3 = round_trip(BlockFormat::Bc3, 32, 32, &image);
        let alpha = rmse(&image, &bc3, &[3]);
        // Measured: 51.9, 7.27 and 7.41.
        assert!(bc1 < 54.0, "BC1 rmse {bc1}");
        assert!(alpha < 8.0, "BC3 alpha rmse {alpha}");
        assert!(bc5 < 8.0, "BC5 rmse {bc5}");
        // Punch-through follows alpha exactly.
        let out = round_trip(BlockFormat::Bc1, 32, 32, &image);
        for (src, dst) in image.chunks_exact(4).zip(out.chunks_exact(4)) {
            assert_eq!(dst[3], if src[3] < 128 { 0 } else { 255 });
        }
    }

    #[test]
    fn punch_through_alpha() {
        // Top-left block transparent, the block below it half transparent
        // (alpha 50), the rest opaque; the opaque texels alternate between
        // two colors, which the endpoints can hold within rounding.
        let mut rgba = Vec::new();
        for y in 0..8 {
            for x in 0..8 {
                let alpha = if x < 4 && y < 4 {
                    0
                } else if x < 2 {
                    50
                } else {
                    255
                };
                let color = if (x + y) % 2 == 0 {
                    [200, 60, 30]
                } else {
                    [40, 180, 220]
                };
                rgba.extend_from_slice(&[color[0], color[1], color[2], alpha]);
            }
        }
        let out = round_trip(BlockFormat::Bc1, 8, 8, &rgba);
        for (src, dst) in rgba.chunks_exact(4).zip(out.chunks_exact(4)) {
            if src[3] < 128 {
                assert_eq!(dst[3], 0);
            } else {
                assert_eq!(dst[3], 255);
                for c in 0..3 {
                    assert!(
                        (src[c] as i32 - dst[c] as i32).abs() <= 4,
                        "{src:?} {dst:?}"
                    );
                }
            }
        }
        // A fully transparent block, and opaque blocks never turn transparent.
        let clear = vec![0u8; 64];
        assert!(round_trip(BlockFormat::Bc1, 4, 4, &clear)
            .chunks_exact(4)
            .all(|p| p[3] == 0));
        let mut rng = Lcg(5);
        for _ in 0..200 {
            let opaque: Vec<u8> = (0..64)
                .map(|i| if i % 4 == 3 { 255 } else { rng.byte() })
                .collect();
            assert!(round_trip(BlockFormat::Bc1, 4, 4, &opaque)
                .chunks_exact(4)
                .all(|p| p[3] == 255));
        }
    }

    #[test]
    fn interpolated_blocks_use_both_modes() {
        // Values at 0 and 255 plus a narrow cluster: the six-value mode keeps
        // the extremes exact and spends the endpoints on the cluster.
        let mut values = [100u8; 16];
        values[..4].copy_from_slice(&[0, 0, 255, 255]);
        values[4..8].copy_from_slice(&[104, 108, 112, 96]);
        let block = encode_interpolated(&values);
        assert!(block[0] <= block[1], "expected the six-value mode");
        let palette = interpolated_palette(block[0], block[1]);
        let decoded: Vec<u8> = (0..16)
            .map(|i| {
                let bits = u64::from_le_bytes([
                    block[2], block[3], block[4], block[5], block[6], block[7], 0, 0,
                ]);
                palette[((bits >> (3 * i)) & 7) as usize]
            })
            .collect();
        assert_eq!(&decoded[..4], &[0, 0, 255, 255]);
        for (v, d) in values.iter().zip(&decoded) {
            assert!((*v as i32 - *d as i32).abs() <= 2, "{values:?} {decoded:?}");
        }
        // An even ramp suits the eight-value mode.
        let ramp: [u8; 16] = std::array::from_fn(|i| 40 + i as u8 * 10);
        let block = encode_interpolated(&ramp);
        assert!(block[0] > block[1], "expected the eight-value mode");
    }

    #[test]
    fn small_mip_levels() {
        // 2x2 and 1x1 images: the padding repeats edge texels.
        let rgba = [
            10, 20, 30, 255, 200, 210, 220, 255, 10, 20, 30, 255, 200, 210, 220, 255,
        ];
        for format in FORMATS {
            let out = round_trip(format, 2, 2, &rgba);
            let channels: &[usize] = if format == BlockFormat::Bc5 {
                &[0, 1]
            } else {
                &[0, 1, 2]
            };
            assert!(rmse(&rgba, &out, channels) < 4.0, "{format:?}");
            let one = round_trip(format, 1, 1, &rgba[4..8]);
            for &c in channels {
                assert!((one[c] as i32 - rgba[4 + c] as i32).abs() <= 1);
            }
        }
    }
}
