//! Materials (APKF type `MAT`, version 2).
//!
//! Object data of a material of class "fx" (offsets from the object start):
//!
//! ```text
//! +0x00  name reference          the material's own name, "<part>.<model>"
//! +0x04  name reference          the material class: "fx" for almost all;
//!                                "cdgunsight" and "cdsimple" are small
//!                                special cases without parameters
//! +0x14  pointer or import       the shader effect (an FX resource)
//! +0x24  u32 parameter count
//! +0x28  pointer                 the parameter table
//! ```
//!
//! Parameter table entries are 0x24 bytes:
//!
//! ```text
//! +0x00  name reference          "diffuse_texture", "specular_power", ...
//! +0x10  u32 type                2 int, 3 float, 4 float4, 11 texture
//! +0x14  pointer                 the value
//! +0x18  u32 value size          4, 4, 16, 28
//! ```
//!
//! A texture value's first field points to a TEX item in the same chunk, or is
//! an import of a texture by name hash (the name "none" means no texture). The
//! rest of its 28 bytes is filled in at run time.

use crate::{linked_name, linked_texture, TextureRef};
use kapf::apkf::{Chunk, Item};

const CLASS_OFFSET: usize = 0x04;
const EFFECT_OFFSET: usize = 0x14;
const PARAM_COUNT_OFFSET: usize = 0x24;
const PARAM_TABLE_OFFSET: usize = 0x28;
const PARAM_ENTRY_SIZE: usize = 0x24;
const PARAM_TYPE_OFFSET: usize = 0x10;
const PARAM_VALUE_OFFSET: usize = 0x14;
const PARAM_SIZE_OFFSET: usize = 0x18;

const TYPE_INT: u32 = 2;
const TYPE_FLOAT: u32 = 3;
const TYPE_FLOAT4: u32 = 4;
const TYPE_TEXTURE: u32 = 11;

/// More parameters than any retail material has (the most seen is 13).
const MAX_PARAMS: u32 = 256;

/// Texture name meaning "no texture".
const NO_TEXTURE: &str = "none";

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Int(i32),
    Float(f32),
    Float4([f32; 4]),
    /// The texture, by name and name hash (`None` for "none").
    Texture(Option<TextureRef>),
    /// A type not seen in the retail data so far.
    Unknown {
        type_id: u32,
        size: u32,
    },
}

#[derive(Debug, Clone, PartialEq)]
pub struct Param {
    pub name: String,
    pub value: Value,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Material {
    pub name: String,
    pub class: String,
    /// The shader effect's name ("p_test_spec1_norm1_detail0", ...).
    pub effect: Option<String>,
    pub params: Vec<Param>,
}

/// What a texture is used for, to map materials onto other engines.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TextureRole {
    BaseColor,
    Normal,
    Specular,
}

/// What a material parameter's texture holds, from the parameter's name.
/// The names belong to each shader, so this is a classification of the
/// names seen in the game's files (`diffusemap_texture`,
/// `normalmap_texture`, `specularmap_texture`, `detailmap_texture`,
/// `hightmap1_texture`, `garand_col_texture`, `k98_n_texture`, ...).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum TextureKind {
    /// A color picture: diffuse and overlay maps, tints.
    Diffuse,
    /// Directions: normal and bump maps.
    Normal,
    /// Specular and gloss maps.
    Specular,
    /// Detail maps (fine normal or noise patterns tiled over a surface).
    Detail,
    /// Other data: height maps, masks, lightmaps, noise, environment maps.
    Other,
}

impl TextureKind {
    /// The kind's name in the ripper's files.
    pub fn label(self) -> &'static str {
        match self {
            TextureKind::Diffuse => "diffuse",
            TextureKind::Normal => "normal",
            TextureKind::Specular => "specular",
            TextureKind::Detail => "detail",
            TextureKind::Other => "other",
        }
    }

    /// The kind a parameter name stands for. Data words are checked before
    /// color words, so `normal_swatch_texture` is a normal map and
    /// `diffuse__w_alpha__texture` a color map. A name with no known word
    /// (`k98_wood_texture`) is taken as a color map: the game's shaders name
    /// their data maps (`_n_`, `spec`, `mask`, `height` ...), and a texture
    /// used as data anywhere else still counts as mixed.
    pub fn of_param(name: &str) -> TextureKind {
        let lower = name.to_ascii_lowercase();
        let has = |words: &[&str]| words.iter().any(|w| lower.contains(w));
        // "_n_texture", "_n_tga_texture": the one-letter normal map suffix.
        let n_suffix = lower.contains("_n_");
        if has(&[
            "lightmap",
            "height",
            "hightmap",
            "mask",
            "noise",
            "env",
            "reflection",
        ]) {
            TextureKind::Other
        } else if has(&["detail"]) {
            TextureKind::Detail
        } else if has(&["normal", "nrm", "norm", "bump"]) || n_suffix {
            TextureKind::Normal
        } else if has(&["spec", "gloss"]) {
            TextureKind::Specular
        } else {
            TextureKind::Diffuse
        }
    }
}

