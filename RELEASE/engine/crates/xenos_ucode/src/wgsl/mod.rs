//! Xenos microcode to WGSL: the Rust renderer's shader translator
//! (`dev/agents/rust-renderer/NOTES-iw4l.md` section 3 (a), milestone M1a).
//!
//! Each shader becomes one WGSL module with one entry point (`main`), built in
//! the shape of IW4L's `d3d9_sm3` (plain strings, no naga IR builder): the
//! control flow program becomes `loop { switch pc { ... } }`, as Xenia does in
//! SPIR-V, and every ALU and fetch instruction becomes straight-line code inside
//! its exec's case. The meaning of each instruction is ported from the SDK's
//! own reading of it (`src/graphics/pipeline/shader/interpreter.cpp` and
//! `spirv_translator*.cpp`, Xenia's, BSD 3-Clause): the Direct3D 9 rule that 0
//! times anything is +0 in `mul`, `mad`, `dp*`, `muls*`; NaN comparisons as
//! `isgreaterequal` / `isless`; `rcpc` / `rcpf` / `rsqc` / `rsqf` / `logc`
//! clamping; vertex fetch formats, endian swaps and normalization; cube map
//! coordinates.
//!
//! # The ABI (what the host renderer binds)
//!
//! Group 0, the same for every draw:
//! - binding 0: `arena: array<vec4<f32>>`, read-only storage. Each draw gets a
//!   header row at `instance_index` (u32 bit patterns): x = the vertex shader's
//!   float constant base row, y = the pixel shader's, z = the fetch constant base
//!   row (48 rows: the 32 x 6 fetch constant dwords, vertex fetch constants as 2
//!   dwords each), w = the misc base row:
//!   - misc + 0..1: the 256 bool constants (8 u32);
//!   - misc + 2..9: the 32 loop constants (count bits 0-7, start 8-15, signed
//!     step 16-23);
//!   - misc + 10: x = vertex index offset (i32 bits);
//!   - misc + 11 / 12: the NDC scale / offset (xyz) applied to the position;
//!   - misc + 13..20: per texture fetch constant tf#, the index into the texture
//!     tables (u32);
//!   - misc + 21..28: per tf#, the host swizzle (3 bits per component: 0-3 =
//!     xyzw, 4 = 0, 5 = 1).
//! - binding 1: `guest: array<u32>`, read-only storage: guest physical memory
//!   from address 0, as the console holds it (vertex fetch swaps the words with
//!   the fetch constant's endianness, and reads 0 outside the buffer the fetch
//!   constant names, as the SDK's interpreter does).
//!
//! Group 1, only when the shader fetches textures: `tex2d`, `tex3d`, `texcube`
//! (binding arrays of `texture_*<f32>`, bindings 0-2) and `samp` (a binding
//! array of samplers, binding 3), all indexed by the table index above.
//!
//! The vertex shader's outputs and the pixel shader's inputs are one struct,
//! `XeVsOut`: the position, the 16 interpolators at locations 0-15 and the
//! draw's header row at location 16 (flat), so any vertex and pixel shader pair
//! links.
//!
//! # Not done yet (each is a known difference from the console)
//!
//! - Texture sign and gamma conversion, the fetch constant's exponent and LOD
//!   biases, the instruction's filter overrides, and Xenia's 1/4096 offset epsilon.
//! - `getCompTexLOD`, border color fraction, memory export, calls: refused.
//! - The pixel shader's `ps_param_gen` register, alpha test, point size, edge
//!   flag and vertex kill.
//! - Denormals: ALU operands are flushed to zero as the console does (`xe_ftz`), but
//!   results are kept as computed, so an exported denormal stays one.

mod alu;
mod fetch;

use std::fmt::Write as _;

use crate::cf::{unpack_pair, CfOpcode};
use crate::fetch::FetchInstruction;
use crate::parsed::*;
use crate::shader::{analyze, Analysis, ShaderType};

