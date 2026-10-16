// cod3 - "CHEATS USED" watermark.
//
// Shown once a cheat that gives an unfair advantage has worked: god,
// notarget, noclip, ufo, give, setviewpos, jumptonode (CoD3's own cheat
// commands, which need sv_cheats), or the player changing a cheat-protected
// dvar (timescale, g_speed, ...). It goes away only when all of these hold:
//   - sv_cheats is back to 0;
//   - every cheat-protected dvar the player changed is back to its default;
//   - god / notarget / noclip / ufo are off (they stay on after sv_cheats 0);
//   - after give / setviewpos / jumptonode, the level has been loaded or
//     restarted since (their effect lasts until then).
//
// How it's detected (addresses in default.xex, see addresses.h `cheats`):
// hooks on the cheat command handlers and on CheatsOk, on Cvar_Set2 calls
// made from the console (force = 0), and on the level start.

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

struct PPCContext;

namespace cod3 {

class CheatWatch {
 public:
  static CheatWatch& Get();

  // --- guest threads (hooks) ------------------------------------------------
  void SetToggle(const char* name, bool on, uint32_t ent);  // god / notarget / noclip / ufo after the command
  void OneShot(const char* name);                  // give / setviewpos / jumptonode worked
  void CheatDvar(const std::string& name, uint32_t cvar, bool at_default);
  void LevelStart();                               // a level loaded or restarted
  void Update(uint8_t* base);                      // every few frames: may clear the watermark
  // Cheats went off (sv_cheats 0, or a level start): every cheat dvar the
  // player changed back to its default, god / notarget / noclip / ufo off.
  // Game main thread.
  void ResetCheats(PPCContext& ctx, uint8_t* base);

  // --- any thread -------------------------------------------------------------
  bool Active() const { return active_.load(std::memory_order_relaxed); }
  std::string Reasons();                           // "god, noclip, timescale"

 private:
  CheatWatch() = default;
  void Trigger(const std::string& why);

  std::mutex mutex_;
  std::set<std::string> toggles_;                  // currently on
  std::set<std::string> one_shots_;                // until the next level start
  std::map<std::string, uint32_t> dvars_;          // changed by the player: name -> cvar_t*
  std::set<std::string> history_;                  // everything used since the watermark came on
  std::atomic<bool> active_{false};
  uint32_t player_ent_ = 0;                        // gentity_t* the toggles were used on (this level)
};

}  // namespace cod3
