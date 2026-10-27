//! `xenos_ucode`: checks the decoder against the SDK's own reading of the game's shaders.
//!
//!     xenos_ucode corpus DIR [--show N]   every shader_*.ucode.bin.vert|frag in DIR (the SDK's
//!                                         dump_shaders) disassembled and compared, character for
//!                                         character, with the SDK's shader_*.ucode.vert|frag
//!     xenos_ucode stats DIR               which instructions, fetches and exports the corpus uses
//!     xenos_ucode dis FILE                one shader's disassembly
//!     xenos_ucode wgsl FILE               one shader translated to WGSL
//!
//! The corpus is the game's data: it stays on the PC (the REA work folder); this tool
//! prints only counts, and for differences the line numbers and the two lines.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use xenos_ucode::alu::{AluInstruction, SCALAR_OPCODES, VECTOR_OPCODES};
use xenos_ucode::cf::unpack_pair;
use xenos_ucode::fetch::{FetchInstruction, FetchOpcode};
use xenos_ucode::parsed::{parse_alu, parse_exec, StorageTarget};
use xenos_ucode::{analyze, dwords_from_le_bytes, ShaderType};

fn shader_type(path: &Path) -> Option<ShaderType> {
    match path.extension()?.to_str()? {
        "vert" => Some(ShaderType::Vertex),
        "frag" => Some(ShaderType::Pixel),
        _ => None,
    }
}

/// The microcode files in a folder: (path, type), sorted.
fn corpus(dir: &Path) -> Vec<(PathBuf, ShaderType)> {
    let mut files = Vec::new();
    if let Ok(entries) = std::fs::read_dir(dir) {
        for e in entries.flatten() {
            let p = e.path();
            let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("");
            if name.starts_with("shader_") && name.contains(".ucode.bin.") {
                if let Some(t) = shader_type(&p) {
                    files.push((p, t));
                }
            }
        }
    }
    files.sort_by(|a, b| a.0.cmp(&b.0));
    files
}

fn read_ucode(path: &Path) -> Result<Vec<u32>, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {}", path.display(), e))?;
    if bytes.len() % 4 != 0 {
        return Err(format!("{}: {} bytes, not whole dwords", path.display(), bytes.len()));
    }
    Ok(dwords_from_le_bytes(&bytes))
}

fn cmd_corpus(dir: &Path, show: usize) -> ExitCode {
    let files = corpus(dir);
    if files.is_empty() {
        eprintln!("xenos_ucode: no shader_*.ucode.bin.vert|frag in {}", dir.display());
        return ExitCode::from(2);
    }
    let (mut same, mut different, mut no_text, mut vertex, mut pixel) = (0, 0, 0, 0, 0);
    let mut shown = 0;
    for (path, t) in &files {
        match t {
            ShaderType::Vertex => vertex += 1,
            ShaderType::Pixel => pixel += 1,
        }
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        let text_path = path.with_file_name(name.replace(".ucode.bin.", ".ucode."));
        let expected = match std::fs::read_to_string(&text_path) {
            Ok(s) => s,
            Err(_) => {
                no_text += 1;
                continue;
            }
        };
        let ucode = match read_ucode(path) {
            Ok(u) => u,
            Err(e) => {
                eprintln!("{}", e);
                different += 1;
                continue;
            }
        };
        let got = analyze(&ucode, *t).disassembly;
        if got == expected {
            same += 1;
            continue;
        }
        different += 1;
        if shown < show {
            shown += 1;
            let hash = name.split('.').next().unwrap_or(&name);
            let (e_lines, g_lines): (Vec<&str>, Vec<&str>) = (expected.lines().collect(), got.lines().collect());
            let first = (0..e_lines.len().max(g_lines.len()))
                .find(|&k| e_lines.get(k) != g_lines.get(k))
                .unwrap_or(0);
            println!(
                "DIFF {} ({:?}, {} dwords): {} lines expected, {} got; first difference at line {}:",
                hash,
                t,
                ucode.len(),
                e_lines.len(),
                g_lines.len(),
                first + 1
            );
            println!("  expected: {:?}", e_lines.get(first).copied().unwrap_or("<end>"));
            println!("  got:      {:?}", g_lines.get(first).copied().unwrap_or("<end>"));
        }
    }
    println!(
        "corpus {}: {} shaders ({} vertex, {} pixel): {} identical to the SDK's disassembly, {} different, {} without the SDK's text",
        dir.display(),
        files.len(),
        vertex,
        pixel,
        same,
        different,
        no_text
    );
    if different == 0 && same > 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::from(1)
    }
}

