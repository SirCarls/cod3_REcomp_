//! xenos_diff: checks `xenos_ucode::wgsl` against the SDK's own shader interpreter.
//!
//! For each shader of the corpus (`shader_*.ucode.bin.vert|frag`), on random inputs (float,
//! bool, loop and fetch constants, the input registers, vertex memory), the same case runs
//! twice:
//! - through the SDK's `ShaderInterpreter` (Xenia's, compiled unchanged; `interp/`), and
//! - through the translated WGSL, parsed by naga and run on the CPU (`eval.rs`),
//!
//! and everything both produce is compared bit for bit: every temporary register when the
//! shader ends, every exported interpolator / position / color / depth component.
//!
//! What can't be compared: texture sampling (the interpreter writes zeros for texture
//! fetches, so the WGSL side samples zeros too) and pixel kill (the interpreter doesn't kill).
//!
//! xenos_diff DIR [--cases N] [--seed S] [--only TEXT] [--skip K] [--take M] [--finite]
//!                [--case K] [--show N]

mod eval;
mod interp;

use std::path::PathBuf;
use std::time::Instant;

use eval::{Buffers, Exec, Outcome, S, V};
use interp::{Interpreter, XiCase, XiExport, XiResult};
use xenos_ucode::{dwords_from_le_bytes, wgsl, ShaderType};

// The arena's layout for one draw (see the ABI in xenos_ucode/src/wgsl/mod.rs).
const ROW_VS: u32 = 1;
const ROW_PS: u32 = 257;
const ROW_FETCH: u32 = 513;
const ROW_MISC: u32 = 561;
const ROWS: usize = 561 + 29;

// Vertex memory: fetch constants point into [GUEST_BASE, GUEST_BASE + 256) with sizes
// below 1280 dwords, so every read either side may make stays inside the window.
const GUEST_BASE: usize = 0x400;
const GUEST_LEN: usize = 0x2000;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: u32) -> u32 {
        (self.next() % n as u64) as u32
    }
    fn unit(&mut self) -> f32 {
        (self.next() >> 40) as f32 / (1u64 << 24) as f32
    }
    fn sign(&mut self) -> f32 {
        if self.below(2) == 0 {
            1.0
        } else {
            -1.0
        }
    }
    /// A float as shaders see them: mostly ordinary values, some exact small integers and
    /// special values, a few infinities and NaNs (unless `finite`). Never a denormal (the
    /// interpreter flushes denormal operands; the WGSL keeps them).
    fn float(&mut self, finite: bool) -> f32 {
        let r = self.below(100);
        match r {
            0..=24 => self.below(9) as f32 - 4.0,
            25..=54 => self.unit() * 4.0 - 2.0,
            55..=79 => self.sign() * (2.0f32).powi(self.below(25) as i32 - 12) * (1.0 + self.unit()),
            80..=86 => [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0][self.below(7) as usize],
            87..=94 => self.sign() * (2.0f32).powi(self.below(121) as i32 - 60) * (1.0 + self.unit()),
            95..=96 if !finite => self.sign() * f32::INFINITY,
            97 if !finite => f32::from_bits(0x7FC0_0000 | (self.below(1 << 22))),
            _ => self.sign() * (2.0f32).powi(self.below(40) as i32 + 60) * (1.0 + self.unit()),
        }
    }
}

struct Case {
    xi: Box<XiCase>,
    guest: Vec<u32>,
    vertex_index: u32,
}