/// Why a shader can't be translated (yet).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// `ccall` / `ret` (none in Call of Duty 3's shaders).
    Call,
    /// Memory export (`eA`, `eM#`).
    MemoryExport,
    /// A fetch operation this translator doesn't do yet.
    TextureOperation(&'static str),
    /// An ALU opcode with no meaning (`opcode_41` and up).
    InvalidOpcode(&'static str),
}

impl std::fmt::Display for Refusal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Refusal::Call => write!(f, "control flow calls"),
            Refusal::MemoryExport => write!(f, "memory export"),
            Refusal::TextureOperation(n) => write!(f, "texture operation {}", n),
            Refusal::InvalidOpcode(n) => write!(f, "invalid ALU opcode {}", n),
        }
    }
}

/// A translated shader.
#[derive(Clone, Debug)]
pub struct Translation {
    pub wgsl: String,
    pub shader_type: ShaderType,
    /// Whether the module declares group 1 (the texture tables).
    pub uses_textures: bool,
    /// One bit per oC# written (pixel shaders).
    pub color_targets: u32,
    pub writes_depth: bool,
}

/// Choices that change what a translation computes. The default is what the renderer uses.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Options {
    /// `getWeights2D` / `getWeights3D` write zeros instead of the bilinear weights. Only for
    /// checking against the SDK's shader interpreter, which has no texture unit and writes
    /// zeros for every texture fetch (`engine/tools/xenos_diff`).
    pub zero_texture_weights: bool,
}

/// Translates one shader (`ucode` in host order, as `dump_shaders` writes it).
pub fn translate(ucode: &[u32], shader_type: ShaderType) -> Result<Translation, Refusal> {
    translate_with(ucode, shader_type, Options::default())
}

/// `translate` with options.
pub fn translate_with(ucode: &[u32], shader_type: ShaderType, options: Options) -> Result<Translation, Refusal> {
    let a = analyze(ucode, shader_type);
    if a.memexport_data_written != 0 || !a.memexport_stream_constants.is_empty() {
        return Err(Refusal::MemoryExport);
    }
    let mut e = Emitter::new(ucode, shader_type, &a);
    e.options = options;
    e.body()?;
    Ok(Translation {
        wgsl: e.module(),
        shader_type,
        uses_textures: e.uses_textures,
        color_targets: a.writes_color_targets,
        writes_depth: a.writes_depth,
    })
}

/// The sentinel `pc` the shader ends at.
const PC_END: &str = "0xFFFFFFFFu";

pub(crate) struct Emitter<'a> {
    ucode: &'a [u32],
    pub(crate) ty: ShaderType,
    a: &'a Analysis,
    /// The body of `switch pc`.
    pub(crate) out: String,
    /// The current indentation inside `out`.
    pub(crate) indent: usize,
    pub(crate) uses_textures: bool,
    /// The number of temporary registers declared.
    pub(crate) registers: u32,
    pub(crate) previous_vfetch_full: FetchInstruction,
    /// A counter for unique `let` names.
    pub(crate) temp: u32,
    pub(crate) options: Options,
}

impl<'a> Emitter<'a> {
    fn new(ucode: &'a [u32], ty: ShaderType, a: &'a Analysis) -> Self {
        let registers = if a.uses_register_dynamic_addressing {
            64
        } else {
            let base = a.register_static_address_bound.max(1);
            if ty == ShaderType::Pixel {
                base.clamp(16, 64)
            } else {
                base
            }
        };
        Emitter {
            ucode,
            ty,
            a,
            out: String::new(),
            indent: 3,
            uses_textures: false,
            registers,
            previous_vfetch_full: FetchInstruction::default(),
            temp: 0,
            options: Options::default(),
        }
    }

    pub(crate) fn line(&mut self, text: &str) {
        for _ in 0..self.indent {
            self.out.push_str("    ");
        }
        self.out.push_str(text);
        self.out.push('\n');
    }