impl Material {
    /// The texture used for `role`, guessed from the parameter names. The
    /// names belong to each shader, so this is a heuristic. The common names
    /// come first ("diffuse_texture", "diffuse_map_texture"); then any
    /// texture whose name contains a typical word ("diff", "col"; "nrm",
    /// "norm", "_n_"; "spec"). Swatch and mask textures are never picked.
    pub fn texture_for(&self, role: TextureRole) -> Option<&str> {
        let (exact, words): (&[&str], &[&str]) = match role {
            TextureRole::BaseColor => (
                &[
                    "diffuse_texture",
                    "diffuse_map_texture",
                    "diffuse_texture_texture",
                ],
                &["diff", "col"],
            ),
            TextureRole::Normal => (
                &["normal_texture", "normal_map_texture"],
                &["nrm", "norm", "_n_"],
            ),
            TextureRole::Specular => (&["specular_texture", "specular_map_texture"], &["spec"]),
        };
        let textures = || {
            self.params.iter().filter_map(|p| match &p.value {
                Value::Texture(Some(t)) => Some((p.name.as_str(), t.name.as_str())),
                _ => None,
            })
        };
        let usable = |name: &str| !name.contains("swatch") && !name.contains("mask");
        exact
            .iter()
            .find_map(|e| textures().find(|(n, _)| n == e).map(|(_, t)| t))
            .or_else(|| {
                textures()
                    .find(|(n, _)| usable(n) && words.iter().any(|w| n.contains(w)))
                    .map(|(_, t)| t)
            })
    }
}

/// Reads the material in `item`. Materials without a parameter table (the
/// special classes) come back with no parameters.
pub fn parse(chunk: &Chunk, item: &Item) -> Material {
    let at = item.object.0;
    let name_or_empty = |field: usize| linked_name(chunk, field).unwrap_or_default();
    let mut material = Material {
        name: item.name.clone(),
        class: name_or_empty(at + CLASS_OFFSET),
        effect: linked_name(chunk, at + EFFECT_OFFSET),
        params: Vec::new(),
    };
    let (Some(count), Some(table)) = (
        chunk.u32_at(at + PARAM_COUNT_OFFSET),
        chunk.pointer(at + PARAM_TABLE_OFFSET),
    ) else {
        return material;
    };
    for i in 0..count.min(MAX_PARAMS) as usize {
        let entry = table + i * PARAM_ENTRY_SIZE;
        let Some(name) = chunk.u32_at(entry).and_then(|v| chunk.name_ref(v)) else {
            break;
        };
        let type_id = chunk.u32_at(entry + PARAM_TYPE_OFFSET).unwrap_or(0);
        let size = chunk.u32_at(entry + PARAM_SIZE_OFFSET).unwrap_or(0);
        let value_at = chunk.pointer(entry + PARAM_VALUE_OFFSET);
        let value = match (type_id, value_at) {
            (TYPE_INT, Some(v)) => Value::Int(chunk.u32_at(v).unwrap_or(0) as i32),
            (TYPE_FLOAT, Some(v)) => Value::Float(chunk.f32_at(v).unwrap_or(0.0)),
            (TYPE_FLOAT4, Some(v)) => {
                let f = |k: usize| chunk.f32_at(v + 4 * k).unwrap_or(0.0);
                Value::Float4([f(0), f(1), f(2), f(3)])
            }
            (TYPE_TEXTURE, Some(v)) => {
                Value::Texture(linked_texture(chunk, v).filter(|t| t.name != NO_TEXTURE))
            }
            _ => Value::Unknown { type_id, size },
        };
        material.params.push(Param { name, value });
    }
    material
}

#[cfg(test)]
mod tests {
    use super::*;

    fn texture(name: &str, tex: &str) -> Param {
        Param {
            name: name.into(),
            value: Value::Texture(Some(TextureRef {
                name: tex.into(),
                hash: kapf::apkf::name_hash(tex),
            })),
        }
    }

    #[test]
    fn kinds_of_parameter_names() {
        use TextureKind::*;
        for (name, kind) in [
            ("diffusemap_texture", Diffuse),
            ("diffusemapb_texture", Diffuse),
            ("diffuse__w_alpha__texture", Diffuse),
            ("diffuse_clamped____w_alpha_", Diffuse),
            ("garand_col_texture", Diffuse),
            ("bspringfield_diff_texture", Diffuse),
            ("bullet_d_tga_texture", Diffuse),
            ("overlay_map_texture", Diffuse),
            ("reflection_tint_texture", Other),
            ("normalmap_texture", Normal),
            ("normal_swatch_texture", Normal),
            ("k98_n_texture", Normal),
            ("w_sten_n_tga_texture", Normal),
            ("garand_nrm_texture", Normal),
            ("specularmap_texture", Specular),
            ("sten_spec_tga_texture", Specular),
            ("detailmap_texture", Detail),
            ("detail_texture", Detail),
            ("hightmap1_texture", Other),
            ("height_map_texture", Other),
            ("cmn_lightmap_texture", Other),
            ("fabric_mask_texture", Other),
            ("k98_metal_mask_texture", Other),
            ("noise_swatch_texture", Other),
            ("env_map_texture", Other),
            ("reflection_cube_map_texture", Other),
            ("k98_wood_texture", Diffuse),
            ("k98_wood_color_texture", Diffuse),
        ] {
            assert_eq!(TextureKind::of_param(name), kind, "{name}");
            assert!(!kind.label().is_empty());
        }
    }

    #[test]
    fn texture_roles() {
        let m = Material {
            name: "m".into(),
            class: "fx".into(),
            effect: None,
            params: vec![
                texture("normal_swatch_texture", "swatch_n"),
                texture("garand_nrm_texture", "garand_n"),
                texture("garand_col_texture", "garand_c"),
                texture("specular_map_texture", "garand_s"),
            ],
        };
        assert_eq!(m.texture_for(TextureRole::BaseColor), Some("garand_c"));
        assert_eq!(m.texture_for(TextureRole::Normal), Some("garand_n"));
        assert_eq!(m.texture_for(TextureRole::Specular), Some("garand_s"));
    }
}
