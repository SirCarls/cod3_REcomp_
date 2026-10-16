//! A progress signal: a thread that waits for work done elsewhere (the GPU
//! finishing a frame, the streaming thread's next block, a job queue
//! draining) sleeps until that work moves on, instead of checking it over
//! and over.
//!
//! The recompiled game taught this one: its Direct3D waits for the GPU by
//! spinning on memory the GPU writes, which held a CPU core at 100% for most
//! of every GPU-bound frame (`recomp/src/game/gpu_wait.h`). A busy core takes
//! power and boost clock from the cores doing the work, and on a PC with few
//! cores, the core itself. The same shape fixed it there: whoever makes
//! progress calls [`Progress::signal`]; a waiter spins only briefly (when the
//! machine's budget allows spinning at all), then sleeps until the next
//! signal and checks again.
//!
//! Cost: `signal` is one atomic add and one atomic load while nobody waits;
//! a lock and a wake-up while somebody does.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

/// See the module documentation.
pub struct Progress {
    count: AtomicU64,
    waiters: AtomicU32,
    lock: Mutex<()>,
    wake: Condvar,
}

impl Default for Progress {
    fn default() -> Self {
        Self::new()
    }
}

impl Progress {
    pub const fn new() -> Progress {
        Progress {
            count: AtomicU64::new(0),
            waiters: AtomicU32::new(0),
            lock: Mutex::new(()),
            wake: Condvar::new(),
        }
    }

    /// How many times progress has been signaled so far.
    pub fn count(&self) -> u64 {
        self.count.load(Ordering::Acquire)
    }

    /// Something a waiter may be waiting for has just happened (call it after
    /// the change is visible).
    pub fn signal(&self) {
        // SeqCst on both sides (here and in `wait_past`): either the waiter
        // sees the new count before it sleeps, or this sees the waiter and
        // wakes it. Taking the lock makes sure a waiter between its check and
        // its sleep is asleep before the notification.
        self.count.fetch_add(1, Ordering::SeqCst);
        if self.waiters.load(Ordering::SeqCst) == 0 {
            return;
        }
        drop(self.lock.lock().unwrap_or_else(|p| p.into_inner()));
        self.wake.notify_all();
    }

    /// Blocks until the count differs from `seen` or `timeout` passes,
    /// whichever is first; returns the count. Read `seen` (with
    /// [`Progress::count`]) before checking what the wait is for, so a signal
    /// in between is not missed.
    pub fn wait_past(&self, seen: u64, timeout: Duration) -> u64 {
        self.waiters.fetch_add(1, Ordering::SeqCst);
        {
            let guard = self.lock.lock().unwrap_or_else(|p| p.into_inner());
            let _ = self
                .wake
                .wait_timeout_while(guard, timeout, |_| {
                    self.count.load(Ordering::SeqCst) == seen
                })
                .unwrap_or_else(|p| p.into_inner());
        }
        self.waiters.fetch_sub(1, Ordering::Relaxed);
        self.count()
    }

    /// Returns once `done()` holds. Checks first; for up to `spin` keeps
    /// checking (pass zero when the machine's budget doesn't allow spinning:
    /// `machine::EngineBudget::spinning_allowed`); then sleeps until the next
    /// signal, at most `max_sleep` at a time, and checks again.
    pub fn wait_until(&self, spin: Duration, max_sleep: Duration, mut done: impl FnMut() -> bool) {
        let mut seen = self.count();
        if done() {
            return;
        }
        let start = Instant::now();
        while start.elapsed() < spin {
            std::hint::spin_loop();
            seen = self.count();
            if done() {
                return;
            }
        }
        loop {
            let now = self.count();
            if now == seen {
                seen = self.wait_past(seen, max_sleep);
            } else {
                seen = now;
            }
            if done() {
                return;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicBool;
    use std::sync::Arc;
    use std::thread;

    #[test]
    fn wakes_on_signal() {
        let progress = Arc::new(Progress::new());
        let flag = Arc::new(AtomicBool::new(false));
        let (p, f) = (progress.clone(), flag.clone());
        let setter = thread::spawn(move || {
            thread::sleep(Duration::from_millis(20));
            f.store(true, Ordering::Release);
            p.signal();
        });
        let start = Instant::now();
        // A long sleep limit: only the signal can end the wait in time.
        progress.wait_until(Duration::ZERO, Duration::from_secs(10), || {
            flag.load(Ordering::Acquire)
        });
        assert!(start.elapsed() < Duration::from_secs(5));
        setter.join().unwrap();
    }

    #[test]
    fn timeout_without_signal() {
        let progress = Progress::new();
        let start = Instant::now();
        let seen = progress.count();
        assert_eq!(progress.wait_past(seen, Duration::from_millis(5)), seen);
        assert!(start.elapsed() >= Duration::from_millis(4));
    }

    #[test]
    fn no_lost_wakeups() {
        // Ping-pong 2000 times, each side waiting for the other's step with a
        // sleep limit far longer than the test: a lost wake-up would hang it.
        let progress = Arc::new(Progress::new());
        let step = Arc::new(AtomicU64::new(0));
        let (p, s) = (progress.clone(), step.clone());
        let other = thread::spawn(move || {
            for i in 0..2000u64 {
                p.wait_until(Duration::ZERO, Duration::from_secs(30), || {
                    s.load(Ordering::Acquire) == 2 * i + 1
                });
                s.store(2 * i + 2, Ordering::Release);
                p.signal();
            }
        });
        for i in 0..2000u64 {
            step.store(2 * i + 1, Ordering::Release);
            progress.signal();
            progress.wait_until(Duration::ZERO, Duration::from_secs(30), || {
                step.load(Ordering::Acquire) == 2 * i + 2
            });
        }
        other.join().unwrap();
    }
}
