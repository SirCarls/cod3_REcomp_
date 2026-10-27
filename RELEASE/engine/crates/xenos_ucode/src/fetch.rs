//! Fetch instructions (vertex and texture), 3 dwords each (Xenia `ucode.h`,
//! `VertexFetchInstruction`, `TextureFetchInstruction`).

use crate::{bits, sbits};

/// `ucode::FetchOpcode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FetchOpcode {
    VertexFetch,
    TextureFetch,
    GetTextureBorderColorFrac,
    GetTextureComputedLod,
    GetTextureGradients,
    GetTextureWeights,
    SetTextureLod,
    SetTextureGradientsHorz,
    SetTextureGradientsVert,
    Unknown(u32),
}

impl FetchOpcode {
    pub fn from_bits(v: u32) -> Self {
        match v {
            0 => Self::VertexFetch,
            1 => Self::TextureFetch,
            16 => Self::GetTextureBorderColorFrac,
            17 => Self::GetTextureComputedLod,
            18 => Self::GetTextureGradients,
            19 => Self::GetTextureWeights,
            24 => Self::SetTextureLod,
            25 => Self::SetTextureGradientsHorz,
            26 => Self::SetTextureGradientsVert,
            other => Self::Unknown(other),
        }
    }
}

/// `xenos::FetchOpDimension`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Dimension {
    D1,
    D2,
    D3OrStacked,
    Cube,
}

impl Dimension {
    pub fn from_bits(v: u32) -> Self {
        match v & 3 {
            0 => Self::D1,
            1 => Self::D2,
            2 => Self::D3OrStacked,
            _ => Self::Cube,
        }
    }
    /// `GetFetchOpDimensionComponentCount`.
    pub fn component_count(self) -> u32 {
        match self {
            Self::D1 => 1,
            Self::D2 => 2,
            Self::D3OrStacked | Self::Cube => 3,
        }
    }
    pub fn index(self) -> usize {
        self as usize
    }
}

/// `xenos::TextureFilter` values: 0 point, 1 linear, 2 basemap, 3 use the fetch constant.
pub const TEXTURE_FILTER_USE_FETCH_CONST: u32 = 3;
/// `xenos::AnisoFilter`: 0 disabled ... 5 max16to1, 7 use the fetch constant.
pub const ANISO_FILTER_USE_FETCH_CONST: u32 = 7;

/// The three dwords of a fetch instruction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FetchInstruction {
    pub dwords: [u32; 3],
}

impl FetchInstruction {
    pub fn new(dwords: &[u32]) -> Self {
        Self { dwords: [dwords[0], dwords[1], dwords[2]] }
    }
    fn d(&self, i: usize) -> u32 {
        self.dwords[i]
    }

    pub fn opcode(&self) -> FetchOpcode {
        FetchOpcode::from_bits(bits(self.d(0), 0, 5))
    }
    pub fn is_predicated(&self) -> bool {
        bits(self.d(1), 31, 1) == 1
    }
    pub fn predicate_condition(&self) -> bool {
        bits(self.d(2), 31, 1) == 1
    }
    pub fn src(&self) -> u32 {
        bits(self.d(0), 5, 6)
    }
    pub fn is_src_relative(&self) -> bool {
        bits(self.d(0), 11, 1) == 1
    }
    pub fn dest(&self) -> u32 {
        bits(self.d(0), 12, 6)
    }
    pub fn is_dest_relative(&self) -> bool {
        bits(self.d(0), 18, 1) == 1
    }
    pub fn dest_swizzle(&self) -> u32 {
        bits(self.d(1), 0, 12)
    }

    // Vertex fetch.
    /// [0, 95]: three vertex fetch constants per 6-dword fetch constant slot.
    pub fn vertex_fetch_constant_index(&self) -> u32 {
        bits(self.d(0), 20, 5) * 3 + bits(self.d(0), 25, 2)
    }
    /// Prefetch count minus 1.
    pub fn vertex_prefetch_count(&self) -> u32 {
        bits(self.d(0), 27, 3)
    }
    /// One absolute 2-bit component.
    pub fn vertex_src_swizzle(&self) -> u32 {
        bits(self.d(0), 30, 2)
    }
    pub fn vertex_is_signed(&self) -> bool {
        bits(self.d(1), 12, 1) == 1
    }
    pub fn vertex_is_normalized(&self) -> bool {
        bits(self.d(1), 13, 1) == 0
    }
    /// 0: zero, clamp -1 (Microsoft); 1: no zero (OpenGL).
    pub fn vertex_signed_rf_mode(&self) -> u32 {
        bits(self.d(1), 14, 1)
    }
    pub fn vertex_is_index_rounded(&self) -> bool {
        bits(self.d(1), 15, 1) == 1
    }
    /// `xenos::VertexFormat`.
    pub fn vertex_data_format(&self) -> u32 {
        bits(self.d(1), 16, 6)
    }
    /// [-32, 31].
    pub fn vertex_exp_adjust(&self) -> i32 {
        sbits(self.d(1), 24, 6)
    }
    pub fn vertex_is_mini_fetch(&self) -> bool {
        bits(self.d(1), 30, 1) == 1
    }
    /// Dwords, [0, 255].
    pub fn vertex_stride(&self) -> u32 {
        bits(self.d(2), 0, 8)
    }
    /// Dwords, [-4194304, 4194303].
    pub fn vertex_offset(&self) -> i32 {
        sbits(self.d(2), 8, 23)
    }