fn make_case(rng: &mut Rng, ty: ShaderType, finite: bool) -> Case {
    // SAFETY: XiCase is plain numbers; all zero is a valid value.
    let mut xi: Box<XiCase> = unsafe { Box::new(std::mem::zeroed()) };
    xi.shader_type = if ty == ShaderType::Pixel { 1 } else { 0 };
    for c in xi.consts.iter_mut() {
        for x in c.iter_mut() {
            *x = rng.float(finite);
        }
    }
    for b in xi.bools.iter_mut() {
        *b = rng.next() as u32;
    }
    for l in xi.loops.iter_mut() {
        let count = rng.below(5);
        let start = rng.below(9);
        let step = (rng.below(5) as i32 - 2) as u32 & 0xFF;
        *l = count | (start << 8) | (step << 16);
    }
    // Every fetch constant slot as a vertex fetch constant (type 3); texture fetches read
    // the same words, which only matter to sampling (zeros on both sides).
    for k in 0..96 {
        let address = (GUEST_BASE as u32) + rng.below(256);
        let size = 256 + rng.below(1024);
        xi.fetch[k * 2] = 3 | (address << 2);
        xi.fetch[k * 2 + 1] = rng.below(4) | (size << 2);
    }
    let vertex_index = rng.below(64);
    match ty {
        ShaderType::Vertex => {
            xi.regs[0][0] = vertex_index as f32;
        }
        ShaderType::Pixel => {
            for r in xi.regs.iter_mut().take(16) {
                for x in r.iter_mut() {
                    *x = rng.float(finite);
                }
            }
        }
    }
    let mut guest = vec![0u32; GUEST_BASE + GUEST_LEN];
    for w in guest.iter_mut().skip(GUEST_BASE) {
        *w = rng.next() as u32;
    }
    Case { xi, guest, vertex_index }
}

