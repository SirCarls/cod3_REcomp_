//! The HD texture pack: upscaled copies of the game's textures, compressed in
//! each texture's own GPU format, which the recompiled game loads in place of
//! the originals.
//!
//! 1. Ripping (`cod3-all`, `cod3-tex`) lists every texture instance the game
//!    could replace (`xenos::hd_replaceable`: 2D, DXT1/3/5, DXN or 8_8_8_8,
//!    with the usual swizzle) in `textures/hd_keys.tsv`, copies that other
//!    archives hold under the same name included:
//!
//!    ```text
//!    <key>\t<width>\t<height>\t<xenos_format>\t<name>\t<file>\t<kind>
//!    ```
//!
//!    The key is `xenos::hd_key`: XXH64 (seed 0) of the untiled, byte-swapped
//!    base level, which the game's texture cache computes the same way from
//!    guest memory. Width, height and the Xenos format number (18 DXT1,
//!    19 DXT2/3, 20 DXT4/5, 49 DXN, 6 8_8_8_8) come from the fetch constant;
//!    the name is the texture's name. `file` is the PNG in `textures/`
//!    (without extension) that holds exactly this key's picture: `<name>`
//!    for the copy the rip saved under the name, `<name>__<key>` for another
//!    picture under the same name (every level has its own `lm0000`
//!    lightmap, for example), or `-` for a texture the pack leaves as the
//!    game has it (`keep_original`: lightmaps, fog ramps and other lookup
//!    tables, whose values are lighting data, not pictures). `kind` is what
//!    the texture holds, from the material parameters that use it
//!    (`Kind`): `diffuse` (a color picture), `normal`, `specular`,
//!    `detail`, `other` (height maps, masks, ...), `mixed` (used as several
//!    kinds) or `unknown` (no material names it: HUD, fonts, effects, menu
//!    pictures, trees). Lines are sorted by name, then key, after a `#`
//!    header line.
//! 2. An upscaler (the setup tool's waifu2x step) writes
//!    `textures_upscaled/<file>.png` for the files hd_keys.tsv lists with a
//!    kind it upscales: by default only `diffuse`, because upscaling and
//!    denoising a normal, specular or detail map as if it were a picture
//!    changes the lighting (`Kind::upscaled_by_default`). The PNG's size is
//!    not fixed: any scale works (1x, 2x, 3x, 4x, or not a whole number),
//!    as long as the aspect ratio is the original's to within one texel of
//!    rounding on each side and the PNG is not smaller than the original.
//!    Another upscaler or model may pick another size, so nothing here
//!    assumes one.
//! 3. `cod3-hd-pack` encodes each upscaled PNG of a wanted kind (the same
//!    default; `--kinds diffuse,normal,...` or `--kinds all` for more) with
//!    a full mip chain (2 x 2 box filter, down to 1 x 1), in the original
//!    format, to `<out>/<file>.dds`, and lists the keys it serves in
//!    `<out>/index.tsv`: the same columns with the original size, and the
//!    DDS file name last. A file serves only the key whose picture it holds,
//!    so a texture never gets another texture's picture. The top level is
//!    the PNG's size, except that a longest side over `MAX_HD_SIDE` (8192) is scaled down to
//!    it, and for the BC formats each side is resampled to the nearest
//!    multiple of 4 (Direct3D 12 needs that for compressed textures). The
//!    summary counts the textures per scale. A DDS newer than its PNG, with
//!    the expected header and length, is kept; DDS files in `<out>` that the
//!    index no longer lists are removed.

use crate::jobs;
use crate::output::write_if_changed;
use crate::paths::sanitize;
use cod3_assets::material::TextureKind;
use std::collections::{BTreeMap, BTreeSet};
use std::io::Read as _;
use std::path::Path;
use std::sync::{Condvar, Mutex};
use std::time::{Instant, SystemTime};
use xenos::Format;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

pub const KEYS_FILE: &str = "hd_keys.tsv";
pub const INDEX_FILE: &str = "index.tsv";
const KEYS_HEADER: &str = "# key\twidth\theight\txenos_format\tname\tfile\tkind\n";
/// The `file` of a texture the pack leaves as the game has it.
pub const NO_FILE: &str = "-";
pub(crate) const INDEX_HEADER: &str = "# key\twidth\theight\txenos_format\tfile\n";
/// Largest texture side the Xenos fetch constant can express.
const MAX_TEXTURE_SIDE: u32 = 8192;
/// Longest side of an HD texture: a bigger upscale is scaled down to it, so
/// one texture can't take gigabytes of the game's memory (or this tool's).
pub(crate) const MAX_HD_SIDE: u32 = 8192;
/// Texels of decoded PNGs the worker threads may hold at once; a PNG bigger
/// than this is done alone. With the copies encoding makes, about 1.5 GB.
const TEXEL_BUDGET: u64 = 2 * 8192 * 8192;
/// Enough of a PNG file to read its size (signature and IHDR).
pub(crate) const PNG_HEADER_BYTES: usize = 33;
/// Problems of one kind printed by name before only counting the rest.
const PROBLEMS_SHOWN: usize = 10;

/// What a texture holds: the kinds of the material parameters that use it
/// (`cod3_assets::material::TextureKind`), or why none is known.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Kind {
    Diffuse,
    Normal,
    Specular,
    Detail,
    Other,
    /// Used as several kinds by different materials.
    Mixed,
    /// No material names it (HUD, fonts, effects, menu pictures, trees),
    /// or the rip hasn't decided yet.
    Unknown,
}

impl Kind {
    pub const ALL: [Kind; 7] = [
        Kind::Diffuse,
        Kind::Normal,
        Kind::Specular,
        Kind::Detail,
        Kind::Other,
        Kind::Mixed,
        Kind::Unknown,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Kind::Diffuse => "diffuse",
            Kind::Normal => "normal",
            Kind::Specular => "specular",
            Kind::Detail => "detail",
            Kind::Other => "other",
            Kind::Mixed => "mixed",
            Kind::Unknown => "unknown",
        }
    }

    pub fn parse(label: &str) -> Option<Kind> {
        Kind::ALL.into_iter().find(|k| k.label() == label)
    }

    /// The kind of a texture that materials use as `kinds`.
    pub fn of_uses(kinds: &BTreeSet<TextureKind>) -> Kind {
        let mut uses = kinds.iter();
        match (uses.next(), uses.next()) {
            (None, _) => Kind::Unknown,
            (Some(_), Some(_)) => Kind::Mixed,
            (Some(TextureKind::Diffuse), None) => Kind::Diffuse,
            (Some(TextureKind::Normal), None) => Kind::Normal,
            (Some(TextureKind::Specular), None) => Kind::Specular,
            (Some(TextureKind::Detail), None) => Kind::Detail,
            (Some(TextureKind::Other), None) => Kind::Other,
        }
    }

    /// Whether the HD pack upscales textures of this kind unless told
    /// otherwise: only color pictures. A normal, specular or detail map
    /// upscaled and denoised like a picture changes the shading, and a
    /// texture no material names may be anything (a font's glyphs, a HUD
    /// element, a lookup table), so those stay as the game has them.
    pub fn upscaled_by_default(self) -> bool {
        self == Kind::Diffuse
    }

    /// The kinds named by a `--kinds` value: labels separated by commas, or
    /// `all`.
    pub fn parse_list(text: &str) -> Result<BTreeSet<Kind>> {
        if text == "all" {
            return Ok(Kind::ALL.into_iter().collect());
        }
        text.split(',')
            .map(|label| {
                Kind::parse(label.trim()).ok_or_else(|| {
                    format!(
                        "unknown texture kind {label:?} (one of {}, or all)",
                        Kind::ALL.map(Kind::label).join(", ")
                    )
                    .into()
                })
            })
            .collect()
    }

    /// The kinds packed and upscaled by default.
    pub fn default_set() -> BTreeSet<Kind> {
        Kind::ALL
            .into_iter()
            .filter(|k| k.upscaled_by_default())
            .collect()
    }
}

