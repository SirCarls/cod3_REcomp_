//! Fetch instructions to WGSL. Vertex fetch reads guest memory through the
//! vertex fetch constant (its base address and endianness) with the format,
//! stride and offset of the instruction; texture fetch samples the host's
//! texture tables. Ported from the SDK's `spirv_translator_fetch.cpp`
//! (`ProcessVertexFetchInstruction`, `ProcessTextureFetchInstruction`, Xenia's,
//! BSD 3-Clause).

use super::{Emitter, Refusal};
use crate::fetch::{Dimension, FetchInstruction, FetchOpcode};
use crate::parsed::*;
use crate::shader::ShaderType;

/// Per component of a packed format: (word, bit offset, width).
type PackedLayout = [(u32, u32, u32); 4];

/// (component count, words, the packed layout), the layout None for the formats read
/// whole (floats, 32-bit integers, halves).
type VertexFormat = (u32, u32, Option<PackedLayout>);

fn vertex_format(format: u32) -> Option<VertexFormat> {
    let packed = |l: PackedLayout| Some(l);
    Some(match format {
        6 => (4, 1, packed([(0, 0, 8), (0, 8, 8), (0, 16, 8), (0, 24, 8)])),
        7 => (4, 1, packed([(0, 0, 10), (0, 10, 10), (0, 20, 10), (0, 30, 2)])),
        16 => (3, 1, packed([(0, 0, 11), (0, 11, 11), (0, 22, 10), (0, 0, 0)])),
        17 => (3, 1, packed([(0, 0, 10), (0, 10, 11), (0, 21, 11), (0, 0, 0)])),
        25 => (2, 1, packed([(0, 0, 16), (0, 16, 16), (0, 0, 0), (0, 0, 0)])),
        26 => (4, 2, packed([(0, 0, 16), (0, 16, 16), (1, 0, 16), (1, 16, 16)])),
        31 => (2, 1, None),
        32 => (4, 2, None),
        33 | 36 => (1, 1, None),
        34 | 37 => (2, 2, None),
        35 | 38 => (4, 4, None),
        57 => (3, 3, None),
        _ => return None,
    })
}

fn float(x: f32) -> String {
    format!("{:?}", x)
}

