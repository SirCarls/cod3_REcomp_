//! Whole-shader analysis: the walk over the control flow program that Xenia's
//! `Shader::AnalyzeUcode` does, producing the disassembly and what the
//! translator needs to know up front (registers, constants, bindings, exports).

use std::collections::BTreeSet;

use crate::alu::{changed_state, AluInstruction, SCALAR_OPCODES, VECTOR_OPCODES};
use crate::cf::{unpack_pair, CfOpcode};
use crate::disasm;
use crate::fetch::{FetchInstruction, FetchOpcode};
use crate::parsed::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ShaderType {
    Vertex,
    Pixel,
}

/// A vertex fetch constant and the attributes fetched through it.
#[derive(Clone, Debug)]
pub struct VertexBinding {
    pub binding_index: usize,
    pub fetch_constant: u32,
    pub stride_words: u32,
    pub attributes: Vec<ParsedVertexFetch>,
}

/// A texture fetch through a texture fetch constant.
#[derive(Clone, Debug)]
pub struct TextureBinding {
    pub binding_index: usize,
    pub fetch_constant: u32,
    pub fetch: ParsedTextureFetch,
}

/// What [`analyze`] finds.
#[derive(Clone, Debug, Default)]
pub struct Analysis {
    /// Xenia's disassembly text.
    pub disassembly: String,
    /// Control flow pairs before the first ALU / fetch instruction.
    pub cf_pair_index_bound: u32,
    pub label_addresses: BTreeSet<u32>,
    /// Highest absolutely addressed r# + 1.
    pub register_static_address_bound: u32,
    pub uses_register_dynamic_addressing: bool,
    /// One bit per float constant vec4.
    pub float_constants: [u64; 4],
    pub float_dynamic_addressing: bool,
    pub bool_constants: [u32; 8],
    pub loop_constants: u32,
    pub vertex_fetch_constants: [u32; 3],
    pub vertex_bindings: Vec<VertexBinding>,
    pub texture_bindings: Vec<TextureBinding>,
    pub uses_texture_fetch_results: bool,
    pub kills_pixels: bool,
    pub writes_depth: bool,
    /// One bit per oC#.
    pub writes_color_targets: u32,
    /// One bit per o#.
    pub writes_interpolators: u32,
    pub writes_point_size_edge_flag_kill_vertex: u32,
    /// One bit per eM# written (memory export).
    pub memexport_data_written: u32,
    pub memexport_stream_constants: BTreeSet<u32>,
}

impl Analysis {
    pub fn float_constant_count(&self) -> u32 {
        if self.float_dynamic_addressing {
            256
        } else {
            self.float_constants.iter().map(|w| w.count_ones()).sum()
        }
    }

    fn operand(&mut self, o: &InstructionOperand) {
        match o.source {
            StorageSource::Register => {
                if o.addressing == AddressingMode::Absolute {
                    self.register_static_address_bound = self.register_static_address_bound.max(o.index + 1);
                } else {
                    self.uses_register_dynamic_addressing = true;
                }
            }
            StorageSource::ConstantFloat => {
                if o.addressing == AddressingMode::Absolute {
                    self.float_constants[(o.index >> 6) as usize & 3] |= 1u64 << (o.index & 63);
                } else {
                    self.float_dynamic_addressing = true;
                }
            }
            StorageSource::VertexFetchConstant => {
                self.vertex_fetch_constants[(o.index >> 5) as usize % 3] |= 1 << (o.index & 31);
            }
            StorageSource::TextureFetchConstant => {}
        }
    }

    fn fetch_result(&mut self, r: &InstructionResult) {
        if r.used_write_mask() == 0 {
            return;
        }
        if r.addressing == AddressingMode::Absolute {
            self.register_static_address_bound = self.register_static_address_bound.max(r.index + 1);
        } else {
            self.uses_register_dynamic_addressing = true;
        }
    }

    fn alu_result(&mut self, r: &InstructionResult) {
        let mask = r.used_write_mask();
        if mask == 0 {
            return;
        }
        match r.target {
            StorageTarget::Register => {
                if r.addressing == AddressingMode::Absolute {
                    self.register_static_address_bound = self.register_static_address_bound.max(r.index + 1);
                } else {
                    self.uses_register_dynamic_addressing = true;
                }
            }
            StorageTarget::Interpolator => self.writes_interpolators |= 1 << r.index,
            StorageTarget::PointSizeEdgeFlagKillVertex => self.writes_point_size_edge_flag_kill_vertex |= mask,
            StorageTarget::ExportData => self.memexport_data_written |= 1 << r.index,
            StorageTarget::Color => self.writes_color_targets |= 1 << r.index,
            StorageTarget::Depth => self.writes_depth = true,
            _ => {}
        }
    }
}