    pub(crate) fn fresh(&mut self, prefix: &str) -> String {
        self.temp += 1;
        format!("{}{}", prefix, self.temp)
    }

    fn cf_count(&self) -> u32 {
        self.a.cf_pair_index_bound * 2
    }

    /// The `switch pc` cases, one per control flow instruction.
    fn body(&mut self) -> Result<(), Refusal> {
        for i in 0..self.a.cf_pair_index_bound {
            let pair = unpack_pair(&self.ucode[i as usize * 3..]);
            for (j, cf) in pair.iter().enumerate() {
                let k = i * 2 + j as u32;
                self.line(&format!("case {}u: {{", k));
                self.indent += 1;
                self.line(&format!("xe_pc = {}u;", k + 1));
                match cf.opcode() {
                    CfOpcode::Nop | CfOpcode::Alloc | CfOpcode::MarkVsFetchDone => {}
                    CfOpcode::CondCall | CfOpcode::Return => return Err(Refusal::Call),
                    CfOpcode::CondJmp => {
                        let b = parse_branch(cf, k);
                        let cond = self.condition(b.kind, b.bool_constant_index, b.condition);
                        self.line(&format!("if ({}) {{ xe_pc = {}u; }}", cond, b.target_address));
                    }
                    CfOpcode::LoopStart => {
                        let l = parse_loop_start(cf, k);
                        self.loop_start(&l);
                    }
                    CfOpcode::LoopEnd => {
                        let l = parse_loop_end(cf, k);
                        self.loop_end(&l, k);
                    }
                    _ => {
                        let exec = parse_exec(cf, k);
                        self.exec(&exec)?;
                    }
                }
                self.indent -= 1;
                self.line("}");
            }
        }
        Ok(())
    }

    /// The WGSL condition of a conditional exec, jump or call.
    fn condition(&self, kind: CondType, bool_constant: u32, condition: bool) -> String {
        match kind {
            CondType::Unconditional => "true".to_owned(),
            CondType::Predicated => format!("xe_p == {}", condition),
            CondType::Conditional => format!("xe_bool(xe_mb, {}u) == {}", bool_constant, condition),
        }
    }

    fn loop_start(&mut self, l: &ParsedLoopStart) {
        self.line(&format!("let xe_lc = xe_loop(xe_mb, {}u);", l.loop_constant_index));
        self.line("if (xe_ld >= 4u) {");
        self.line(&format!("    xe_pc = {}u;", l.loop_skip_address));
        self.line("} else {");
        if !l.is_repeat {
            self.line("    xe_liter[xe_ld] = 0u;");
        }
        self.line("    xe_lconst[xe_ld] = xe_lc;");
        self.line("    if (xe_liter[xe_ld] >= (xe_lc & 0xFFu)) {");
        self.line(&format!("        xe_pc = {}u;", l.loop_skip_address));
        self.line("    } else {");
        self.line("        xe_aL = xe_loop_address(xe_lc, xe_liter[xe_ld]);");
        self.line("        xe_ld = xe_ld + 1u;");
        self.line("    }");
        self.line("}");
    }

    fn loop_end(&mut self, l: &ParsedLoopEnd, _k: u32) {
        self.line("if (xe_ld > 0u) {");
        self.line("    let xe_d = xe_ld - 1u;");
        self.line("    xe_liter[xe_d] = xe_liter[xe_d] + 1u;");
        let brk = if l.is_predicated_break {
            format!(" && xe_p != {}", l.predicate_condition)
        } else {
            String::new()
        };
        self.line(&format!("    if (xe_liter[xe_d] < (xe_lconst[xe_d] & 0xFFu){}) {{", brk));
        self.line("        xe_aL = xe_loop_address(xe_lconst[xe_d], xe_liter[xe_d]);");
        self.line(&format!("        xe_pc = {}u;", l.loop_body_address));
        self.line("    } else {");
        self.line("        xe_ld = xe_d;");
        self.line("        if (xe_d > 0u) { xe_aL = xe_loop_address(xe_lconst[xe_d - 1u], xe_liter[xe_d - 1u]); } else { xe_aL = 0; }");
        self.line("    }");
        self.line("}");
    }

