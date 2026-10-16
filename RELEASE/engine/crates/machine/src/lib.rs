//! What this computer has, read once from the operating system, and how many
//! threads each kind of work gets on it.
//!
//! The owner's rule (2026-10-13): "the game needs to scale to what the machine
//! actually has. I want this to be in-engine." Both engines follow it with the
//! same numbers: the ReXGlue port in C++
//! (`COD_3_WAW_Setup-tool/recomp/src/game/machine_profile.cpp`) and the Rust
//! engine through this crate. `budget_cases.tsv` lists test machines and the
//! budgets both must give them; the Rust tests and `tools/check_budgets.cpp`
//! check each side against it. The rules are written out in
//! `docs/SCALING.md`.
//!
//! - [`Profile`]: what the OS reports. Never guessed from the CPU's name: the
//!   CPUs this process may actually use (its affinity mask, and on Linux a
//!   container's CPU quota), physical cores, groups of cores sharing a
//!   last-level cache (the two halves of a Ryzen 9 5900X), efficiency cores
//!   (Intel hybrid CPUs), memory, and whether a Windows build runs under Wine.
//! - [`ThreadBudget`]: the ReXGlue port's plan (its frame passes through four
//!   threads in a row, one of them the SDK's GPU emulation thread).
//! - [`EngineBudget`]: the Rust engine's plan. It has no GPU emulation thread
//!   and no single render thread: the game thread runs the simulation, and
//!   everything else, draw recording included, runs as jobs on one pool sized
//!   here (the `jobs` crate), so rendering spreads over every core the game
//!   may use.
//!
//! Sources (`docs/SOURCES.md`): the Linux sysfs CPU ABI and CPU topology
//! pages, cgroup v2 `cpu.max`, proc(5) `Cpus_allowed_list`, perf's
//! `intel-hybrid.txt`; on Windows `GetLogicalProcessorInformationEx`,
//! `PROCESSOR_RELATIONSHIP`, `GetProcessAffinityMask`, `GlobalMemoryStatusEx`,
//! `RtlGetVersion`, and Wine's `wine_get_version` export.

#![cfg_attr(not(windows), forbid(unsafe_code))]

mod cpu_list;
#[cfg(target_os = "linux")]
mod linux;
#[cfg(windows)]
mod windows;

pub use cpu_list::{join_cpu_list, parse_cpu_list};

use std::collections::BTreeSet;
use std::fmt::Write as _;
use std::sync::OnceLock;

/// What this machine has. Lists of CPUs hold logical CPU numbers as the OS
/// counts them, sorted.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Profile {
    /// "Linux 6.8.0 x86_64", "Windows 10.0.22631", "Windows 10.0.19045 (Wine 9.0)".
    pub os: String,
    /// As the OS reports it; empty when it doesn't.
    pub cpu_model: String,
    /// A Windows build running under Wine or Proton.
    pub under_wine: bool,
    /// Online logical CPUs in the whole machine.
    pub logical_cpus: usize,
    /// The logical CPUs this process may run on (its affinity mask).
    pub allowed_cpus: Vec<usize>,
    /// How many CPUs a container's quota covers (Linux cgroup v2 `cpu.max`);
    /// 0 = no quota.
    pub cpu_quota: f64,
    /// One entry per physical core among the allowed CPUs: its logical CPUs.
    pub cores: Vec<Vec<usize>>,
    /// One entry per last-level (L3) cache among the allowed CPUs.
    pub l3_groups: Vec<Vec<usize>>,
    /// Allowed logical CPUs on efficiency cores (hybrid CPUs only).
    pub efficiency_cpus: Vec<usize>,
    pub ram_total_bytes: u64,
    /// Available when the profile was read.
    pub ram_available_bytes: u64,
}

