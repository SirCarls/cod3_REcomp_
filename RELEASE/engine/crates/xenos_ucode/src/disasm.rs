//! Disassembly text, character for character what Xenia's
//! `shader_translator_disasm.cc` writes (so it can be compared with the SDK's
//! `dump_shaders` output).

use std::fmt::Write;

use crate::cf::AllocType;
use crate::fetch::{vertex_format_name, ANISO_FILTER_USE_FETCH_CONST, TEXTURE_FILTER_USE_FETCH_CONST};
use crate::parsed::*;

fn predicate_prefix(out: &mut String, is_predicated: bool, condition: bool) {
    if is_predicated {
        out.push_str(if condition { " (p0) " } else { "(!p0) " });
    } else {
        out.push_str("      ");
    }
}

/// `DisassembleResultOperand`.
pub(crate) fn result_operand(r: &InstructionResult, out: &mut String) {
    let indexed = match r.target {
        StorageTarget::Register => {
            out.push('r');
            true
        }
        StorageTarget::Interpolator => {
            out.push('o');
            true
        }
        StorageTarget::Position => {
            out.push_str("oPos");
            false
        }
        StorageTarget::PointSizeEdgeFlagKillVertex => {
            out.push_str("oPts");
            false
        }
        StorageTarget::ExportAddress => {
            out.push_str("eA");
            false
        }
        StorageTarget::ExportData => {
            out.push_str("eM");
            true
        }
        StorageTarget::Color => {
            out.push_str("oC");
            true
        }
        StorageTarget::Depth => {
            out.push_str("oDepth");
            false
        }
        StorageTarget::None => false,
    };
    if indexed {
        let _ = match r.addressing {
            AddressingMode::Absolute => write!(out, "{}", r.index),
            AddressingMode::AddressRegisterRelative => write!(out, "[{}+a0]", r.index),
            AddressingMode::LoopRelative => write!(out, "[{}+aL]", r.index),
        };
    }
    // The original write mask: components without an effect are still in the microcode.
    if r.original_write_mask == 0 {
        out.push_str("._");
    } else if r.original_write_mask != 0b1111
        || r.components != [Swizzle::X, Swizzle::Y, Swizzle::Z, Swizzle::W]
    {
        out.push('.');
        for i in 0..4 {
            if r.original_write_mask & (1 << i) != 0 {
                out.push(r.components[i].as_char());
            } else {
                out.push('_');
            }
        }
    }
}

/// `DisassembleSourceOperand`.
pub(crate) fn source_operand(o: &InstructionOperand, out: &mut String) {
    if o.is_negated {
        out.push('-');
    }
    match o.source {
        StorageSource::Register => out.push('r'),
        StorageSource::ConstantFloat => out.push('c'),
        // Fetch constants are printed by the fetch instructions themselves.
        StorageSource::VertexFetchConstant | StorageSource::TextureFetchConstant => {}
    }
    if o.is_absolute_value {
        out.push_str("_abs");
    }
    let _ = match o.addressing {
        AddressingMode::Absolute => {
            if o.is_absolute_value {
                write!(out, "[{}]", o.index)
            } else {
                write!(out, "{}", o.index)
            }
        }
        AddressingMode::AddressRegisterRelative => write!(out, "[{}+a0]", o.index),
        AddressingMode::LoopRelative => write!(out, "[{}+aL]", o.index),
    };
    if !o.is_standard_swizzle() {
        out.push('.');
        for j in 0..(o.component_count as usize).min(4) {
            out.push(o.components[j].as_char());
        }
    }
}

pub(crate) fn exec(i: &ParsedExec, out: &mut String) {
    match i.kind {
        CondType::Unconditional => {
            out.push_str("      ");
            out.push_str(i.name);
        }
        CondType::Predicated => {
            out.push_str(if i.condition { " (p0) " } else { "(!p0) " });
            out.push_str(i.name);
        }
        CondType::Conditional => {
            let _ = write!(out, "      {} {}b{}", i.name, if i.condition { "" } else { "!" }, i.bool_constant_index);
        }
    }
    if i.is_yield {
        if i.kind == CondType::Conditional {
            out.push(',');
        }
        out.push_str(" Yield=true");
    }
    if !i.is_predicate_clean {
        out.push_str("    // PredicateClean=false");
    }
    out.push('\n');
}