/// One texture instance. The field order is the files' sort order: by name,
/// then key.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct KeyLine {
    pub name: String,
    pub key: u64,
    pub width: u32,
    pub height: u32,
    /// Xenos format number.
    pub format: u32,
    /// The PNG (without extension) in `textures/` that holds this key's
    /// picture, `NO_FILE` for a texture left as it is, or empty while the
    /// rip hasn't decided (another instance with the same key wrote it).
    pub file: String,
    /// What the texture holds; `Unknown` until the key list is resolved.
    pub kind: Kind,
    /// The game's hash of the texture's full name (`kapf::apkf::name_hash`),
    /// which the materials' texture parameters refer to; 0 when read back
    /// from the file, where it isn't kept.
    pub name_hash: u32,
}

impl KeyLine {
    /// The line for hd_keys.tsv or index.tsv, ending in `last_column`.
    pub(crate) fn tsv(&self, last_column: &str) -> String {
        format!(
            "{:016x}\t{}\t{}\t{}\t{last_column}\n",
            self.key, self.width, self.height, self.format
        )
    }

    /// The line for hd_keys.tsv.
    fn keys_tsv(&self) -> String {
        self.tsv(&format!(
            "{}\t{}\t{}",
            self.name,
            self.file,
            self.kind.label()
        ))
    }
}

/// True for textures the HD pack leaves as the game has them because they
/// hold data rather than a picture: the level lightmaps (`lm0000` ...), the
/// terrain's color lightmaps (`pg_colorlm_*`), fog ramps and other lookup
/// tables (8 texels or fewer on a side). Upscaling those changes the
/// lighting, and a lightmap atlas would bleed between its parts.
pub fn keep_original(name: &str, width: u32, height: u32) -> bool {
    let lower = name.to_ascii_lowercase();
    let level_lightmap = lower
        .strip_prefix("lm")
        .is_some_and(|rest| !rest.is_empty() && rest.bytes().all(|b| b.is_ascii_digit()));
    level_lightmap
        || lower.contains("colorlm")
        || lower.contains("lightmap")
        || lower.starts_with("fogramp")
        || width.min(height) <= 8
}

/// The PNG name (without extension) for a picture saved under `name` that
/// isn't the one saved as `<name>.png`.
pub fn variant_file(name: &str, key: u64) -> String {
    format!("{name}__{key:016x}")
}

/// The key lines all rip threads find (duplicates are kept once), the keys
/// whose picture some thread has saved, and what the materials say about
/// each texture (by name hash: the parameter kinds that use it, and the
/// members of each texture set).
#[derive(Default)]
pub struct KeyList {
    lines: Mutex<BTreeSet<KeyLine>>,
    saved: Mutex<BTreeSet<u64>>,
    uses: Mutex<BTreeMap<u32, BTreeSet<TextureKind>>>,
    set_members: Mutex<BTreeMap<u32, Vec<u32>>>,
}

impl KeyList {
    pub fn add(&self, line: KeyLine) {
        self.lines
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(line);
    }

    /// True if this call is the first to claim saving `key`'s picture.
    pub fn claim_picture(&self, key: u64) -> bool {
        self.saved
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(key)
    }

    /// Notes that a material uses the texture (or texture set) with name
    /// hash `name_hash` as `kind`.
    pub fn add_use(&self, name_hash: u32, kind: TextureKind) {
        self.uses
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .entry(name_hash)
            .or_default()
            .insert(kind);
    }

    /// Notes the textures a texture set lists (by name hash): a material
    /// that names the set uses each of them.
    pub fn add_set(&self, name_hash: u32, members: Vec<u32>) {
        self.set_members
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(name_hash, members);
    }

    /// The lines with every `file` and `kind` decided: a key takes the file
    /// some instance saved its picture to (the one named like an instance
    /// first: two threads can save the same picture under two names), and
    /// `NO_FILE` when no file holds it or the texture is left as it is. A
    /// key's kind is the kind of every use of every instance of it (the same
    /// picture under two names is one picture), through texture sets.
    fn resolved(self) -> BTreeSet<KeyLine> {
        let lines = self.lines.into_inner().unwrap_or_else(|e| e.into_inner());
        let mut uses = self.uses.into_inner().unwrap_or_else(|e| e.into_inner());
        let set_members = self
            .set_members
            .into_inner()
            .unwrap_or_else(|e| e.into_inner());
        // A set's uses are its members' uses (sets don't nest).
        for (set, members) in &set_members {
            let Some(kinds) = uses.get(set).cloned() else {
                continue;
            };
            for member in members {
                uses.entry(*member)
                    .or_default()
                    .extend(kinds.iter().copied());
            }
        }
        // Per key, the files holding its picture: (not saved under the
        // line's own name, file); the smallest wins. And its uses.
        let mut file_of: BTreeMap<u64, (bool, &str)> = BTreeMap::new();
        let mut uses_of: BTreeMap<u64, BTreeSet<TextureKind>> = BTreeMap::new();
        for line in &lines {
            if let Some(kinds) = uses.get(&line.name_hash) {
                uses_of
                    .entry(line.key)
                    .or_default()
                    .extend(kinds.iter().copied());
            }
            if line.file.is_empty() || line.file == NO_FILE {
                continue;
            }
            let candidate = (line.file != line.name, line.file.as_str());
            file_of
                .entry(line.key)
                .and_modify(|best| *best = (*best).min(candidate))
                .or_insert(candidate);
        }
        let no_uses = BTreeSet::new();
        lines
            .iter()
            .map(|line| {
                let mut line = line.clone();
                line.file = if keep_original(&line.name, line.width, line.height) {
                    NO_FILE.to_string()
                } else {
                    file_of
                        .get(&line.key)
                        .map_or(NO_FILE, |&(_, file)| file)
                        .to_string()
                };
                line.kind = Kind::of_uses(uses_of.get(&line.key).unwrap_or(&no_uses));
                line
            })
            .collect()
    }

    /// Writes `<textures_dir>/hd_keys.tsv` (unless it already holds the same
    /// lines). Returns a short report.
    pub fn write(self, textures_dir: &Path) -> Result<String> {
        let lines = self.resolved();
        let mut text = String::from(KEYS_HEADER);
        for line in &lines {
            text.push_str(&line.keys_tsv());
        }
        std::fs::create_dir_all(textures_dir)?;
        let wrote = write_if_changed(&textures_dir.join(KEYS_FILE), text.as_bytes())?;
        let names: BTreeSet<&str> = lines.iter().map(|l| l.name.as_str()).collect();
        let files: BTreeSet<&str> = lines
            .iter()
            .map(|l| l.file.as_str())
            .filter(|f| *f != NO_FILE)
            .collect();
        let kept = lines.iter().filter(|l| l.file == NO_FILE).count();
        // Pictures per kind, as the pack and the upscaler see them.
        let mut per_kind: BTreeMap<Kind, BTreeSet<&str>> = BTreeMap::new();
        for line in lines.iter().filter(|l| l.file != NO_FILE) {
            per_kind.entry(line.kind).or_default().insert(&line.file);
        }
        let kinds: Vec<String> = per_kind
            .iter()
            .map(|(kind, files)| format!("{} {}", files.len(), kind.label()))
            .collect();
        Ok(format!(
            "{KEYS_FILE}: {} texture keys, {} names, {} pictures for the HD pack ({}), {kept} \
             keys left as the game has them{}\n",
            lines.len(),
            names.len(),
            files.len(),
            kinds.join(", "),
            if wrote { "" } else { " (unchanged)" }
        ))
    }
}

