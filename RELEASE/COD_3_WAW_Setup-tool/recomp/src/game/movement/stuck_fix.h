// cod3 / cod3mp - the levitation fix: the game's "stuck fix" frees a player from the world only, never from
// another character.
//
// [acct2-ocr] 2026-11-05, owner 20:28: "could we change the MP logic to be more robust?", "a toggle will have to
// exist and be server protected"; 20:36: "perhaps look at how the other games handle pvp collision".
//
// The glitch (dev/agents/acct2-ocr/NOTES.md, 2026-11-05): when a player pushes the stick and the move leaves the
// origin where it was, the game "unsticks" them. In cod3mp that is 0x824A13F0 (from ClientThink_real): first it
// pushes the player out of the other players (0x8249AD48, tracemask CONTENTS_PLAYER only, radius 15.1, a little
// wider than a player's 15), then out of the world (0x8249F070, CONTENTS_PLAYER cleared). A standing player who
// walks into a prone one at a wall is pushed into the wall by the first pass and lifted out of it by the second,
// 12.11 at a time; the prone one is lifted the same way, and the two climb each other: from 9 to 396 units in about
// 8 seconds (the owner's run, 2026-11-05 20:21: the wall pass lifted them 795 and 77 units in all).
//
// How the others do it: Call of Duty 4 (KisakCOD's g_active_mp.cpp and bg_pmove.cpp; World at War is the same
// engine) has no unstick. Players are never moved out of each other: a move that starts inside a character ignores
// characters for the rest of that Pmove (PM_playerTrace: pm->tracemask &= ~MASK_CHARACTER), and ClientEndFrame's
// StuckInClient pushes overlapping living players apart sideways with velocity, never up. Call of Duty 3 has the
// same PM_playerTrace rule (cod3mp 0x824A1828, at 0x824A1AC8).
//
// The fix, while it is on: cod3mp skips the player pass (stuck_fix_mp.cpp); the wall pass stays as on the console.
// The campaign has one player; its unstick (0x82483C20, called by ClientThink_real and by Pmove) runs with the
// characters (players and AI: 0x0200C000, its PM_playerTrace test, as CoD4's MASK_CHARACTER) cleared from the
// pmove_t's tracemask (+ 100) for the call, so it frees the player from walls only (CharactersLeftOut below).
//
// The toggle, the same in both programs: levitation_fix (1 on, the default; 0 the game's own), a
// server_sided_protected dvar (game/protected_dvars.h), the row "Levitation fix" on F2 > Game > Server, which only
// the host sees. cod3mp reads it through protected_dvars::Get too (stuck_fix_mp.cpp; owner 21:37: "review the f2
// menu for these settings and confirm it works right": until then cod3mp had its own per-player
// cod3mp_levitation_fix, and the F2 row changed nothing in a match). Each console runs the unstick for its own
// players (system link test acct2-ocr-sl-a). On a guest, Get gives the server's value once one has arrived, else
// the match default (1, on), never the guest's own value: a guest can't turn the fix off. [acct2-ocr] 2026-11-07:
// cod3mp doesn't know a match's host yet (game/mp/session.cpp: IsHost() is false during any match), so every console
// is locked in a match, the host's too, and the fix is on in every match (split screen and system link: runs
// acct2-ocr-lev7-off, acct2-ocr-sl-b-A/B, own value 0 -> "levitation_fix 1, the server's", the player push skipped).
// A 0 applies in the campaign; in multiplayer once session.cpp knows the host and the ServerInfoPairs glue sends it
// (also wanted by cod3mp_movement_rule).

#pragma once

#include <cstdint>

#include "game/guest.h"

namespace cod3::movement::stuck_fix {

inline constexpr uint32_t kPmTracemask = 100;           // pmove_t + 100, both programs
inline constexpr uint32_t kMpCharacters = 0x02000000;   // CONTENTS_PLAYER
inline constexpr uint32_t kSpCharacters = 0x0200C000;   // CONTENTS_PLAYER | CONTENTS_ACTOR | CONTENTS_FAKE_ACTOR

// For its lifetime, the pmove_t's tracemask without the `characters` bits it had; afterwards exactly those bits
// are set again (anything else the call did to the mask stays). Does nothing when `on` is false, the pointer is
// not a guest address, or the mask already has none of them (inside Pmove after a trace started in a character).
class CharactersLeftOut {
 public:
  CharactersLeftOut(uint8_t* base, uint32_t pm, uint32_t characters, bool on) : base_(base), pm_(pm) {
    if (!on || pm < 0x10000 || (pm & 3) != 0) return;
    const uint32_t mask = guest::ReadU32(base, pm + kPmTracemask);
    cleared_ = mask & characters;
    if (cleared_ != 0) guest::WriteU32(base, pm + kPmTracemask, mask & ~cleared_);
  }
  ~CharactersLeftOut() {
    if (cleared_ == 0) return;
    guest::WriteU32(base_, pm_ + kPmTracemask, guest::ReadU32(base_, pm_ + kPmTracemask) | cleared_);
  }
  CharactersLeftOut(const CharactersLeftOut&) = delete;
  CharactersLeftOut& operator=(const CharactersLeftOut&) = delete;

  bool applied() const { return cleared_ != 0; }

 private:
  uint8_t* base_;
  uint32_t pm_;
  uint32_t cleared_ = 0;
};

// cod3mp: dev probes (levitate_probe_mp.cpp) see each stuck fix call, before (after = false) and after it.
// `applied`: the characters were left out of this call.
using Observer = void (*)(uint8_t* base, uint32_t pm, bool after, bool applied);
void SetObserver(Observer observer);

}  // namespace cod3::movement::stuck_fix