    fn exec(&mut self, exec: &ParsedExec) -> Result<(), Refusal> {
        let cond = self.condition(exec.kind, exec.bool_constant_index, exec.condition);
        self.line(&format!("if ({}) {{", cond));
        self.indent += 1;
        let mut sequence = exec.sequence;
        for offset in exec.instruction_address..exec.instruction_address + exec.instruction_count {
            let start = offset as usize * 3;
            let words = match self.ucode.get(start..start + 3) {
                Some(w) => [w[0], w[1], w[2]],
                None => [0, 0, 0],
            };
            if sequence & 0b01 != 0 {
                let op = FetchInstruction::new(&words);
                self.fetch(&op)?;
            } else {
                let op = crate::alu::AluInstruction::new(&words);
                let parsed = parse_alu(&op, self.ty);
                self.alu(&parsed)?;
            }
            sequence >>= 2;
        }
        if exec.is_end {
            self.line(&format!("xe_pc = {};", PC_END));
        }
        self.indent -= 1;
        self.line("}");
        Ok(())
    }

    // Operands and results, shared by the ALU and fetch code.

    /// The vec4 an operand reads, before swizzle, absolute value and negation.
    pub(crate) fn operand_storage(&self, o: &InstructionOperand) -> String {
        match o.source {
            StorageSource::Register => match o.addressing {
                AddressingMode::Absolute => format!("xe_r[{}]", o.index),
                _ => format!("xe_r[u32({} + xe_aL) & 63u]", o.index),
            },
            StorageSource::ConstantFloat => match o.addressing {
                AddressingMode::Absolute => format!("arena[xe_cb + {}u]", o.index),
                AddressingMode::AddressRegisterRelative => format!("xe_fconst(xe_cb, {} + xe_a0)", o.index),
                AddressingMode::LoopRelative => format!("xe_fconst(xe_cb, {} + xe_aL)", o.index),
            },
            // Fetch constants are read by the fetch code itself.
            _ => "vec4<f32>(0.0)".to_owned(),
        }
    }

    /// Applies `|x|` then `-x`.
    fn modifiers(o: &InstructionOperand, value: String) -> String {
        let v = if o.is_absolute_value { format!("abs({})", value) } else { value };
        if o.is_negated {
            format!("(-{})", v)
        } else {
            v
        }
    }

    /// One component of the operand (by index into its swizzle) with its modifiers.
    pub(crate) fn operand_scalar(&self, o: &InstructionOperand, i: u32) -> String {
        Self::modifiers(o, format!("{}.{}", self.operand_storage(o), o.component(i).as_char()))
    }

    /// An ALU operand as a vec4 (component_count 4) with its modifiers, denormals flushed to (signed)
    /// zero before the modifiers, as the console's ALU (and the SDK's interpreter) reads them.
    /// Fetch coordinates are not flushed.
    pub(crate) fn alu_operand_vec4(&self, o: &InstructionOperand) -> String {
        let s: String = (0..4).map(|i| o.component(i).as_char()).collect();
        let v = if s == "xyzw" {
            self.operand_storage(o)
        } else {
            format!("{}.{}", self.operand_storage(o), s)
        };
        Self::modifiers(o, format!("xe_ftz4({})", v))
    }

    /// One component of an ALU operand, denormals flushed (see `alu_operand_vec4`).
    pub(crate) fn alu_operand_scalar(&self, o: &InstructionOperand, i: u32) -> String {
        Self::modifiers(o, format!("xe_ftz({}.{})", self.operand_storage(o), o.component(i).as_char()))
    }

