//! The Rust engine's job pool: one set of worker threads for everything that
//! isn't the game thread's own simulation, sized from the machine
//! ([`machine::engine_budget`]), so a frame's work spreads over every core
//! the game may use instead of queuing behind one render or GPU thread
//! (owner, 2026-10-13: "the rust port shouldn't be limited by the 1 gpu
//! thread"; the design is in `docs/SCALING.md`).
//!
//! - [`Pool::for_each_chunk`] / [`Pool::parallel_for`]: a loop split over the
//!   workers, the thread that asks working too. The renderer records draws
//!   this way: each chunk fills its own command buffer for a slice of a pass,
//!   and the buffers are submitted in chunk order afterwards, so recording is
//!   parallel and the submission stays one cheap call. The loop may borrow the
//!   caller's data: it returns only when every chunk has run.
//! - [`Pool::spawn`] / [`Pool::spawn_task`]: one job, at a [`Priority`]:
//!   [`Priority::Frame`] work runs first, then [`Priority::Now`] (what a frame
//!   is waiting for: a streamed zone's decompression, a pipeline a draw needs),
//!   then [`Priority::Background`] (shader warm-up, cache writes, HD
//!   textures), and background jobs never hold more than `background_max`
//!   workers, so frame work always finds a free one.
//!
//! A job that panics doesn't take its worker down: a loop's panic comes back
//! to the caller of the loop, a task's to whoever waits for it.
//!
//! - [`Progress`]: a waiter sleeps until the work it waits for moves on (a
//!   GPU fence, the streaming thread's next block) instead of checking it in
//!   a loop; see `progress.rs`.
//!
//! Costs (`cargo run --release -p jobs --example bench`): a spawned job is one
//! allocation and a lock; a parallel loop adds one shared allocation and one
//! job per helping worker, so a chunk should hold at least a few microseconds
//! of work.

mod progress;
pub use progress::Progress;

use std::any::Any;
use std::collections::VecDeque;
use std::ops::Range;
use std::panic::{self, AssertUnwindSafe};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

/// Which work runs first when several jobs wait.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Priority {
    /// Part of the frame being built (draw recording, culling, animation).
    Frame = 0,
    /// Something a frame waits for.
    Now = 1,
    /// Work nobody waits for; at most `background_max` at once.
    Background = 2,
}

type Job = Box<dyn FnOnce() + Send + 'static>;

struct State {
    queues: [VecDeque<Job>; 3],
    background_running: usize,
    shutting_down: bool,
}

impl State {
    /// The next job a worker may run, and whether it is a background job.
    fn take(&mut self, background_max: usize) -> Option<(Job, bool)> {
        if let Some(job) = self.queues[Priority::Frame as usize].pop_front() {
            return Some((job, false));
        }
        if let Some(job) = self.queues[Priority::Now as usize].pop_front() {
            return Some((job, false));
        }
        if self.background_running < background_max {
            if let Some(job) = self.queues[Priority::Background as usize].pop_front() {
                self.background_running += 1;
                return Some((job, true));
            }
        }
        None
    }
}

