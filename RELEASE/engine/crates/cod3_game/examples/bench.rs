//! How fast the engine layer is, and how it grows with input size.
//!
//!     cargo run --release -p cod3_game --example bench
//!
//! Prints nanoseconds per call for typical sizes and for sizes far past the
//! Xbox 360's limits, so a function that slows down faster than its input
//! grows (or allocates where it shouldn't) shows up as a jump between rows.
//! The numbers in docs/DECOMP.md come from here.

use std::hint::black_box;
use std::time::{Duration, Instant};

use cod3_game::engine::info::InfoString;
use cod3_game::engine::math::{self, angles};
use cod3_game::engine::names::{name_hash, Name};
use cod3_game::engine::script::ScriptRandom;
use cod3_game::engine::strings;
use cod3_game::net::bit_buffer::BitBuffer;
use cod3_game::net::position::{
    read_position_update, write_position_update, LocalEntity, PlayerSnapshot, PositionBaseline,
    RemotePlayer,
};

/// Runs `f` until about 200 ms have passed and returns nanoseconds per call.
fn time(mut f: impl FnMut()) -> f64 {
    let mut calls: u64 = 0;
    let start = Instant::now();
    let budget = Duration::from_millis(200);
    while start.elapsed() < budget {
        for _ in 0..64 {
            f();
        }
        calls += 64;
    }
    start.elapsed().as_nanos() as f64 / calls as f64
}

fn row(what: &str, ns: f64) {
    println!("  {what:<52} {ns:>12.1} ns");
}

/// An info string with `pairs` pairs of 8-character keys and `value_len`-character values.
fn info_with(pairs: usize, value_len: usize) -> (InfoString, Vec<Vec<u8>>) {
    let mut info = InfoString::new();
    let mut keys = Vec::with_capacity(pairs);
    let value = vec![b'v'; value_len];
    for i in 0..pairs {
        let key = format!("key{i:05}").into_bytes();
        info.set(&key, &value).unwrap();
        keys.push(key);
    }
    (info, keys)
}

