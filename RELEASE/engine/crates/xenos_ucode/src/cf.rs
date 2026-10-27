//! Control flow instructions: 48 bits each, packed in pairs into 3 dwords at the
//! start of the microcode (Xenia `ucode.h`, `ControlFlow*Instruction`).

use crate::bits;

/// Control flow opcodes (`ucode::ControlFlowOpcode`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CfOpcode {
    Nop,
    Exec,
    ExecEnd,
    /// `cexec` on a bool constant, PredicateClean = false.
    CondExec,
    CondExecEnd,
    /// `(p0) exec`.
    CondExecPred,
    CondExecPredEnd,
    LoopStart,
    LoopEnd,
    CondCall,
    Return,
    CondJmp,
    Alloc,
    /// `cexec` on a bool constant, PredicateClean = true.
    CondExecPredClean,
    CondExecPredCleanEnd,
    MarkVsFetchDone,
}

impl CfOpcode {
    pub fn from_bits(v: u32) -> Self {
        match v & 15 {
            0 => Self::Nop,
            1 => Self::Exec,
            2 => Self::ExecEnd,
            3 => Self::CondExec,
            4 => Self::CondExecEnd,
            5 => Self::CondExecPred,
            6 => Self::CondExecPredEnd,
            7 => Self::LoopStart,
            8 => Self::LoopEnd,
            9 => Self::CondCall,
            10 => Self::Return,
            11 => Self::CondJmp,
            12 => Self::Alloc,
            13 => Self::CondExecPredClean,
            14 => Self::CondExecPredCleanEnd,
            _ => Self::MarkVsFetchDone,
        }
    }

    /// Whether it executes ALU or fetch instructions.
    pub fn is_exec(self) -> bool {
        matches!(
            self,
            Self::Exec
                | Self::ExecEnd
                | Self::CondExec
                | Self::CondExecEnd
                | Self::CondExecPred
                | Self::CondExecPredEnd
                | Self::CondExecPredClean
                | Self::CondExecPredCleanEnd
        )
    }

    /// Whether the shader ends after it (if its condition passes).
    pub fn ends_shader(self) -> bool {
        matches!(self, Self::ExecEnd | Self::CondExecEnd | Self::CondExecPredEnd | Self::CondExecPredCleanEnd)
    }
}

/// `ucode::AllocType`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AllocType {
    None,
    VsPosition,
    /// Vertex shader interpolators or pixel shader colors.
    VsInterpolatorsOrPsColors,
    Memory,
}

/// One control flow instruction: `dword_0` (32 bits) and the low 16 bits of `dword_1`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CfInstruction {
    pub dword_0: u32,
    pub dword_1: u32,
}

impl CfInstruction {
    pub fn opcode(&self) -> CfOpcode {
        CfOpcode::from_bits(bits(self.dword_1, 12, 4))
    }

    // Exec family (exec, cexec, (p0) exec).
    /// Address of the first ALU / fetch instruction (in 3-dword slots).
    pub fn exec_address(&self) -> u32 {
        bits(self.dword_0, 0, 12)
    }
    pub fn exec_count(&self) -> u32 {
        bits(self.dword_0, 12, 3)
    }
    pub fn exec_is_yield(&self) -> bool {
        bits(self.dword_0, 15, 1) == 1
    }
    /// Two bits per instruction: bit 0 fetch (else ALU), bit 1 serialize.
    pub fn exec_sequence(&self) -> u32 {
        bits(self.dword_0, 16, 12)
    }
    /// `exec` and `(p0) exec` only.
    pub fn exec_is_predicate_clean(&self) -> bool {
        bits(self.dword_1, 9, 1) == 1
    }
    /// `cexec`: the bool constant.
    pub fn cond_exec_bool_address(&self) -> u32 {
        bits(self.dword_1, 2, 8)
    }
    /// `cexec` and `(p0) exec`: the required value.
    pub fn exec_condition(&self) -> bool {
        bits(self.dword_1, 10, 1) == 1
    }

    // Loops.
    pub fn loop_address(&self) -> u32 {
        bits(self.dword_0, 0, 13)
    }
    pub fn loop_start_is_repeat(&self) -> bool {
        bits(self.dword_0, 13, 1) == 1
    }
    pub fn loop_id(&self) -> u32 {
        bits(self.dword_0, 16, 5)
    }
    pub fn loop_end_is_predicated_break(&self) -> bool {
        bits(self.dword_0, 21, 1) == 1
    }
    pub fn loop_end_condition(&self) -> bool {
        bits(self.dword_1, 10, 1) == 1
    }

    // Calls and jumps.
    pub fn target_address(&self) -> u32 {
        bits(self.dword_0, 0, 13)
    }
    pub fn is_unconditional(&self) -> bool {
        bits(self.dword_0, 13, 1) == 1
    }
    pub fn is_predicated(&self) -> bool {
        bits(self.dword_0, 14, 1) == 1
    }
    pub fn bool_address(&self) -> u32 {
        bits(self.dword_1, 2, 8)
    }
    pub fn condition(&self) -> bool {
        bits(self.dword_1, 10, 1) == 1
    }

    // Alloc.
    pub fn alloc_size(&self) -> u32 {
        bits(self.dword_0, 0, 3)
    }
    pub fn alloc_type(&self) -> AllocType {
        match bits(self.dword_1, 9, 2) {
            0 => AllocType::None,
            1 => AllocType::VsPosition,
            2 => AllocType::VsInterpolatorsOrPsColors,
            _ => AllocType::Memory,
        }
    }

    /// The label address this instruction refers to, if it's a call, jump or loop.
    pub fn label_address(&self) -> Option<u32> {
        match self.opcode() {
            CfOpcode::CondCall | CfOpcode::CondJmp | CfOpcode::LoopStart | CfOpcode::LoopEnd => {
                Some(self.target_address())
            }
            _ => None,
        }
    }
}

/// Unpacks the control flow pair at `dwords[0..3]` (`UnpackControlFlowInstructions`).
pub fn unpack_pair(dwords: &[u32]) -> [CfInstruction; 2] {
    let (d0, d1, d2) = (dwords[0], dwords[1], dwords[2]);
    [
        CfInstruction { dword_0: d0, dword_1: d1 & 0xFFFF },
        CfInstruction { dword_0: (d1 >> 16) | (d2 << 16), dword_1: d2 >> 16 },
    ]
}