/// Parses hd_keys.tsv. The ripper wrote it, so any malformed line is an
/// error; names must be plain file names (they become paths).
pub(crate) fn parse_keys(text: &str) -> std::result::Result<Vec<KeyLine>, String> {
    let mut lines = Vec::new();
    for (number, line) in text.lines().enumerate() {
        let line = line.trim_end_matches('\r');
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let bad = |why: &str| format!("{KEYS_FILE} line {}: {why}: {line:?}", number + 1);
        let fields: Vec<&str> = line.split('\t').collect();
        let [key, width, height, format, name, file, kind] = fields[..] else {
            return Err(if (5..=6).contains(&fields.len()) {
                bad("made by an older ripper (no kind column): rip the game again")
            } else {
                bad("expected 7 tab-separated fields")
            });
        };
        let key = match u64::from_str_radix(key, 16) {
            Ok(k) if key.len() == 16 => k,
            _ => return Err(bad("the key is not 16 hex digits")),
        };
        let side = |s: &str| {
            s.parse::<u32>()
                .ok()
                .filter(|v| (1..=MAX_TEXTURE_SIDE).contains(v))
        };
        let (Some(width), Some(height)) = (side(width), side(height)) else {
            return Err(bad("bad width or height"));
        };
        let format = match format.parse::<u32>() {
            Ok(f) if Format::from_xenos(f).block().is_some() => f,
            _ => return Err(bad("not one of the formats 18, 19, 20, 49, 6")),
        };
        if sanitize(name) != name {
            return Err(bad("the name is not a plain file name"));
        }
        if file != NO_FILE && sanitize(file) != file {
            return Err(bad("the file is not a plain file name"));
        }
        let Some(kind) = Kind::parse(kind) else {
            return Err(bad("unknown texture kind"));
        };
        lines.push(KeyLine {
            name: name.to_string(),
            key,
            width,
            height,
            format,
            file: file.to_string(),
            kind,
            name_hash: 0,
        });
    }
    Ok(lines)
}

/// A texture in the pack.
struct Packed {
    /// The keys its DDS serves.
    keys: Vec<KeyLine>,
    /// The PNG's scale (its longest side over the original's), in hundredths.
    scale: u32,
    /// Whether the DDS has another size than the PNG (sides rounded to a
    /// multiple of 4, or scaled down to `MAX_HD_SIDE`).
    resized: bool,
}

/// What became of one texture name.
enum Outcome {
    /// The DDS was written.
    Made(Packed),
    /// The DDS was newer than the PNG and as this run would write it.
    UpToDate(Packed),
    MissingPng,
    /// The PNG isn't the original scaled up: another aspect ratio, or smaller.
    WrongSize {
        found: (u32, u32),
        original: (u32, u32),
    },
    Unsupported(String),
    Failed(String),
}

/// True if `png` is `original` scaled up by any factor (1 or more, not
/// necessarily a whole number): the aspect ratio is the same to within one
/// texel of rounding on each side of the PNG.
pub(crate) fn is_scaled_copy(original: (u32, u32), png: (u32, u32)) -> bool {
    let (w, h) = (original.0 as u64, original.1 as u64);
    let (pw, ph) = (png.0 as u64, png.1 as u64);
    // pw = s * w + e1 and ph = s * h + e2 with |e1|, |e2| <= 1 give
    // |pw * h - ph * w| = |e1 * h - e2 * w| <= w + h.
    pw >= w && ph >= h && (pw * h).abs_diff(ph * w) <= w + h
}

/// The scale of `png` over `original` (longest sides), in hundredths.
pub(crate) fn scale_hundredths(original: (u32, u32), png: (u32, u32)) -> u32 {
    let original_side = original.0.max(original.1) as u64;
    let png_side = png.0.max(png.1) as u64;
    ((png_side * 100 + original_side / 2) / original_side) as u32
}

/// "2x", "2.5x", "1.33x".
pub(crate) fn scale_label(hundredths: u32) -> String {
    if hundredths % 100 == 0 {
        format!("{}x", hundredths / 100)
    } else {
        let text = format!("{}.{:02}", hundredths / 100, hundredths % 100);
        format!("{}x", text.trim_end_matches('0'))
    }
}

/// The size of the HD texture made from a PNG of `png` size: the same, but
/// with the longest side at most `MAX_HD_SIDE` and, for BC formats, each side
/// at the nearest multiple of 4 (Direct3D 12 can't create a compressed
/// texture of another size; a tie rounds up).
fn hd_size(format: Format, png: (u32, u32)) -> (u32, u32) {
    let (mut width, mut height) = png;
    let longest = width.max(height);
    if longest > MAX_HD_SIDE {
        let fit = |side: u32| {
            let scaled = (side as u64 * MAX_HD_SIDE as u64 + longest as u64 / 2) / longest as u64;
            (scaled as u32).max(1)
        };
        (width, height) = (fit(width), fit(height));
    }
    if format.block().is_some_and(|(block, _)| block == 4) {
        let round = |side: u32| ((side + 2) / 4 * 4).max(4);
        (width, height) = (round(width), round(height));
    }
    (width, height)
}

/// For each texel along an axis resampled from `from` to `to` texels: the
/// first source texel it reads and the weights of that one and the next ones
/// (they add up to 1). The weights are a tent around the texel's center,
/// widened when scaling down so that every source texel counts.
fn axis_weights(from: usize, to: usize) -> Vec<(usize, Vec<f32>)> {
    let step = from as f64 / to as f64;
    let reach = step.max(1.0);
    (0..to)
        .map(|i| {
            let center = (i as f64 + 0.5) * step;
            let first = (center - reach).floor().max(0.0) as usize;
            let end = ((center + reach).ceil() as usize).min(from);
            let mut weights: Vec<f32> = (first..end)
                .map(|j| (1.0 - (j as f64 + 0.5 - center).abs() / reach).max(0.0) as f32)
                .collect();
            // The nearest source texel is always within half a texel, so the
            // sum is at least 0.5.
            let sum: f32 = weights.iter().sum();
            weights.iter_mut().for_each(|w| *w /= sum);
            (first, weights)
        })
        .collect()
}

/// Resamples a `width` x `height` RGBA image along one axis to `to` texels.
fn resize_axis(rgba: &[u8], width: usize, height: usize, to: usize, across: bool) -> Vec<u8> {
    let weights = axis_weights(if across { width } else { height }, to);
    let (out_width, out_height) = if across { (to, height) } else { (width, to) };
    let mut out = vec![0u8; out_width * out_height * 4];
    for y in 0..out_height {
        for x in 0..out_width {
            let (first, row_weights) = &weights[if across { x } else { y }];
            let mut sum = [0f32; 4];
            for (k, weight) in row_weights.iter().enumerate() {
                let (sx, sy) = if across {
                    (first + k, y)
                } else {
                    (x, first + k)
                };
                let texel = &rgba[(sy * width + sx) * 4..][..4];
                for c in 0..4 {
                    sum[c] += weight * texel[c] as f32;
                }
            }
            for c in 0..4 {
                out[(y * out_width + x) * 4 + c] = sum[c].round().clamp(0.0, 255.0) as u8;
            }
        }
    }
    out
}

/// Resamples an RGBA image to `to` (width, height), one axis at a time.
pub(crate) fn resize(rgba: Vec<u8>, from: (u32, u32), to: (u32, u32)) -> Vec<u8> {
    let (width, height) = (from.0 as usize, from.1 as usize);
    let mut image = rgba;
    if to.0 != from.0 {
        image = resize_axis(&image, width, height, to.0 as usize, true);
    }
    if to.1 != from.1 {
        image = resize_axis(&image, to.0 as usize, height, to.1 as usize, false);
    }
    image
}

