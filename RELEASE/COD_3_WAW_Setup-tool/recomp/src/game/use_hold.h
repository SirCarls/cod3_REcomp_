// cod3 - how long Use is held to pick something up, man a gun or get into a
// vehicle, in both programs (see use_hold.cpp).
//
// [acct2-decomp] 2026-11-05 (owner 08:50: "add the option to reduce or
// completely eliminate the need to "press and hold" with the ability to just
// "press" Please make the hints reflect this change").
//
// The game's Use hold: a press of Use with a weapon or ammo, a mounted gun or
// a vehicle seat in reach starts a hold (the client's useHoldEntity and
// useHoldTime), and the thing is used once the button has been held
// g_useholdtime milliseconds. The dvar is registered as 0; every controller
// layout's cfg (assets/config/xenon_a..d.cfg, mp_xenon_a..d.cfg) sets it to
// 250. The controller's X is +activatereload: let go before the hold ends and
// it reloads instead, which is why the hold is there. The keyboard's Use key
// (F, +activate) never reloads, but is held all the same.
//
// The game reads the value in one place in each program: campaign 0x8254A4F4,
// cod3mp 0x8256B578 (`lwz r11,12(r11)`, the integer of the dvar's vmCvar,
// campaign 0x829B7AB0, cod3mp 0x82B33DF0), then compares it with
// level.time - useHoldTime. A midasm hook on the compare after it
// (Cod3UseHoldTime: config/default.toml, mp/config/codmp.toml) gives the
// game cod3_use_hold_ms instead, when that is 0 or more.
//
// Scripted holds (planting a charge, "Hold [ACTIVATE] to open Main Gates")
// are timed by the level scripts and keep their own time.
//
// The hints (game/button_prompts.cpp): with a hold of 0, a text's
// "Press and hold [USE]" reads "Press [USE]" and the campaign's "[HOLD_USE]"
// word "[VERB_USE]" ("Press" + the button), in the game's own words for
// "Press and hold" and "Press" (Pressed). Each player's own buttons or keys
// are then put in by the prompts as before.
#pragma once

#include <cstdint>
#include <string>

namespace cod3::use_hold {

// The setting: -1 keeps the game's hold (g_useholdtime), else milliseconds
// (0: a press uses at once).
int SettingMs();

// The hold the game uses now: the setting, else the game's own g_useholdtime.
// [acct2-ocr] 2026-11-07 the active local client's (the player whose text is made now; player 1 in the campaign).
int CurrentMs(uint8_t* base);

// [acct2-ocr] 2026-11-07 Per player (owner 23:40: "it's per person"). cod3mp's split-screen players 2 to 4 have their
// own, cod3_use_hold_ms_p2 .. _p4: -1 (the default) = player 1's cod3_use_hold_ms, -2 = the game's own hold, else
// milliseconds. The server's check (cod3mp 0x8256B478, r3 the entity) takes the hold of the local player whose
// entity it is (the local players' entities: ReadU32(ReadU32(0x82BCC7FC) + 4 * (local client + 1)),
// game/movement/program_mp.h); an entity that is none of this console's players gets the game's own hold. Each
// console runs its own players' ClientThink (docs/GAMEPLAY_PATCHES.md 3.4), so in system link each player's own
// setting counts on their own console. `player` is the local client, 0-3.
inline constexpr int kPlayers = 4;
// The hold `player` asks for: -1 the game's own, else milliseconds.
int SettingMsFor(int player);
// The player's own setting as stored: player 0 SettingMs(); players 1-3 -1 (player 1's), -2 (the game's) or ms.
int OwnSettingMs(int player);
// Its setting's name ("cod3_use_hold_ms", "cod3_use_hold_ms_p2" ...), or nullptr past the players.
const char* SettingName(int player);
// What `player`'s Use takes now (CurrentMs for that player).
int CurrentMsFor(uint8_t* base, int player);
// Every player's setting in one text (the prompts lay their texts out again when it changes).
std::string SettingsKey();

// `text` with the Use hold's words made a press: "[HOLD_USE]" -> "[VERB_USE]",
// and `press_and_hold` + " [USE]" -> `press` + " [USE]" (also with a small
// first letter, mid-sentence). The words are the game's (INGAME_PRESS_AND_HOLD
// and INGAME_PRESS in the language shown); English ones are tried too. Call it
// only when the hold is 0 (CurrentMs).
std::string Pressed(const std::string& text, const std::string& press_and_hold, const std::string& press);

}  // namespace cod3::use_hold
