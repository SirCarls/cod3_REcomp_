//! Xbox 360 (Xenos GPU) shader microcode: decoding, analysis and disassembly.
//!
//! The first piece of the Rust renderer's shader translator
//! (`dev/agents/rust-renderer/DESIGN.md`, `NOTES-iw4l.md` 3 (a)): the game's
//! shaders are Xenos microcode, and the renderer has to read them the way the
//! console's GPU does. This crate decodes the three instruction kinds (control
//! flow, fetch, ALU) into parsed instructions and prints the same disassembly
//! text Xenia's (and so ReXGlue's) `ShaderTranslator` writes, so every shader the
//! game draws can be checked against the emulator's own reading of it
//! (`xenos_ucode corpus`, with the SDK's `dump_shaders` output).
//!
//! [`wgsl`] translates each shader to a WGSL module (the Rust renderer's
//! shader translator, M1a; [acct2-decomp] 2026-10-25).
//!
//! ## Licenses
//!
//! This is a port of Xenia's microcode description and parser
//! (`ucode.h`, `ucode.cpp`, `shader.h`, `shader_translator.cc`,
//! `shader_translator_disasm.cc`, as adapted by ReXGlue):
//!
//! > Xenia : Xbox 360 Emulator Research Project. Copyright 2015-2022 Ben Vanik.
//! > All rights reserved. Released under the BSD license (3-clause).
//!
//! Parts of Xenia's instruction layouts come from the freedreno project
//! (`instr-a2xx.h`):
//!
//! > Copyright (c) 2012 Rob Clark <robdclark@gmail.com>. Permission is hereby
//! > granted, free of charge, to any person obtaining a copy of this software and
//! > associated documentation files (the "Software"), to deal in the Software
//! > without restriction, including without limitation the rights to use, copy,
//! > modify, merge, publish, distribute, sublicense, and/or sell copies of the
//! > Software, and to permit persons to whom the Software is furnished to do so,
//! > subject to the following conditions: The above copyright notice and this
//! > permission notice (including the next paragraph) shall be included in all
//! > copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
//! > "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.

#![forbid(unsafe_code)]

pub mod alu;
pub mod cf;
mod disasm;
pub mod fetch;
pub mod parsed;
pub mod shader;
pub mod wgsl;

pub use shader::{analyze, Analysis, ShaderType};

/// Bits `lo .. lo + n` of `v` (`n` < 32).
#[inline]
pub(crate) fn bits(v: u32, lo: u32, n: u32) -> u32 {
    (v >> lo) & ((1u32 << n) - 1)
}

/// The signed bit field `lo .. lo + n` of `v` (`n` < 32).
#[inline]
pub(crate) fn sbits(v: u32, lo: u32, n: u32) -> i32 {
    ((v << (32 - lo - n)) as i32) >> (32 - n)
}

/// Reads a dumped microcode file (`shader_<hash>.ucode.bin.vert|frag`, the SDK's
/// `dump_shaders`): host-order (little-endian) dwords.
pub fn dwords_from_le_bytes(bytes: &[u8]) -> Vec<u32> {
    bytes
        .chunks_exact(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bit_fields() {
        assert_eq!(bits(0xABCD_1234, 0, 4), 0x4);
        assert_eq!(bits(0xABCD_1234, 28, 4), 0xA);
        assert_eq!(sbits(0b111 << 5, 5, 3), -1);
        assert_eq!(sbits(0b011 << 5, 5, 3), 3);
        assert_eq!(sbits(0x8000_0000 >> 1, 8, 23), -(1 << 22));
    }

    #[test]
    fn minimal_shader_disassembly() {
        // One control flow pair: `exece` of one ALU instruction, then `cnop`; the ALU
        // instruction is the default nop (max r0._, r0, r0 + retain_prev r0._).
        let cf0_d0 = 1 | (1 << 12); // address 1, count 1, sequence 0 (ALU)
        let cf0_d1 = (2 << 12) | (1 << 9); // ExecEnd, PredicateClean
        let cf1_d0 = 0u32;
        let cf1_d1 = 0u32; // Nop
        let ucode = [
            cf0_d0,
            cf0_d1 | (cf1_d0 << 16),
            (cf1_d0 >> 16) | (cf1_d1 << 16),
            50 << 26,                             // scalar retain_prev
            0,                                    // swizzles 0 (relative: xyzw)
            (2 << 24) | (1 << 31) | (1 << 30),    // vector max, src1 and src2 temporaries
        ];
        let a = analyze(&ucode, ShaderType::Pixel);
        assert_eq!(
            a.disassembly,
            "/*    0.0 */       exece\n/*    1   */          nop\n/*    0.1 */       cnop\n"
        );
        assert_eq!(a.cf_pair_index_bound, 1);
    }
}