impl Profile {
    /// How many logical CPUs the game can count on: the allowed ones, capped by
    /// the quota (rounded down, at least 1).
    pub fn usable_cpus(&self) -> usize {
        let mut usable = self.allowed_cpus.len();
        if self.cpu_quota > 0.0 {
            usable = usable.min((self.cpu_quota.floor() as usize).max(1));
        }
        usable.max(1)
    }

    pub fn physical_cores(&self) -> usize {
        self.cores.len()
    }

    /// A machine with `allowed` logical CPUs spread evenly over `physical`
    /// cores and an optional quota: for tests and `budget_cases.tsv`.
    pub fn synthetic(allowed: usize, physical: usize, quota: f64) -> Profile {
        let allowed = allowed.max(1);
        let physical = physical.clamp(1, allowed);
        let cpus: Vec<usize> = (0..allowed).collect();
        let cores = (0..physical)
            .map(|core| {
                cpus.iter()
                    .copied()
                    .filter(|cpu| cpu % physical == core)
                    .collect()
            })
            .collect();
        Profile {
            os: "test".into(),
            logical_cpus: allowed,
            allowed_cpus: cpus.clone(),
            cpu_quota: quota,
            cores,
            l3_groups: vec![cpus],
            ..Profile::default()
        }
    }

    /// One line, for a log.
    pub fn summary(&self) -> String {
        let mut s = String::new();
        let model = if self.cpu_model.is_empty() {
            "CPU"
        } else {
            &self.cpu_model
        };
        let _ = write!(s, "{model}: {} logical CPUs", self.logical_cpus);
        if self.allowed_cpus.len() != self.logical_cpus {
            let _ = write!(s, " ({} allowed)", self.allowed_cpus.len());
        }
        if self.cpu_quota > 0.0 {
            let _ = write!(s, " (quota {})", self.cpu_quota);
        }
        let _ = write!(s, ", {} cores", self.cores.len());
        if self.l3_groups.len() > 1 {
            let _ = write!(s, " in {} cache groups", self.l3_groups.len());
        }
        if !self.efficiency_cpus.is_empty() {
            let _ = write!(s, ", {} on E-cores", self.efficiency_cpus.len());
        }
        let _ = write!(
            s,
            "; RAM {} ({} free)",
            gib(self.ram_total_bytes),
            gib(self.ram_available_bytes)
        );
        if self.under_wine {
            s.push_str("; under Wine/Proton");
        }
        s
    }

    /// Several lines, for a report: everything above plus the core and cache
    /// groups.
    pub fn describe(&self) -> String {
        let mut s = String::new();
        let _ = writeln!(s, "OS: {}", self.os);
        let model = if self.cpu_model.is_empty() {
            "(not reported)"
        } else {
            &self.cpu_model
        };
        let _ = writeln!(s, "CPU: {model}");
        let _ = write!(
            s,
            "Logical CPUs: {}; this process may use {} ({})",
            self.logical_cpus,
            self.allowed_cpus.len(),
            join_cpu_list(&self.allowed_cpus)
        );
        if self.cpu_quota > 0.0 {
            let _ = write!(s, ", CPU quota {}", self.cpu_quota);
        }
        let _ = writeln!(s, "; usable {}", self.usable_cpus());
        let _ = write!(
            s,
            "Physical cores: {} (logical CPUs per core:",
            self.cores.len()
        );
        for core in &self.cores {
            let _ = write!(s, " {}", join_cpu_list(core));
        }
        let _ = writeln!(s, ")");
        let _ = write!(s, "Shared-cache groups: {}", self.l3_groups.len());
        for group in &self.l3_groups {
            let _ = write!(s, " [{}]", join_cpu_list(group));
        }
        let _ = writeln!(s);
        if !self.efficiency_cpus.is_empty() {
            let _ = writeln!(s, "E-cores: {}", join_cpu_list(&self.efficiency_cpus));
        }
        let _ = writeln!(
            s,
            "RAM: {} total, {} available at start",
            gib(self.ram_total_bytes),
            gib(self.ram_available_bytes)
        );
        if self.under_wine {
            let _ = writeln!(s, "Running under Wine/Proton");
        }
        s
    }
}