fn arena(xi: &XiCase) -> Vec<[u32; 4]> {
    let mut a = vec![[0u32; 4]; ROWS];
    a[0] = [ROW_VS, ROW_PS, ROW_FETCH, ROW_MISC];
    for (i, c) in xi.consts.iter().enumerate() {
        a[ROW_VS as usize + i] = c.map(f32::to_bits);
    }
    for i in 0..48 {
        a[ROW_FETCH as usize + i].copy_from_slice(&xi.fetch[i * 4..i * 4 + 4]);
    }
    let m = ROW_MISC as usize;
    a[m].copy_from_slice(&xi.bools[0..4]);
    a[m + 1].copy_from_slice(&xi.bools[4..8]);
    for i in 0..8 {
        a[m + 2 + i].copy_from_slice(&xi.loops[i * 4..i * 4 + 4]);
    }
    a[m + 10] = [0; 4];
    a[m + 11] = [1.0f32.to_bits(); 4];
    a[m + 12] = [0; 4];
    // Each tf# uses texture table entry 0 with the identity swizzle (x y z w).
    let identity = 1 << 3 | 2 << 6 | 3 << 9;
    for i in 0..8 {
        a[m + 13 + i] = [0; 4];
        a[m + 21 + i] = [identity; 4];
    }
    a
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Cmp {
    Same,
    /// +0 against -0, or a denormal against 0 or another denormal.
    ZeroOrDenormal,
    /// A NaN against a number.
    NanAgainstNumber,
    Different,
}

fn cmp(a: f32, b: f32) -> Cmp {
    if a.to_bits() == b.to_bits() || (a.is_nan() && b.is_nan()) {
        return Cmp::Same;
    }
    if a.is_nan() || b.is_nan() {
        return Cmp::NanAgainstNumber;
    }
    let tiny = |x: f32| x == 0.0 || x.is_subnormal();
    if tiny(a) && tiny(b) {
        return Cmp::ZeroOrDenormal;
    }
    Cmp::Different
}

fn vec4_of(v: &V) -> [f32; 4] {
    let c = v.comps();
    let mut o = [0.0; 4];
    for (i, s) in c.iter().enumerate().take(4) {
        o[i] = s.f();
    }
    o
}

/// The last value the interpreter exported to each component of each export register.
fn merged_exports(r: &XiResult) -> Vec<(u32, [Option<f32>; 4])> {
    let mut out: Vec<(u32, [Option<f32>; 4])> = Vec::new();
    for e in &r.ex[..r.count as usize] {
        let XiExport { reg, mask, v } = *e;
        let slot = match out.iter_mut().position(|(r, _)| *r == reg) {
            Some(i) => i,
            None => {
                out.push((reg, [None; 4]));
                out.len() - 1
            }
        };
        for (i, x) in v.iter().enumerate() {
            if mask & (1 << i) != 0 {
                out[slot].1[i] = Some(*x);
            }
        }
    }
    out
}

struct Finding {
    worst: Cmp,
    first: Option<String>,
    /// Every component that isn't the same.
    all: Vec<String>,
}

fn compare(
    ty: ShaderType,
    registers: usize,
    r: &XiResult,
    o: &Outcome,
    fields: &[(String, Option<naga::Binding>)],
) -> Finding {
    let mut worst = Cmp::Same;
    let mut first: Option<(Cmp, String)> = None;
    let mut all: Vec<String> = Vec::new();
    let mut note = |c: Cmp, what: String, a: f32, b: f32| {
        if c > worst {
            worst = c;
        }
        if c != Cmp::Same {
            let line = format!("{}: interpreter {:e} ({:08X}), WGSL {:e} ({:08X})", what, a, a.to_bits(), b, b.to_bits());
            all.push(format!("{:?} {}", c, line));
            if first.as_ref().is_none_or(|(fc, _)| c > *fc) {
                first = Some((c, line));
            }
        }
    };
    // Every declared temporary register.
    if let Some((_, V::Agg(regs))) = o.locals.iter().find(|(n, _)| n.as_deref() == Some("xe_r")) {
        for (i, rv) in regs.iter().enumerate().take(registers) {
            for (c, w) in vec4_of(rv).into_iter().enumerate() {
                note(cmp(r.regs[i][c], w), format!("r{}.{}", i, b"xyzw"[c] as char), r.regs[i][c], w);
            }
        }
    }
    // Every exported component.
    let result = match &o.result {
        Some(V::Agg(f)) => f.clone(),
        Some(v) => vec![v.clone()],
        None => Vec::new(),
    };
    let field = |name: &str| -> Option<[f32; 4]> {
        let i = fields.iter().position(|(n, _)| n == name)?;
        match &result[i] {
            V::S(S::F(x)) => Some([*x, 0.0, 0.0, 0.0]),
            v => Some(vec4_of(v)),
        }
    };
    for (reg, comps) in merged_exports(r) {
        let (name, expect): (String, [Option<f32>; 4]) = match (ty, reg) {
            (ShaderType::Vertex, 0..=15) => (format!("o{}", reg), comps),
            (ShaderType::Vertex, 62) => {
                // The WGSL applies the NDC scale (1) and offset (0) to what was exported,
                // over the default position (0, 0, 0, 1).
                let d = [0.0, 0.0, 0.0, 1.0];
                let p: [f32; 4] = std::array::from_fn(|i| comps[i].unwrap_or(d[i]));
                let t = |x: f32| x * 1.0 + 0.0 * p[3];
                ("pos".into(), [Some(t(p[0])), Some(t(p[1])), Some(t(p[2])), Some(p[3])])
            }
            (ShaderType::Pixel, 0..=3) => (format!("c{}", reg), comps),
            (ShaderType::Pixel, 61) => ("depth".into(), [comps[0], None, None, None]),
            _ => continue,
        };
        let Some(got) = field(&name) else {
            note(Cmp::Different, format!("{} (exported by the interpreter, not by the WGSL)", name), 0.0, 0.0);
            continue;
        };
        for (c, e) in expect.iter().enumerate() {
            if let Some(a) = *e {
                note(cmp(a, got[c]), format!("{}.{}", name, b"xyzw"[c] as char), a, got[c]);
            }
        }
    }
    Finding { worst, first: first.map(|(_, s)| s), all }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let opt = |name: &str| args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned();
    let flag = |name: &str| args.iter().any(|a| a == name);
    let dir = PathBuf::from(args.first().expect(
        "usage: xenos_diff DIR [--cases N] [--seed S] [--only TEXT] [--skip K] [--take M] [--finite] [--case K] [--show N]",
    ));
    let cases: u32 = opt("--cases").and_then(|n| n.parse().ok()).unwrap_or(10_000);
    let seed: u64 = opt("--seed").and_then(|n| n.parse().ok()).unwrap_or(20_261_025);
    let only = opt("--only");
    let skip: usize = opt("--skip").and_then(|n| n.parse().ok()).unwrap_or(0);
    let take: usize = opt("--take").and_then(|n| n.parse().ok()).unwrap_or(usize::MAX);
    let one_case: Option<u32> = opt("--case").and_then(|n| n.parse().ok());
    let finite = flag("--finite");

    let mut files: Vec<(PathBuf, ShaderType)> = std::fs::read_dir(&dir)
        .expect("the corpus folder")
        .flatten()
        .map(|e| e.path())
        .filter_map(|p| {
            let name = p.file_name()?.to_str()?.to_owned();
            if !name.starts_with("shader_") || !name.contains(".ucode.bin.") {
                return None;
            }
            if let Some(o) = &only {
                if !name.contains(o.as_str()) {
                    return None;
                }
            }
            match p.extension()?.to_str()? {
                "vert" => Some((p, ShaderType::Vertex)),
                "frag" => Some((p, ShaderType::Pixel)),
                _ => None,
            }
        })
        .collect();
    files.sort_by(|a, b| a.0.cmp(&b.0));
    let files: Vec<_> = files.into_iter().skip(skip).take(take).collect();

    let mut interp = Interpreter::new().expect("the SDK's guest memory (Memory::Initialize)");
    let started = Instant::now();
    let (mut clean, mut minor, mut nan_only, mut bad, mut broken, mut weights) = (0, 0, 0, 0, 0, 0);
    let mut out_of_window_guard = 0usize;
    for (path, ty) in &files {
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        let ucode = dwords_from_le_bytes(&std::fs::read(path).expect("read"));
        // The interpreter writes zeros for getWeights (no texture unit): so does this translation.
        let options = wgsl::Options { zero_texture_weights: true };
        let t = match wgsl::translate_with(&ucode, *ty, options) {
            Ok(t) => t,
            Err(e) => {
                println!("{}: not translated ({})", name, e);
                broken += 1;
                continue;
            }
        };
        let module = match naga::front::wgsl::parse_str(&t.wgsl) {
            Ok(m) => m,
            Err(e) => {
                println!("{}: WGSL doesn't parse: {}", name, e.emit_to_string(&t.wgsl).lines().next().unwrap_or(""));
                broken += 1;
                continue;
            }
        };
        let registers = t
            .wgsl
            .lines()
            .find_map(|l| l.trim().strip_prefix("var xe_r: array<vec4<f32>, ").and_then(|r| r.strip_suffix(">;")))
            .and_then(|n| n.parse::<usize>().ok())
            .unwrap_or(0);
        // Tags for the report: what the shader uses that the comparison treats specially.
        let dis = &xenos_ucode::analyze(&ucode, *ty).disassembly;
        let mut tags = Vec::new();
        for (needle, tag) in [("getWeights", "getWeights"), ("loop", "loop"), ("tfetch", "tfetch"), ("kill", "kill")] {
            if dis.contains(needle) {
                tags.push(tag);
            }
        }
        let tags = if tags.is_empty() { String::new() } else { format!(" [{}]", tags.join(" ")) };
        let incomparable = dis.contains("getWeights");
        let ex = Exec::new(&module);
        let fields = ex.result_fields(0);
        let shader_seed = seed ^ name.bytes().fold(0xcbf2_9ce4_8422_2325u64, |h, b| (h ^ b as u64).wrapping_mul(0x100_0000_01b3));
        let (mut n_minor, mut n_nan, mut n_bad, mut killed) = (0u32, 0u32, 0u32, 0u32);
        let mut first_bad: Option<String> = None;
        let mut error: Option<String> = None;
        let range: Box<dyn Iterator<Item = u32>> = match one_case {
            Some(k) => Box::new(k..k + 1),
            None => Box::new(0..cases),
        };
        let mut result: Box<XiResult> = unsafe { Box::new(std::mem::zeroed()) };
        let t0 = Instant::now();
        let mut ran = 0u32;
        for k in range {
            let mut rng = Rng(shader_seed ^ (k as u64).wrapping_mul(0xD1B5_4A32_D192_ED03));
            let case = make_case(&mut rng, *ty, finite);
            interp.write_guest(GUEST_BASE, &case.guest[GUEST_BASE..]);
            interp.run(&ucode, &case.xi, &mut result);
            if result.overflow != 0 {
                out_of_window_guard += 1;
            }
            let buffers = Buffers { arena: arena(&case.xi), guest: case.guest };
            let args = match ty {
                ShaderType::Vertex => vec![V::S(S::U(case.vertex_index)), V::S(S::U(0))],
                ShaderType::Pixel => {
                    let mut f = vec![V::Vec(4, [S::F(0.0), S::F(0.0), S::F(0.5), S::F(1.0)])];
                    for i in 0..16 {
                        f.push(V::Vec(4, case.xi.regs[i].map(S::F)));
                    }
                    f.push(V::S(S::U(0)));
                    vec![V::Agg(f)]
                }
            };
            let o = match ex.run(0, args, &buffers) {
                Ok(o) => o,
                Err(e) => {
                    error = Some(format!("case {}: {}", k, e));
                    break;
                }
            };
            ran += 1;
            if ex.killed.get() {
                killed += 1;
            }
            let f = compare(*ty, registers, &result, &o, &fields);
            match f.worst {
                Cmp::Same => {}
                Cmp::ZeroOrDenormal => n_minor += 1,
                Cmp::NanAgainstNumber => n_nan += 1,
                Cmp::Different => {
                    n_bad += 1;
                    if first_bad.is_none() {
                        first_bad = Some(format!("case {}: {}", k, f.first.unwrap_or_default()));
                    }
                }
            }
            if one_case.is_some() {
                println!("{}: case {}: {:?}", name, k, f.worst);
                for line in &f.all {
                    println!("    {}", line);
                }
                for (i, r) in case.xi.regs.iter().enumerate().take(registers.max(1)) {
                    if r.iter().any(|x| *x != 0.0) {
                        println!("    input r{} = {:?}", i, r);
                    }
                }
            }
            // Keep the borrowed guest vector alive until here.
            drop(buffers);
        }
        let ms = t0.elapsed().as_millis();
        let state = if error.is_some() {
            broken += 1;
            "ERROR"
        } else if n_bad > 0 && incomparable {
            weights += 1;
            "weights"
        } else if n_bad > 0 {
            bad += 1;
            "DIFFERS"
        } else if n_nan > 0 {
            nan_only += 1;
            "nan-only"
        } else if n_minor > 0 {
            minor += 1;
            "zero-sign"
        } else {
            clean += 1;
            "same"
        };
        println!(
            "{} {}{}: {} cases, {} differ, {} NaN against a number, {} only +0/-0/denormal, {} killed ({} ms){}{}",
            state,
            name,
            tags,
            ran,
            n_bad,
            n_nan,
            n_minor,
            killed,
            ms,
            first_bad.map(|s| format!("\n    first: {}", s)).unwrap_or_default(),
            error.map(|s| format!("\n    error: {}", s)).unwrap_or_default(),
        );
    }
    println!(
        "{} shaders in {:.1} s: {} the same, {} differ only in NaNs, {} only in +0/-0/denormals, {} differ where getWeights makes them incomparable, {} differ, {} not run{}",
        files.len(),
        started.elapsed().as_secs_f64(),
        clean,
        nan_only,
        minor,
        weights,
        bad,
        broken,
        if out_of_window_guard > 0 { format!(" ({} cases exported more than 256 times)", out_of_window_guard) } else { String::new() }
    );
}