    /// The l-value of a result target, as a vec4 (or the depth scalar).
    fn result_target(&self, r: &InstructionResult) -> Result<String, Refusal> {
        Ok(match r.target {
            StorageTarget::None => String::new(),
            StorageTarget::Register => match r.addressing {
                AddressingMode::Absolute => format!("xe_r[{}]", r.index),
                _ => format!("xe_r[u32({} + xe_aL) & 63u]", r.index),
            },
            StorageTarget::Interpolator => format!("xe_o[{}]", r.index & 15),
            StorageTarget::Position => "xe_opos".to_owned(),
            StorageTarget::PointSizeEdgeFlagKillVertex => "xe_opts".to_owned(),
            StorageTarget::Color => format!("xe_oc[{}]", r.index & 3),
            StorageTarget::Depth => "xe_odepth".to_owned(),
            StorageTarget::ExportAddress | StorageTarget::ExportData => return Err(Refusal::MemoryExport),
        })
    }

    /// Stores `value` (a vec4 expression, or a scalar if `scalar`) through a result:
    /// the write mask, the result swizzle (with 0 and 1) and the clamp.
    pub(crate) fn store(&mut self, r: &InstructionResult, value: &str, scalar: bool) -> Result<(), Refusal> {
        let mask = r.used_write_mask();
        if mask == 0 || r.target == StorageTarget::None {
            return Ok(());
        }
        let target = self.result_target(r)?;
        let v = if r.is_clamped { format!("saturate({})", value) } else { value.to_owned() };
        let name = self.fresh("xe_s");
        self.line(&format!("let {} = {};", name, v));
        if r.target == StorageTarget::Depth {
            let c = if scalar {
                name.clone()
            } else {
                match r.components[0] {
                    Swizzle::Zero => "0.0".to_owned(),
                    Swizzle::One => "1.0".to_owned(),
                    s => format!("{}.{}", name, s.as_char()),
                }
            };
            self.line(&format!("{} = {};", target, c));
            return Ok(());
        }
        if !scalar && r.is_standard_swizzle() {
            self.line(&format!("{} = {};", target, name));
            return Ok(());
        }
        for i in 0..4 {
            if mask & (1 << i) == 0 {
                continue;
            }
            let c = match r.components[i] {
                Swizzle::Zero => "0.0".to_owned(),
                Swizzle::One => "1.0".to_owned(),
                s if scalar => {
                    let _ = s;
                    name.clone()
                }
                s => format!("{}.{}", name, s.as_char()),
            };
            self.line(&format!("{}.{} = {};", target, ['x', 'y', 'z', 'w'][i], c));
        }
        Ok(())
    }

