//! Parsed instructions: the decoded meaning of each instruction, independent of
//! its encoding (Xenia `shader.h` `Parsed*Instruction`, `shader_translator.cc`
//! `Parse*`). The translator and the disassembler both work from these.

use crate::alu::{self, AluInstruction, SCALAR_OPCODES, VECTOR_OPCODES};
use crate::cf::{AllocType, CfInstruction, CfOpcode};
use crate::fetch::{Dimension, FetchInstruction, FetchOpcode};
use crate::shader::ShaderType;

/// Where a component comes from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Swizzle {
    X,
    Y,
    Z,
    W,
    Zero,
    One,
}

impl Swizzle {
    pub fn from_index(i: u32) -> Self {
        match i & 3 {
            0 => Self::X,
            1 => Self::Y,
            2 => Self::Z,
            _ => Self::W,
        }
    }
    pub fn as_char(self) -> char {
        match self {
            Self::X => 'x',
            Self::Y => 'y',
            Self::Z => 'z',
            Self::W => 'w',
            Self::Zero => '0',
            Self::One => '1',
        }
    }
    pub fn is_component(self) -> bool {
        matches!(self, Self::X | Self::Y | Self::Z | Self::W)
    }
}

/// `GetSwizzledAluSourceComponent`.
pub fn alu_source_component(swizzle: u32, component_index: u32) -> Swizzle {
    Swizzle::from_index(AluInstruction::swizzled_component_index(swizzle, component_index))
}

const XYZW: [Swizzle; 4] = [Swizzle::X, Swizzle::Y, Swizzle::Z, Swizzle::W];

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum StorageTarget {
    None,
    /// r# [0-63].
    Register,
    /// o# [0-15], vertex shader.
    Interpolator,
    /// oPos.
    Position,
    /// oPts: point size, edge flag, kill vertex.
    PointSizeEdgeFlagKillVertex,
    /// eA.
    ExportAddress,
    /// eM# [0-4].
    ExportData,
    /// oC# [0-3], pixel shader.
    Color,
    /// oDepth (X).
    Depth,
}