pub(crate) fn loop_start(i: &ParsedLoopStart, out: &mut String) {
    let _ = write!(out, "      loop i{}, L{}", i.loop_constant_index, i.loop_skip_address);
    if i.is_repeat {
        out.push_str(", Repeat=true");
    }
    out.push('\n');
}

pub(crate) fn loop_end(i: &ParsedLoopEnd, out: &mut String) {
    if i.is_predicated_break {
        out.push_str(if i.predicate_condition { " (p0) " } else { "(!p0) " });
    } else {
        out.push_str("      ");
    }
    let _ = writeln!(out, "endloop i{}, L{}", i.loop_constant_index, i.loop_body_address);
}

/// `call` (`is_call`) or `jmp`.
pub(crate) fn branch(i: &ParsedBranch, is_call: bool, out: &mut String) {
    let (plain, conditional) = if is_call { ("call ", "ccall ") } else { ("jmp ", "cjmp ") };
    match i.kind {
        CondType::Unconditional => {
            out.push_str("      ");
            out.push_str(plain);
        }
        CondType::Predicated => {
            out.push_str(if i.condition { " (p0) " } else { "(!p0) " });
            out.push_str(plain);
        }
        CondType::Conditional => {
            out.push_str("      ");
            out.push_str(conditional);
            if !i.condition {
                out.push('!');
            }
            let _ = write!(out, "b{}, ", i.bool_constant_index);
        }
    }
    let _ = writeln!(out, "L{}", i.target_address);
}

pub(crate) fn alloc(i: &ParsedAlloc, out: &mut String) {
    out.push_str("      alloc ");
    match i.kind {
        AllocType::None => {}
        AllocType::VsPosition => out.push_str("position"),
        AllocType::VsInterpolatorsOrPsColors => {
            out.push_str(if i.is_vertex_shader { "interpolators" } else { "colors" })
        }
        AllocType::Memory => {
            let _ = write!(out, "export = {}", i.count);
        }
    }
    out.push('\n');
}

pub(crate) fn vertex_fetch(i: &ParsedVertexFetch, out: &mut String) {
    out.push_str("   ");
    predicate_prefix(out, i.is_predicated, i.predicate_condition);
    out.push_str(i.name);
    out.push(' ');
    result_operand(&i.result, out);
    let a = &i.attributes;
    if !i.is_mini_fetch {
        out.push_str(", ");
        source_operand(&i.operands[0], out);
        // Xenia prints the vertex fetch constant counted down from 95.
        let _ = write!(out, ", vf{}", 95i64 - i.operands[1].index as i64);
        if a.is_index_rounded {
            out.push_str(", RoundIndex=true");
        }
    }
    if a.exp_adjust != 0 {
        let _ = write!(out, ", ExpAdjust={}", a.exp_adjust);
    }
    if a.offset != 0 {
        let _ = write!(out, ", Offset={}", a.offset);
    }
    if a.data_format != 0 {
        match vertex_format_name(a.data_format) {
            Some(name) => {
                let _ = write!(out, ", DataFormat={}", name);
            }
            None => {
                let _ = write!(out, ", DataFormat=FMT_{}", a.data_format);
            }
        }
    }
    if !i.is_mini_fetch && a.stride != 0 {
        let _ = write!(out, ", Stride={}", a.stride);
    }
    if a.is_signed {
        out.push_str(", Signed=true");
    }
    if a.is_integer {
        out.push_str(", NumFormat=integer");
    }
    if a.prefetch_count != 0 {
        let _ = write!(out, ", PrefetchCount={}", a.prefetch_count + 1);
    }
    out.push('\n');
}

const TEXTURE_FILTER_NAMES: [&str; 4] = ["point", "linear", "basemap", "keep"];
const ANISO_FILTER_NAMES: [&str; 8] =
    ["disabled", "max1to1", "max2to1", "max4to1", "max8to1", "max16to1", "keep", "keep"];

/// fmt's `{:g}` for the values these fields can hold (multiples of 1/16 in [-8, 8]):
/// Rust's shortest round-trip form is the same text.
fn g(v: f32) -> String {
    format!("{}", v)
}

