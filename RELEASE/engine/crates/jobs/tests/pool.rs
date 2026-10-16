//! The pool's promises: every loop item runs exactly once, loops may borrow,
//! priorities and the background limit hold, panics come back to whoever
//! waits, nested loops can't deadlock, and dropping the pool runs what was
//! queued.

use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Barrier, Mutex};
use std::time::Duration;

use jobs::{Pool, Priority};

#[test]
fn every_item_runs_exactly_once() {
    for workers in [1, 2, 3, 8] {
        let pool = Pool::new(workers, 1, workers > 2);
        for count in [0, 1, 2, 7, 64, 1000, 4097] {
            let hits: Vec<AtomicUsize> = (0..count).map(|_| AtomicUsize::new(0)).collect();
            pool.parallel_for(count, |i| {
                hits[i].fetch_add(1, Ordering::Relaxed);
            });
            assert!(
                hits.iter().all(|h| h.load(Ordering::Relaxed) == 1),
                "{workers} workers, {count} items"
            );
        }
    }
}

#[test]
fn chunks_cover_the_items_in_order() {
    let pool = Pool::new(4, 1, false);
    // Like recording draws: each chunk fills its own buffer, joined in chunk
    // order afterwards.
    let items: Vec<u32> = (0..1000).collect();
    let chunk_size = 37;
    let chunks = items.len().div_ceil(chunk_size);
    let buffers: Vec<Mutex<Vec<u32>>> = (0..chunks).map(|_| Mutex::new(Vec::new())).collect();
    pool.for_each_chunk(items.len(), chunk_size, |chunk, range| {
        assert_eq!(range.start, chunk * chunk_size);
        buffers[chunk].lock().unwrap().extend(&items[range]);
    });
    let joined: Vec<u32> = buffers
        .into_iter()
        .flat_map(|b| b.into_inner().unwrap())
        .collect();
    assert_eq!(joined, items);
}

#[test]
fn a_loop_uses_more_than_one_thread() {
    let pool = Pool::new(2, 1, false);
    // Two chunks that each wait for the other: only finishes if two threads
    // run them at once (the caller and a worker).
    let both = Barrier::new(2);
    pool.for_each_chunk(2, 1, |_, _| {
        both.wait();
    });
}

#[test]
fn a_chunk_panic_reaches_the_caller_and_the_pool_survives() {
    let pool = Pool::new(2, 1, false);
    let ran = AtomicUsize::new(0);
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        pool.for_each_chunk(100, 1, |chunk, _| {
            ran.fetch_add(1, Ordering::Relaxed);
            if chunk == 42 {
                panic!("chunk 42");
            }
        });
    }));
    let payload = result.expect_err("the panic comes back");
    assert_eq!(payload.downcast_ref::<&str>(), Some(&"chunk 42"));
    // Every other chunk still ran, and the pool still works.
    assert_eq!(ran.load(Ordering::Relaxed), 100);
    let sum = AtomicUsize::new(0);
    pool.parallel_for(10, |i| {
        sum.fetch_add(i, Ordering::Relaxed);
    });
    assert_eq!(sum.load(Ordering::Relaxed), 45);
}

#[test]
fn nested_loops_finish_even_with_one_worker() {
    let pool = Arc::new(Pool::new(1, 1, false));
    let total = AtomicUsize::new(0);
    pool.parallel_for(8, |_| {
        pool.parallel_for(8, |_| {
            total.fetch_add(1, Ordering::Relaxed);
        });
    });
    assert_eq!(total.load(Ordering::Relaxed), 64);

    // A loop inside a job, while the only worker runs that job.
    let (send, receive) = mpsc::channel();
    let inner = Arc::clone(&pool);
    pool.spawn(Priority::Now, move || {
        let count = AtomicUsize::new(0);
        inner.parallel_for(100, |_| {
            count.fetch_add(1, Ordering::Relaxed);
        });
        send.send(count.load(Ordering::Relaxed)).unwrap();
    });
    assert_eq!(receive.recv_timeout(Duration::from_secs(10)), Ok(100));
}