/// Limits the texels of decoded PNGs held by all worker threads at once.
pub(crate) struct TexelBudget {
    left: Mutex<u64>,
    freed: Condvar,
}

/// Texels taken from a `TexelBudget`, given back when dropped.
pub(crate) struct TexelLease<'a> {
    budget: &'a TexelBudget,
    texels: u64,
}

impl TexelBudget {
    pub(crate) fn new() -> Self {
        TexelBudget {
            left: Mutex::new(TEXEL_BUDGET),
            freed: Condvar::new(),
        }
    }

    /// Waits until `texels` (the whole budget at most) are free and takes them.
    pub(crate) fn take(&self, texels: u64) -> TexelLease<'_> {
        let texels = texels.min(TEXEL_BUDGET);
        let mut left = self.left.lock().unwrap_or_else(|e| e.into_inner());
        while *left < texels {
            left = self.freed.wait(left).unwrap_or_else(|e| e.into_inner());
        }
        *left -= texels;
        TexelLease {
            budget: self,
            texels,
        }
    }
}

impl Drop for TexelLease<'_> {
    fn drop(&mut self) {
        *self.budget.left.lock().unwrap_or_else(|e| e.into_inner()) += self.texels;
        self.budget.freed.notify_all();
    }
}

/// Up to `len` bytes from the start of a file.
pub(crate) fn read_prefix(path: &Path, len: usize) -> std::io::Result<Vec<u8>> {
    let mut prefix = Vec::with_capacity(len);
    std::fs::File::open(path)?
        .take(len as u64)
        .read_to_end(&mut prefix)?;
    Ok(prefix)
}

/// Sizes of the mip chain of a `width` x `height` texture, down to 1 x 1.
fn mip_sizes(width: u32, height: u32) -> Vec<(u32, u32)> {
    let mut sizes = vec![(width, height)];
    let (mut w, mut h) = (width, height);
    while w > 1 || h > 1 {
        w = (w / 2).max(1);
        h = (h / 2).max(1);
        sizes.push((w, h));
    }
    sizes
}

/// Bytes of one level in `format` (whole blocks for BC formats).
fn level_bytes(format: Format, width: u32, height: u32) -> usize {
    let (block, block_bytes) = format.block().expect("one of the five formats");
    (width.div_ceil(block) * height.div_ceil(block) * block_bytes) as usize
}

/// The next mip level: each texel the rounded average of a 2 x 2 square;
/// at an odd or single-texel edge the last row or column is used twice.
fn half_size(rgba: &[u8], width: usize, height: usize) -> (Vec<u8>, usize, usize) {
    let (w, h) = ((width / 2).max(1), (height / 2).max(1));
    let mut out = vec![0u8; w * h * 4];
    for y in 0..h {
        let rows = [2 * y, (2 * y + 1).min(height - 1)];
        for x in 0..w {
            let columns = [2 * x, (2 * x + 1).min(width - 1)];
            for c in 0..4 {
                let mut sum = 2u32; // rounds to nearest
                for row in rows {
                    for column in columns {
                        sum += rgba[(row * width + column) * 4 + c] as u32;
                    }
                }
                out[(y * w + x) * 4 + c] = (sum / 4) as u8;
            }
        }
    }
    (out, w, h)
}

/// One level in `format`'s storage order: BC formats encode R, G, B, A as
/// decoded (DXN uses R and G); 8_8_8_8 stores bytes B, G, R, A per texel,
/// the order `xenos::rgba8` reads.
fn encode_level(format: Format, width: usize, height: usize, rgba: &[u8]) -> Vec<u8> {
    match format.block_format() {
        Some(block_format) => xenos::bc_encode::encode(block_format, width, height, rgba),
        None => xenos::bgra_to_rgba(rgba),
    }
}

/// The DDS file: every level from `width` x `height` down to 1 x 1.
pub(crate) fn build_dds(format: Format, width: u32, height: u32, rgba: Vec<u8>) -> Result<Vec<u8>> {
    let mut levels = Vec::new();
    let (mut level, mut w, mut h) = (rgba, width as usize, height as usize);
    loop {
        levels.push(encode_level(format, w, h, &level));
        if w == 1 && h == 1 {
            break;
        }
        (level, w, h) = half_size(&level, w, h);
    }
    Ok(xenos::dds_file_mipmapped(format, width, height, &levels)?)
}

/// True if `dds` is at least as new as the PNG and is exactly the file this
/// run would write in shape (header and length), so a changed format, a new
/// size or a cut-off write is rebuilt.
fn is_up_to_date(dds: &Path, png_modified: SystemTime, format: Format, w: u32, h: u32) -> bool {
    let Ok(meta) = std::fs::metadata(dds) else {
        return false;
    };
    if !meta.modified().is_ok_and(|t| t >= png_modified) {
        return false;
    }
    let sizes = mip_sizes(w, h);
    let length: usize = xenos::DDS_HEADER_BYTES
        + sizes
            .iter()
            .map(|&(lw, lh)| level_bytes(format, lw, lh))
            .sum::<usize>();
    let Ok(header) =
        xenos::dds_mipmapped_header(format, w, h, level_bytes(format, w, h), sizes.len())
    else {
        return false;
    };
    meta.len() == length as u64
        && read_prefix(dds, xenos::DDS_HEADER_BYTES).is_ok_and(|prefix| prefix == header)
}

/// Builds (or keeps) `<out>/<file>.dds` from `<upscaled>/<file>.png` for
/// `keys`: the instances whose picture the file holds (one key, maybe under
/// several names, so one format and size).
fn pack_one(
    file: &str,
    keys: &[KeyLine],
    upscaled: &Path,
    out: &Path,
    budget: &TexelBudget,
) -> Outcome {
    let png_path = upscaled.join(format!("{file}.png"));
    let png_modified = match std::fs::metadata(&png_path) {
        Ok(meta) if meta.is_file() => meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
        Ok(_) => return Outcome::MissingPng,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Outcome::MissingPng,
        Err(e) => return Outcome::Failed(format!("{}: {e}", png_path.display())),
    };
    // The size alone decides whether the DDS is current, so up-to-date
    // textures cost two small reads.
    let png_size = match read_prefix(&png_path, PNG_HEADER_BYTES) {
        Ok(prefix) => match png_read::dimensions(&prefix) {
            Ok(size) => size,
            Err(e) => return Outcome::Unsupported(e.to_string()),
        },
        Err(e) => return Outcome::Failed(format!("{}: {e}", png_path.display())),
    };
    let first = &keys[0];
    let (format, source_width, source_height) = (first.format, first.width, first.height);
    if keys.iter().any(|k| {
        (k.key, k.format, k.width, k.height) != (first.key, format, source_width, source_height)
    }) {
        return Outcome::Failed(format!(
            "{file}: hd_keys.tsv gives this file more than one texture; rip the game again"
        ));
    }
    if !is_scaled_copy((source_width, source_height), png_size) {
        return Outcome::WrongSize {
            found: png_size,
            original: (source_width, source_height),
        };
    }
    let keys = vec![first.clone()];
    let format = Format::from_xenos(format);
    let (width, height) = hd_size(format, png_size);
    let packed = Packed {
        keys,
        scale: scale_hundredths((source_width, source_height), png_size),
        resized: (width, height) != png_size,
    };
    let dds_path = out.join(format!("{file}.dds"));
    if is_up_to_date(&dds_path, png_modified, format, width, height) {
        return Outcome::UpToDate(packed);
    }
    // Big PNGs wait their turn, so the threads together don't run out of
    // memory with a large upscale.
    let _lease = budget.take(png_size.0 as u64 * png_size.1 as u64);
    let image = match std::fs::read(&png_path) {
        Ok(bytes) => match png_read::decode(&bytes) {
            Ok(image) => image,
            Err(e) => return Outcome::Unsupported(e.to_string()),
        },
        Err(e) => return Outcome::Failed(format!("{}: {e}", png_path.display())),
    };
    // A PNG without alpha decodes with alpha 255, so DXT1 stays opaque.
    let rgba = resize(image.rgba, png_size, (width, height));
    let written =
        build_dds(format, width, height, rgba).and_then(|dds| Ok(std::fs::write(&dds_path, dds)?));
    match written {
        Ok(()) => Outcome::Made(packed),
        Err(e) => Outcome::Failed(format!("{}: {e}", dds_path.display())),
    }
}