    // Texture fetch (and the other texture-unit operations).
    /// [0, 31].
    pub fn texture_fetch_constant_index(&self) -> u32 {
        bits(self.d(0), 20, 5)
    }
    pub fn texture_fetch_valid_only(&self) -> bool {
        bits(self.d(0), 19, 1) == 1
    }
    pub fn texture_unnormalized_coordinates(&self) -> bool {
        bits(self.d(0), 25, 1) == 1
    }
    /// Three absolute 2-bit components.
    pub fn texture_src_swizzle(&self) -> u32 {
        bits(self.d(0), 26, 6)
    }
    pub fn texture_mag_filter(&self) -> u32 {
        bits(self.d(1), 12, 2)
    }
    pub fn texture_min_filter(&self) -> u32 {
        bits(self.d(1), 14, 2)
    }
    pub fn texture_mip_filter(&self) -> u32 {
        bits(self.d(1), 16, 2)
    }
    pub fn texture_aniso_filter(&self) -> u32 {
        bits(self.d(1), 18, 3)
    }
    pub fn texture_arbitrary_filter(&self) -> u32 {
        bits(self.d(1), 21, 3)
    }
    pub fn texture_vol_mag_filter(&self) -> u32 {
        bits(self.d(1), 24, 2)
    }
    pub fn texture_vol_min_filter(&self) -> u32 {
        bits(self.d(1), 26, 2)
    }
    pub fn texture_use_computed_lod(&self) -> bool {
        bits(self.d(1), 28, 1) == 1
    }
    pub fn texture_use_register_lod(&self) -> bool {
        bits(self.d(1), 29, 1) == 1
    }
    pub fn texture_use_register_gradients(&self) -> bool {
        bits(self.d(2), 0, 1) == 1
    }
    /// 0 centroid, 1 center.
    pub fn texture_sample_location(&self) -> u32 {
        bits(self.d(2), 1, 1)
    }
    /// In 1/16 steps, [-4, 3.9375].
    pub fn texture_lod_bias(&self) -> f32 {
        sbits(self.d(2), 2, 7) as f32 * (1.0 / 16.0)
    }
    pub fn texture_dimension(&self) -> Dimension {
        Dimension::from_bits(bits(self.d(2), 14, 2))
    }
    /// In 1/2 steps, [-8, 7.5].
    pub fn texture_offset_x(&self) -> f32 {
        sbits(self.d(2), 16, 5) as f32 * 0.5
    }
    pub fn texture_offset_y(&self) -> f32 {
        sbits(self.d(2), 21, 5) as f32 * 0.5
    }
    pub fn texture_offset_z(&self) -> f32 {
        sbits(self.d(2), 26, 5) as f32 * 0.5
    }
}

/// Names of `xenos::VertexFormat` values in the disassembly (`DataFormat=`).
pub fn vertex_format_name(format: u32) -> Option<&'static str> {
    Some(match format {
        6 => "FMT_8_8_8_8",
        7 => "FMT_2_10_10_10",
        16 => "FMT_10_11_11",
        17 => "FMT_11_11_10",
        25 => "FMT_16_16",
        26 => "FMT_16_16_16_16",
        31 => "FMT_16_16_FLOAT",
        32 => "FMT_16_16_16_16_FLOAT",
        33 => "FMT_32",
        34 => "FMT_32_32",
        35 => "FMT_32_32_32_32",
        36 => "FMT_32_FLOAT",
        37 => "FMT_32_32_FLOAT",
        38 => "FMT_32_32_32_32_FLOAT",
        57 => "FMT_32_32_32_FLOAT",
        _ => return None,
    })
}
