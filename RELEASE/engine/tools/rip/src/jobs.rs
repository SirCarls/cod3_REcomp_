//! Running jobs on all CPU cores: one per input file (archives) or per item
//! (textures of the HD pack).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

/// Number of worker threads: `requested`, or one per CPU core.
pub fn worker_count(requested: Option<usize>) -> usize {
    requested
        .or_else(|| std::thread::available_parallelism().ok().map(|n| n.get()))
        .unwrap_or(1)
        .max(1)
}

/// All files under `root` (recursively) whose extension is `extension`
/// (compared without case), sorted so runs are repeatable.
pub fn find_files(root: &Path, extension: &str) -> std::io::Result<Vec<PathBuf>> {
    let mut found = Vec::new();
    let mut folders = vec![root.to_path_buf()];
    while let Some(folder) = folders.pop() {
        for entry in std::fs::read_dir(&folder)? {
            let path = entry?.path();
            if path.is_dir() {
                folders.push(path);
            } else if path
                .extension()
                .is_some_and(|e| e.eq_ignore_ascii_case(extension))
            {
                found.push(path);
            }
        }
    }
    found.sort();
    Ok(found)
}

/// Runs `job` on every item with `workers` threads, taking items in order
/// (so callers put the slowest first for an even spread). `done` gets each
/// item and its result as jobs finish, one call at a time, so it can print
/// and count without locking.
pub fn for_each_parallel<T, R, F, D>(items: &[T], workers: usize, job: F, done: D)
where
    T: Sync,
    R: Send,
    F: Fn(&T) -> R + Sync,
    D: FnMut(&T, R) + Send,
{
    let next = AtomicUsize::new(0);
    let done = Mutex::new(done);
    std::thread::scope(|scope| {
        for _ in 0..workers.min(items.len()).max(1) {
            scope.spawn(|| loop {
                let i = next.fetch_add(1, Ordering::Relaxed);
                let Some(item) = items.get(i) else { break };
                let result = job(item);
                let mut done = done.lock().unwrap_or_else(|e| e.into_inner());
                (*done)(item, result);
            });
        }
    });
}

/// Runs `job` on every file with `workers` threads. Each job returns its
/// report text; reports are printed whole so lines from different files
/// don't interleave. Returns the number of jobs that failed.
pub fn run_all<F>(files: &[PathBuf], workers: usize, job: F) -> usize
where
    F: Fn(&Path) -> Result<String, Box<dyn std::error::Error>> + Sync,
{
    let mut failures = 0;
    for_each_parallel(
        files,
        workers,
        |file| job(file).map_err(|e| e.to_string()),
        |file, result| match result {
            Ok(report) => print!("{report}"),
            Err(e) => {
                failures += 1;
                eprintln!("error: {}: {e}", file.display());
            }
        },
    );
    failures
}

/// Names already taken by some thread, so a shared output file (a texture
/// several archives contain) is written once.
#[derive(Default)]
pub struct Claims(Mutex<std::collections::HashSet<String>>);

impl Claims {
    /// True if this call claimed `name` first.
    pub fn claim(&self, name: &str) -> bool {
        self.0
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(name.to_string())
    }

    pub fn contains(&self, name: &str) -> bool {
        self.0
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .contains(name)
    }
}
