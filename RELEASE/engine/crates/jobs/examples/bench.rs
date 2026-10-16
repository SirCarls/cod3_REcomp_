//! What the pool costs and what it gains, on this machine:
//! `cargo run --release -p jobs --example bench`.
//!
//! The last row stands in for a frame's draw recording: 3,000 draws (CoD3's
//! busy scenes reach that) at about 2 µs of CPU each, recorded on one thread
//! and then split over the pool the way the Rust renderer will.

use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use jobs::{Pool, Priority};

fn time<F: FnMut()>(rounds: u32, mut f: F) -> Duration {
    f(); // warm up
    let start = Instant::now();
    for _ in 0..rounds {
        f();
    }
    start.elapsed() / rounds
}

/// About `micros` of CPU work that the optimizer can't remove.
fn work(seed: u64, micros: u64) -> u64 {
    let mut x = seed | 1;
    for _ in 0..micros * 150 {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    x
}

fn main() {
    let pool = Pool::for_this_machine();
    println!("{}", machine::get().summary());
    println!("{}", machine::engine_budget(machine::get()).describe());
    println!();

    let round_trip = time(2000, || {
        let task = pool.spawn_task(Priority::Now, || black_box(1u32));
        black_box(task.wait().ok());
    });
    println!(
        "one task, queued and waited for:   {:>8.2} µs",
        round_trip.as_secs_f64() * 1e6
    );

    let empty_loop = time(2000, || {
        pool.for_each_chunk(pool.parallelism(), 1, |_, _| {})
    });
    println!(
        "an empty loop over {} threads:     {:>8.2} µs",
        pool.parallelism(),
        empty_loop.as_secs_f64() * 1e6
    );

    const DRAWS: usize = 3000;
    const MICROS_PER_DRAW: u64 = 2;
    let sink = AtomicU64::new(0);
    let serial = time(5, || {
        let mut total = 0u64;
        for draw in 0..DRAWS {
            total = total.wrapping_add(work(draw as u64, MICROS_PER_DRAW));
        }
        sink.fetch_add(total, Ordering::Relaxed);
    });
    let chunk = DRAWS.div_ceil(pool.parallelism() * 4);
    let parallel = time(5, || {
        pool.for_each_chunk(DRAWS, chunk, |_, range| {
            let mut total = 0u64;
            for draw in range {
                total = total.wrapping_add(work(draw as u64, MICROS_PER_DRAW));
            }
            sink.fetch_add(total, Ordering::Relaxed);
        });
    });
    println!(
        "{DRAWS} draws recorded, one thread:  {:>8.2} ms",
        serial.as_secs_f64() * 1e3
    );
    println!(
        "{DRAWS} draws recorded, the pool:    {:>8.2} ms ({:.1}x)",
        parallel.as_secs_f64() * 1e3,
        serial.as_secs_f64() / parallel.as_secs_f64()
    );
    black_box(sink.load(Ordering::Relaxed));
}