    /// The whole module.
    fn module(&self) -> String {
        let mut m = String::new();
        m.push_str(PRELUDE);
        if self.uses_textures {
            m.push_str(TEXTURE_BINDINGS);
        }
        let n = self.registers;
        let cf = self.cf_count();
        match self.ty {
            ShaderType::Vertex => {
                m.push_str(
                    "@vertex\nfn main(@builtin(vertex_index) xe_vid: u32, @builtin(instance_index) xe_base: u32) -> XeVsOut {\n",
                );
                m.push_str("    let xe_hdr = bitcast<vec4<u32>>(arena[xe_base]);\n");
                m.push_str("    let xe_cb = xe_hdr.x;\n");
            }
            ShaderType::Pixel => {
                let ret = if self.a.writes_color_targets != 0 || self.a.writes_depth { " -> XePsOut" } else { "" };
                let _ = write!(m, "@fragment\nfn main(xe_in: XeVsOut){} {{\n", ret);
                m.push_str("    let xe_base = xe_in.base;\n");
                m.push_str("    let xe_hdr = bitcast<vec4<u32>>(arena[xe_base]);\n");
                m.push_str("    let xe_cb = xe_hdr.y;\n");
            }
        }
        m.push_str("    let xe_fb = xe_hdr.z;\n    let xe_mb = xe_hdr.w;\n");
        let _ = writeln!(m, "    var xe_r: array<vec4<f32>, {}>;", n);
        m.push_str(
            "    var xe_p: bool = false;\n    var xe_ps: f32 = 0.0;\n    var xe_a0: i32 = 0;\n    var xe_aL: i32 = 0;\n    \
             var xe_ld: u32 = 0u;\n    var xe_lconst: array<u32, 4>;\n    var xe_liter: array<u32, 4>;\n    \
             var xe_vaddr: i32 = 0;\n    var xe_lod: f32 = 0.0;\n    var xe_gh: vec3<f32> = vec3<f32>(0.0);\n    \
             var xe_gv: vec3<f32> = vec3<f32>(0.0);\n",
        );
        match self.ty {
            ShaderType::Vertex => {
                m.push_str(
                    "    var xe_o: array<vec4<f32>, 16>;\n    var xe_opos: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n    \
                     var xe_opts: vec4<f32> = vec4<f32>(0.0);\n",
                );
                m.push_str("    xe_r[0].x = f32(i32(xe_vid) + bitcast<i32>(xe_u(xe_mb + 10u, 0u)));\n");
            }
            ShaderType::Pixel => {
                m.push_str("    var xe_oc: array<vec4<f32>, 4>;\n    var xe_odepth: f32 = xe_in.pos.z;\n");
                for i in 0..n.min(16) {
                    let _ = writeln!(m, "    xe_r[{}] = xe_in.o{};", i, i);
                }
            }
        }
        let _ = write!(
            m,
            "    var xe_pc: u32 = 0u;\n    loop {{\n        if (xe_pc >= {}u) {{ break; }}\n        switch xe_pc {{\n",
            cf
        );
        m.push_str(&self.out);
        let _ = write!(m, "            default: {{ xe_pc = {}; }}\n        }}\n    }}\n", PC_END);
        match self.ty {
            ShaderType::Vertex => {
                m.push_str("    var xe_out: XeVsOut;\n");
                m.push_str("    let xe_ns = arena[xe_mb + 11u].xyz;\n    let xe_no = arena[xe_mb + 12u].xyz;\n");
                m.push_str("    xe_out.pos = vec4<f32>(xe_opos.xyz * xe_ns + xe_no * xe_opos.w, xe_opos.w);\n");
                for i in 0..16 {
                    let _ = writeln!(m, "    xe_out.o{} = xe_o[{}];", i, i);
                }
                m.push_str("    xe_out.base = xe_base;\n    return xe_out;\n}\n");
            }
            ShaderType::Pixel => {
                let mut fields = String::new();
                let mut sets = String::new();
                for i in 0..4 {
                    if self.a.writes_color_targets & (1 << i) != 0 {
                        let _ = writeln!(fields, "    @location({}) c{}: vec4<f32>,", i, i);
                        let _ = writeln!(sets, "    xe_out.c{} = xe_oc[{}];", i, i);
                    }
                }
                if self.a.writes_depth {
                    fields.push_str("    @builtin(frag_depth) depth: f32,\n");
                    sets.push_str("    xe_out.depth = xe_odepth;\n");
                }
                if fields.is_empty() {
                    m.push_str("}\n");
                } else {
                    m.push_str("    var xe_out: XePsOut;\n");
                    m.push_str(&sets);
                    m.push_str("    return xe_out;\n}\n");
                    let _ = write!(m, "\nstruct XePsOut {{\n{}}}\n", fields);
                }
            }
        }
        m
    }
}

/// Bindings, the stage interface and the helpers every module has.
const PRELUDE: &str = r#"// Translated from Xbox 360 (Xenos) shader microcode by xenos_ucode::wgsl.
diagnostic(off, derivative_uniformity);

