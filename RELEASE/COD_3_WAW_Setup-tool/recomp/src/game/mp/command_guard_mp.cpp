// cod3mp - [waw-decomp] 2026-11-16 no command reaches the game's command buffer from a map pack's map table or from a
// config file the disc doesn't ship (owner 14:35: "make sure cod3's can't inject scripts for MP").
//
// The rules are in command_guard_rules.h (unit-tested); this file holds the two hooks. Found by
// dev/agents/waw-decomp/recomp/RC2-MPSAFE.md (helper rc2-mpsafe), read in the generated code and the disc's program:
//  - 0x823FD238 starts or restarts a match from its settings: it sets mp_gametype from a fixed table (the mode byte
//    at 0x82680CA8 + 0x59, bounds-checked to 0..7), then builds "%s %s\n" from "map" or "map_restart" and the name
//    0x823914A0 returns for the settings' map id, and hands it to Cbuf_AddText. Message types 0x25 and 0x72 from
//    another console reach it. 0x823914A0 looks the id up in the game's map table (0x825D2240, 114-byte rows) and
//    returns the row's name, or the game's own "NULL" when no row has the id; the rows of a map pack come from that
//    pack's mp/dlmaps.txt. A pack copied in by hand (game/mp/map_packs_mp.cpp mounts every package folder) could
//    give a map the name "mp_x;bind ..." and every console that has the pack would run the rest when the map is
//    picked. The hook below hands the game's "NULL" back for any name that isn't a plain map name.
//  - Cmd_Exec_f (0x8249FE38) runs a config file's lines as console commands, past the port's cheat guard. Com_Init
//    asks for autoexec.cfg, bro.cfg, language.cfg and safemode.cfg at every start; no known disc ships them
//    (c3w/disc_md5), so a file dropped into the game folder would run at start. The hook refuses those four and
//    any name that isn't a plain file name, and during a match lets only the game's own config files run
//    (default.cfg, the controller and stick layouts). The player's own commands go in cod3_autoexec, which runs
//    through the port's console and its cheat guard.
// Addresses: codmp_xenonf.xex SHA-1 907985e2 (USA, Europe), as every cod3mp file today; another disc's build gets
// them from its address table when it is wired (waw-decomp's recompile plan, step 4).

#include "game/mp/command_guard_rules.h"

#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/multiplayer.h"
#include "ui/game_console.h"

REX_EXTERN(__imp__sub_823914A0);  // the map table's name for a map id
REX_EXTERN(__imp__Cmd_Exec_f);    // 0x8249FE38, the console command exec

namespace cod3::mp::command_guard {
namespace {

constexpr uint32_t kNullName = 0x820828F4;       // the game's own "NULL", 0x823914A0's answer for an unknown id
constexpr uint32_t kCmdArgvBuffer = 0x82484E80;  // Cmd_ArgvBuffer(index, buffer, size): one word of the command
constexpr uint32_t kArgBytes = 256;
constexpr size_t kMaxScan = 4096;                // how far a name's end is looked for in guest memory

bool ValidPtr(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u - kMaxScan; }

// Each refused name is logged once a session (a bad map table row would otherwise log at every match start).
bool FirstTime(const std::string& key) {
  static std::mutex mutex;
  static std::set<std::string> told;
  std::lock_guard lock(mutex);
  if (told.size() >= 256) return false;
  return told.insert(key).second;
}

// Printable for the log: at most 64 characters, anything outside plain ASCII as '?'.
std::string ForLog(std::string_view s) {
  std::string out;
  for (const char c : s.substr(0, 64)) out += (c >= 0x20 && c < 0x7F) ? c : '?';
  return out;
}

}  // namespace
}  // namespace cod3::mp::command_guard

// The map table's name for a map id (r3 the id, r4 a count the callers pass; returns the name in r3).
REX_HOOK_RAW(sub_823914A0) {
  namespace cg = cod3::mp::command_guard;
  __imp__sub_823914A0(ctx, base);
  const uint32_t name_at = ctx.r3.u32;
  if (!cg::ValidPtr(name_at)) return;
  const char* host = reinterpret_cast<const char*>(base + name_at);
  const size_t length = strnlen(host, cg::kMaxScan);
  const std::string_view name(host, length);
  if (cg::IsPlainMapName(name)) return;
  // The game's own "unknown map" answer when it is where it should be (this build); else the name's own end, an
  // empty name, which the map command refuses the same way.
  const bool null_here = cod3::guest::ReadString(base, cg::kNullName, 8) == "NULL";
  ctx.r3.u64 = null_here ? cg::kNullName : (length < cg::kMaxScan ? name_at + length : cg::kNullName);
  if (cg::FirstTime("map:" + std::string(name.substr(0, 64)))) {
    REXLOG_WARN("cod3mp: command guard: the map table names a map '{}' ({} characters), which isn't a plain map "
                "name (letters, digits and '_'): a map pack's mp/dlmaps.txt is the likely source; the map is "
                "treated as unknown, so no command can follow its name",
                cg::ForLog(name), length);
  }
}

// exec <file>: runs the file's lines as console commands.
REX_HOOK_RAW(Cmd_Exec_f) {
  namespace cg = cod3::mp::command_guard;
  std::string raw;
  {
    cod3::guest::Call call(ctx, base);
    const uint32_t buffer = call.PushString(std::string(cg::kArgBytes - 1, ' '));
    call.Invoke(cg::kCmdArgvBuffer, {1, buffer, cg::kArgBytes});
    raw = cod3::guest::ReadString(base, buffer, cg::kArgBytes - 1);
  }
  const cg::ExecVerdict verdict = cg::CheckExec(raw, cod3::mp::IsMatchRunning());
  if (verdict == cg::ExecVerdict::kAllow) {
    __imp__Cmd_Exec_f(ctx, base);
    return;
  }
  const std::string shown = cg::ForLog(raw);
  std::string why;
  switch (verdict) {
    case cg::ExecVerdict::kRefuseStartupFile:
      why = "the game looks for it at every start and no disc ships it; put your own commands in cod3_autoexec";
      break;
    case cg::ExecVerdict::kRefusePath:
      why = "only plain file names in the game's config folder";
      break;
    default:
      why = "during a match only the game's own config files run; exec it in the menus";
      break;
  }
  // The startup files are asked for at every start and are normally simply missing: one log line, nothing in the
  // console. The other refusals answer a command someone gave, so the console says why.
  if (verdict == cg::ExecVerdict::kRefuseStartupFile) {
    if (cg::FirstTime("exec:" + shown)) REXLOG_INFO("cod3mp: command guard: exec {} not run: {}", shown, why);
    return;
  }
  if (cg::FirstTime("exec:" + shown + (verdict == cg::ExecVerdict::kRefuseInMatch ? ":match" : "")))
    REXLOG_WARN("cod3mp: command guard: exec {} refused: {}", shown, why);
  cod3::ui::GameConsole::Get().Print(cod3::ui::ConsoleChannel::kError, "^1exec " + shown + ": " + why + ".\n");
}
