// cod3 - voices played twice: the sound library's check for stuck sounds, in
// both programs (owner 2026-10-23: "Voices will play twice, Often happening
// during in-game cutscenes. It happens when framerate dips I think").
//
// Treyarch's sound library (NSL) gives every playing sound to FMOD and, each
// update, reads the FMOD channel's position. nslFmodUpdate (the campaign's
// 0x8211A250, cod3mp's 0x8212C600) stops a playing, resident, not looping XMA
// sound as finished when that position hasn't moved for more than 32 ms
// (0x825A1800 / 0x825B199C) and more than 4 updates (the word after it):
// the check at 0x8211ABF4 / 0x8212CFEC, reached only when the position is
// the same as at the last update. nslUpdate keeps the clock it compares with
// (milliseconds, 0x8290CA30 / 0x82A898BC) and counts its updates (the word
// after it). On the console's 30 frames a second 4 updates took 133 ms; at a
// PC's frame rates only the 32 ms count, and FMOD's mixer falling behind for
// that long (the frame rate dips with it) had voices stopped in the middle.
// The game's sound events (0x8248A958) and its scenes start a sound that
// stopped while it should still play again, from the beginning: the line is
// heard twice. A sound that really ends calls FMOD's end callback, so this
// check only matters for a sound whose decoder never ends.
//
// The hook runs right before the check (recomp/config/default.toml; cod3mp's
// site comes from recomp/mp/config/campaign_map.tsv): it puts
// cod3_sound_stall_ms in place of the 32 ms and logs a stall the console's
// rule would have stopped.

#include <cstdint>
#include <mutex>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/program.h"

REXCVAR_DEFINE_INT32(cod3_sound_stall_ms, 1000, "cod3/Audio",
                     "How long a playing sound's position may stand still before the game's sound library "
                     "stops it as stuck. The console's 32 ms stopped voices whenever the frame rate dipped, "
                     "and the game started them again from the beginning; 32 is the console's value")
    .range(32, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kStallMs = COD3_ADDR(0x825A1800, 0x825B199C);  // u32, 32 in the game's data
constexpr uint32_t kConsoleStallMs = 32;
constexpr uint32_t kConsoleStallUpdates = 4;                       // the next word, kept
constexpr uint32_t kClock = COD3_ADDR(0x8290CA30, 0x82A898BC);     // nslUpdate's milliseconds
constexpr uint32_t kUpdates = kClock + 4;                          // and its update count
// The voice's FMOD data (r31): the position at the last update (+0x5C), when
// it was last seen moving (+0x60, ms) and at which update (+0x64).
constexpr uint32_t kMovedAt = 0x60, kMovedAtUpdate = 0x64;

std::mutex g_mutex;
std::unordered_set<uint64_t> g_seen;     // (voice, when it last moved): one line per stall
std::unordered_set<uint64_t> g_stopped;
uint64_t g_stalls = 0;

}  // namespace

void Cod3SoundStallCheck(PPCRegister& r31, uint8_t* base) {
  using cod3::guest::ReadU32;
  const uint32_t limit = uint32_t(REXCVAR_GET(cod3_sound_stall_ms));
  if (ReadU32(base, kStallMs) != limit) cod3::guest::WriteU32(base, kStallMs, limit);

  const uint32_t voice = r31.u32;
  const uint32_t moved_at = ReadU32(base, voice + kMovedAt);
  const uint32_t still_ms = ReadU32(base, kClock) - moved_at;
  const uint32_t still_updates = ReadU32(base, kUpdates) - ReadU32(base, voice + kMovedAtUpdate);
  if (still_ms <= kConsoleStallMs || still_updates <= kConsoleStallUpdates || still_ms > 600000) return;
  const uint64_t id = (uint64_t(voice) << 32) | moved_at;
  std::lock_guard lock(g_mutex);
  if (still_ms > limit) {
    // The game stops it now (the check after this hook).
    if (!g_stopped.insert(id).second) return;
    if (g_stopped.size() > 4096) g_stopped.clear();
    REXLOG_INFO("cod3: sound: a playing sound's position stood still for {} ms ({} updates): stopped as stuck "
                "(cod3_sound_stall_ms {})",
                still_ms, still_updates, limit);
    return;
  }
  if (!g_seen.insert(id).second) return;
  if (g_seen.size() > 4096) g_seen.clear();
  ++g_stalls;
  if (g_stalls <= 20 || g_stalls % 100 == 0)
    REXLOG_INFO("cod3: sound: a playing sound's position stood still for {} ms ({} updates): kept playing "
                "(the console would have stopped it; cod3_sound_stall_ms {}); {} so far",
                still_ms, still_updates, limit, g_stalls);
}
