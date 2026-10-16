// cod3 - what this machine has, read once at start, so the port can size
// itself to it (owner, 2026-10-13: "the game needs to scale to what the
// machine actually has. I want this to be in-engine").
//
// Read from the OS, never guessed from the CPU's name:
//   - logical CPUs, and the ones this process may actually use (its affinity
//     mask, and on Linux a container's CPU quota): a game started with
//     `taskset`, in a container or by a launcher that limits it gets less
//     than the machine has;
//   - physical cores (logical CPUs sharing a core = SMT siblings);
//   - groups of cores sharing a last-level (L3) cache: the two halves of a
//     Ryzen 9 5900X are two groups. Threads that trade data every frame go
//     faster inside one group;
//   - efficiency cores (Intel hybrid CPUs), which the frame's own threads
//     should avoid;
//   - memory: total and available now;
//   - whether the Windows build runs under Wine or Proton.
//
// The session header in the log and `cod3_report` show it (game/diagnostics).
// ThreadBudget turns it into the sizes from dev/agents/hw-scaling/PLAN.md
// section 5; for now it is shown, not yet applied.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cod3::machine {

struct Profile {
  std::string os;         // "Linux 6.8.0 x86_64", "Windows 10.0.22631", "Windows (Wine 9.0)"
  std::string cpu_model;  // as the OS reports it; empty if it doesn't
  bool under_wine = false;

  int logical_cpus = 0;   // online logical CPUs in the machine
  // Logical CPUs this process may run on (affinity mask), and how many of
  // them its CPU quota covers (0 = no quota; e.g. 3.5 = a container's limit).
  std::vector<int> allowed_cpus;
  double cpu_quota = 0;

  // Among the allowed CPUs:
  std::vector<std::vector<int>> cores;      // one entry per physical core: its logical CPUs
  std::vector<std::vector<int>> l3_groups;  // one entry per shared L3: its logical CPUs
  std::vector<int> efficiency_cpus;         // E-core logical CPUs (hybrid CPUs only)

  uint64_t ram_total_bytes = 0;
  uint64_t ram_available_bytes = 0;  // at start

  // How many logical CPUs the game can count on: the allowed ones, capped by
  // the quota (rounded down, at least 1).
  int UsableCpus() const;
  int PhysicalCores() const { return int(cores.size()); }

  // One line, for the session header.
  std::string Summary() const;
  // Several lines, for the report: everything above plus the core and cache
  // groups.
  std::string Describe() const;
};

// Read on first use (thread-safe); never changes afterwards.
const Profile& Get();

// What the plan (dev/agents/hw-scaling/PLAN.md section 5) would give each
// kind of work on this machine. Shown in the report; nothing uses it yet.
struct ThreadBudget {
  // The threads every frame passes through (game main thread, render job,
  // GPU emulation, presenting) each get a physical core of their own.
  bool frame_threads_own_cores = false;
  int draw_workers = 0;       // threads building draws beside the GPU thread
  int pool_threads = 1;       // shared pool: shaders, pipelines, copies, files
  bool spinning_allowed = false;  // busy-waiting only with a core to spare
  std::string Describe() const;
};
ThreadBudget Budget(const Profile& profile);

}  // namespace cod3::machine