/// Walks the shader's control flow (`Shader::AnalyzeUcode`): the disassembly, the
/// register and constant usage, the vertex and texture bindings and the exports.
/// `ucode` is the microcode in host order (as `dump_shaders` writes it).
///
/// Not ported: the memory export flow analysis (`cf_memexport_info_`); Call of
/// Duty 3's shaders don't export to memory (the M0 census).
pub fn analyze(ucode: &[u32], shader_type: ShaderType) -> Analysis {
    let mut a = Analysis::default();
    let pairs = (ucode.len() / 3) as u32;

    // The control flow program ends where the first exec's instructions start (what
    // freedreno does); the bound shrinks while it's scanned.
    a.cf_pair_index_bound = pairs;
    let mut i = 0;
    while i < a.cf_pair_index_bound {
        for cf in unpack_pair(&ucode[i as usize * 3..]) {
            if cf.opcode().is_exec() {
                a.cf_pair_index_bound = a.cf_pair_index_bound.min(cf.exec_address());
            }
            if let Some(label) = cf.label_address() {
                a.label_addresses.insert(label);
            }
        }
        i += 1;
    }

    let mut out = String::new();
    let mut previous_vfetch_full = FetchInstruction::default();
    let mut unique_texture_bindings = 0usize;
    for i in 0..a.cf_pair_index_bound {
        let pair = unpack_pair(&ucode[i as usize * 3..]);
        for (j, cf) in pair.iter().enumerate() {
            let cf_index = i * 2 + j as u32;
            if a.label_addresses.contains(&cf_index) {
                out.push_str(&format!("                label L{}\n", cf_index));
            }
            out.push_str(&format!("/* {:4}.{} */ ", i, j));
            let mut bool_constant = None;
            match cf.opcode() {
                CfOpcode::Nop => out.push_str("      cnop\n"),
                CfOpcode::Exec
                | CfOpcode::ExecEnd
                | CfOpcode::CondExec
                | CfOpcode::CondExecEnd
                | CfOpcode::CondExecPredClean
                | CfOpcode::CondExecPredCleanEnd
                | CfOpcode::CondExecPred
                | CfOpcode::CondExecPredEnd => {
                    let exec = parse_exec(cf, cf_index);
                    if exec.kind == CondType::Conditional {
                        bool_constant = Some(exec.bool_constant_index);
                    }
                    gather_exec(
                        &mut a,
                        &exec,
                        ucode,
                        shader_type,
                        &mut previous_vfetch_full,
                        &mut unique_texture_bindings,
                        &mut out,
                    );
                }
                CfOpcode::LoopStart => {
                    let l = parse_loop_start(cf, cf_index);
                    disasm::loop_start(&l, &mut out);
                    a.loop_constants |= 1 << l.loop_constant_index;
                }
                CfOpcode::LoopEnd => {
                    let l = parse_loop_end(cf, cf_index);
                    disasm::loop_end(&l, &mut out);
                    a.loop_constants |= 1 << l.loop_constant_index;
                }
                CfOpcode::CondCall => {
                    let b = parse_branch(cf, cf_index);
                    disasm::branch(&b, true, &mut out);
                    if b.kind == CondType::Conditional {
                        bool_constant = Some(b.bool_constant_index);
                    }
                }
                CfOpcode::Return => out.push_str("      ret\n"),
                CfOpcode::CondJmp => {
                    let b = parse_branch(cf, cf_index);
                    disasm::branch(&b, false, &mut out);
                    if b.kind == CondType::Conditional {
                        bool_constant = Some(b.bool_constant_index);
                    }
                }
                CfOpcode::Alloc => {
                    let al = parse_alloc(cf, cf_index, shader_type == ShaderType::Vertex);
                    disasm::alloc(&al, &mut out);
                }
                // Xenia writes nothing for it (not even a line end).
                CfOpcode::MarkVsFetchDone => {}
            }
            if let Some(b) = bool_constant {
                a.bool_constants[(b / 32) as usize & 7] |= 1 << (b % 32);
            }
        }
    }
    a.disassembly = out;
    a
}