/// A job that holds the pool's only worker until `open` is sent.
fn block_worker(pool: &Pool) -> mpsc::Sender<()> {
    let (open, gate) = mpsc::channel::<()>();
    let (started, running) = mpsc::channel();
    pool.spawn(Priority::Frame, move || {
        started.send(()).unwrap();
        let _ = gate.recv();
    });
    running.recv_timeout(Duration::from_secs(10)).unwrap();
    open
}

#[test]
fn frame_jobs_run_before_now_jobs_before_background() {
    let pool = Pool::new(1, 1, false);
    let open = block_worker(&pool);
    let order = Arc::new(Mutex::new(Vec::new()));
    for (priority, name) in [
        (Priority::Background, "background"),
        (Priority::Now, "now"),
        (Priority::Frame, "frame"),
        (Priority::Now, "now 2"),
    ] {
        let order = Arc::clone(&order);
        pool.spawn(priority, move || order.lock().unwrap().push(name));
    }
    open.send(()).unwrap();
    drop(pool); // runs what is queued
    assert_eq!(
        *order.lock().unwrap(),
        ["frame", "now", "now 2", "background"]
    );
}

#[test]
fn background_jobs_never_take_every_worker() {
    let pool = Pool::new(3, 1, true);
    let running = Arc::new(AtomicUsize::new(0));
    let most = Arc::new(AtomicUsize::new(0));
    let (open, gate) = mpsc::channel::<()>();
    let gate = Arc::new(Mutex::new(gate));
    for _ in 0..3 {
        let (running, most, gate) = (Arc::clone(&running), Arc::clone(&most), Arc::clone(&gate));
        pool.spawn(Priority::Background, move || {
            let now = running.fetch_add(1, Ordering::SeqCst) + 1;
            most.fetch_max(now, Ordering::SeqCst);
            let _ = gate.lock().unwrap().recv_timeout(Duration::from_secs(10));
            running.fetch_sub(1, Ordering::SeqCst);
        });
    }
    // While a background job holds a worker, frame work still gets the others.
    let frame_ran = Arc::new(AtomicBool::new(false));
    let flag = Arc::clone(&frame_ran);
    let mut task = pool.spawn_task(Priority::Frame, move || flag.store(true, Ordering::SeqCst));
    let started = std::time::Instant::now();
    while task.try_take().is_none() {
        assert!(
            started.elapsed() < Duration::from_secs(10),
            "frame job never ran"
        );
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(frame_ran.load(Ordering::SeqCst));
    for _ in 0..3 {
        open.send(()).unwrap();
    }
    drop(pool);
    assert_eq!(most.load(Ordering::SeqCst), 1);
}

#[test]
fn tasks_return_their_result_or_their_panic() {
    let pool = Pool::new(2, 1, false);
    let answer = pool.spawn_task(Priority::Now, || 6 * 7);
    assert_eq!(answer.wait().ok(), Some(42));
    let broken = pool.spawn_task(Priority::Now, || -> u32 { panic!("broken task") });
    let payload = broken.wait().expect_err("the panic comes back");
    assert_eq!(payload.downcast_ref::<&str>(), Some(&"broken task"));
}

#[test]
fn dropping_the_pool_runs_queued_jobs() {
    let done = Arc::new(AtomicUsize::new(0));
    let pool = Pool::new(2, 1, false);
    for _ in 0..50 {
        let done = Arc::clone(&done);
        pool.spawn(Priority::Background, move || {
            done.fetch_add(1, Ordering::Relaxed);
        });
    }
    drop(pool);
    assert_eq!(done.load(Ordering::Relaxed), 50);
}

#[test]
fn sized_for_this_machine() {
    let pool = Pool::for_this_machine();
    let budget = machine::engine_budget(machine::get());
    assert_eq!(pool.workers(), budget.workers);
    assert_eq!(pool.parallelism(), budget.frame_parallelism());
}
