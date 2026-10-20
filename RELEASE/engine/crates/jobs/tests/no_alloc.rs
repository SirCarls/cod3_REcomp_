//! A parallel loop allocates nothing once the pool has run one (DESIGN 3.8:
//! the renderer records draws with several loops a frame). Counts every
//! allocation in the process, so it runs without the test harness's threads
//! (`harness = false` in Cargo.toml).

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};

use jobs::Pool;

struct Counting;

static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);

// SAFETY: forwards to the system allocator, counting.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        unsafe { System.realloc(ptr, layout, new_size) }
    }
}

#[global_allocator]
static GLOBAL: Counting = Counting;

fn loops(pool: &Pool, rounds: usize, sums: &[AtomicU64]) {
    for round in 0..rounds {
        pool.for_each_chunk(4096, 64, |chunk, range| {
            let mut sum = 0u64;
            for i in range {
                sum = sum.wrapping_add((i * round) as u64);
            }
            sums[chunk].fetch_add(sum, Ordering::Relaxed);
        });
        // A loop inside a loop: a second state, made once.
        pool.parallel_for(8, |_| {
            pool.parallel_for(64, |i| {
                sums[i].fetch_add(1, Ordering::Relaxed);
            });
        });
    }
}

fn main() {
    for (workers, spin) in [(1, false), (3, true), (8, false)] {
        let pool = Pool::new(workers, 1, spin);
        let sums: Vec<AtomicU64> = (0..64).map(|_| AtomicU64::new(0)).collect();
        // Warm up: the pool makes the loop states it needs (one per loop
        // running at the same time) and its helper queue reaches its size.
        loops(&pool, 50, &sums);
        let states = pool.loop_states();
        let before = ALLOCATIONS.load(Ordering::Relaxed);
        loops(&pool, 2000, &sums);
        let allocations = ALLOCATIONS.load(Ordering::Relaxed) - before;
        println!(
            "{workers} workers: {allocations} allocations in 4000 loops after warm-up; {states} loop states, {} after",
            pool.loop_states()
        );
        assert_eq!(
            allocations, 0,
            "{workers} workers: parallel loops allocated"
        );
        assert_eq!(
            pool.loop_states(),
            states,
            "{workers} workers: new loop states after warm-up"
        );
    }
    println!("no_alloc: ok");
}