fn gib(bytes: u64) -> String {
    format!("{:.1} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
}

/// This machine, read on first use (thread-safe); never changes afterwards.
pub fn get() -> &'static Profile {
    static PROFILE: OnceLock<Profile> = OnceLock::new();
    PROFILE.get_or_init(read)
}

/// Reads the machine now. [`get`] keeps the first reading.
pub fn read() -> Profile {
    #[cfg(target_os = "linux")]
    let mut p = linux::read();
    #[cfg(windows)]
    let mut p = windows::read();
    #[cfg(not(any(target_os = "linux", windows)))]
    let mut p = Profile {
        os: std::env::consts::OS.to_string(),
        allowed_cpus: (0..std::thread::available_parallelism().map_or(1, |n| n.get())).collect(),
        ..Profile::default()
    };
    // Never report nothing: one CPU, one core.
    if p.allowed_cpus.is_empty() {
        p.allowed_cpus.push(0);
    }
    p.logical_cpus = p.logical_cpus.max(p.allowed_cpus.len());
    if p.cores.is_empty() {
        p.cores = p.allowed_cpus.iter().map(|&cpu| vec![cpu]).collect();
    }
    p
}

/// Keeps only the allowed CPUs of each group, drops groups left empty, sorts
/// and removes duplicate groups (every CPU of a core reports the same core).
pub(crate) fn restrict(groups: &[Vec<usize>], allowed: &BTreeSet<usize>) -> Vec<Vec<usize>> {
    let unique: BTreeSet<Vec<usize>> = groups
        .iter()
        .map(|group| {
            let kept: BTreeSet<usize> = group
                .iter()
                .copied()
                .filter(|c| allowed.contains(c))
                .collect();
            kept.into_iter().collect::<Vec<usize>>()
        })
        .filter(|group| !group.is_empty())
        .collect();
    unique.into_iter().collect()
}

/// The ReXGlue port's thread plan (`dev/agents/hw-scaling/PLAN.md` section 5;
/// C++: `cod3::machine::Budget`). The same numbers in both languages.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ThreadBudget {
    /// The threads every frame passes through (game main thread, render job,
    /// GPU emulation, presenting) each get a physical core of their own.
    pub frame_threads_own_cores: bool,
    /// Threads building draws beside the GPU emulation thread.
    pub draw_workers: usize,
    /// Shared pool: shaders, pipelines, copies, files.
    pub pool_threads: usize,
    /// Busy-waiting only with a core to spare.
    pub spinning_allowed: bool,
}

/// Logical CPUs left to the OS and other programs: one from 8 up.
fn os_reserve(usable: usize) -> usize {
    if usable >= 8 {
        1
    } else {
        0
    }
}

/// Spinning (waiting by checking in a loop) wastes a core unless there are
/// cores to spare: from 6 usable CPUs, as the SDK's GPU thread already does
/// (patch 0022).
fn spinning_allowed(usable: usize) -> bool {
    usable >= 6
}

pub fn budget(p: &Profile) -> ThreadBudget {
    let usable = p.usable_cpus();
    let physical = p.physical_cores();
    let frame_threads_own_cores = physical >= 4 && usable >= 4;
    let draw_workers = if usable < 8 {
        0
    } else {
        (usable / 6).clamp(1, 8)
    };
    let frame_threads = if frame_threads_own_cores { 4 } else { 2 };
    let pool_threads = usable
        .saturating_sub(frame_threads + draw_workers + os_reserve(usable))
        .max(1);
    ThreadBudget {
        frame_threads_own_cores,
        draw_workers,
        pool_threads,
        spinning_allowed: spinning_allowed(usable),
    }
}