fn cmd_stats(dir: &Path) -> ExitCode {
    let files = corpus(dir);
    let mut cf: BTreeMap<String, u64> = BTreeMap::new();
    let mut vector: BTreeMap<&'static str, u64> = BTreeMap::new();
    let mut scalar: BTreeMap<&'static str, u64> = BTreeMap::new();
    let mut fetch: BTreeMap<String, u64> = BTreeMap::new();
    let mut exports: BTreeMap<String, u64> = BTreeMap::new();
    let mut features: BTreeMap<&'static str, u64> = BTreeMap::new();
    let (mut dwords, mut max_regs, mut max_consts) = (0usize, 0u32, 0u32);
    for (path, t) in &files {
        let Ok(ucode) = read_ucode(path) else { continue };
        dwords += ucode.len();
        let a = analyze(&ucode, *t);
        max_regs = max_regs.max(a.register_static_address_bound);
        max_consts = max_consts.max(a.float_constant_count());
        let add = |m: &mut BTreeMap<&'static str, u64>, k: &'static str| *m.entry(k).or_default() += 1;
        if a.uses_register_dynamic_addressing {
            add(&mut features, "temporary registers indexed (aL)");
        }
        if a.float_dynamic_addressing {
            add(&mut features, "float constants indexed (a0 / aL)");
        }
        if a.loop_constants != 0 {
            add(&mut features, "loops");
        }
        if a.bool_constants.iter().any(|&b| b != 0) {
            add(&mut features, "bool constant branches");
        }
        if a.kills_pixels {
            add(&mut features, "pixel kill");
        }
        if a.writes_depth {
            add(&mut features, "depth export");
        }
        if a.memexport_data_written != 0 {
            add(&mut features, "memory export");
        }
        for i in 0..a.cf_pair_index_bound {
            for (j, c) in unpack_pair(&ucode[i as usize * 3..]).iter().enumerate() {
                *cf.entry(format!("{:?}", c.opcode())).or_default() += 1;
                if !c.opcode().is_exec() {
                    continue;
                }
                let exec = parse_exec(c, i * 2 + j as u32);
                let mut seq = exec.sequence;
                for off in exec.instruction_address..exec.instruction_address + exec.instruction_count {
                    let s = off as usize * 3;
                    let Some(w) = ucode.get(s..s + 3) else { break };
                    if seq & 1 != 0 {
                        let f = FetchInstruction::new(w);
                        let key = match f.opcode() {
                            FetchOpcode::VertexFetch => {
                                format!("{} fmt {}", if f.vertex_is_mini_fetch() { "vfetch_mini" } else { "vfetch_full" }, f.vertex_data_format())
                            }
                            other => format!("{:?} {:?}", other, f.texture_dimension()),
                        };
                        *fetch.entry(key).or_default() += 1;
                    } else {
                        let op = AluInstruction::new(w);
                        let p = parse_alu(&op, *t);
                        if !p.is_vector_op_default_nop() {
                            *vector.entry(VECTOR_OPCODES[op.vector_opcode() as usize].name).or_default() += 1;
                        }
                        if !p.is_scalar_op_default_nop() {
                            *scalar.entry(SCALAR_OPCODES[op.scalar_opcode() as usize].name).or_default() += 1;
                        }
                        if p.vector_result.target != StorageTarget::Register {
                            *exports.entry(format!("{:?}", p.vector_result.target)).or_default() += 1;
                        }
                        if p.vector_result.is_clamped || p.scalar_result.is_clamped {
                            *features.entry("saturate").or_default() += 1;
                        }
                        if p.is_predicated {
                            *features.entry("predicated ALU").or_default() += 1;
                        }
                    }
                    seq >>= 2;
                }
            }
        }
    }
    let show = |title: &str, m: &BTreeMap<String, u64>| {
        let mut v: Vec<_> = m.iter().collect();
        v.sort_by(|a, b| b.1.cmp(a.1));
        println!("{} ({} kinds): {}", title, v.len(), v.iter().map(|(k, n)| format!("{} {}", k, n)).collect::<Vec<_>>().join(", "));
    };
    let show_s = |title: &str, m: &BTreeMap<&'static str, u64>| {
        let mut v: Vec<_> = m.iter().collect();
        v.sort_by(|a, b| b.1.cmp(a.1));
        println!("{} ({} kinds): {}", title, v.len(), v.iter().map(|(k, n)| format!("{} {}", k, n)).collect::<Vec<_>>().join(", "));
    };
    println!("{} shaders, {} dwords; at most {} temporary registers, {} float constants", files.len(), dwords, max_regs, max_consts);
    show("control flow", &cf);
    show_s("vector operations", &vector);
    show_s("scalar operations", &scalar);
    show("fetches", &fetch);
    show("exports (vector results)", &exports);
    show_s("features (shaders, or instructions for saturate / predicated)", &features);
    ExitCode::SUCCESS
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("corpus") if args.len() >= 2 => {
            let show = args
                .iter()
                .position(|a| a == "--show")
                .and_then(|i| args.get(i + 1))
                .and_then(|n| n.parse().ok())
                .unwrap_or(10);
            cmd_corpus(Path::new(&args[1]), show)
        }
        Some("stats") if args.len() >= 2 => cmd_stats(Path::new(&args[1])),
        Some("dis") if args.len() >= 2 => {
            let p = Path::new(&args[1]);
            let Some(t) = shader_type(p) else {
                eprintln!("xenos_ucode: {} isn't .vert or .frag", p.display());
                return ExitCode::from(2);
            };
            match read_ucode(p) {
                Ok(u) => {
                    print!("{}", analyze(&u, t).disassembly);
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("{}", e);
                    ExitCode::from(2)
                }
            }
        }
        Some("wgsl") if args.len() >= 2 => {
            let p = Path::new(&args[1]);
            let Some(t) = shader_type(p) else {
                eprintln!("xenos_ucode: {} isn't .vert or .frag", p.display());
                return ExitCode::from(2);
            };
            match read_ucode(p).map(|u| xenos_ucode::wgsl::translate(&u, t)) {
                Ok(Ok(tr)) => {
                    print!("{}", tr.wgsl);
                    ExitCode::SUCCESS
                }
                Ok(Err(refusal)) => {
                    eprintln!("xenos_ucode: not translated: {}", refusal);
                    ExitCode::from(1)
                }
                Err(e) => {
                    eprintln!("{}", e);
                    ExitCode::from(2)
                }
            }
        }
        _ => {
            eprintln!("usage: xenos_ucode corpus DIR [--show N] | stats DIR | dis FILE | wgsl FILE");
            ExitCode::from(2)
        }
    }
}