impl Emitter<'_> {
    pub(crate) fn fetch(&mut self, op: &FetchInstruction) -> Result<(), Refusal> {
        if op.opcode() == FetchOpcode::VertexFetch {
            let (fetch, was_full) = parse_vertex_fetch(op, &self.previous_vfetch_full);
            if was_full {
                self.previous_vfetch_full = *op;
            }
            self.predicated(fetch.is_predicated, fetch.predicate_condition, |e| e.vertex_fetch(&fetch))
        } else {
            let fetch = parse_texture_fetch(op);
            self.predicated(fetch.is_predicated, fetch.predicate_condition, |e| e.texture_fetch(&fetch))
        }
    }

    fn predicated(
        &mut self,
        is_predicated: bool,
        condition: bool,
        f: impl FnOnce(&mut Self) -> Result<(), Refusal>,
    ) -> Result<(), Refusal> {
        if is_predicated {
            self.line(&format!("if (xe_p == {}) {{", condition));
            self.indent += 1;
        }
        f(self)?;
        if is_predicated {
            self.indent -= 1;
            self.line("}");
        }
        Ok(())
    }

    fn vertex_fetch(&mut self, f: &ParsedVertexFetch) -> Result<(), Refusal> {
        let constant = f.operands[1].index;
        let word0 = constant * 2;
        if !f.is_mini_fetch {
            // The base address (in dwords) plus the index (rounded or floored) times the stride.
            let mut address = format!("i32(xe_u(xe_fb + {}u, {}u) >> 2u)", word0 / 4, word0 % 4);
            if f.attributes.stride != 0 {
                let index = self.operand_scalar(&f.operands[0], 0);
                let index = if f.attributes.is_index_rounded { format!("({} + 0.5)", index) } else { index };
                address = format!("{} + i32(floor({})) * {}", address, index, f.attributes.stride);
            }
            self.line(&format!("xe_vaddr = {};", address));
        }
        let used = f.result.used_result_components();
        if used == 0 {
            // Only constants (0 / 1) are written, if anything: the SDK still stores them.
            return self.store(&f.result, "vec4<f32>(0.0)", false);
        }
        let Some((count, words, packed)) = vertex_format(f.attributes.data_format) else {
            // Undefined formats read as zeros, as the GPU would give nothing meaningful.
            return self.store(&f.result, "vec4<f32>(0.0)", false);
        };
        let word1 = word0 + 1;
        let endian = self.fresh("xe_endian");
        self.line(&format!("let {} = xe_u(xe_fb + {}u, {}u) & 3u;", endian, word1 / 4, word1 % 4));
        // The buffer the fetch constant names (address and size in dwords): reads outside it are 0.
        let base = self.fresh("xe_vb");
        self.line(&format!("let {} = xe_u(xe_fb + {}u, {}u) >> 2u;", base, word0 / 4, word0 % 4));
        let size = self.fresh("xe_vn");
        self.line(&format!("let {} = (xe_u(xe_fb + {}u, {}u) >> 2u) & 0xFFFFFFu;", size, word1 / 4, word1 % 4));
        let mut w = Vec::new();
        for i in 0..words {
            let name = self.fresh("xe_w");
            let offset = f.attributes.offset + i as i32;
            self.line(&format!("let {} = xe_vword(xe_vaddr + {}, {}, {}, {});", name, offset, base, size, endian));
            w.push(name);
        }
        let signed = f.attributes.is_signed;
        let normalized = !f.attributes.is_integer;
        let no_zero = f.attributes.signed_rf_mode == 1;
        let mut comps: Vec<String> = Vec::new();
        match (f.attributes.data_format, packed) {
            (_, Some(layout)) => {
                for &(word, offset, width) in layout.iter().take(count as usize) {
                    let raw = if signed {
                        format!("f32(extractBits(bitcast<i32>({}), {}u, {}u))", w[word as usize], offset, width)
                    } else {
                        format!("f32(extractBits({}, {}u, {}u))", w[word as usize], offset, width)
                    };
                    // Divisions (not reciprocal products), as the SDK's interpreter computes them.
                    let value = if !normalized {
                        raw
                    } else if signed {
                        let max = ((1u32 << (width - 1)) - 1) as f32;
                        if no_zero {
                            let range = ((1u64 << width) - 1) as f32;
                            format!("((({} + 0.5) * 2.0) / {})", raw, float(range))
                        } else {
                            format!("max({} / {}, -1.0)", raw, float(max))
                        }
                    } else {
                        let max = ((1u64 << width) - 1) as f32;
                        format!("({} / {})", raw, float(max))
                    };
                    comps.push(value);
                }
            }
            (31 | 32, None) => {
                for word in &w {
                    let h = self.fresh("xe_h");
                    self.line(&format!("let {} = unpack2x16float({});", h, word));
                    comps.push(format!("{}.x", h));
                    comps.push(format!("{}.y", h));
                }
            }
            (33..=35, None) => {
                for word in &w {
                    let raw = if signed { format!("f32(bitcast<i32>({}))", word) } else { format!("f32({})", word) };
                    let value = if !normalized {
                        raw
                    } else if signed {
                        if no_zero {
                            format!("(({} + 0.5) / {})", raw, float(2147483647.5))
                        } else {
                            format!("({} / {})", raw, float(2147483647.0))
                        }
                    } else {
                        format!("({} / {})", raw, float(4294967295.0))
                    };
                    comps.push(value);
                }
            }
            _ => {
                for word in &w {
                    comps.push(format!("bitcast<f32>({})", word));
                }
            }
        }
        comps.truncate(count as usize);
        while comps.len() < 4 {
            comps.push("0.0".to_owned());
        }
        let mut value = format!("vec4<f32>({})", comps.join(", "));
        if f.attributes.exp_adjust != 0 {
            value = format!("({} * {})", value, float(2f32.powi(f.attributes.exp_adjust)));
        }
        // Components the format lacks are 0 (the dest swizzle gives 1 explicitly): the
        // padding above.
        self.store(&f.result, &value, false)
    }

    fn texture_fetch(&mut self, f: &ParsedTextureFetch) -> Result<(), Refusal> {
        let coord = |e: &Self, i: u32| e.operand_scalar(&f.operands[0], i);
        match f.opcode {
            FetchOpcode::SetTextureLod => {
                let v = coord(self, 0);
                self.line(&format!("xe_lod = {};", v));
                return Ok(());
            }
            FetchOpcode::SetTextureGradientsHorz | FetchOpcode::SetTextureGradientsVert => {
                let target = if f.opcode == FetchOpcode::SetTextureGradientsHorz { "xe_gh" } else { "xe_gv" };
                let (x, y, z) = (coord(self, 0), coord(self, 1), coord(self, 2));
                self.line(&format!("{} = vec3<f32>({}, {}, {});", target, x, y, z));
                return Ok(());
            }
            FetchOpcode::GetTextureGradients => {
                if f.result.used_result_components() == 0 {
                    return self.store(&f.result, "vec4<f32>(0.0)", false);
                }
                let (x, y) = (coord(self, 0), coord(self, 1));
                let value = if self.ty == ShaderType::Pixel {
                    format!(
                        "vec4<f32>(dpdxCoarse({x}), dpdyCoarse({x}), dpdxCoarse({y}), dpdyCoarse({y}))",
                        x = x,
                        y = y
                    )
                } else {
                    "vec4<f32>(0.0)".to_owned()
                };
                return self.store(&f.result, &value, false);
            }
            FetchOpcode::TextureFetch | FetchOpcode::GetTextureWeights => {}
            FetchOpcode::GetTextureComputedLod => return Err(Refusal::TextureOperation("getCompTexLOD")),
            FetchOpcode::GetTextureBorderColorFrac => {
                // Xenia returns 0 for it too (no border color texture is bound).
                return self.store(&f.result, "vec4<f32>(0.0)", false);
            }
            FetchOpcode::VertexFetch | FetchOpcode::Unknown(_) => {
                return Err(Refusal::TextureOperation("unknown fetch"));
            }
        }
        if f.result.used_result_components() == 0 {
            // Only constants (0 / 1) are written, if anything: the SDK still stores them.
            return self.store(&f.result, "vec4<f32>(0.0)", false);
        }
        self.uses_textures = true;
        let tf = f.operands[1].index & 31;
        let slot = self.fresh("xe_slot");
        self.line(&format!("let {} = xe_u(xe_mb + {}u, {}u);", slot, 13 + tf / 4, tf % 4));
        let at = &f.attributes;
        let (table, dims) = match f.dimension {
            Dimension::D1 | Dimension::D2 => ("tex2d", 2),
            Dimension::D3OrStacked => ("tex3d", 3),
            Dimension::Cube => ("texcube", 3),
        };
        let vty = if dims == 2 { "vec2<f32>" } else { "vec3<f32>" };
        // The coordinates (1D textures are 2D ones, one texel high).
        let c = match f.dimension {
            Dimension::D1 => format!("vec2<f32>({}, 0.5)", coord(self, 0)),
            Dimension::D2 => format!("vec2<f32>({}, {})", coord(self, 0), coord(self, 1)),
            _ => format!("vec3<f32>({}, {}, {})", coord(self, 0), coord(self, 1), coord(self, 2)),
        };
        let cn = self.fresh("xe_c");
        self.line(&format!("var {} = {};", cn, c));
        let size = self.fresh("xe_size");
        if f.dimension == Dimension::Cube {
            self.line(&format!("let {} = vec2<f32>(textureDimensions({}[{}], 0));", size, table, slot));
        } else {
            self.line(&format!("let {} = {}(textureDimensions({}[{}], 0));", size, vty, table, slot));
        }
        let offsets = [at.offset_x, at.offset_y, at.offset_z];
        if f.opcode == FetchOpcode::GetTextureWeights {
            if self.options.zero_texture_weights {
                return self.store(&f.result, "vec4<f32>(0.0)", false);
            }
            // The bilinear weights at LOD 0 (Xenia's getWeights also disregards the LOD).
            let texels = if at.unnormalized_coordinates {
                format!("{}.xy", cn)
            } else {
                format!("{}.xy * {}.xy", cn, size)
            };
            let off = format!("vec2<f32>({}, {})", float(offsets[0] - 0.5), float(offsets[1] - 0.5));
            let value = format!("vec4<f32>(fract({} + {}), 0.0, 0.0)", texels, off);
            return self.store(&f.result, &value, false);
        }
        match f.dimension {
            Dimension::Cube => {
                if offsets[2] != 0.0 {
                    self.line(&format!("{}.z = {}.z + {};", cn, cn, float(offsets[2])));
                }
                if offsets[0] != 0.0 || offsets[1] != 0.0 {
                    self.line(&format!(
                        "{c}.x = {c}.x + {} / {s}.x; {c}.y = {c}.y + {} / {s}.y;",
                        float(offsets[0]),
                        float(offsets[1]),
                        c = cn,
                        s = size
                    ));
                }
                self.line(&format!("{} = xe_cube_dir({});", cn, cn));
            }
            _ => {
                if at.unnormalized_coordinates {
                    // WGSL can't assign a multi-component swizzle; the vector is exactly that size.
                    self.line(&format!("{c} = {c} / {s};", c = cn, s = size));
                }
                let off: Vec<String> = offsets[..dims].iter().map(|o| float(*o)).collect();
                if offsets[..dims].iter().any(|o| *o != 0.0) {
                    self.line(&format!("{c} = {c} + {t}({}) / {s};", off.join(", "), c = cn, t = vty, s = size));
                }
            }
        }
        // The LOD: register LOD, explicit gradients, or computed (pixel shaders only).
        let bias = float(at.lod_bias);
        let tex = format!("{}[{}], samp[{}]", table, slot, slot);
        let sample = if self.ty == ShaderType::Vertex || at.use_register_lod || !at.use_computed_lod {
            let lod = if at.use_register_lod { format!("xe_lod + {}", bias) } else { bias.clone() };
            format!("textureSampleLevel({}, {}, {})", tex, cn, lod)
        } else if at.use_register_gradients {
            let k = if dims == 2 { "xy" } else { "xyz" };
            format!("textureSampleGrad({}, {}, xe_gh.{}, xe_gv.{})", tex, cn, k, k)
        } else if at.lod_bias != 0.0 {
            format!("textureSampleBias({}, {}, {})", tex, cn, bias)
        } else {
            format!("textureSample({}, {})", tex, cn)
        };
        let swz = format!("xe_u(xe_mb + {}u, {}u)", 21 + tf / 4, tf % 4);
        let value = format!("xe_tswz({}, {})", sample, swz);
        self.store(&f.result, &value, false)
    }
}