struct Shared {
    state: Mutex<State>,
    work_ready: Condvar,
    background_max: usize,
    spin: bool,
    /// Counts every change that may give an idle worker something to run (a
    /// job queued, a background job finished); read without the lock by
    /// spinning workers.
    generation: AtomicUsize,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, State> {
        // Jobs run outside the lock, so a panic can't leave the queues half
        // changed; a poisoned lock is still consistent.
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

/// How long an idle worker keeps checking for work before it sleeps, when
/// the budget allows spinning: shorter than a frame's gaps between batches of
/// jobs, long enough to skip the wake-up's cost when jobs come in bursts.
const SPIN_TIME: Duration = Duration::from_micros(50);

fn worker_loop(shared: &Shared) {
    loop {
        let (job, background) = {
            let mut state = shared.lock();
            // One spin per idle spell, then sleep: a queue holding only
            // background jobs that may not start yet must not keep a worker
            // spinning.
            let mut spun = false;
            loop {
                if let Some(found) = state.take(shared.background_max) {
                    break found;
                }
                // Queued jobs run before the pool closes, so dropping a pool
                // never loses work.
                if state.shutting_down {
                    return;
                }
                if shared.spin && !spun {
                    spun = true;
                    let seen = shared.generation.load(Ordering::Relaxed);
                    drop(state);
                    let start = Instant::now();
                    while shared.generation.load(Ordering::Relaxed) == seen
                        && start.elapsed() < SPIN_TIME
                    {
                        std::hint::spin_loop();
                    }
                    state = shared.lock();
                    continue;
                }
                state = shared
                    .work_ready
                    .wait(state)
                    .unwrap_or_else(|poisoned| poisoned.into_inner());
            }
        };
        // A panicking job is reported by the panic hook and, for loops and
        // tasks, to whoever waits for it; the worker carries on.
        let _ = panic::catch_unwind(AssertUnwindSafe(job));
        if background {
            let mut state = shared.lock();
            state.background_running -= 1;
            if !state.queues[Priority::Background as usize].is_empty() {
                shared.generation.fetch_add(1, Ordering::Relaxed);
                shared.work_ready.notify_one();
            }
        }
    }
}

/// The pool. Dropping it runs the jobs still queued, then joins its threads.
pub struct Pool {
    shared: Arc<Shared>,
    threads: Vec<JoinHandle<()>>,
}

impl Pool {
    /// `workers` threads (at least 1); at most `background_max` (at least 1)
    /// of them on background jobs at once; `spin`: idle workers check for
    /// work briefly before sleeping.
    pub fn new(workers: usize, background_max: usize, spin: bool) -> Pool {
        let workers = workers.max(1);
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                queues: [VecDeque::new(), VecDeque::new(), VecDeque::new()],
                background_running: 0,
                shutting_down: false,
            }),
            work_ready: Condvar::new(),
            background_max: background_max.clamp(1, workers),
            spin,
            generation: AtomicUsize::new(0),
        });
        let threads = (0..workers)
            .map(|index| {
                let shared = Arc::clone(&shared);
                thread::Builder::new()
                    // Linux shows 15 characters of a thread's name.
                    .name(format!("job worker {index}"))
                    .spawn(move || worker_loop(&shared))
                    .expect("starting a job worker thread")
            })
            .collect();
        Pool { shared, threads }
    }

    /// Sized for this machine: [`machine::engine_budget`] of [`machine::get`].
    pub fn for_this_machine() -> Pool {
        let budget = machine::engine_budget(machine::get());
        Pool::new(
            budget.workers,
            budget.background_max,
            budget.spinning_allowed,
        )
    }

    pub fn workers(&self) -> usize {
        self.threads.len()
    }

    /// How many pieces a parallel loop is worth splitting into: every worker
    /// and the thread that asks.
    pub fn parallelism(&self) -> usize {
        self.workers() + 1
    }

    /// Queues one job.
    pub fn spawn(&self, priority: Priority, job: impl FnOnce() + Send + 'static) {
        let mut state = self.shared.lock();
        state.queues[priority as usize].push_back(Box::new(job));
        self.shared.generation.fetch_add(1, Ordering::Relaxed);
        drop(state);
        self.shared.work_ready.notify_one();
    }

    /// Queues one job whose result someone will wait for.
    pub fn spawn_task<T: Send + 'static>(
        &self,
        priority: Priority,
        task: impl FnOnce() -> T + Send + 'static,
    ) -> Pending<T> {
        let slot = Arc::new(Slot {
            result: Mutex::new(None),
            ready: Condvar::new(),
        });
        let filled = Arc::clone(&slot);
        self.spawn(priority, move || {
            let result = panic::catch_unwind(AssertUnwindSafe(task));
            *filled.result.lock().unwrap_or_else(|p| p.into_inner()) = Some(result);
            filled.ready.notify_all();
        });
        Pending { slot }
    }

    /// Runs `f(chunk, range)` for `0..items` cut into ranges of `chunk_size`
    /// (the last one shorter), chunk numbers counting from 0, on the workers
    /// and on this thread, at [`Priority::Frame`]. Returns when every chunk
    /// has run; a chunk's panic is raised again here, after the others have
    /// finished.
    pub fn for_each_chunk<F>(&self, items: usize, chunk_size: usize, f: F)
    where
        F: Fn(usize, Range<usize>) + Sync,
    {
        let chunk_size = chunk_size.max(1);
        let chunks = items.div_ceil(chunk_size);
        if chunks == 0 {
            return;
        }
        if chunks == 1 {
            f(0, 0..items);
            return;
        }
        let f: &(dyn Fn(usize, Range<usize>) + Sync) = &f;
        // SAFETY: the loop's state may outlive this call (a helper job can
        // start after the loop is over), but `body` is only called for a chunk
        // a thread has claimed, and this call returns only once every chunk
        // has finished; a helper that starts late finds no chunk left and
        // never touches `body`. So `f` is alive whenever `body` is used.
        let body: *const (dyn Fn(usize, Range<usize>) + Sync + 'static) =
            unsafe { std::mem::transmute(f as *const (dyn Fn(usize, Range<usize>) + Sync + '_)) };
        let state = Arc::new(LoopState {
            body: LoopBody(body),
            items,
            chunk_size,
            chunks,
            next: AtomicUsize::new(0),
            finished: AtomicUsize::new(0),
            all_done: Mutex::new(false),
            done: Condvar::new(),
            panic: Mutex::new(None),
        });
        for _ in 0..(chunks - 1).min(self.workers()) {
            let state = Arc::clone(&state);
            self.spawn(Priority::Frame, move || state.run_chunks());
        }
        state.run_chunks();
        let mut all_done = state.all_done.lock().unwrap_or_else(|p| p.into_inner());
        while !*all_done {
            all_done = state.done.wait(all_done).unwrap_or_else(|p| p.into_inner());
        }
        drop(all_done);
        let payload = state.panic.lock().unwrap_or_else(|p| p.into_inner()).take();
        if let Some(payload) = payload {
            panic::resume_unwind(payload);
        }
    }

    /// Runs `f(i)` for every `i` in `0..count`, split into about four chunks
    /// per thread so uneven items even out.
    pub fn parallel_for<F>(&self, count: usize, f: F)
    where
        F: Fn(usize) + Sync,
    {
        let chunk_size = count.div_ceil(self.parallelism() * 4).max(1);
        self.for_each_chunk(count, chunk_size, |_, range| range.for_each(&f));
    }
}

impl Drop for Pool {
    fn drop(&mut self) {
        self.shared.lock().shutting_down = true;
        self.shared.work_ready.notify_all();
        for thread in self.threads.drain(..) {
            let _ = thread.join();
        }
    }
}

/// The result of [`Pool::spawn_task`], once it has run.
pub struct Pending<T> {
    slot: Arc<Slot<T>>,
}

struct Slot<T> {
    result: Mutex<Option<thread::Result<T>>>,
    ready: Condvar,
}

impl<T> Pending<T> {
    /// The result if the task has finished (`Err` holds its panic).
    pub fn try_take(&mut self) -> Option<thread::Result<T>> {
        self.slot
            .result
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .take()
    }

    /// Blocks until the task has finished.
    pub fn wait(self) -> thread::Result<T> {
        let mut result = self.slot.result.lock().unwrap_or_else(|p| p.into_inner());
        loop {
            if let Some(done) = result.take() {
                return done;
            }
            result = self
                .slot
                .ready
                .wait(result)
                .unwrap_or_else(|p| p.into_inner());
        }
    }
}

/// A loop body shared with the helper jobs (see the SAFETY note in
/// [`Pool::for_each_chunk`]).
struct LoopBody(*const (dyn Fn(usize, Range<usize>) + Sync + 'static));
// SAFETY: the body is `Sync` (it is called from several threads at once) and
// only used while the loop that owns it waits.
unsafe impl Send for LoopBody {}
unsafe impl Sync for LoopBody {}

struct LoopState {
    body: LoopBody,
    items: usize,
    chunk_size: usize,
    chunks: usize,
    /// The next chunk to claim.
    next: AtomicUsize,
    /// Chunks that have finished running.
    finished: AtomicUsize,
    all_done: Mutex<bool>,
    done: Condvar,
    /// The first chunk panic, raised again by the loop's caller.
    panic: Mutex<Option<Box<dyn Any + Send>>>,
}

impl LoopState {
    fn run_chunks(&self) {
        loop {
            let chunk = self.next.fetch_add(1, Ordering::Relaxed);
            if chunk >= self.chunks {
                return;
            }
            let start = chunk * self.chunk_size;
            let range = start..(start + self.chunk_size).min(self.items);
            // SAFETY: this chunk is claimed and not yet finished, so the loop's
            // caller is still waiting and the body is alive.
            let body = unsafe { &*self.body.0 };
            if let Err(payload) = panic::catch_unwind(AssertUnwindSafe(|| body(chunk, range))) {
                let mut first = self.panic.lock().unwrap_or_else(|p| p.into_inner());
                first.get_or_insert(payload);
            }
            // AcqRel: the last chunk to finish sees every chunk's writes, and
            // hands them to the caller through `all_done`'s lock.
            if self.finished.fetch_add(1, Ordering::AcqRel) + 1 == self.chunks {
                *self.all_done.lock().unwrap_or_else(|p| p.into_inner()) = true;
                self.done.notify_all();
            }
        }
    }
}
