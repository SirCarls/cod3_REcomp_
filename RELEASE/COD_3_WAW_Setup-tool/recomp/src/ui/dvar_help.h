// cod3 - one line of help for each of the game's dvars.
//
// Call of Duty 3's dvars are Quake III cvars: they carry no description
// (World at War's carry one each). These lines are the port's own, written
// from what the game does with each dvar (docs/DVARS.md). The table is
// docs/dvars.tsv; tools/gen_dvar_help.py turns it into dvar_help.inc (sorted
// by name, no game text in it). The console shows a dvar's line under its
// value and domain when its name is typed alone, in the completion box (one
// match, or the highlighted one) and for `help <name>`; `help <words>`
// searches the names and the lines.
//
// Nothing here changes a dvar; it is text only. Lookups are a binary search
// over a constant table (no allocation, any thread).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cod3::ui::dvar_help {

// Where the game has the dvar (bits).
enum Program : uint8_t { kCampaign = 1, kMultiplayer = 2 };

struct Entry {
  const char* name;
  const char* category;  // a key: "view", "hud", "r" ... (CategoryTitle)
  uint8_t programs;      // Program bits
  const char* text;      // one line
};

// Case-insensitive; null when the port has no line for `name`.
const Entry* Find(std::string_view name);
// "View and field of view" for "view"; the key itself when unknown.
const char* CategoryTitle(std::string_view key);
// "campaign", "multiplayer" or "campaign and multiplayer".
const char* ProgramsText(uint8_t programs);
// Entries whose name or line contains every word of `words` (spaces between
// words, case-insensitive), in name order.
std::vector<const Entry*> Search(std::string_view words);
size_t Count();

}  // namespace cod3::ui::dvar_help