/// Problems of one kind, for the report: the first few by name, then a count.
#[derive(Default)]
struct Problems {
    count: usize,
    shown: Vec<String>,
}

impl Problems {
    fn add(&mut self, text: String) {
        self.count += 1;
        if self.shown.len() < PROBLEMS_SHOWN {
            self.shown.push(text);
        }
    }

    fn print(&self, prefix: &str) {
        for line in &self.shown {
            println!("{prefix}{line}");
        }
        if self.count > self.shown.len() {
            println!("{prefix}... and {} more", self.count - self.shown.len());
        }
    }
}

/// How many textures `pack` made, kept and skipped.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackSummary {
    pub made: usize,
    pub up_to_date: usize,
    /// Textures in the pack (made or up to date) per scale of their PNG over
    /// the original, in hundredths (200 = 2x).
    pub per_scale: BTreeMap<u32, usize>,
    /// Textures in the pack whose DDS has another size than their PNG (sides
    /// rounded to a multiple of 4, or scaled down to 8192).
    pub resized: usize,
    pub missing_png: usize,
    pub wrong_size: usize,
    pub unsupported: usize,
    /// Textures that failed for a reason other than their PNG (a write
    /// error, say); missing, wrong-size or unreadable PNGs are only skipped.
    pub failed: usize,
    /// Lines written to index.tsv.
    pub index_keys: usize,
    /// Keys left out because their kind isn't packed (normal maps and so on
    /// by default).
    pub not_packed_kind: usize,
}

/// Removes the `.dds` files directly in `out` that `listed` doesn't name
/// (made by an earlier pack for a texture it no longer serves), so the
/// folder holds only what the index uses. Returns how many were removed.
pub(crate) fn remove_unlisted(out: &Path, listed: &BTreeSet<String>) -> usize {
    let Ok(entries) = std::fs::read_dir(out) else {
        return 0;
    };
    let mut removed = 0;
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let is_file = entry.file_type().is_ok_and(|t| t.is_file());
        if is_file
            && name.to_ascii_lowercase().ends_with(".dds")
            && !listed.contains(&name)
            && std::fs::remove_file(entry.path()).is_ok()
        {
            removed += 1;
        }
    }
    removed
}