/// `Shader::GatherExecInformation`.
fn gather_exec(
    a: &mut Analysis,
    exec: &ParsedExec,
    ucode: &[u32],
    shader_type: ShaderType,
    previous_vfetch_full: &mut FetchInstruction,
    unique_texture_bindings: &mut usize,
    out: &mut String,
) {
    disasm::exec(exec, out);
    let mut sequence = exec.sequence;
    for offset in exec.instruction_address..exec.instruction_address + exec.instruction_count {
        out.push_str(&format!("/* {:4}   */ ", offset));
        if sequence & 0b10 != 0 {
            out.push_str("         serialize\n             ");
        }
        let start = offset as usize * 3;
        let words = match ucode.get(start..start + 3) {
            Some(w) => w,
            // Past the end of the microcode: Xenia would read beyond its buffer.
            None => &[0, 0, 0][..],
        };
        if sequence & 0b01 != 0 {
            let op = FetchInstruction::new(words);
            if op.opcode() == FetchOpcode::VertexFetch {
                let (fetch, was_full) = parse_vertex_fetch(&op, previous_vfetch_full);
                if was_full {
                    *previous_vfetch_full = op;
                }
                disasm::vertex_fetch(&fetch, out);
                gather_vertex_fetch(a, &op, fetch);
            } else {
                let fetch = parse_texture_fetch(&op);
                disasm::texture_fetch(&fetch, out);
                gather_texture_fetch(a, &op, fetch, unique_texture_bindings);
            }
        } else {
            let op = AluInstruction::new(words);
            let alu = parse_alu(&op, shader_type);
            disasm::alu(&alu, out);
            a.kills_pixels = a.kills_pixels
                || VECTOR_OPCODES[op.vector_opcode() as usize].changed_state & changed_state::PIXEL_KILL != 0
                || SCALAR_OPCODES[op.scalar_opcode() as usize].changed_state & changed_state::PIXEL_KILL != 0;
            a.alu_result(&alu.vector_result);
            a.alu_result(&alu.scalar_result);
            for o in &alu.vector_operands[..alu.vector_operand_count] {
                a.operand(o);
            }
            for o in &alu.scalar_operands[..alu.scalar_operand_count] {
                a.operand(o);
            }
            if alu.vector_result.target == StorageTarget::ExportAddress {
                if let Some(c) = alu.memexport_stream_constant() {
                    a.memexport_stream_constants.insert(c);
                }
            }
        }
        sequence >>= 2;
    }
}

/// `Shader::GatherVertexFetchInformation`.
fn gather_vertex_fetch(a: &mut Analysis, op: &FetchInstruction, fetch: ParsedVertexFetch) {
    a.fetch_result(&fetch.result);
    // vfetch_mini inherits its operands from the vfetch_full.
    if !fetch.is_mini_fetch {
        for o in &fetch.operands {
            a.operand(o);
        }
    }
    if fetch.result.used_result_components() == 0 {
        return;
    }
    let constant = op.vertex_fetch_constant_index();
    if let Some(binding) = a.vertex_bindings.iter_mut().find(|b| b.fetch_constant == constant) {
        binding.attributes.push(fetch);
        return;
    }
    let binding_index = a.vertex_bindings.len();
    a.vertex_bindings.push(VertexBinding {
        binding_index,
        fetch_constant: constant,
        stride_words: fetch.attributes.stride,
        attributes: vec![fetch],
    });
}

/// `Shader::GatherTextureFetchInformation`.
fn gather_texture_fetch(
    a: &mut Analysis,
    op: &FetchInstruction,
    fetch: ParsedTextureFetch,
    unique_texture_bindings: &mut usize,
) {
    a.fetch_result(&fetch.result);
    for o in &fetch.operands[..fetch.operand_count] {
        a.operand(o);
    }
    if fetch.result.used_result_components() != 0 {
        a.uses_texture_fetch_results = true;
    }
    match op.opcode() {
        FetchOpcode::SetTextureLod | FetchOpcode::SetTextureGradientsHorz | FetchOpcode::SetTextureGradientsVert => {
            return
        }
        _ => {}
    }
    let fetch_constant = fetch.operands[1].index;
    let binding_index = match a.texture_bindings.iter().find(|b| b.fetch_constant == fetch_constant) {
        Some(b) => b.binding_index,
        None => {
            let index = *unique_texture_bindings;
            *unique_texture_bindings += 1;
            index
        }
    };
    a.texture_bindings.push(TextureBinding { binding_index, fetch_constant, fetch });
}
