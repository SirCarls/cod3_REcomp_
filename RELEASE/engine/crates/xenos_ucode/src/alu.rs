//! ALU instructions: a vector and a scalar operation co-issued, 3 dwords
//! (Xenia `ucode.h` `AluInstruction`, `ucode.cpp` opcode tables).

use crate::bits;

/// What an operation changes besides its result (`ucode::AluOpChangedState`).
pub mod changed_state {
    pub const NONE: u32 = 0;
    pub const ADDRESS_REGISTER: u32 = 1 << 0;
    pub const PREDICATE: u32 = 1 << 1;
    pub const PIXEL_KILL: u32 = 1 << 2;
}

/// `ucode::AluScalarOpcodeInfo`.
#[derive(Clone, Copy, Debug)]
pub struct ScalarOpcodeInfo {
    pub name: &'static str,
    /// 0: no operands; 1: one r# or c# (`a` = W, and `b` = X if two-component);
    /// 2: c#.w and r#.x (mulsc, addsc, subsc).
    pub operand_count: u32,
    pub single_operand_is_two_component: bool,
    pub changed_state: u32,
}

/// `ucode::AluVectorOpcodeInfo`.
#[derive(Clone, Copy, Debug)]
pub struct VectorOpcodeInfo {
    pub name: &'static str,
    pub operand_components_used: [u32; 3],
    pub changed_state: u32,
}

impl VectorOpcodeInfo {
    pub fn operand_count(&self) -> u32 {
        if self.operand_components_used[2] != 0 {
            3
        } else if self.operand_components_used[1] != 0 {
            2
        } else if self.operand_components_used[0] != 0 {
            1
        } else {
            0
        }
    }
}

use changed_state::{ADDRESS_REGISTER as AR, NONE as N, PIXEL_KILL as PK, PREDICATE as PR};

const fn s(name: &'static str, operand_count: u32, two: bool, changed_state: u32) -> ScalarOpcodeInfo {
    ScalarOpcodeInfo { name, operand_count, single_operand_is_two_component: two, changed_state }
}

/// The 64 scalar opcodes (6 bits).
pub const SCALAR_OPCODES: [ScalarOpcodeInfo; 64] = [
    s("adds", 1, true, N),
    s("adds_prev", 1, false, N),
    s("muls", 1, true, N),
    s("muls_prev", 1, false, N),
    s("muls_prev2", 1, true, N),
    s("maxs", 1, true, N),
    s("mins", 1, true, N),
    s("seqs", 1, false, N),
    s("sgts", 1, false, N),
    s("sges", 1, false, N),
    s("snes", 1, false, N),
    s("frcs", 1, false, N),
    s("truncs", 1, false, N),
    s("floors", 1, false, N),
    s("exp", 1, false, N),
    s("logc", 1, false, N),
    s("log", 1, false, N),
    s("rcpc", 1, false, N),
    s("rcpf", 1, false, N),
    s("rcp", 1, false, N),
    s("rsqc", 1, false, N),
    s("rsqf", 1, false, N),
    s("rsq", 1, false, N),
    s("maxas", 1, true, AR),
    s("maxasf", 1, true, AR),
    s("subs", 1, true, N),
    s("subs_prev", 1, false, N),
    s("setp_eq", 1, false, PR),
    s("setp_ne", 1, false, PR),
    s("setp_gt", 1, false, PR),
    s("setp_ge", 1, false, PR),
    s("setp_inv", 1, false, PR),
    s("setp_pop", 1, false, PR),
    s("setp_clr", 0, false, PR),
    s("setp_rstr", 1, false, PR),
    s("kills_eq", 1, false, PK),
    s("kills_gt", 1, false, PK),
    s("kills_ge", 1, false, PK),
    s("kills_ne", 1, false, PK),
    s("kills_one", 1, false, PK),
    s("sqrt", 1, false, N),
    s("opcode_41", 0, false, N),
    s("mulsc", 2, false, N),
    s("mulsc", 2, false, N),
    s("addsc", 2, false, N),
    s("addsc", 2, false, N),
    s("subsc", 2, false, N),
    s("subsc", 2, false, N),
    s("sin", 1, false, N),
    s("cos", 1, false, N),
    s("retain_prev", 0, false, N),
    s("opcode_51", 0, false, N),
    s("opcode_52", 0, false, N),
    s("opcode_53", 0, false, N),
    s("opcode_54", 0, false, N),
    s("opcode_55", 0, false, N),
    s("opcode_56", 0, false, N),
    s("opcode_57", 0, false, N),
    s("opcode_58", 0, false, N),
    s("opcode_59", 0, false, N),
    s("opcode_60", 0, false, N),
    s("opcode_61", 0, false, N),
    s("opcode_62", 0, false, N),
    s("opcode_63", 0, false, N),
];