/// `rip cod3-hd-pack`: see the module documentation. `kinds` are the
/// texture kinds to pack (`Kind::default_set` unless the user chose).
pub fn pack(
    textures: &Path,
    upscaled: &Path,
    out: &Path,
    kinds: &BTreeSet<Kind>,
    workers: usize,
) -> Result<PackSummary> {
    let keys_path = textures.join(KEYS_FILE);
    let text = std::fs::read_to_string(&keys_path).map_err(|e| {
        format!(
            "{}: {e} (the textures must come from a rip with output layout 6 or later)",
            keys_path.display()
        )
    })?;
    let lines = parse_keys(&text)?;
    let key_count = lines.len();
    // Each file holds one key's picture; textures left as they are have
    // none, and nor do textures of a kind that isn't packed.
    let mut kept_original = 0usize;
    let mut kind_skipped = 0usize;
    let mut by_file: BTreeMap<String, Vec<KeyLine>> = BTreeMap::new();
    for line in lines {
        if line.file == NO_FILE || keep_original(&line.name, line.width, line.height) {
            kept_original += 1;
            continue;
        }
        if !kinds.contains(&line.kind) {
            kind_skipped += 1;
            continue;
        }
        by_file.entry(line.file.clone()).or_default().push(line);
    }
    // Biggest textures first, so no thread is left alone with a big one at
    // the end.
    let mut work: Vec<(&str, &[KeyLine])> = by_file
        .iter()
        .map(|(file, entries)| (file.as_str(), entries.as_slice()))
        .collect();
    work.sort_by_key(|(_, entries)| {
        let texels = entries.iter().map(|e| e.width as u64 * e.height as u64);
        std::cmp::Reverse(texels.max())
    });
    std::fs::create_dir_all(out)?;
    // No paths in progress lines: the setup tool reads "done/total" pairs.
    let kind_labels: Vec<&str> = kinds.iter().map(|k| k.label()).collect();
    println!(
        "{} textures, {key_count} keys ({kept_original} left as the game has them, \
         {kind_skipped} not of the kinds packed: {}), {workers} worker threads",
        work.len(),
        kind_labels.join(", ")
    );

    let start = Instant::now();
    let budget = TexelBudget::new();
    let total = work.len();
    let mut finished = 0usize;
    let mut last_percent = 0usize;
    let mut outcomes: BTreeMap<&str, Outcome> = BTreeMap::new();
    jobs::for_each_parallel(
        &work,
        workers,
        |&(file, entries)| pack_one(file, entries, upscaled, out, &budget),
        |&(name, _), outcome| {
            finished += 1;
            let percent = finished * 100 / total;
            if percent > last_percent {
                println!("{finished}/{total} textures");
                last_percent = percent;
            }
            outcomes.insert(name, outcome);
        },
    );

    let (mut made, mut up_to_date, mut resized) = (0usize, 0usize, 0usize);
    let mut per_scale: BTreeMap<u32, usize> = BTreeMap::new();
    let (mut missing, mut wrong_size, mut unsupported, mut failed) =
        <(Problems, Problems, Problems, Problems)>::default();
    let mut index_lines: Vec<KeyLine> = Vec::new();
    for (name, outcome) in outcomes {
        let packed = match outcome {
            Outcome::Made(packed) => {
                made += 1;
                packed
            }
            Outcome::UpToDate(packed) => {
                up_to_date += 1;
                packed
            }
            Outcome::MissingPng => {
                missing.add(format!("{name}.png"));
                continue;
            }
            Outcome::WrongSize { found, original } => {
                wrong_size.add(format!(
                    "{name}.png is {}x{}, not a scaled-up copy of the original {}x{} \
                     (another aspect ratio, or smaller)",
                    found.0, found.1, original.0, original.1
                ));
                continue;
            }
            Outcome::Unsupported(why) => {
                unsupported.add(format!("{name}.png: {why}"));
                continue;
            }
            Outcome::Failed(why) => {
                failed.add(why);
                continue;
            }
        };
        *per_scale.entry(packed.scale).or_default() += 1;
        resized += usize::from(packed.resized);
        index_lines.extend(packed.keys);
    }

    // index.tsv: one line per key, by name then key, naming the DDS made
    // from that key's own picture.
    index_lines.sort();
    let mut seen = BTreeSet::new();
    let mut listed_files = BTreeSet::new();
    let mut index = String::from(INDEX_HEADER);
    for line in &index_lines {
        if seen.insert(line.key) {
            let dds = format!("{}.dds", line.file);
            index.push_str(&line.tsv(&dds));
            listed_files.insert(dds);
        }
    }
    write_if_changed(&out.join(INDEX_FILE), index.as_bytes())?;
    let removed = remove_unlisted(out, &listed_files);

    missing.print("skipped (missing PNG): ");
    wrong_size.print("skipped (wrong size): ");
    unsupported.print("skipped (unsupported PNG): ");
    failed.print("error: ");
    let seconds = start.elapsed().as_secs_f64();
    println!(
        "done: {made} made, {up_to_date} up to date, skipped: {} missing PNG, {} wrong size, \
         {} unsupported, {} failed ({seconds:.1} s, {:.1} made per second)",
        missing.count,
        wrong_size.count,
        unsupported.count,
        failed.count,
        made as f64 / seconds.max(0.001)
    );
    // Per scale, most common first.
    let mut scales: Vec<(u32, usize)> = per_scale.iter().map(|(&s, &n)| (s, n)).collect();
    scales.sort_by_key(|&(scale, count)| (std::cmp::Reverse(count), scale));
    let scales: Vec<String> = scales
        .iter()
        .map(|&(scale, count)| format!("{count} at {}", scale_label(scale)))
        .collect();
    println!(
        "scales: {} ({resized} resized: BC sides to a multiple of 4, or the longest side to \
         {MAX_HD_SIDE})",
        if scales.is_empty() {
            "none".to_string()
        } else {
            scales.join(", ")
        }
    );
    println!(
        "wrote {INDEX_FILE} with {} keys of {} textures; {kept_original} keys left as the game \
         has them (lightmaps, fog ramps, lookup tables) and {kind_skipped} not of the kinds \
         packed (normal, specular and detail maps change the lighting when upscaled; textures \
         no material names may be anything); {removed} old DDS files no longer listed removed",
        seen.len(),
        made + up_to_date,
    );
    Ok(PackSummary {
        made,
        up_to_date,
        per_scale,
        resized,
        missing_png: missing.count,
        wrong_size: wrong_size.count,
        unsupported: unsupported.count,
        failed: failed.count,
        index_keys: seen.len(),
        not_packed_kind: kind_skipped,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use xenos::bc::{self, BlockFormat};

    fn key_line(key: u64, width: u32, height: u32, format: u32, name: &str) -> KeyLine {
        key_file(key, width, height, format, name, name)
    }

    /// A line as `parse_keys` returns it (hash 0), a diffuse map unless
    /// changed.
    fn key_file(key: u64, width: u32, height: u32, format: u32, name: &str, file: &str) -> KeyLine {
        KeyLine {
            name: name.to_string(),
            key,
            width,
            height,
            format,
            file: file.to_string(),
            kind: Kind::Diffuse,
            name_hash: 0,
        }
    }

    /// A line as the rip adds it: the name's hash, the kind not decided.
    fn ripped(key: u64, width: u32, height: u32, format: u32, name: &str, file: &str) -> KeyLine {
        KeyLine {
            kind: Kind::Unknown,
            name_hash: kapf::apkf::name_hash(name),
            ..key_file(key, width, height, format, name, file)
        }
    }

    #[test]
    fn parses_key_lines_and_refuses_bad_ones() {
        let text = "# key\twidth\theight\txenos_format\tname\tfile\tkind\r\n\
                    00000000000000ff\t64\t32\t18\tcloth$burlap_n\tcloth$burlap_n\tnormal\r\n\n\
                    0123456789abcdef\t8\t8\t49\tb\tb__0123456789abcdef\tunknown\n\
                    0000000000000001\t64\t64\t6\tlm0001\t-\tother\n";
        let lines = parse_keys(text).unwrap();
        assert_eq!(
            lines[0],
            KeyLine {
                kind: Kind::Normal,
                ..key_line(0xFF, 64, 32, 18, "cloth$burlap_n")
            }
        );
        assert_eq!(
            lines[1],
            KeyLine {
                kind: Kind::Unknown,
                ..key_file(0x0123_4567_89AB_CDEF, 8, 8, 49, "b", "b__0123456789abcdef")
            }
        );
        assert_eq!(
            lines[2],
            KeyLine {
                kind: Kind::Other,
                ..key_file(1, 64, 64, 6, "lm0001", NO_FILE)
            }
        );
        for bad in [
            "ff\t64\t32\t18\ta\ta\tdiffuse",                  // short key
            "00000000000000ff\t0\t32\t18\ta\ta\tdiffuse",     // zero width
            "00000000000000ff\t64\t32\t7\ta\ta\tdiffuse",     // unknown format
            "00000000000000ff\t64\t32\t18\t../a\ta\tdiffuse", // a path as the name
            "00000000000000ff\t64\t32\t18\ta\t../a\tdiffuse", // a path as the file
            "00000000000000ff\t64\t32\t18\ta\ta\tpicture",    // not a kind
            "00000000000000ff\t64\t32\t18\ta\ta",             // an older rip (no kind)
            "00000000000000ff\t64\t32\t18\ta",                // an older rip (no file)
            "00000000000000ff\t64\t32\t18",                   // fields missing
        ] {
            assert!(parse_keys(bad).is_err(), "{bad}");
        }
        // The writer's format reads back.
        let line = KeyLine {
            kind: Kind::Specular,
            ..key_file(0xABC, 16, 8, 20, "x", "x__0000000000000abc")
        };
        assert_eq!(parse_keys(&line.keys_tsv()).unwrap(), vec![line]);
    }

    #[test]
    fn kinds_have_labels_and_a_default() {
        for kind in Kind::ALL {
            assert_eq!(Kind::parse(kind.label()), Some(kind));
        }
        assert_eq!(Kind::default_set(), BTreeSet::from([Kind::Diffuse]));
        assert_eq!(
            Kind::parse_list("normal, specular").unwrap(),
            BTreeSet::from([Kind::Normal, Kind::Specular])
        );
        assert_eq!(Kind::parse_list("all").unwrap().len(), Kind::ALL.len());
        assert!(Kind::parse_list("diffuse,colour").is_err());
        let uses = |kinds: &[TextureKind]| Kind::of_uses(&kinds.iter().copied().collect());
        assert_eq!(uses(&[]), Kind::Unknown);
        assert_eq!(uses(&[TextureKind::Diffuse]), Kind::Diffuse);
        assert_eq!(uses(&[TextureKind::Detail]), Kind::Detail);
        assert_eq!(
            uses(&[TextureKind::Diffuse, TextureKind::Specular]),
            Kind::Mixed
        );
    }

    #[test]
    fn lightmaps_and_lookup_tables_stay_original() {
        for (name, w, h) in [
            ("lm0000", 512, 512),
            ("LM0041", 512, 512),
            ("pg_colorlm_008_003", 64, 64),
            ("fogramp", 256, 8),
            ("fogramp_default", 256, 8),
            ("some_lightmap", 128, 128),
            ("gradient", 256, 4),
        ] {
            assert!(keep_original(name, w, h), "{name}");
        }
        for (name, w, h) in [
            ("lm", 64, 64),
            ("lmetal", 64, 64),
            ("helmet_c", 256, 256),
            ("film01", 64, 64),
        ] {
            assert!(!keep_original(name, w, h), "{name}");
        }
    }

    #[test]
    fn every_key_gets_its_own_picture_and_kind() {
        // Two levels' lm0000 and fog: one saved under the name, others as
        // variants; "c" saved twice (same picture under "c" and "d").
        let hash = kapf::apkf::name_hash;
        let list = KeyList::default();
        list.add(ripped(1, 64, 64, 18, "wall", "wall"));
        list.add(ripped(2, 64, 64, 18, "wall", "wall__0000000000000002"));
        list.add(ripped(3, 64, 64, 18, "wall", "")); // no instance saved its picture
        list.add(ripped(1, 64, 64, 18, "wall_copy", "")); // key 1's picture under another name
        list.add(ripped(9, 512, 512, 20, "lm0000", "lm0000"));
        list.add(ripped(10, 512, 512, 20, "lm0000", ""));
        list.add(ripped(5, 32, 32, 18, "c", "c"));
        list.add(ripped(5, 32, 32, 18, "d", "d__0000000000000005"));
        list.add(ripped(6, 32, 32, 18, "head1", "head1"));
        list.add(ripped(7, 32, 32, 18, "head2", "head2"));
        // Materials: wall is a color map; its copy a specular map (so the
        // picture is used as both); lm0000 a lightmap; the heads come
        // through a texture set; nothing names c or d.
        list.add_use(hash("wall"), TextureKind::Diffuse);
        list.add_use(hash("wall_copy"), TextureKind::Specular);
        list.add_use(hash("lm0000"), TextureKind::Other);
        list.add_use(hash("heads"), TextureKind::Diffuse);
        list.add_set(hash("heads"), vec![hash("head1"), hash("head2")]);
        let lines: Vec<(String, u64, String, Kind)> = list
            .resolved()
            .into_iter()
            .map(|l| (l.name, l.key, l.file, l.kind))
            .collect();
        let expect = |name: &str, key: u64, file: &str, kind| {
            (name.to_string(), key, file.to_string(), kind)
        };
        assert_eq!(
            lines,
            vec![
                expect("c", 5, "c", Kind::Unknown),
                expect("d", 5, "c", Kind::Unknown), // the file saved under its own name wins
                expect("head1", 6, "head1", Kind::Diffuse),
                expect("head2", 7, "head2", Kind::Diffuse),
                expect("lm0000", 9, NO_FILE, Kind::Other),
                expect("lm0000", 10, NO_FILE, Kind::Other),
                expect("wall", 1, "wall", Kind::Mixed), // key 1 is also wall_copy
                expect("wall", 2, "wall__0000000000000002", Kind::Diffuse),
                expect("wall", 3, NO_FILE, Kind::Diffuse), // no file holds its picture
                expect("wall_copy", 1, "wall", Kind::Mixed),
            ]
        );
    }

    #[test]
    fn mip_chain_sizes_and_box_filter() {
        assert_eq!(mip_sizes(8, 2), vec![(8, 2), (4, 1), (2, 1), (1, 1)]);
        assert_eq!(mip_sizes(1, 1), vec![(1, 1)]);
        // 2 x 2 to 1 x 1: the rounded average.
        let (out, w, h) = half_size(&[0, 0, 0, 0, 1, 2, 3, 4, 1, 2, 3, 4, 2, 2, 2, 2], 2, 2);
        assert_eq!((out, w, h), (vec![1, 2, 2, 3], 1, 1));
        // 3 x 1: the odd column is dropped; the single row is used twice.
        let row = [10, 0, 0, 0, 20, 0, 0, 0, 90, 0, 0, 0];
        assert_eq!(half_size(&row, 3, 1), (vec![15, 0, 0, 0], 1, 1));
    }

    #[test]
    fn any_scale_with_the_same_aspect_ratio_is_a_copy() {
        for png in [
            (64, 32),
            (128, 64),
            (192, 96),
            (160, 80),
            (161, 80),
            (200, 100),
        ] {
            assert!(is_scaled_copy((64, 32), png), "{png:?}");
        }
        // 1.5x of an odd size rounds either way; 3.33x of 30 x 15.
        assert!(is_scaled_copy((30, 17), (45, 26)));
        assert!(is_scaled_copy((30, 17), (45, 25)));
        assert!(is_scaled_copy((30, 15), (100, 50)));
        for png in [(128, 70), (128, 128), (32, 16), (63, 32)] {
            assert!(!is_scaled_copy((64, 32), png), "{png:?}");
        }
        assert_eq!(scale_hundredths((64, 32), (128, 64)), 200);
        assert_eq!(scale_hundredths((30, 15), (100, 50)), 333);
        assert_eq!(scale_label(200), "2x");
        assert_eq!(scale_label(250), "2.5x");
        assert_eq!(scale_label(333), "3.33x");
        assert_eq!(scale_label(310), "3.1x");
    }

    #[test]
    fn hd_sizes_fit_the_gpu() {
        // BC: each side to the nearest multiple of 4 (a tie rounds up).
        assert_eq!(hd_size(Format::Dxt1, (128, 64)), (128, 64));
        assert_eq!(hd_size(Format::Dxt1, (90, 45)), (92, 44));
        assert_eq!(hd_size(Format::Dxn, (10, 2)), (12, 4));
        // 32-bit textures keep any size.
        assert_eq!(hd_size(Format::A8R8G8B8, (90, 45)), (90, 45));
        // The longest side is capped, keeping the aspect ratio.
        assert_eq!(hd_size(Format::Dxt4_5, (16384, 8192)), (8192, 4096));
        assert_eq!(hd_size(Format::Dxt1, (12000, 30)), (8192, 20));
        assert_eq!(hd_size(Format::A8R8G8B8, (10000, 3)), (8192, 2));
    }

    #[test]
    fn resampling_averages_and_interpolates() {
        // Same size: unchanged.
        let image = test_image(5, 3, |x, y| (x * 40 + y) as u8);
        assert_eq!(resize(image.clone(), (5, 3), (5, 3)), image);
        // 2 x 1 to 1 x 1: the average of both.
        let pair = vec![0, 0, 0, 0, 255, 255, 255, 255];
        assert_eq!(resize(pair.clone(), (2, 1), (1, 1)), vec![128; 4]);
        // 2 x 1 to 4 x 1: linear between the two, the ends kept.
        let wide = resize(pair, (2, 1), (4, 1));
        let reds: Vec<u8> = wide.chunks_exact(4).map(|t| t[0]).collect();
        assert_eq!(reds, vec![0, 64, 191, 255]);
        // A flat color stays flat at any size.
        let flat: Vec<u8> = [10, 20, 30, 40].repeat(7 * 5);
        let resized = resize(flat, (7, 5), (12, 8));
        assert!(resized.chunks_exact(4).all(|t| t == [10, 20, 30, 40]));
        assert_eq!(resized.len(), 12 * 8 * 4);
    }

    /// An image whose channels vary smoothly, with `alpha(x, y)`.
    fn test_image(w: u32, h: u32, alpha: impl Fn(u32, u32) -> u8) -> Vec<u8> {
        let mut rgba = Vec::new();
        for y in 0..h {
            for x in 0..w {
                rgba.extend_from_slice(&[(x * 16) as u8, (y * 16) as u8, 128, alpha(x, y)]);
            }
        }
        rgba
    }

    #[test]
    fn packs_every_format_and_keeps_up_to_date_files() {
        let root = std::env::temp_dir().join(format!("rip-hd-pack-test-{}", std::process::id()));
        let (textures, upscaled, out) = (root.join("t"), root.join("up"), root.join("hd"));
        std::fs::create_dir_all(&textures).unwrap();
        std::fs::create_dir_all(&upscaled).unwrap();
        std::fs::create_dir_all(&out).unwrap();
        let keys = [
            key_file(0xAA, 16, 16, 18, "a", "a"),
            key_file(0xAB, 16, 16, 18, "a", "a__00000000000000ab"), // another picture of "a"
            key_file(0xAC, 16, 16, 20, "a", "a__00000000000000ac"), // its PNG is missing
            key_file(0xB0, 32, 16, 49, "b", "b"),
            key_file(0xC0, 16, 16, 6, "c", "c"),
            key_file(0xD0, 16, 16, 19, "d", "d"), // no PNG
            key_file(0xE0, 16, 16, 18, "e", "e"), // PNG of another aspect ratio
            key_file(0xAA, 16, 16, 18, "f", "a"), // a's picture under another name
            key_file(0x60, 16, 16, 18, "g", "g"), // 3x
            key_file(0x70, 12, 12, 20, "h", "h"), // 2.5x: 30 x 30, made 32 x 32
            key_file(0x80, 256, 8, 6, "fogramp", NO_FILE), // left as the game has it
            KeyLine {
                kind: Kind::Normal,
                ..key_file(0x90, 16, 16, 20, "n", "n") // a normal map: not packed by default
            },
        ];
        // Written as hd_keys.tsv (resolved() would decide the kinds itself).
        let mut text = String::from(KEYS_HEADER);
        keys.iter().for_each(|k| text.push_str(&k.keys_tsv()));
        std::fs::write(textures.join(KEYS_FILE), text).unwrap();
        let png = |file: &str, w: u32, h: u32, rgba: &[u8]| {
            let path = upscaled.join(format!("{file}.png"));
            std::fs::write(path, png_write::encode_rgba(w, h, rgba)).unwrap();
        };
        let a = test_image(32, 32, |x, _| if x < 16 { 0 } else { 255 });
        let b = test_image(64, 32, |_, _| 255);
        let c = test_image(16, 16, |x, y| (x * 15 + y) as u8);
        png("a", 32, 32, &a);
        png(
            "a__00000000000000ab",
            32,
            32,
            &test_image(32, 32, |_, _| 255),
        );
        png("b", 64, 32, &b);
        png("c", 16, 16, &c);
        png("e", 48, 24, &test_image(48, 24, |_, _| 255));
        png("g", 48, 48, &test_image(48, 48, |_, _| 255));
        png("h", 30, 30, &test_image(30, 30, |x, _| (x * 8) as u8));
        png("n", 32, 32, &test_image(32, 32, |_, _| 255));
        // A DDS an earlier pack made for a texture no longer served.
        std::fs::write(out.join("lm0000.dds"), b"old").unwrap();

        let summary = pack(&textures, &upscaled, &out, &Kind::default_set(), 2).unwrap();
        assert_eq!(
            summary,
            PackSummary {
                made: 6,
                up_to_date: 0,
                per_scale: BTreeMap::from([(100, 1), (200, 3), (250, 1), (300, 1)]),
                resized: 1,
                missing_png: 2,
                wrong_size: 1,
                unsupported: 0,
                failed: 0,
                index_keys: 6,
                not_packed_kind: 1,
            }
        );
        assert!(
            !out.join("n.dds").exists(),
            "a normal map is not packed by default"
        );
        let index = std::fs::read_to_string(out.join(INDEX_FILE)).unwrap();
        assert_eq!(
            index,
            "# key\twidth\theight\txenos_format\tfile\n\
             00000000000000aa\t16\t16\t18\ta.dds\n\
             00000000000000ab\t16\t16\t18\ta__00000000000000ab.dds\n\
             00000000000000b0\t32\t16\t49\tb.dds\n\
             00000000000000c0\t16\t16\t6\tc.dds\n\
             0000000000000060\t16\t16\t18\tg.dds\n\
             0000000000000070\t12\t12\t20\th.dds\n"
        );
        assert!(
            !out.join("lm0000.dds").exists(),
            "an unlisted DDS is removed"
        );
        let expected_len = |format: Format, w: u32, h: u32| {
            128 + mip_sizes(w, h)
                .iter()
                .map(|&(lw, lh)| level_bytes(format, lw, lh))
                .sum::<usize>()
        };

        // a: DXT1 32 x 32 with every level, transparent where alpha was 0.
        let dds = std::fs::read(out.join("a.dds")).unwrap();
        assert_eq!(xenos::dds_info(&dds), Some((Format::Dxt1, 32, 32)));
        assert_eq!(dds.len(), expected_len(Format::Dxt1, 32, 32));
        let top = bc::decode(BlockFormat::Bc1, 32, 32, &dds[128..]).unwrap();
        for (src, dst) in a.chunks_exact(4).zip(top.chunks_exact(4)) {
            assert_eq!(dst[3], if src[3] < 128 { 0 } else { 255 });
        }
        // b: DXN keeps red and green.
        let dds = std::fs::read(out.join("b.dds")).unwrap();
        assert_eq!(xenos::dds_info(&dds), Some((Format::Dxn, 64, 32)));
        let top = bc::decode(BlockFormat::Bc5, 64, 32, &dds[128..]).unwrap();
        for (src, dst) in b.chunks_exact(4).zip(top.chunks_exact(4)) {
            assert!((src[0] as i32 - dst[0] as i32).abs() <= 2);
            assert!((src[1] as i32 - dst[1] as i32).abs() <= 2);
        }
        // c: 8_8_8_8 at 1x stores B, G, R, A.
        let dds = std::fs::read(out.join("c.dds")).unwrap();
        assert_eq!(xenos::dds_info(&dds), Some((Format::A8R8G8B8, 16, 16)));
        assert_eq!(dds.len(), expected_len(Format::A8R8G8B8, 16, 16));
        assert_eq!(&dds[128..132], &[c[2], c[1], c[0], c[3]]);
        // g: 3x.
        let dds = std::fs::read(out.join("g.dds")).unwrap();
        assert_eq!(xenos::dds_info(&dds), Some((Format::Dxt1, 48, 48)));
        assert_eq!(dds.len(), expected_len(Format::Dxt1, 48, 48));
        // h: 2.5x (30 x 30) resampled to 32 x 32 for DXT5; alpha still rises
        // from left to right.
        let dds = std::fs::read(out.join("h.dds")).unwrap();
        assert_eq!(xenos::dds_info(&dds), Some((Format::Dxt4_5, 32, 32)));
        assert_eq!(dds.len(), expected_len(Format::Dxt4_5, 32, 32));
        let top = bc::decode(BlockFormat::Bc3, 32, 32, &dds[128..]).unwrap();
        let alpha: Vec<u8> = top[..32 * 4].chunks_exact(4).map(|t| t[3]).collect();
        assert!(
            alpha.windows(2).all(|p| p[1] as i32 + 4 >= p[0] as i32),
            "{alpha:?}"
        );
        assert!(alpha[0] < 40 && alpha[31] > 200, "{alpha:?}");

        // Nothing changed: everything is up to date and untouched.
        let modified = |name: &str| {
            std::fs::metadata(out.join(name))
                .unwrap()
                .modified()
                .unwrap()
        };
        let before = modified("a.dds");
        let again = pack(&textures, &upscaled, &out, &Kind::default_set(), 2).unwrap();
        assert_eq!((again.made, again.up_to_date), (0, 6));
        assert_eq!((again.per_scale.len(), again.resized), (4, 1));
        assert_eq!(modified("a.dds"), before);
        // A damaged DDS is rebuilt even though it is newer than its PNG.
        std::fs::write(out.join("b.dds"), b"DDS ").unwrap();
        let repaired = pack(&textures, &upscaled, &out, &Kind::default_set(), 1).unwrap();
        assert_eq!((repaired.made, repaired.up_to_date), (1, 5));
        // Asked for normal maps too, the pack adds the normal map.
        let all_kinds = Kind::parse_list("all").unwrap();
        let with_normals = pack(&textures, &upscaled, &out, &all_kinds, 1).unwrap();
        assert_eq!((with_normals.made, with_normals.index_keys), (1, 7));
        assert!(out.join("n.dds").exists());
        // Back to the default, the normal map's DDS is removed as unlisted.
        pack(&textures, &upscaled, &out, &Kind::default_set(), 1).unwrap();
        assert!(!out.join("n.dds").exists());
        // An older rip's hd_keys.tsv (no kind column) is refused.
        std::fs::write(
            textures.join(KEYS_FILE),
            "00000000000000aa\t16\t16\t18\ta\ta\n",
        )
        .unwrap();
        assert!(pack(&textures, &upscaled, &out, &Kind::default_set(), 1).is_err());
        std::fs::remove_dir_all(&root).unwrap();
    }
}
