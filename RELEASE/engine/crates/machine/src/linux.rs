//! Reading the machine on Linux, from files the kernel publishes, so no
//! system call needs `unsafe`:
//!
//! - `/sys/devices/system/cpu/online`, and per CPU `topology/core_cpus_list`
//!   (older kernels: `thread_siblings_list`) and `cache/index*/{level,
//!   shared_cpu_list}` (the sysfs CPU ABI);
//! - `Cpus_allowed_list` in `/proc/self/status` (proc(5)): the affinity mask,
//!   as `sched_getaffinity` gives it;
//! - the cgroup v2 `cpu.max` of this process's cgroup and its parents (a
//!   container's CPU limit);
//! - `/sys/bus/event_source/devices/cpu_atom/cpus` (perf's
//!   `intel-hybrid.txt`): an Intel hybrid CPU's E-cores;
//! - `/proc/cpuinfo` (`model name`), `/proc/meminfo` (`MemTotal`,
//!   `MemAvailable`), `/proc/sys/kernel/{ostype,osrelease}`.

use std::collections::BTreeSet;
use std::fs;

use crate::{parse_cpu_list, restrict, Profile};

fn first_line(path: &str) -> String {
    fs::read_to_string(path)
        .ok()
        .and_then(|text| text.lines().next().map(str::to_string))
        .unwrap_or_default()
}

/// The cgroup v2 CPU quota of this process, in CPUs (0 = none). `max 100000`
/// is no limit; `350000 100000` is 3.5 CPUs. The tightest limit on the
/// process's cgroup or any parent wins.
fn cgroup_quota() -> f64 {
    let Ok(cgroups) = fs::read_to_string("/proc/self/cgroup") else {
        return 0.0;
    };
    let Some(path) = cgroups.lines().rev().find_map(|l| l.strip_prefix("0::")) else {
        return 0.0;
    };
    if path.contains("..") {
        return 0.0;
    }
    let mut quota = 0.0f64;
    let mut current = path.trim_end_matches('/').to_string();
    loop {
        let text = first_line(&format!("/sys/fs/cgroup{current}/cpu.max"));
        let mut fields = text.split_whitespace();
        if let (Some(max), Some(period)) = (fields.next(), fields.next()) {
            if let (Ok(max), Ok(period)) = (max.parse::<f64>(), period.parse::<f64>()) {
                if max > 0.0 && period > 0.0 {
                    let q = max / period;
                    if quota == 0.0 || q < quota {
                        quota = q;
                    }
                }
            }
        }
        match current.rfind('/') {
            Some(slash) if !current.is_empty() => current.truncate(slash),
            _ => break,
        }
    }
    quota
}

pub(crate) fn read() -> Profile {
    let mut p = Profile {
        os: format!(
            "{} {} {}",
            first_line("/proc/sys/kernel/ostype"),
            first_line("/proc/sys/kernel/osrelease"),
            std::env::consts::ARCH
        ),
        ..Profile::default()
    };

    if let Ok(cpuinfo) = fs::read_to_string("/proc/cpuinfo") {
        if let Some(model) = cpuinfo
            .lines()
            .find(|l| l.starts_with("model name"))
            .and_then(|l| l.split_once(':'))
        {
            p.cpu_model = model.1.trim().to_string();
        }
    }

    let online = parse_cpu_list(&first_line("/sys/devices/system/cpu/online"));
    p.logical_cpus = online.len();

    let mut allowed: BTreeSet<usize> = fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|status| {
            status
                .lines()
                .find_map(|l| l.strip_prefix("Cpus_allowed_list:").map(parse_cpu_list))
        })
        .unwrap_or_default()
        .into_iter()
        .filter(|cpu| online.is_empty() || online.contains(cpu))
        .collect();
    if allowed.is_empty() {
        allowed = online.iter().copied().collect();
    }
    p.allowed_cpus = allowed.iter().copied().collect();
    p.cpu_quota = cgroup_quota();

    let mut cores = Vec::new();
    let mut caches = Vec::new();
    for &cpu in &online {
        let base = format!("/sys/devices/system/cpu/cpu{cpu}");
        let mut siblings = first_line(&format!("{base}/topology/core_cpus_list"));
        if siblings.is_empty() {
            siblings = first_line(&format!("{base}/topology/thread_siblings_list"));
        }
        let core = parse_cpu_list(&siblings);
        cores.push(if core.is_empty() { vec![cpu] } else { core });
        // The highest cache level present (L3 on desktop CPUs).
        let mut best_level = 0;
        let mut best_list = String::new();
        for index in 0..8 {
            let cache = format!("{base}/cache/index{index}");
            let Ok(level) = first_line(&format!("{cache}/level")).trim().parse::<u32>() else {
                continue;
            };
            if level > best_level {
                best_level = level;
                best_list = first_line(&format!("{cache}/shared_cpu_list"));
            }
        }
        if best_level >= 2 && !best_list.is_empty() {
            caches.push(parse_cpu_list(&best_list));
        }
    }
    p.cores = restrict(&cores, &allowed);
    p.l3_groups = restrict(&caches, &allowed);

    // Intel hybrid CPUs list their E-cores in the "cpu_atom" event source;
    // /sys/devices/cpu_atom is the same directory on most kernels.
    let mut atom = first_line("/sys/bus/event_source/devices/cpu_atom/cpus");
    if atom.is_empty() {
        atom = first_line("/sys/devices/cpu_atom/cpus");
    }
    p.efficiency_cpus = parse_cpu_list(&atom)
        .into_iter()
        .filter(|cpu| allowed.contains(cpu))
        .collect();

    if let Ok(meminfo) = fs::read_to_string("/proc/meminfo") {
        for line in meminfo.lines() {
            let mut fields = line.split_whitespace();
            let (Some(key), Some(kib)) = (fields.next(), fields.next()) else {
                continue;
            };
            let Ok(kib) = kib.parse::<u64>() else {
                continue;
            };
            match key {
                "MemTotal:" => p.ram_total_bytes = kib * 1024,
                "MemAvailable:" => p.ram_available_bytes = kib * 1024,
                _ => {}
            }
        }
    }
    p
}

#[cfg(test)]
mod tests {
    #[test]
    fn affinity_matches_std() {
        // std's available_parallelism also reads the affinity mask (and the
        // cgroup quota), so the two can't disagree by more than the quota.
        let p = super::read();
        let std_count = std::thread::available_parallelism().map_or(1, |n| n.get());
        assert!(
            p.allowed_cpus.len() >= std_count,
            "{} < {std_count}",
            p.allowed_cpus.len()
        );
        assert!(p.usable_cpus() <= p.allowed_cpus.len());
    }
}