const fn v(name: &'static str, used: [u32; 3], changed_state: u32) -> VectorOpcodeInfo {
    VectorOpcodeInfo { name, operand_components_used: used, changed_state }
}

/// The 32 vector opcodes (5 bits).
pub const VECTOR_OPCODES: [VectorOpcodeInfo; 32] = [
    v("add", [0b1111, 0b1111, 0], N),
    v("mul", [0b1111, 0b1111, 0], N),
    v("max", [0b1111, 0b1111, 0], N),
    v("min", [0b1111, 0b1111, 0], N),
    v("seq", [0b1111, 0b1111, 0], N),
    v("sgt", [0b1111, 0b1111, 0], N),
    v("sge", [0b1111, 0b1111, 0], N),
    v("sne", [0b1111, 0b1111, 0], N),
    v("frc", [0b1111, 0, 0], N),
    v("trunc", [0b1111, 0, 0], N),
    v("floor", [0b1111, 0, 0], N),
    v("mad", [0b1111, 0b1111, 0b1111], N),
    v("cndeq", [0b1111, 0b1111, 0b1111], N),
    v("cndge", [0b1111, 0b1111, 0b1111], N),
    v("cndgt", [0b1111, 0b1111, 0b1111], N),
    v("dp4", [0b1111, 0b1111, 0], N),
    v("dp3", [0b0111, 0b0111, 0], N),
    v("dp2add", [0b0011, 0b0011, 0b0001], N),
    v("cube", [0b1111, 0b1111, 0], N),
    v("max4", [0b1111, 0, 0], N),
    v("setp_eq_push", [0b1001, 0b1001, 0], PR),
    v("setp_ne_push", [0b1001, 0b1001, 0], PR),
    v("setp_gt_push", [0b1001, 0b1001, 0], PR),
    v("setp_ge_push", [0b1001, 0b1001, 0], PR),
    v("kill_eq", [0b1111, 0b1111, 0], PK),
    v("kill_gt", [0b1111, 0b1111, 0], PK),
    v("kill_ge", [0b1111, 0b1111, 0], PK),
    v("kill_ne", [0b1111, 0b1111, 0], PK),
    v("dst", [0b0110, 0b1010, 0], N),
    v("maxa", [0b1111, 0b1111, 0], AR),
    v("opcode_30", [0, 0, 0], N),
    v("opcode_31", [0, 0, 0], N),
];

/// Vector opcode numbers used by the parser.
pub mod vector_op {
    pub const MAX: u32 = 2;
    pub const MAD: u32 = 11;
}

/// Scalar opcode numbers used by the parser.
pub mod scalar_op {
    pub const RETAIN_PREV: u32 = 50;
}

/// Export register numbers (`ucode::ExportRegister`) for `vector_dest` when exporting.
pub mod export {
    pub const VS_INTERPOLATOR_0: u32 = 0;
    pub const VS_INTERPOLATOR_15: u32 = 15;
    pub const VS_POSITION: u32 = 62;
    pub const VS_POINT_SIZE_EDGE_FLAG_KILL_VERTEX: u32 = 63;
    pub const PS_COLOR_0: u32 = 0;
    pub const PS_COLOR_3: u32 = 3;
    pub const PS_DEPTH: u32 = 61;
    pub const EXPORT_ADDRESS: u32 = 32;
    pub const EXPORT_DATA_0: u32 = 33;
    pub const EXPORT_DATA_4: u32 = 37;
}

/// The three dwords of an ALU instruction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AluInstruction {
    pub dwords: [u32; 3],
}

impl AluInstruction {
    pub fn new(dwords: &[u32]) -> Self {
        Self { dwords: [dwords[0], dwords[1], dwords[2]] }
    }
    fn d(&self, i: usize) -> u32 {
        self.dwords[i]
    }

    // Dword 0.
    pub fn vector_dest(&self) -> u32 {
        bits(self.d(0), 0, 6)
    }
    pub fn is_vector_dest_relative(&self) -> bool {
        bits(self.d(0), 6, 1) == 1
    }
    pub fn abs_constants(&self) -> bool {
        bits(self.d(0), 7, 1) == 1
    }
    pub fn scalar_dest(&self) -> u32 {
        bits(self.d(0), 8, 6)
    }
    pub fn is_scalar_dest_relative(&self) -> bool {
        bits(self.d(0), 14, 1) == 1
    }
    pub fn is_export(&self) -> bool {
        bits(self.d(0), 15, 1) == 1
    }
    pub fn vector_write_mask(&self) -> u32 {
        bits(self.d(0), 16, 4)
    }
    pub fn scalar_write_mask(&self) -> u32 {
        bits(self.d(0), 20, 4)
    }
    pub fn vector_clamp(&self) -> bool {
        bits(self.d(0), 24, 1) == 1
    }
    pub fn scalar_clamp(&self) -> bool {
        bits(self.d(0), 25, 1) == 1
    }
    pub fn scalar_opcode(&self) -> u32 {
        bits(self.d(0), 26, 6)
    }