impl StorageTarget {
    /// `GetInstructionStorageTargetUsedComponentCount`.
    pub fn used_component_count(self) -> u32 {
        match self {
            Self::None => 0,
            Self::PointSizeEdgeFlagKillVertex => 3,
            Self::Depth => 1,
            _ => 4,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AddressingMode {
    Absolute,
    /// Indexed by a0 (float constants only).
    AddressRegisterRelative,
    /// Indexed by aL.
    LoopRelative,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InstructionResult {
    pub target: StorageTarget,
    pub index: u32,
    pub addressing: AddressingMode,
    pub is_clamped: bool,
    /// The write mask as encoded, components the target lacks included.
    pub original_write_mask: u32,
    pub components: [Swizzle; 4],
}

impl Default for InstructionResult {
    fn default() -> Self {
        Self {
            target: StorageTarget::None,
            index: 0,
            addressing: AddressingMode::Absolute,
            is_clamped: false,
            original_write_mask: 0,
            components: XYZW,
        }
    }
}

impl InstructionResult {
    pub fn used_write_mask(&self) -> u32 {
        self.original_write_mask & ((1u32 << self.target.used_component_count()) - 1)
    }
    pub fn is_standard_swizzle(&self) -> bool {
        self.used_write_mask() == 0b1111 && self.components == XYZW
    }
    /// Components of the result, before swizzling, that are written (not constants).
    pub fn used_result_components(&self) -> u32 {
        let mask = self.used_write_mask();
        let mut used = 0;
        for i in 0..4 {
            if mask & (1 << i) != 0 && self.components[i].is_component() {
                used |= 1 << (self.components[i] as u32);
            }
        }
        used
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum StorageSource {
    /// r# [0-63].
    Register,
    /// c# [0-255].
    ConstantFloat,
    /// vf# [0-95].
    VertexFetchConstant,
    /// tf# [0-31].
    TextureFetchConstant,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InstructionOperand {
    pub source: StorageSource,
    pub index: u32,
    pub addressing: AddressingMode,
    pub is_negated: bool,
    /// Taken before the negation.
    pub is_absolute_value: bool,
    pub component_count: u32,
    pub components: [Swizzle; 4],
}

impl Default for InstructionOperand {
    fn default() -> Self {
        Self {
            source: StorageSource::Register,
            index: 0,
            addressing: AddressingMode::Absolute,
            is_negated: false,
            is_absolute_value: false,
            component_count: 4,
            components: XYZW,
        }
    }
}

impl InstructionOperand {
    pub fn is_standard_swizzle(&self) -> bool {
        self.component_count == 4 && self.components == XYZW
    }
    /// The swizzle of component `index`, the rightmost one repeated past the count.
    pub fn component(&self, index: u32) -> Swizzle {
        self.components[index.min(self.component_count.saturating_sub(1)) as usize]
    }
}

// Control flow.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CondType {
    Unconditional,
    /// On a bool constant.
    Conditional,
    /// On the predicate.
    Predicated,
}

#[derive(Clone, Copy, Debug)]
pub struct ParsedExec {
    pub dword_index: u32,
    pub opcode: CfOpcode,
    pub name: &'static str,
    pub instruction_address: u32,
    pub instruction_count: u32,
    pub kind: CondType,
    pub bool_constant_index: u32,
    pub condition: bool,
    pub is_end: bool,
    pub is_predicate_clean: bool,
    pub is_yield: bool,
    pub sequence: u32,
}

impl ParsedExec {
    fn base(cf: &CfInstruction, cf_index: u32, name: &'static str, kind: CondType) -> Self {
        Self {
            dword_index: cf_index,
            opcode: cf.opcode(),
            name,
            instruction_address: cf.exec_address(),
            instruction_count: cf.exec_count(),
            kind,
            bool_constant_index: 0,
            condition: false,
            is_end: false,
            is_predicate_clean: true,
            is_yield: cf.exec_is_yield(),
            sequence: cf.exec_sequence(),
        }
    }
}

/// `ParseControlFlowExec`, `ParseControlFlowCondExec`, `ParseControlFlowCondExecPred`.
pub fn parse_exec(cf: &CfInstruction, cf_index: u32) -> ParsedExec {
    match cf.opcode() {
        CfOpcode::Exec | CfOpcode::ExecEnd => {
            let end = cf.opcode() == CfOpcode::ExecEnd;
            let mut i = ParsedExec::base(cf, cf_index, if end { "exece" } else { "exec" }, CondType::Unconditional);
            i.is_end = end;
            i.is_predicate_clean = cf.exec_is_predicate_clean();
            i
        }
        CfOpcode::CondExecPred | CfOpcode::CondExecPredEnd => {
            let end = cf.opcode() == CfOpcode::CondExecPredEnd;
            let mut i = ParsedExec::base(cf, cf_index, if end { "exece" } else { "exec" }, CondType::Predicated);
            i.condition = cf.exec_condition();
            i.is_end = end;
            i.is_predicate_clean = cf.exec_is_predicate_clean();
            i
        }
        _ => {
            // CondExec, CondExecEnd, CondExecPredClean, CondExecPredCleanEnd.
            let end = matches!(cf.opcode(), CfOpcode::CondExecEnd | CfOpcode::CondExecPredCleanEnd);
            let mut i = ParsedExec::base(cf, cf_index, if end { "cexece" } else { "cexec" }, CondType::Conditional);
            i.is_end = end;
            i.bool_constant_index = cf.cond_exec_bool_address();
            i.condition = cf.exec_condition();
            if matches!(cf.opcode(), CfOpcode::CondExec | CfOpcode::CondExecEnd) {
                i.is_predicate_clean = false;
            }
            i
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ParsedLoopStart {
    pub dword_index: u32,
    pub loop_constant_index: u32,
    pub is_repeat: bool,
    pub loop_skip_address: u32,
}

pub fn parse_loop_start(cf: &CfInstruction, cf_index: u32) -> ParsedLoopStart {
    ParsedLoopStart {
        dword_index: cf_index,
        loop_constant_index: cf.loop_id(),
        is_repeat: cf.loop_start_is_repeat(),
        loop_skip_address: cf.loop_address(),
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ParsedLoopEnd {
    pub dword_index: u32,
    pub is_predicated_break: bool,
    pub predicate_condition: bool,
    pub loop_constant_index: u32,
    pub loop_body_address: u32,
}

pub fn parse_loop_end(cf: &CfInstruction, cf_index: u32) -> ParsedLoopEnd {
    ParsedLoopEnd {
        dword_index: cf_index,
        is_predicated_break: cf.loop_end_is_predicated_break(),
        predicate_condition: cf.loop_end_condition(),
        loop_constant_index: cf.loop_id(),
        loop_body_address: cf.loop_address(),
    }
}

/// A call or a jump (`ParsedCallInstruction`, `ParsedJumpInstruction`).
#[derive(Clone, Copy, Debug)]
pub struct ParsedBranch {
    pub dword_index: u32,
    pub target_address: u32,
    pub kind: CondType,
    pub bool_constant_index: u32,
    pub condition: bool,
}

pub fn parse_branch(cf: &CfInstruction, cf_index: u32) -> ParsedBranch {
    let mut b = ParsedBranch {
        dword_index: cf_index,
        target_address: cf.target_address(),
        kind: CondType::Unconditional,
        bool_constant_index: 0,
        condition: false,
    };
    if cf.is_unconditional() {
    } else if cf.is_predicated() {
        b.kind = CondType::Predicated;
        b.condition = cf.condition();
    } else {
        b.kind = CondType::Conditional;
        b.bool_constant_index = cf.bool_address();
        b.condition = cf.condition();
    }
    b
}

#[derive(Clone, Copy, Debug)]
pub struct ParsedAlloc {
    pub dword_index: u32,
    pub kind: AllocType,
    pub count: u32,
    pub is_vertex_shader: bool,
}

pub fn parse_alloc(cf: &CfInstruction, cf_index: u32, is_vertex_shader: bool) -> ParsedAlloc {
    ParsedAlloc { dword_index: cf_index, kind: cf.alloc_type(), count: cf.alloc_size(), is_vertex_shader }
}

// Fetch.

/// `ParseFetchInstructionResult`.
fn parse_fetch_result(dest: u32, swizzle: u32, is_relative: bool) -> InstructionResult {
    let mut r = InstructionResult {
        target: StorageTarget::Register,
        index: dest,
        addressing: if is_relative { AddressingMode::LoopRelative } else { AddressingMode::Absolute },
        is_clamped: false,
        original_write_mask: 0b1111,
        components: XYZW,
    };
    for i in 0..4 {
        let mut source = Swizzle::Zero;
        match (swizzle >> (3 * i)) & 0b111 {
            0 => source = Swizzle::X,
            1 => source = Swizzle::Y,
            2 => source = Swizzle::Z,
            3 => source = Swizzle::W,
            5 => source = Swizzle::One,
            7 => r.original_write_mask &= !(1 << i),
            // 4 (zero) and the invalid 6.
            _ => {}
        }
        r.components[i as usize] = source;
    }
    r
}

#[derive(Clone, Copy, Debug, Default)]
pub struct VertexFetchAttributes {
    /// `xenos::VertexFormat`, 0 undefined.
    pub data_format: u32,
    pub offset: i32,
    /// In dwords.
    pub stride: u32,
    pub exp_adjust: i32,
    /// Prefetch count minus 1.
    pub prefetch_count: u32,
    pub signed_rf_mode: u32,
    pub is_index_rounded: bool,
    pub is_signed: bool,
    pub is_integer: bool,
}

#[derive(Clone, Copy, Debug)]
pub struct ParsedVertexFetch {
    pub name: &'static str,
    pub is_mini_fetch: bool,
    pub is_predicated: bool,
    pub predicate_condition: bool,
    pub result: InstructionResult,
    /// The index register and the vertex fetch constant; for vfetch_mini, those of the
    /// vfetch_full before it.
    pub operands: [InstructionOperand; 2],
    pub attributes: VertexFetchAttributes,
}

/// `ParseVertexFetchInstruction`: `previous_full` is the last vfetch_full (zeros at the
/// start). Returns whether this was a vfetch_full (then it becomes the previous one).
pub fn parse_vertex_fetch(op: &FetchInstruction, previous_full: &FetchInstruction) -> (ParsedVertexFetch, bool) {
    let mini = op.vertex_is_mini_fetch();
    let full = if mini { previous_full } else { op };
    let mut src = InstructionOperand {
        source: StorageSource::Register,
        index: full.src(),
        addressing: if full.is_src_relative() { AddressingMode::LoopRelative } else { AddressingMode::Absolute },
        component_count: 1,
        ..InstructionOperand::default()
    };
    src.components[0] = Swizzle::from_index(full.vertex_src_swizzle() & 3);
    let constant = InstructionOperand {
        source: StorageSource::VertexFetchConstant,
        index: full.vertex_fetch_constant_index(),
        ..InstructionOperand::default()
    };
    let instr = ParsedVertexFetch {
        name: if mini { "vfetch_mini" } else { "vfetch_full" },
        is_mini_fetch: mini,
        is_predicated: op.is_predicated(),
        predicate_condition: op.predicate_condition(),
        result: parse_fetch_result(op.dest(), op.dest_swizzle(), op.is_dest_relative()),
        operands: [src, constant],
        attributes: VertexFetchAttributes {
            data_format: op.vertex_data_format(),
            offset: op.vertex_offset(),
            stride: full.vertex_stride(),
            exp_adjust: op.vertex_exp_adjust(),
            prefetch_count: op.vertex_prefetch_count(),
            signed_rf_mode: op.vertex_signed_rf_mode(),
            is_index_rounded: full.vertex_is_index_rounded(),
            is_signed: op.vertex_is_signed(),
            is_integer: !op.vertex_is_normalized(),
        },
    };
    (instr, !mini)
}

#[derive(Clone, Copy, Debug)]
pub struct TextureFetchAttributes {
    pub fetch_valid_only: bool,
    pub unnormalized_coordinates: bool,
    /// `xenos::TextureFilter`; 3 = from the fetch constant.
    pub mag_filter: u32,
    pub min_filter: u32,
    pub mip_filter: u32,
    /// `xenos::AnisoFilter`; 7 = from the fetch constant.
    pub aniso_filter: u32,
    pub vol_mag_filter: u32,
    pub vol_min_filter: u32,
    pub use_computed_lod: bool,
    pub use_register_lod: bool,
    pub use_register_gradients: bool,
    pub lod_bias: f32,
    pub offset_x: f32,
    pub offset_y: f32,
    pub offset_z: f32,
}

impl Default for TextureFetchAttributes {
    fn default() -> Self {
        Self {
            fetch_valid_only: true,
            unnormalized_coordinates: false,
            mag_filter: 3,
            min_filter: 3,
            mip_filter: 3,
            aniso_filter: 7,
            vol_mag_filter: 3,
            vol_min_filter: 3,
            use_computed_lod: true,
            use_register_lod: false,
            use_register_gradients: false,
            lod_bias: 0.0,
            offset_x: 0.0,
            offset_y: 0.0,
            offset_z: 0.0,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ParsedTextureFetch {
    pub opcode: FetchOpcode,
    pub name: &'static str,
    pub dimension: Dimension,
    pub is_predicated: bool,
    pub predicate_condition: bool,
    pub result: InstructionResult,
    pub operand_count: usize,
    /// The coordinates, and the texture fetch constant if the operation uses one.
    pub operands: [InstructionOperand; 2],
    pub attributes: TextureFetchAttributes,
}

impl ParsedTextureFetch {
    pub fn has_result(&self) -> bool {
        self.result.target != StorageTarget::None
    }
}

/// `ParseTextureFetchInstruction`.
pub fn parse_texture_fetch(op: &FetchInstruction) -> ParsedTextureFetch {
    let dimension = op.texture_dimension();
    let d = dimension.index();
    // (name, has destination, has constant, has attributes, source component count override)
    let (name, has_dest, has_const, has_attributes, override_count): (&'static str, bool, bool, bool, u32) =
        match op.opcode() {
            FetchOpcode::TextureFetch => (["tfetch1D", "tfetch2D", "tfetch3D", "tfetchCube"][d], true, true, true, 0),
            FetchOpcode::GetTextureBorderColorFrac => {
                (["getBCF1D", "getBCF2D", "getBCF3D", "getBCFCube"][d], true, true, true, 0)
            }
            FetchOpcode::GetTextureComputedLod => (
                ["getCompTexLOD1D", "getCompTexLOD2D", "getCompTexLOD3D", "getCompTexLODCube"][d],
                true,
                true,
                true,
                0,
            ),
            FetchOpcode::GetTextureGradients => ("getGradients", true, true, true, 2),
            FetchOpcode::GetTextureWeights => {
                (["getWeights1D", "getWeights2D", "getWeights3D", "getWeightsCube"][d], true, true, true, 0)
            }
            FetchOpcode::SetTextureLod => ("setTexLOD", false, false, false, 1),
            FetchOpcode::SetTextureGradientsHorz => ("setGradientH", false, false, false, 3),
            FetchOpcode::SetTextureGradientsVert => ("setGradientV", false, false, false, 3),
            // Not a valid texture operation (Xenia asserts); kept so the disassembly goes on.
            FetchOpcode::VertexFetch | FetchOpcode::Unknown(_) => ("tfetch_unknown", false, false, false, 1),
        };
    let result = if has_dest {
        parse_fetch_result(op.dest(), op.dest_swizzle(), op.is_dest_relative())
    } else {
        InstructionResult::default()
    };
    let count = if override_count != 0 { override_count } else { dimension.component_count() };
    let mut src = InstructionOperand {
        source: StorageSource::Register,
        index: op.src(),
        addressing: if op.is_src_relative() { AddressingMode::LoopRelative } else { AddressingMode::Absolute },
        component_count: count,
        ..InstructionOperand::default()
    };
    let mut swizzle = op.texture_src_swizzle();
    for j in 0..count as usize {
        src.components[j] = Swizzle::from_index(swizzle & 3);
        swizzle >>= 2;
    }
    let mut operands = [src, InstructionOperand::default()];
    let mut operand_count = 1;
    if has_const {
        operands[1] = InstructionOperand {
            source: StorageSource::TextureFetchConstant,
            index: op.texture_fetch_constant_index(),
            ..InstructionOperand::default()
        };
        operand_count = 2;
    }
    let attributes = if has_attributes {
        TextureFetchAttributes {
            fetch_valid_only: op.texture_fetch_valid_only(),
            unnormalized_coordinates: op.texture_unnormalized_coordinates(),
            mag_filter: op.texture_mag_filter(),
            min_filter: op.texture_min_filter(),
            mip_filter: op.texture_mip_filter(),
            aniso_filter: op.texture_aniso_filter(),
            vol_mag_filter: op.texture_vol_mag_filter(),
            vol_min_filter: op.texture_vol_min_filter(),
            use_computed_lod: op.texture_use_computed_lod(),
            use_register_lod: op.texture_use_register_lod(),
            use_register_gradients: op.texture_use_register_gradients(),
            lod_bias: op.texture_lod_bias(),
            offset_x: op.texture_offset_x(),
            offset_y: op.texture_offset_y(),
            offset_z: op.texture_offset_z(),
        }
    } else {
        TextureFetchAttributes::default()
    };
    ParsedTextureFetch {
        opcode: op.opcode(),
        name,
        dimension,
        is_predicated: op.is_predicated(),
        predicate_condition: op.predicate_condition(),
        result,
        operand_count,
        operands,
        attributes,
    }
}

// ALU.

#[derive(Clone, Copy, Debug)]
pub struct ParsedAlu {
    pub vector_opcode: u32,
    pub scalar_opcode: u32,
    pub vector_name: &'static str,
    pub scalar_name: &'static str,
    pub is_predicated: bool,
    pub predicate_condition: bool,
    /// The vector result, also carrying constant 0 and 1 export writes.
    pub vector_result: InstructionResult,
    pub scalar_result: InstructionResult,
    pub vector_operand_count: usize,
    pub vector_operands: [InstructionOperand; 3],
    pub scalar_operand_count: usize,
    pub scalar_operands: [InstructionOperand; 2],
}

/// `ParseAluInstructionOperand`.
fn parse_alu_operand(op: &AluInstruction, i: u32, swizzle_count: u32, scalar_second_component: u32) -> InstructionOperand {
    let mut out = InstructionOperand { is_negated: op.src_negate(i), ..InstructionOperand::default() };
    let reg = op.src_reg(i);
    if op.src_is_temp(i) {
        out.source = StorageSource::Register;
        out.index = AluInstruction::src_temp_reg(reg);
        out.is_absolute_value = AluInstruction::is_src_temp_value_absolute(reg);
        out.addressing = if AluInstruction::is_src_temp_relative(reg) {
            AddressingMode::LoopRelative
        } else {
            AddressingMode::Absolute
        };
    } else {
        out.source = StorageSource::ConstantFloat;
        out.index = reg;
        out.addressing = if op.src_const_is_addressed(i) {
            if op.is_const_address_register_relative() {
                AddressingMode::AddressRegisterRelative
            } else {
                AddressingMode::LoopRelative
            }
        } else {
            AddressingMode::Absolute
        };
        out.is_absolute_value = op.abs_constants();
    }
    out.component_count = swizzle_count;
    let swizzle = op.src_swizzle(i);
    match swizzle_count {
        1 => out.components[0] = alu_source_component(swizzle, 3),
        2 => {
            out.components[0] = alu_source_component(swizzle, 3);
            out.components[1] = alu_source_component(swizzle, scalar_second_component);
        }
        4 => {
            for j in 0..4 {
                out.components[j as usize] = alu_source_component(swizzle, j);
            }
        }
        _ => {}
    }
    out
}

/// `ParseAluInstruction`.
pub fn parse_alu(op: &AluInstruction, shader_type: ShaderType) -> ParsedAlu {
    let is_export = op.is_export();
    let mut target = StorageTarget::Register;
    let mut export_index = 0;
    if is_export {
        target = StorageTarget::None;
        let r = op.vector_dest();
        if r == alu::export::EXPORT_ADDRESS {
            target = StorageTarget::ExportAddress;
        } else if (alu::export::EXPORT_DATA_0..=alu::export::EXPORT_DATA_4).contains(&r) {
            target = StorageTarget::ExportData;
            export_index = r - alu::export::EXPORT_DATA_0;
        } else if shader_type == ShaderType::Vertex {
            if (alu::export::VS_INTERPOLATOR_0..=alu::export::VS_INTERPOLATOR_15).contains(&r) {
                target = StorageTarget::Interpolator;
                export_index = r - alu::export::VS_INTERPOLATOR_0;
            } else if r == alu::export::VS_POSITION {
                target = StorageTarget::Position;
            } else if r == alu::export::VS_POINT_SIZE_EDGE_FLAG_KILL_VERTEX {
                target = StorageTarget::PointSizeEdgeFlagKillVertex;
            }
        } else if (alu::export::PS_COLOR_0..=alu::export::PS_COLOR_3).contains(&r) {
            target = StorageTarget::Color;
            export_index = r - alu::export::PS_COLOR_0;
        } else if r == alu::export::PS_DEPTH {
            target = StorageTarget::Depth;
        }
    }

    // Vector operation and the constant 0 / 1 export writes.
    let vector_opcode = op.vector_opcode();
    let vinfo = &VECTOR_OPCODES[vector_opcode as usize];
    let mut vector_result = InstructionResult {
        target,
        addressing: AddressingMode::Absolute,
        is_clamped: op.vector_clamp(),
        ..InstructionResult::default()
    };
    if is_export {
        vector_result.index = export_index;
    } else {
        vector_result.index = op.vector_dest();
        if op.is_vector_dest_relative() {
            vector_result.addressing = AddressingMode::LoopRelative;
        }
    }
    let c0 = op.constant_0_write_mask();
    let c1 = op.constant_1_write_mask();
    vector_result.original_write_mask = op.vector_op_result_write_mask() | c0 | c1;
    for i in 0..4u32 {
        vector_result.components[i as usize] = if c0 & (1 << i) != 0 {
            Swizzle::Zero
        } else if c1 & (1 << i) != 0 {
            Swizzle::One
        } else {
            Swizzle::from_index(i)
        };
    }
    let vector_operand_count = vinfo.operand_count() as usize;
    let mut vector_operands = [InstructionOperand::default(); 3];
    for (i, operand) in vector_operands.iter_mut().enumerate().take(vector_operand_count) {
        *operand = parse_alu_operand(op, i as u32 + 1, 4, 0);
    }

    // Scalar operation.
    let scalar_opcode = op.scalar_opcode();
    let sinfo = &SCALAR_OPCODES[scalar_opcode as usize];
    let mut scalar_result = InstructionResult {
        target,
        addressing: AddressingMode::Absolute,
        is_clamped: op.scalar_clamp(),
        original_write_mask: op.scalar_op_result_write_mask(),
        components: XYZW,
        ..InstructionResult::default()
    };
    if is_export {
        scalar_result.index = export_index;
    } else {
        scalar_result.index = op.scalar_dest();
        if op.is_scalar_dest_relative() {
            scalar_result.addressing = AddressingMode::LoopRelative;
        }
    }
    let scalar_operand_count = sinfo.operand_count as usize;
    let mut scalar_operands = [InstructionOperand::default(); 2];
    if scalar_operand_count == 1 {
        // The scalar operation shares source 3 with the vector one: its `b` component is Z
        // when the vector operation reads source 3, else X (Xenia Canary PR #1149, the
        // stretched grass in Call of Duty 3, vertex shader 4B4E6FDA02FB39C3).
        scalar_operands[0] = parse_alu_operand(
            op,
            3,
            if sinfo.single_operand_is_two_component { 2 } else { 1 },
            if vinfo.operand_count() == 3 { 2 } else { 0 },
        );
    } else if scalar_operand_count == 2 {
        // mulsc / addsc / subsc: a constant (`a`, W) and a temporary register (`b`, X).
        let negate = op.src_negate(3);
        let swizzle = op.src_swizzle(3);
        let mut constant = InstructionOperand {
            source: StorageSource::ConstantFloat,
            index: op.src_reg(3),
            is_negated: negate,
            is_absolute_value: op.abs_constants(),
            addressing: if op.src_const_is_addressed(3) {
                if op.is_const_address_register_relative() {
                    AddressingMode::AddressRegisterRelative
                } else {
                    AddressingMode::LoopRelative
                }
            } else {
                AddressingMode::Absolute
            },
            component_count: 1,
            ..InstructionOperand::default()
        };
        constant.components[0] = alu_source_component(swizzle, 3);
        let mut temp = InstructionOperand {
            source: StorageSource::Register,
            index: op.scalar_const_reg_op_src_temp_reg(),
            is_negated: negate,
            is_absolute_value: op.abs_constants(),
            addressing: AddressingMode::Absolute,
            component_count: 1,
            ..InstructionOperand::default()
        };
        temp.components[0] = alu_source_component(swizzle, 0);
        scalar_operands = [constant, temp];
    }

    ParsedAlu {
        vector_opcode,
        scalar_opcode,
        vector_name: vinfo.name,
        scalar_name: sinfo.name,
        is_predicated: op.is_predicated(),
        predicate_condition: op.predicate_condition(),
        vector_result,
        scalar_result,
        vector_operand_count,
        vector_operands,
        scalar_operand_count,
        scalar_operands,
    }
}

fn is_plain_r0(o: &InstructionOperand) -> bool {
    o.source == StorageSource::Register
        && o.index == 0
        && o.addressing == AddressingMode::Absolute
        && !o.is_negated
        && !o.is_absolute_value
        && o.is_standard_swizzle()
}

impl ParsedAlu {
    /// `IsVectorOpDefaultNop`: `max r0._, r0, r0`.
    pub fn is_vector_op_default_nop(&self) -> bool {
        let r = &self.vector_result;
        if self.vector_opcode != alu::vector_op::MAX
            || r.original_write_mask != 0
            || r.is_clamped
            || !is_plain_r0(&self.vector_operands[0])
            || !is_plain_r0(&self.vector_operands[1])
        {
            return false;
        }
        if r.target == StorageTarget::Register {
            r.index == 0 && r.addressing == AddressingMode::Absolute
        } else {
            // An export keeps its vector operation to show it's an export.
            !self.is_scalar_op_default_nop()
        }
    }

    /// `IsScalarOpDefaultNop`: `retain_prev r0._`.
    pub fn is_scalar_op_default_nop(&self) -> bool {
        let r = &self.scalar_result;
        if self.scalar_opcode != alu::scalar_op::RETAIN_PREV || r.original_write_mask != 0 || r.is_clamped {
            return false;
        }
        if r.target == StorageTarget::Register {
            return r.index == 0 && r.addressing == AddressingMode::Absolute;
        }
        true
    }

    /// `IsNop`: writes nothing and changes no state.
    pub fn is_nop(&self) -> bool {
        self.scalar_opcode == alu::scalar_op::RETAIN_PREV
            && self.scalar_result.used_write_mask() == 0
            && self.vector_result.used_write_mask() == 0
            && VECTOR_OPCODES[self.vector_opcode as usize].changed_state == 0
    }

    /// `GetMemExportStreamConstant`: the c# of `mad eA, r#, const0100, c#`.
    pub fn memexport_stream_constant(&self) -> Option<u32> {
        let o = &self.vector_operands[2];
        if self.vector_result.target == StorageTarget::ExportAddress
            && self.vector_opcode == alu::vector_op::MAD
            && self.vector_result.used_result_components() == 0b1111
            && !self.vector_result.is_clamped
            && o.source == StorageSource::ConstantFloat
            && o.addressing == AddressingMode::Absolute
            && o.is_standard_swizzle()
            && !o.is_negated
            && !o.is_absolute_value
        {
            Some(o.index)
        } else {
            None
        }
    }
}
