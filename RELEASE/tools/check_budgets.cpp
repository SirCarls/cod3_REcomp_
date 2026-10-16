// Checks the ReXGlue port's thread budget (game/machine_profile.cpp) against
// the test machines in engine/crates/machine/budget_cases.tsv, the same table
// the Rust engine's machine crate is tested with, so both engines size
// themselves identically (docs/SCALING.md).
//
// From the project folder:
//   c++ -std=c++20 -I COD_3_WAW_Setup-tool/recomp/src -o check_budgets
//       tools/check_budgets.cpp COD_3_WAW_Setup-tool/recomp/src/game/machine_profile.cpp
// (one command line),
//   ./check_budgets engine/crates/machine/budget_cases.tsv
// (Windows: the same with clang++ or MinGW's g++; MSVC: cl /std:c++20 /EHsc
// /I COD_3_WAW_Setup-tool\recomp\src with the two files.) It also prints this
// machine's profile and budget.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "game/machine_profile.h"

namespace {

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, '\t')) fields.push_back(field);
  return fields;
}

// The same synthetic machine as Profile::synthetic in the Rust crate:
// `allowed` logical CPUs spread evenly over `cores` physical cores.
cod3::machine::Profile Synthetic(int allowed, int cores, double quota) {
  cod3::machine::Profile p;
  if (allowed < 1) allowed = 1;
  if (cores < 1) cores = 1;
  if (cores > allowed) cores = allowed;
  p.logical_cpus = allowed;
  for (int cpu = 0; cpu < allowed; ++cpu) p.allowed_cpus.push_back(cpu);
  p.cores.resize(size_t(cores));
  for (int cpu = 0; cpu < allowed; ++cpu) p.cores[size_t(cpu % cores)].push_back(cpu);
  p.l3_groups.push_back(p.allowed_cpus);
  p.cpu_quota = quota;
  return p;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: check_budgets <budget_cases.tsv>\n");
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "can't read %s\n", argv[1]);
    return 2;
  }
  int checked = 0, failed = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const auto f = SplitTabs(line);
    if (f.size() != 10) {
      std::fprintf(stderr, "bad line (want 10 fields): %s\n", line.c_str());
      return 2;
    }
    const auto p = Synthetic(std::stoi(f[1]), std::stoi(f[3]), std::stod(f[2]));
    const auto b = cod3::machine::Budget(p);
    const bool ok = b.frame_threads_own_cores == (f[4] == "yes") &&
                    b.draw_workers == std::stoi(f[5]) && b.pool_threads == std::stoi(f[6]) &&
                    b.spinning_allowed == (f[7] == "yes");
    ++checked;
    if (!ok) {
      ++failed;
      std::printf("FAIL %s: got %s\n", f[0].c_str(), b.Describe().c_str());
    }
  }
  std::printf("%d test machines, %d differ from the table\n", checked, failed);
  const auto& here = cod3::machine::Get();
  std::printf("this machine: %s\nits budget: %s\n", here.Summary().c_str(),
              cod3::machine::Budget(here).Describe().c_str());
  return failed == 0 && checked > 0 ? 0 : 1;
}
