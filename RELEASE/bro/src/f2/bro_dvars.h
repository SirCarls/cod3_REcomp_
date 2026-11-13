// [f2-menus] Big Red One's own cvars on the F2 window's pages: which page and section each one is on, its label, its
// kind and range, and a plain line about it. Every cvar the game makes is on the DVARS page too, in a group
// (GroupOf), whether or not a page has a row for it. The full list, with the program each one comes from:
// dev/agents/f2-menus/bro/DVARS.md.
#pragma once

#include <cstddef>

namespace bro_f2 {

enum class Kind { kCheck, kInt, kFloat, kText };

struct DvarRow {
  const char* page;     // "VIDEO", "GAME", "CONTROLS", "INTERFACE", "PERFORMANCE"
  const char* section;  // the heading it is under on that page
  const char* name;     // the game's cvar
  const char* label;
  Kind kind;
  double min, max;
  const char* help;
};

const DvarRow* DvarRows(size_t* count);
const DvarRow* FindDvarRow(const char* name);

// The DVARS page's groups, in the order they are shown; GroupOf gives a cvar's index into them.
const char* const* DvarGroups(size_t* count);
size_t GroupOf(const char* name);

}  // namespace bro_f2