fn main() {
    println!("info strings (engine::info::InfoString)");
    for (pairs, value_len) in [
        (10, 16),
        (100, 16),
        (1_000, 16),
        (10_000, 16),
        (100, 10_000),
    ] {
        let (mut info, keys) = info_with(pairs, value_len);
        let size = info.as_bytes().len();
        let first = keys[0].clone();
        let last = keys[pairs - 1].to_ascii_uppercase();
        row(
            &format!("get first key, {pairs} pairs ({size} bytes)"),
            time(|| {
                black_box(info.get(black_box(&first)));
            }),
        );
        row(
            &format!("get last key, {pairs} pairs ({size} bytes)"),
            time(|| {
                black_box(info.get(black_box(&last)));
            }),
        );
        let middle = keys[pairs / 2].clone();
        let value = vec![b'w'; value_len];
        row(
            &format!("set an existing key, {pairs} pairs"),
            time(|| {
                info.set(black_box(&middle), black_box(&value)).unwrap();
            }),
        );
    }
    let build = Instant::now();
    let _ = info_with(10_000, 16);
    row(
        "build 10,000 pairs one set() at a time",
        build.elapsed().as_nanos() as f64,
    );

    println!("names and text");
    for len in [8, 64, 4_096] {
        let text: Vec<u8> = (0..len).map(|i| b'a' + (i % 26) as u8).collect();
        row(
            &format!("name_hash, {len} characters"),
            time(|| {
                black_box(name_hash(black_box(&text)));
            }),
        );
        row(
            &format!("Name::new, {len} characters"),
            time(|| {
                black_box(Name::new(black_box(&text)));
            }),
        );
        let upper = text.to_ascii_uppercase();
        row(
            &format!("equal_ignore_case, {len} characters"),
            time(|| {
                black_box(strings::equal_ignore_case(
                    black_box(&text),
                    black_box(&upper),
                ));
            }),
        );
        row(
            &format!("compare_ignore_case, {len} characters"),
            time(|| {
                black_box(strings::compare_ignore_case(
                    black_box(&text),
                    black_box(&upper),
                ));
            }),
        );
    }

    println!("math");
    // Inputs cycle through 1,024 prepared values, so the loop measures the
    // function and not the making of its input (and nothing drifts into
    // denormal numbers, which x86 CPUs handle about 100 times slower).
    let mut r = 0x2545_F491u32;
    let mut random = move || {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        r as f32 / u32::MAX as f32
    };
    let usual: Vec<f32> = (0..1024).map(|_| random() * 1080.0 - 540.0).collect();
    let huge: Vec<f32> = (0..1024).map(|_| random() * 2.0e6 - 1.0e6).collect();
    let vectors: Vec<[f32; 3]> = (0..1024)
        .map(|_| {
            [
                random() * 200.0 - 100.0,
                random() * 200.0 - 100.0,
                random() * 200.0 - 100.0,
            ]
        })
        .collect();
    let radians: Vec<f64> = (0..1024)
        .map(|_| f64::from(random()) * 20.0 - 10.0)
        .collect();
    let mut i = 0usize;
    let mut next = || {
        i = (i + 1) & 1023;
        i
    };
    row(
        "angles::normalize_180, angles within ±540",
        time(|| {
            black_box(angles::normalize_180(black_box(usual[next()])));
        }),
    );
    row(
        "angles::normalize_180, huge angles",
        time(|| {
            black_box(angles::normalize_180(black_box(huge[next()])));
        }),
    );
    row(
        "math::normalize (vec3)",
        time(|| {
            let mut v = vectors[next()];
            black_box(math::normalize(black_box(&mut v)));
            black_box(v);
        }),
    );
    row(
        "math::length_squared (vec3, two fused multiply-adds)",
        time(|| {
            black_box(math::length_squared(black_box(&vectors[next()])));
        }),
    );
    row(
        "math::sin (the console's, deterministic)",
        time(|| {
            black_box(math::sin(black_box(radians[next()])));
        }),
    );
    row(
        "f64::sin (the platform's, for comparison)",
        time(|| {
            black_box(black_box(radians[next()]).sin());
        }),
    );
    let mut script_random = ScriptRandom::default();
    row(
        "script random int_range",
        time(|| {
            black_box(script_random.int_range(black_box(0), black_box(100)));
        }),
    );
    row(
        "script random float_range (a fused multiply-add)",
        time(|| {
            black_box(script_random.float_range(black_box(-1.0), black_box(1.0)));
        }),
    );

    println!("network messages (net::position; console layer, for every peer)");
    let players: Vec<PlayerSnapshot> = (0..1024)
        .map(|_| PlayerSnapshot {
            origin: [
                random() * 8000.0 - 4000.0,
                random() * 8000.0 - 4000.0,
                random() * 500.0,
            ],
            velocity: [random() * 400.0 - 200.0, random() * 400.0 - 200.0, 0.0],
            angles: [random() * 60.0 - 30.0, random() * 360.0, 0.0],
            weapon: 3,
            flags: 0x10,
            use_code: 1,
            stance: 0,
            state: 0,
        })
        .collect();
    let empty = PositionBaseline {
        counter: 0,
        origin: [0.0; 3],
        angles: [0.0; 3],
        velocity: [0.0; 3],
        use_code: 0,
        weapon: 0,
        flags: 0x10,
        stance: 0,
    };
    // One buffer, emptied before each message, so the timing has no allocation in it.
    let mut buffer = BitBuffer::new(false);
    let restart = |b: &mut BitBuffer| {
        b.data.clear();
        b.data.push(0);
        (b.write_position, b.written, b.read_position, b.read_failed) = (1, 1, 1, false);
    };
    row(
        "write_position_update, full",
        time(|| {
            restart(&mut buffer);
            let mut baseline = empty;
            let p = &players[next()];
            black_box(write_position_update(
                &mut buffer,
                &mut baseline,
                Some(p),
                true,
                true,
            ));
        }),
    );
    row(
        "write_position_update, delta (moved, turned)",
        time(|| {
            restart(&mut buffer);
            let p = &players[next()];
            let mut baseline = PositionBaseline {
                origin: [p.origin[0] - 20.0, p.origin[1], p.origin[2]],
                velocity: p.velocity,
                ..empty
            };
            black_box(write_position_update(
                &mut buffer,
                &mut baseline,
                Some(p),
                false,
                false,
            ));
        }),
    );
    let mut remote = RemotePlayer {
        last_update_time: 0,
        use_code: 0,
        field_54: 0,
        pitch: 0.0,
        origin: [0.0; 3],
        velocity: [0.0; 3],
        view: [0.0; 3],
        baseline: empty,
        update_interval: 0,
        view_reset_pending: false,
        view_reset_pitch: 0.0,
        view_reset_yaw: 0.0,
    };
    let sin_cos: fn(&[f32; 4]) -> [f32; 4] = |a| [a[0].sin(), a[1].cos(), a[2].sin(), a[3].cos()];
    row(
        "read_position_update, full (sin/cos stand-in)",
        time(|| {
            restart(&mut buffer);
            let mut baseline = empty;
            write_position_update(
                &mut buffer,
                &mut baseline,
                Some(&players[next()]),
                true,
                true,
            );
            buffer.read_position = 1;
            let mut ok = true;
            black_box(read_position_update(
                &mut buffer,
                &mut remote,
                LocalEntity::NoClient,
                &mut ok,
                0,
                sin_cos,
            ));
        }),
    );
}
