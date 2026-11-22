// [waw-decomp] 2026-11-16 Tests for recomp/src/game/mp/command_guard_rules.h (which map names and which exec'd files
// cod3mp lets reach the game's command buffer). Plain C++: tests/test_command_guard_rules.py builds and runs it
// with the system's compiler.
//
//   c++ -std=c++17 -I recomp/src tests/command_guard_rules_test.cpp && ./a.out

#include "game/mp/command_guard_rules.h"

#include <cstdio>
#include <string>

using cod3::mp::command_guard::CheckExec;
using cod3::mp::command_guard::ExecVerdict;
using cod3::mp::command_guard::IsPlainMapName;
using cod3::mp::command_guard::NormalizeExecName;

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAILED: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  // Map names: the disc's and the packs' real names pass.
  for (const char* name : {"mp_merv", "mpmaynne", "rouen", "verdun", "mp_farm", "mpdepot2", "ordnance", "rimling",
                           "MP_Hilltops", "a"})
    Expect(IsPlainMapName(name), name);
  // Anything that could end the map command and start another doesn't.
  for (const char* name : {"", "mp_merv;quit", "mp_merv\nquit", "mp merv", "mp_merv\"", "../mp_merv", "mp-merv",
                           "mp_merv\\", "mp_merv;bind F1 \"devmap rouen\"", "\x01"})
    Expect(!IsPlainMapName(name), name);
  Expect(!IsPlainMapName(std::string(64, 'a')), "64 characters");
  Expect(IsPlainMapName(std::string(63, 'a')), "63 characters");

  // Names as exec opens them.
  Expect(NormalizeExecName("XENON_A") == "xenon_a.cfg", "upper case, no extension");
  Expect(NormalizeExecName("config\\mp_xenon_b.cfg") == "mp_xenon_b.cfg", "config\\ folder");
  Expect(NormalizeExecName("config/ts_def.cfg") == "ts_def.cfg", "config/ folder");
  Expect(NormalizeExecName("../autoexec.cfg").empty(), "parent folder");
  Expect(NormalizeExecName("d:\\autoexec.cfg").empty(), "drive");
  Expect(NormalizeExecName("/etc/passwd").empty(), "absolute path");
  Expect(NormalizeExecName("sub/x.cfg").empty(), "sub folder");
  Expect(NormalizeExecName("a b.cfg").empty(), "space");
  Expect(NormalizeExecName("x.cfg;quit").empty(), "semicolon");
  Expect(NormalizeExecName(".cfg").empty(), "starts with a dot");

  // The startup probes are refused always, in and out of a match, however written.
  for (const char* name : {"autoexec.cfg", "AUTOEXEC", "config\\language.cfg", "bro.cfg", "safemode", "Bro.CFG"}) {
    Expect(CheckExec(name, false) == ExecVerdict::kRefuseStartupFile, name);
    Expect(CheckExec(name, true) == ExecVerdict::kRefuseStartupFile, name);
  }
  // The game's own config files run, in and out of a match.
  for (const char* name : {"default.cfg", "xenon_a.cfg", "xenon_d", "mp_xenon_c.cfg", "ts_def.cfg", "ts_legsp.cfg",
                           "config\\default.cfg"}) {
    Expect(CheckExec(name, false) == ExecVerdict::kAllow, name);
    Expect(CheckExec(name, true) == ExecVerdict::kAllow, name);
  }
  // A player's own file: in the menus yes, in a match no.
  Expect(CheckExec("mybinds.cfg", false) == ExecVerdict::kAllow, "own file, menus");
  Expect(CheckExec("mybinds.cfg", true) == ExecVerdict::kRefuseInMatch, "own file, match");
  Expect(CheckExec("xenon.cfg.txt", true) == ExecVerdict::kRefuseInMatch, "not a .cfg in a match");
  // Paths are refused everywhere; no name is the game's usage line.
  Expect(CheckExec("..\\..\\x.cfg", false) == ExecVerdict::kRefusePath, "path, menus");
  Expect(CheckExec("c:/x.cfg", true) == ExecVerdict::kRefusePath, "path, match");
  Expect(CheckExec("", true) == ExecVerdict::kAllow, "no name");

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("command_guard_rules: all passed\n");
  return 0;
}