@group(0) @binding(0) var<storage, read> arena: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> guest: array<u32>;

struct XeVsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) o0: vec4<f32>,
    @location(1) o1: vec4<f32>,
    @location(2) o2: vec4<f32>,
    @location(3) o3: vec4<f32>,
    @location(4) o4: vec4<f32>,
    @location(5) o5: vec4<f32>,
    @location(6) o6: vec4<f32>,
    @location(7) o7: vec4<f32>,
    @location(8) o8: vec4<f32>,
    @location(9) o9: vec4<f32>,
    @location(10) o10: vec4<f32>,
    @location(11) o11: vec4<f32>,
    @location(12) o12: vec4<f32>,
    @location(13) o13: vec4<f32>,
    @location(14) o14: vec4<f32>,
    @location(15) o15: vec4<f32>,
    @location(16) @interpolate(flat) base: u32,
}

fn xe_u(row: u32, c: u32) -> u32 {
    return bitcast<u32>(arena[row][c]);
}

// A float constant through a0 or aL: 0 outside c0-c255 (as the GPU reads it).
fn xe_fconst(base: u32, index: i32) -> vec4<f32> {
    if (index < 0 || index > 255) {
        return vec4<f32>(0.0);
    }
    return arena[base + u32(index)];
}

fn xe_bool(misc: u32, i: u32) -> bool {
    return ((xe_u(misc + (i >> 7u), (i >> 5u) & 3u) >> (i & 31u)) & 1u) != 0u;
}

fn xe_loop(misc: u32, i: u32) -> u32 {
    return xe_u(misc + 2u + (i >> 2u), i & 3u);
}

// aL: the loop constant's start plus the iteration times its signed step.
fn xe_loop_address(c: u32, iteration: u32) -> i32 {
    return i32((c >> 8u) & 0xFFu) + i32(iteration) * extractBits(bitcast<i32>(c), 16u, 8u);
}

const XE_FLT_MAX: f32 = 3.40282346e38;

// Direct3D 9 multiplication: 0 (or a denormal) times anything is +0.
fn xe_mul(a: vec4<f32>, b: vec4<f32>) -> vec4<f32> {
    return select(vec4<f32>(0.0), a * b, (a != vec4<f32>(0.0)) & (b != vec4<f32>(0.0)));
}
fn xe_muls(a: f32, b: f32) -> f32 {
    return select(0.0, a * b, a != 0.0 && b != 0.0);
}
// max / min with the console's NaN behavior (isgreaterequal / isless).
fn xe_max(a: vec4<f32>, b: vec4<f32>) -> vec4<f32> {
    return select(b, a, a >= b);
}
fn xe_min(a: vec4<f32>, b: vec4<f32>) -> vec4<f32> {
    return select(b, a, a < b);
}
fn xe_maxs(a: f32, b: f32) -> f32 {
    return select(b, a, a >= b);
}
fn xe_mins(a: f32, b: f32) -> f32 {
    return select(b, a, a < b);
}
// Denormals flushed to zero, keeping the sign: what the console's ALU does to its operands.
fn xe_ftz(x: f32) -> f32 {
    let b = bitcast<u32>(x);
    return select(x, bitcast<f32>(b & 0x80000000u), (b & 0x7F800000u) == 0u);
}
fn xe_ftz4(x: vec4<f32>) -> vec4<f32> {
    let b = bitcast<vec4<u32>>(x);
    return select(x, bitcast<vec4<f32>>(b & vec4<u32>(0x80000000u)), (b & vec4<u32>(0x7F800000u)) == vec4<u32>(0u));
}
fn xe_isfinite(x: f32) -> bool {
    return (bitcast<u32>(x) & 0x7F800000u) != 0x7F800000u;
}
// The address register: round to nearest after clamping to -256..255.
fn xe_a0_from(x: f32) -> i32 {
    return i32(floor(clamp(x, -256.0, 255.0) + 0.5));
}

