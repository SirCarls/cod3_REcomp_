//! ALU instructions to WGSL: a vector and a scalar operation co-issued. Both
//! read their sources first; the vector one runs first (it may set the
//! predicate or a0 for the scalar one, as in the SDK's interpreter); then the
//! results are stored. `xe_ps` is the previous scalar result, kept unclamped.
//! Ported from the SDK's `interpreter.cpp` (`ExecuteAluInstruction`, Xenia's,
//! BSD 3-Clause).

use super::{Emitter, Refusal};
use crate::alu::{SCALAR_OPCODES, VECTOR_OPCODES};
use crate::parsed::*;
use crate::shader::ShaderType;

impl Emitter<'_> {
    pub(crate) fn alu(&mut self, alu: &ParsedAlu) -> Result<(), Refusal> {
        if alu.is_nop() {
            return Ok(());
        }
        if alu.is_predicated {
            self.line(&format!("if (xe_p == {}) {{", alu.predicate_condition));
            self.indent += 1;
        }
        let vector_used = alu.vector_result.used_write_mask() != 0
            || VECTOR_OPCODES[alu.vector_opcode as usize].changed_state != 0;
        let scalar_used = !alu.is_scalar_op_default_nop();

        // Sources first: the vector operands and the scalar ones.
        let mut v = Vec::new();
        if vector_used {
            for o in &alu.vector_operands[..alu.vector_operand_count] {
                let name = self.fresh("xe_v");
                let value = self.alu_operand_vec4(o);
                self.line(&format!("let {} = {};", name, value));
                v.push(name);
            }
        }
        let (mut sa, mut sb) = (String::from("0.0"), String::from("0.0"));
        if scalar_used {
            match alu.scalar_operand_count {
                1 => {
                    let o = alu.scalar_operands[0];
                    sa = self.fresh("xe_sa");
                    let value = self.alu_operand_scalar(&o, 0);
                    self.line(&format!("let {} = {};", sa, value));
                    if o.component_count == 2 {
                        sb = self.fresh("xe_sb");
                        let value = self.alu_operand_scalar(&o, 1);
                        self.line(&format!("let {} = {};", sb, value));
                    }
                }
                2 => {
                    sa = self.fresh("xe_sa");
                    let value = self.alu_operand_scalar(&alu.scalar_operands[0], 0);
                    self.line(&format!("let {} = {};", sa, value));
                    sb = self.fresh("xe_sb");
                    let value = self.alu_operand_scalar(&alu.scalar_operands[1], 0);
                    self.line(&format!("let {} = {};", sb, value));
                }
                _ => {}
            }
        }

        // The vector operation.
        let mut vector_value = None;
        if vector_used {
            let value = self.vector_op(alu.vector_opcode, &v)?;
            let name = self.fresh("xe_vr");
            self.line(&format!("let {} = {};", name, value));
            vector_value = Some(name);
        }

        // The scalar operation (into xe_ps).
        if scalar_used {
            self.scalar_op(alu.scalar_opcode, &sa, &sb)?;
        }

        if let Some(name) = vector_value {
            self.store(&alu.vector_result, &name, false)?;
        }
        if scalar_used {
            let ps = self.fresh("xe_psr");
            self.line(&format!("let {} = xe_ps;", ps));
            self.store(&alu.scalar_result, &ps, true)?;
        }
        if alu.is_predicated {
            self.indent -= 1;
            self.line("}");
        }
        Ok(())
    }

    /// The vector operation's value (a vec4 expression); state changes as statements.
    fn vector_op(&mut self, opcode: u32, v: &[String]) -> Result<String, Refusal> {
        let a = v.first().cloned().unwrap_or_default();
        let b = v.get(1).cloned().unwrap_or_default();
        let c = v.get(2).cloned().unwrap_or_default();
        let splat = |x: String| format!("vec4<f32>({})", x);
        Ok(match opcode {
            0 => format!("{} + {}", a, b),
            1 => format!("xe_mul({}, {})", a, b),
            2 => format!("xe_max({}, {})", a, b),
            3 => format!("xe_min({}, {})", a, b),
            4 => format!("select(vec4<f32>(0.0), vec4<f32>(1.0), {} == {})", a, b),
            5 => format!("select(vec4<f32>(0.0), vec4<f32>(1.0), {} > {})", a, b),
            6 => format!("select(vec4<f32>(0.0), vec4<f32>(1.0), {} >= {})", a, b),
            7 => format!("select(vec4<f32>(0.0), vec4<f32>(1.0), {} != {})", a, b),
            8 => format!("{} - floor({})", a, a),
            9 => format!("trunc({})", a),
            10 => format!("floor({})", a),
            // +0 + -0 must be +0: the product is added even when it's 0.
            11 => format!("xe_mul({}, {}) + {}", a, b, c),
            12 => format!("select({}, {}, {} == vec4<f32>(0.0))", c, b, a),
            13 => format!("select({}, {}, {} >= vec4<f32>(0.0))", c, b, a),
            14 => format!("select({}, {}, {} > vec4<f32>(0.0))", c, b, a),
            15 => {
                let m = self.fresh("xe_dp");
                self.line(&format!("let {} = xe_mul({}, {});", m, a, b));
                splat(format!("0.0 + {m}.x + {m}.y + {m}.z + {m}.w", m = m))
            }
            16 => {
                let m = self.fresh("xe_dp");
                self.line(&format!("let {} = xe_mul({}, {});", m, a, b));
                splat(format!("0.0 + {m}.x + {m}.y + {m}.z", m = m))
            }
            17 => {
                let m = self.fresh("xe_dp");
                self.line(&format!("let {} = xe_mul({}, {});", m, a, b));
                splat(format!("0.0 + {m}.x + {m}.y + {}.x", c, m = m))
            }
            18 => {
                // Operand 0 is .z_xy: x = [2], y = [3], z = [0]. Result: T, S, 2 * major
                // axis, face ID.
                let r = self.fresh("xe_cube");
                self.line(&format!("var {}: vec4<f32>;", r));
                self.line("{");
                self.line(&format!("    let x = {}.z; let y = {}.w; let z = {}.x;", a, a, a));
                self.line("    let xa = abs(x); let ya = abs(y); let za = abs(z);");
                self.line("    if (za >= xa && za >= ya) {");
                self.line(&format!("        {} = vec4<f32>(-y, select(x, -x, z < 0.0), z, select(4.0, 5.0, z < 0.0));", r));
                self.line("    } else if (ya >= xa) {");
                self.line(&format!("        {} = vec4<f32>(select(z, -z, y < 0.0), x, y, select(2.0, 3.0, y < 0.0));", r));
                self.line("    } else {");
                self.line(&format!("        {} = vec4<f32>(-y, select(-z, z, x < 0.0), x, select(0.0, 1.0, x < 0.0));", r));
                self.line("    }");
                self.line(&format!("    {}.z = {}.z * 2.0;", r, r));
                self.line("}");
                r
            }
            19 => splat(format!(
                "select(select(select({a}.w, {a}.z, {a}.z >= {a}.w), {a}.y, {a}.y >= {a}.z && {a}.y >= {a}.w), {a}.x, \
                 {a}.x >= {a}.y && {a}.x >= {a}.z && {a}.x >= {a}.w)",
                a = a
            )),
            20..=23 => {
                let cmp = |x: &str| match opcode {
                    20 => format!("{} == 0.0", x),
                    21 => format!("{} != 0.0", x),
                    22 => format!("{} > 0.0", x),
                    _ => format!("{} >= 0.0", x),
                };
                self.line(&format!("xe_p = {}.w == 0.0 && {};", a, cmp(&format!("{}.w", b))));
                splat(format!(
                    "select({}.x + 1.0, 0.0, {}.x == 0.0 && {})",
                    a,
                    a,
                    cmp(&format!("{}.x", b))
                ))
            }
            24..=27 => {
                let op = ["==", ">", ">=", "!="][(opcode - 24) as usize];
                let k = self.fresh("xe_kill");
                self.line(&format!("let {} = any({} {} {});", k, a, op, b));
                if self.ty == ShaderType::Pixel {
                    self.line(&format!("if ({}) {{ discard; }}", k));
                }
                splat(format!("select(0.0, 1.0, {})", k))
            }
            28 => format!("vec4<f32>(1.0, xe_muls({a}.y, {b}.y), {a}.z, {b}.w)", a = a, b = b),
            29 => {
                self.line(&format!("xe_a0 = xe_a0_from({}.w);", a));
                format!("xe_max({}, {})", a, b)
            }
            _ => return Err(Refusal::InvalidOpcode(VECTOR_OPCODES[opcode as usize].name)),
        })
    }

    /// The scalar operation: assigns `xe_ps` (and the predicate, a0, kill).
    fn scalar_op(&mut self, opcode: u32, a: &str, b: &str) -> Result<(), Refusal> {
        let set = |e: &mut Self, x: String| e.line(&format!("xe_ps = {};", x));
        match opcode {
            0 | 44 | 45 => set(self, format!("{} + {}", a, b)),
            1 => set(self, format!("{} + xe_ps", a)),
            2 | 42 | 43 => set(self, format!("xe_muls({}, {})", a, b)),
            3 => set(self, format!("xe_muls({}, xe_ps)", a)),
            4 => set(
                self,
                format!(
                    "select(xe_muls({a}, xe_ps), -XE_FLT_MAX, xe_ps == -XE_FLT_MAX || !xe_isfinite(xe_ps) || \
                     !xe_isfinite({b}) || {b} <= 0.0)",
                    a = a,
                    b = b
                ),
            ),
            5 => set(self, format!("xe_maxs({}, {})", a, b)),
            6 => set(self, format!("xe_mins({}, {})", a, b)),
            7 => set(self, format!("select(0.0, 1.0, {} == 0.0)", a)),
            8 => set(self, format!("select(0.0, 1.0, {} > 0.0)", a)),
            9 => set(self, format!("select(0.0, 1.0, {} >= 0.0)", a)),
            10 => set(self, format!("select(0.0, 1.0, {} != 0.0)", a)),
            11 => set(self, format!("{} - floor({})", a, a)),
            12 => set(self, format!("trunc({})", a)),
            13 => set(self, format!("floor({})", a)),
            14 => set(self, format!("exp2({})", a)),
            15 => {
                let t = self.fresh("xe_t");
                self.line(&format!("let {} = log2({});", t, a));
                // Only -infinity is below -FLT_MAX.
                set(self, format!("select({t}, -XE_FLT_MAX, {t} < -XE_FLT_MAX)", t = t));
            }
            16 => set(self, format!("log2({})", a)),
            17 | 20 => {
                let t = self.fresh("xe_t");
                let x = if opcode == 17 { format!("1.0 / {}", a) } else { format!("1.0 / sqrt({})", a) };
                self.line(&format!("let {} = {};", t, x));
                set(self, format!("select({t}, sign({t}) * XE_FLT_MAX, !xe_isfinite({t}) && {t} == {t})", t = t));
            }
            18 | 21 => {
                let t = self.fresh("xe_t");
                let x = if opcode == 18 { format!("1.0 / {}", a) } else { format!("1.0 / sqrt({})", a) };
                self.line(&format!("let {} = {};", t, x));
                // +-infinity become +-0.
                set(self, format!("select({t}, select(0.0, -0.0, {t} < 0.0), !xe_isfinite({t}) && {t} == {t})", t = t));
            }
            19 => set(self, format!("1.0 / {}", a)),
            22 => set(self, format!("1.0 / sqrt({})", a)),
            23 => {
                self.line(&format!("xe_a0 = xe_a0_from({});", a));
                set(self, format!("xe_maxs({}, {})", a, b));
            }
            24 => {
                self.line(&format!("xe_a0 = i32(floor(clamp({}, -256.0, 255.0)));", a));
                set(self, format!("xe_maxs({}, {})", a, b));
            }
            25 | 46 | 47 => set(self, format!("{} - {}", a, b)),
            26 => set(self, format!("{} - xe_ps", a)),
            27..=30 => {
                let op = ["==", "!=", ">", ">="][(opcode - 27) as usize];
                self.line(&format!("xe_p = {} {} 0.0;", a, op));
                set(self, "select(1.0, 0.0, xe_p)".to_owned());
            }
            31 => {
                self.line(&format!("xe_p = {} == 1.0;", a));
                set(self, format!("select(select({a}, 1.0, {a} == 0.0), 0.0, xe_p)", a = a));
            }
            32 => {
                let t = self.fresh("xe_t");
                self.line(&format!("let {} = {} - 1.0;", t, a));
                self.line(&format!("xe_p = {} <= 0.0;", t));
                set(self, format!("select({}, 0.0, xe_p)", t));
            }
            33 => {
                self.line("xe_p = false;");
                set(self, "XE_FLT_MAX".to_owned());
            }
            34 => {
                self.line(&format!("xe_p = {} == 0.0;", a));
                set(self, format!("select({}, 0.0, xe_p)", a));
            }
            35..=39 => {
                let cond = match opcode {
                    35 => format!("{} == 0.0", a),
                    36 => format!("{} > 0.0", a),
                    37 => format!("{} >= 0.0", a),
                    38 => format!("{} != 0.0", a),
                    _ => format!("{} == 1.0", a),
                };
                let k = self.fresh("xe_kill");
                self.line(&format!("let {} = {};", k, cond));
                if self.ty == ShaderType::Pixel {
                    self.line(&format!("if ({}) {{ discard; }}", k));
                }
                set(self, format!("select(0.0, 1.0, {})", k));
            }
            40 => set(self, format!("sqrt({})", a)),
            48 => set(self, format!("sin({})", a)),
            49 => set(self, format!("cos({})", a)),
            50 => {}
            _ => return Err(Refusal::InvalidOpcode(SCALAR_OPCODES[opcode as usize].name)),
        }
        Ok(())
    }
}
