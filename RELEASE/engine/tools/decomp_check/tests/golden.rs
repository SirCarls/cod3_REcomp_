//! Replays the original game's recorded answers (`golden/*.txt`, written by
//! `python3 tools/decomp/decomp.py golden`) through the Rust ports.
//!
//! The checker proves the ports right against the original on the machine
//! that runs the emulator. This test proves that every other machine computes
//! the same bits: Windows, Linux, macOS, x86 or ARM, any C library, built
//! for any CPU. Players on different systems must agree on every float the
//! game shares (multiplayer between the Xbox 360, the recomp and the Rust
//! engine, docs/MULTIPLAYER.md), and this is where a platform that rounds
//! one bit differently shows up. It needs no game files and no emulator.
//!
//! Replies are compared like the checker does (tools/decomp/harness.py):
//! exactly, except that any NaN matches any NaN (the PowerPC and x86 make
//! different NaN bits) and a spec may allow a few units in the last place.

use std::io::{BufRead, BufReader, Write};
use std::path::Path;
use std::process::{Command, Stdio};

/// One recorded case.
struct Case {
    request: String,
    reply: String,
    ulps: u32,
    /// Reply tokens (0 = the result) that are buffers of big-endian `f32`s.
    float_tokens: Vec<usize>,
}

fn read_cases() -> Vec<Case> {
    let folder = Path::new(env!("CARGO_MANIFEST_DIR")).join("golden");
    let mut files: Vec<_> = std::fs::read_dir(&folder)
        .expect("golden/ folder")
        .map(|entry| entry.unwrap().path())
        .filter(|path| path.extension().is_some_and(|e| e == "txt"))
        .collect();
    files.sort();
    let mut cases = Vec::new();
    for path in files {
        let text = std::fs::read_to_string(&path).unwrap();
        for line in text
            .lines()
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
        {
            let mut fields = line.split('\t');
            let request = fields.next().unwrap().to_string();
            let reply = fields.next().expect("a reply").to_string();
            let mut case = Case {
                request,
                reply,
                ulps: 0,
                float_tokens: Vec::new(),
            };
            for rule in fields.next().unwrap_or("").split(' ') {
                if let Some(n) = rule.strip_prefix("ulps=") {
                    case.ulps = n.parse().unwrap();
                } else if let Some(list) = rule.strip_prefix("floats=") {
                    case.float_tokens = list.split(',').map(|i| i.parse().unwrap()).collect();
                }
            }
            cases.push(case);
        }
    }
    cases
}

/// Distance between two `f32` bit patterns in units in the last place.
fn ulp_distance(a: u32, b: u32) -> u64 {
    let ordered = |x: u32| -> i64 {
        if x < 0x8000_0000 {
            x as i64
        } else {
            0x8000_0000 - x as i64
        }
    };
    (ordered(a) - ordered(b)).unsigned_abs()
}

fn floats_match(a: u32, b: u32, ulps: u32) -> bool {
    let (fa, fb) = (f32::from_bits(a), f32::from_bits(b));
    (fa.is_nan() && fb.is_nan()) || a == b || ulp_distance(a, b) <= u64::from(ulps)
}

fn hex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

/// One reply token, as the checker compares it.
fn tokens_match(expected: &str, actual: &str, ulps: u32, float_buffer: bool) -> bool {
    if expected == actual {
        return true;
    }
    if expected.get(..2) != actual.get(..2) {
        return false;
    }
    match &expected[..2] {
        "f:" => {
            let bits = |t: &str| u32::from_str_radix(&t[2..], 16).unwrap();
            floats_match(bits(expected), bits(actual), ulps)
        }
        "d:" => {
            let value = |t: &str| f64::from_bits(u64::from_str_radix(&t[2..], 16).unwrap());
            value(expected).is_nan() && value(actual).is_nan()
        }
        "b:" if float_buffer && expected.len() == actual.len() => {
            let (e, a) = (hex(&expected[2..]), hex(&actual[2..]));
            e.len() % 4 == 0
                && e.chunks(4).zip(a.chunks(4)).all(|(x, y)| {
                    let bits = |b: &[u8]| u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
                    floats_match(bits(x), bits(y), ulps)
                })
        }
        _ => false,
    }
}

fn replies_match(case: &Case, actual: &str) -> bool {
    let expected: Vec<&str> = case.reply.split(' ').collect();
    let actual: Vec<&str> = actual.split(' ').collect();
    if expected[0] == "err" {
        // The port must refuse the same input with the same exit (the level may follow).
        return actual.len() >= 2 && actual[..2] == expected[..2];
    }
    expected.len() == actual.len()
        && expected[0] == actual[0]
        && expected[1..]
            .iter()
            .zip(&actual[1..])
            .enumerate()
            .all(|(i, (e, a))| tokens_match(e, a, case.ulps, case.float_tokens.contains(&i)))
}

#[test]
fn the_ports_give_the_originals_recorded_answers_on_this_machine() {
    let cases = read_cases();
    assert!(!cases.is_empty(), "no golden cases found");
    let mut checker = Command::new(env!("CARGO_BIN_EXE_decomp_check"))
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .expect("start decomp_check");
    let mut input = checker.stdin.take().unwrap();
    let mut output = BufReader::new(checker.stdout.take().unwrap());
    let mut failures = Vec::new();
    let mut differing = 0;
    for case in &cases {
        writeln!(input, "{}", case.request).unwrap();
        input.flush().unwrap();
        let mut reply = String::new();
        output.read_line(&mut reply).unwrap();
        let reply = reply.trim_end();
        if !replies_match(case, reply) {
            differing += 1;
            if failures.len() < 10 {
                failures.push(format!(
                    "{}\n    original: {}\n    here:     {}",
                    case.request, case.reply, reply
                ));
            }
        }
    }
    drop(input);
    checker.wait().unwrap();
    assert!(
        differing == 0,
        "{} of {} recorded answers differ on this machine (first ones):\n{}",
        differing,
        cases.len(),
        failures.join("\n")
    );
}