impl ThreadBudget {
    /// The C++ side's wording, for logs.
    pub fn describe(&self) -> String {
        format!(
            "frame threads on their own cores: {}; draw workers {}; shared pool {}; busy-waiting {}",
            if self.frame_threads_own_cores { "yes" } else { "no" },
            self.draw_workers,
            self.pool_threads,
            if self.spinning_allowed { "allowed" } else { "off" }
        )
    }
}

/// The Rust engine's thread plan. One game thread runs the simulation; all
/// other work is jobs on one pool, so nothing in a frame waits on a single
/// render or GPU thread:
///
/// - **frame** jobs: draw recording (each job fills its own command buffer for
///   a slice of a pass, submitted in order afterwards), culling, animation,
///   particles;
/// - **now** jobs: what a frame waits for (a streamed zone's decompression,
///   a pipeline a draw needs);
/// - **background** jobs: shader warm-up, cache writes, HD textures. They
///   never hold more than `background_max` workers, so frame jobs always find
///   free ones.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EngineBudget {
    /// Pool threads: the usable CPUs minus the game thread and the OS reserve.
    pub workers: usize,
    /// At most this many background jobs run at once.
    pub background_max: usize,
    /// Pool threads check for work briefly before they sleep.
    pub spinning_allowed: bool,
}

impl EngineBudget {
    /// How many pieces a frame's parallel loop splits into: every worker plus
    /// the thread that asked, which works too.
    pub fn frame_parallelism(&self) -> usize {
        self.workers + 1
    }

    pub fn describe(&self) -> String {
        format!(
            "game thread + {} pool worker{} (a frame's work splits {} ways); background at most {}; busy-waiting {}",
            self.workers,
            if self.workers == 1 { "" } else { "s" },
            self.frame_parallelism(),
            self.background_max,
            if self.spinning_allowed { "allowed" } else { "off" }
        )
    }
}

pub fn engine_budget(p: &Profile) -> EngineBudget {
    let usable = p.usable_cpus();
    let workers = usable.saturating_sub(1 + os_reserve(usable)).max(1);
    EngineBudget {
        workers,
        background_max: (workers / 2).max(1),
        spinning_allowed: spinning_allowed(usable),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn this_machine_reads_something_sensible() {
        let p = read();
        assert!(p.usable_cpus() >= 1);
        assert!(p.logical_cpus >= p.allowed_cpus.len());
        assert!(!p.cores.is_empty());
        // Every allowed CPU belongs to exactly one core.
        let mut seen = BTreeSet::new();
        for core in &p.cores {
            for &cpu in core {
                assert!(seen.insert(cpu), "CPU {cpu} in two cores");
                assert!(p.allowed_cpus.contains(&cpu));
            }
        }
        assert!(!p.summary().is_empty());
        assert!(p.describe().contains("Logical CPUs"));
    }

    #[test]
    fn get_reads_once() {
        assert!(std::ptr::eq(get(), get()));
    }

    #[test]
    fn quota_caps_usable_cpus() {
        assert_eq!(Profile::synthetic(16, 8, 3.5).usable_cpus(), 3);
        assert_eq!(Profile::synthetic(16, 8, 0.5).usable_cpus(), 1);
        assert_eq!(Profile::synthetic(4, 4, 8.0).usable_cpus(), 4);
    }

    #[test]
    fn restrict_drops_disallowed_and_duplicate_groups() {
        let allowed: BTreeSet<usize> = [0, 1, 2, 3].into_iter().collect();
        let groups = vec![vec![2, 0], vec![0, 2], vec![1, 9], vec![8, 9]];
        assert_eq!(restrict(&groups, &allowed), vec![vec![0, 2], vec![1]]);
    }

    #[test]
    fn engine_has_no_single_render_thread() {
        // Every CPU but the game thread (and the OS reserve) is a worker that
        // can record draws.
        let b = engine_budget(&Profile::synthetic(24, 12, 0.0));
        assert_eq!(b.workers, 22);
        assert_eq!(b.frame_parallelism(), 23);
        assert!(b.background_max < b.workers);
    }
}