// The fetch constant's endianness (0 none, 1 8in16, 2 8in32, 3 16in32).
fn xe_swap(v: u32, endian: u32) -> u32 {
    var x = v;
    if (endian == 1u || endian == 2u) {
        x = ((x & 0x00FF00FFu) << 8u) | ((x >> 8u) & 0x00FF00FFu);
    }
    if (endian == 2u || endian == 3u) {
        x = (x << 16u) | (x >> 16u);
    }
    return x;
}

// One vertex fetch dword: 0 outside the fetch constant's buffer (its address and size
// in dwords), as the SDK's interpreter reads it.
fn xe_vword(address: i32, base: u32, size: u32, endian: u32) -> u32 {
    let a = bitcast<u32>(address);
    if (a < base || a - base >= size) {
        return 0u;
    }
    return xe_swap(guest[a], endian);
}
"#;

/// The texture tables (group 1) and their helpers.
const TEXTURE_BINDINGS: &str = r#"
@group(1) @binding(0) var tex2d: binding_array<texture_2d<f32>>;
@group(1) @binding(1) var tex3d: binding_array<texture_3d<f32>>;
@group(1) @binding(2) var texcube: binding_array<texture_cube<f32>>;
@group(1) @binding(3) var samp: binding_array<sampler>;

fn xe_tswz1(v: vec4<f32>, code: u32) -> f32 {
    switch code {
        case 0u: { return v.x; }
        case 1u: { return v.y; }
        case 2u: { return v.z; }
        case 3u: { return v.w; }
        case 5u: { return 1.0; }
        default: { return 0.0; }
    }
}
// The host's swizzle for a fetch constant (3 bits per component).
fn xe_tswz(v: vec4<f32>, s: u32) -> vec4<f32> {
    return vec4<f32>(xe_tswz1(v, s & 7u), xe_tswz1(v, (s >> 3u) & 7u), xe_tswz1(v, (s >> 6u) & 7u),
                     xe_tswz1(v, (s >> 9u) & 7u));
}
// Xenos cube coordinates (SC and TC in 1...2, the face index) to a direction.
fn xe_cube_dir(c: vec3<f32>) -> vec3<f32> {
    let sc = c.x * 2.0 - 3.0;
    let tc = c.y * 2.0 - 3.0;
    let face = u32(clamp(c.z, 0.0, 5.0));
    let negative = (face & 1u) != 0u;
    let sign = select(1.0, -1.0, negative);
    switch face >> 1u {
        case 0u: { return vec3<f32>(sign, -tc, select(-sc, sc, negative)); }
        case 1u: { return vec3<f32>(sc, sign, select(tc, -tc, negative)); }
        default: { return vec3<f32>(select(sc, -sc, negative), -tc, sign); }
    }
}
"#;

#[cfg(test)]
mod tests {
    use super::*;

    /// The one-instruction shader of `lib.rs`'s test: `exece` of a default nop.
    fn minimal() -> Vec<u32> {
        let cf0_d0 = 1 | (1 << 12);
        let cf0_d1 = (2 << 12) | (1 << 9);
        vec![cf0_d0, cf0_d1, 0, 50 << 26, 0, (2 << 24) | (1 << 31) | (1 << 30)]
    }

    #[test]
    fn minimal_pixel_shader() {
        let t = translate(&minimal(), ShaderType::Pixel).unwrap();
        assert!(t.wgsl.contains("@fragment"));
        assert!(t.wgsl.contains("case 0u: {"));
        assert!(t.wgsl.contains("xe_pc = 0xFFFFFFFFu;"));
        assert!(!t.uses_textures);
    }

    #[test]
    fn minimal_vertex_shader() {
        let t = translate(&minimal(), ShaderType::Vertex).unwrap();
        assert!(t.wgsl.contains("@vertex"));
        assert!(t.wgsl.contains("xe_out.pos ="));
    }
}