    // Dword 1.
    /// Component-relative swizzle of source `i` (1-3).
    pub fn src_swizzle(&self, i: u32) -> u32 {
        match i {
            1 => bits(self.d(1), 16, 8),
            2 => bits(self.d(1), 8, 8),
            _ => bits(self.d(1), 0, 8),
        }
    }
    pub fn src_negate(&self, i: u32) -> bool {
        match i {
            1 => bits(self.d(1), 26, 1) == 1,
            2 => bits(self.d(1), 25, 1) == 1,
            _ => bits(self.d(1), 24, 1) == 1,
        }
    }
    pub fn predicate_condition(&self) -> bool {
        bits(self.d(1), 27, 1) == 1
    }
    pub fn is_predicated(&self) -> bool {
        bits(self.d(1), 28, 1) == 1
    }
    pub fn is_const_address_register_relative(&self) -> bool {
        bits(self.d(1), 29, 1) == 1
    }
    fn const_1_rel_abs(&self) -> bool {
        bits(self.d(1), 30, 1) == 1
    }
    fn const_0_rel_abs(&self) -> bool {
        bits(self.d(1), 31, 1) == 1
    }

    // Dword 2.
    /// Full constant index, or packed temporary register (see [`Self::src_temp_reg`]).
    pub fn src_reg(&self, i: u32) -> u32 {
        match i {
            1 => bits(self.d(2), 16, 8),
            2 => bits(self.d(2), 8, 8),
            _ => bits(self.d(2), 0, 8),
        }
    }
    pub fn vector_opcode(&self) -> u32 {
        bits(self.d(2), 24, 5)
    }
    pub fn src_is_temp(&self, i: u32) -> bool {
        match i {
            1 => bits(self.d(2), 31, 1) == 1,
            2 => bits(self.d(2), 30, 1) == 1,
            _ => bits(self.d(2), 29, 1) == 1,
        }
    }
    fn src3_sel(&self) -> u32 {
        bits(self.d(2), 29, 1)
    }

    pub fn src_temp_reg(src_reg: u32) -> u32 {
        src_reg & 0x3F
    }
    pub fn is_src_temp_relative(src_reg: u32) -> bool {
        src_reg & 0x40 != 0
    }
    pub fn is_src_temp_value_absolute(src_reg: u32) -> bool {
        src_reg & 0x80 != 0
    }

    /// Whether constant source `i` is relatively addressed (`src_const_is_addressed`):
    /// the first constant of the instruction uses const_0_rel_abs, the others const_1_rel_abs.
    pub fn src_const_is_addressed(&self, i: u32) -> bool {
        match i {
            1 => self.const_0_rel_abs(),
            2 => {
                if self.src_is_temp(1) {
                    self.const_0_rel_abs()
                } else {
                    self.const_1_rel_abs()
                }
            }
            _ => {
                if self.src_is_temp(1) && self.src_is_temp(2) {
                    self.const_0_rel_abs()
                } else {
                    self.const_1_rel_abs()
                }
            }
        }
    }

    /// The temporary register of mulsc / addsc / subsc.
    pub fn scalar_const_reg_op_src_temp_reg(&self) -> u32 {
        (self.scalar_opcode() & 1) | (self.src3_sel() << 1) | (self.src_swizzle(3) & 0x3C)
    }

    /// The absolute component index of a component-relative swizzle.
    pub fn swizzled_component_index(swizzle: u32, component_index: u32) -> u32 {
        ((swizzle >> (2 * component_index)) + component_index) & 3
    }

    pub fn vector_op_result_write_mask(&self) -> u32 {
        let mut mask = self.vector_write_mask();
        if self.is_export() {
            mask &= !self.scalar_write_mask();
        }
        mask
    }
    pub fn scalar_op_result_write_mask(&self) -> u32 {
        let mut mask = self.scalar_write_mask();
        if self.is_export() {
            mask &= !self.vector_write_mask();
        }
        mask
    }
    pub fn constant_0_write_mask(&self) -> u32 {
        if !self.is_export() || !self.is_scalar_dest_relative() {
            return 0;
        }
        0b1111 & !(self.vector_write_mask() | self.scalar_write_mask())
    }
    pub fn constant_1_write_mask(&self) -> u32 {
        if !self.is_export() {
            return 0;
        }
        self.vector_write_mask() & self.scalar_write_mask()
    }
}