pub(crate) fn texture_fetch(i: &ParsedTextureFetch, out: &mut String) {
    out.push_str("   ");
    predicate_prefix(out, i.is_predicated, i.predicate_condition);
    out.push_str(i.name);
    out.push(' ');
    let mut needs_comma = false;
    if i.has_result() {
        result_operand(&i.result, out);
        needs_comma = true;
    }
    if needs_comma {
        out.push_str(", ");
    }
    source_operand(&i.operands[0], out);
    if i.operand_count > 1 {
        if needs_comma {
            out.push_str(", ");
        }
        let _ = write!(out, "tf{}", i.operands[1].index);
    }
    let a = &i.attributes;
    if !a.fetch_valid_only {
        out.push_str(", FetchValidOnly=false");
    }
    if a.unnormalized_coordinates {
        out.push_str(", UnnormalizedTextureCoords=true");
    }
    for (value, label) in [
        (a.mag_filter, "MagFilter"),
        (a.min_filter, "MinFilter"),
        (a.mip_filter, "MipFilter"),
    ] {
        if value != TEXTURE_FILTER_USE_FETCH_CONST {
            let _ = write!(out, ", {}={}", label, TEXTURE_FILTER_NAMES[value as usize & 3]);
        }
    }
    if a.aniso_filter != ANISO_FILTER_USE_FETCH_CONST {
        let _ = write!(out, ", AnisoFilter={}", ANISO_FILTER_NAMES[a.aniso_filter as usize & 7]);
    }
    for (value, label) in [(a.vol_mag_filter, "VolMagFilter"), (a.vol_min_filter, "VolMinFilter")] {
        if value != TEXTURE_FILTER_USE_FETCH_CONST {
            let _ = write!(out, ", {}={}", label, TEXTURE_FILTER_NAMES[value as usize & 3]);
        }
    }
    if !a.use_computed_lod {
        out.push_str(", UseComputedLOD=false");
    }
    if a.use_register_lod {
        out.push_str(", UseRegisterLOD=true");
    }
    if a.use_register_gradients {
        out.push_str(", UseRegisterGradients=true");
    }
    if a.lod_bias != 0.0 {
        let _ = write!(out, ", LODBias={}", g(a.lod_bias));
    }
    let components = i.dimension.component_count();
    if a.offset_x != 0.0 {
        let _ = write!(out, ", OffsetX={}", g(a.offset_x));
    }
    if components > 1 && a.offset_y != 0.0 {
        let _ = write!(out, ", OffsetY={}", g(a.offset_y));
    }
    if components > 2 && a.offset_z != 0.0 {
        let _ = write!(out, ", OffsetZ={}", g(a.offset_z));
    }
    out.push('\n');
}

pub(crate) fn alu(i: &ParsedAlu, out: &mut String) {
    let vector_nop = i.is_vector_op_default_nop();
    let scalar_nop = i.is_scalar_op_default_nop();
    if vector_nop && scalar_nop {
        out.push_str("   ");
        predicate_prefix(out, i.is_predicated, i.predicate_condition);
        out.push_str("nop\n");
        return;
    }
    if !vector_nop {
        out.push_str("   ");
        predicate_prefix(out, i.is_predicated, i.predicate_condition);
        out.push_str(i.vector_name);
        if i.vector_result.is_clamped {
            out.push_str("_sat");
        }
        out.push(' ');
        result_operand(&i.vector_result, out);
        for operand in &i.vector_operands[..i.vector_operand_count] {
            out.push_str(", ");
            source_operand(operand, out);
        }
        out.push('\n');
    }
    if !scalar_nop {
        out.push_str(if vector_nop { "   " } else { "              + " });
        predicate_prefix(out, i.is_predicated, i.predicate_condition);
        out.push_str(i.scalar_name);
        if i.scalar_result.is_clamped {
            out.push_str("_sat");
        }
        out.push(' ');
        result_operand(&i.scalar_result, out);
        for operand in &i.scalar_operands[..i.scalar_operand_count] {
            out.push_str(", ");
            source_operand(operand, out);
        }
        out.push('\n');
    }
}
