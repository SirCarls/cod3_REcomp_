// cod3 - the game's Options menus as category pages, in the campaign and in
// multiplayer (owner 2026-10-20 20:51: "maybe a menu re-design is needed. We
// have a lot of settings.", then chose category pages in both programs;
// 20:46: "also the battle action should be under gameplay.").
//
// Pause > OPTIONS lists the categories (GAMEPLAY, CONTROLS, AIM, VIDEO,
// AUDIO, MOVEMENT) and RESTORE DEFAULTS; A on a category shows its page,
// B goes back to the list. This file is the one table of which rows each
// page has, in each program. It calls nothing in the game: each program's
// menu code shows the rows on its own pages (cod3mp: game/mp/options_mp.cpp;
// the campaign: game/menu_fov.cpp). dev/agents/waw-decomp/MENU_REDESIGN.md
// has the plan.
//
// A row is either the game's own option on that page (it keeps its own
// saving: the page's "save changes?" on B covers it) or one of the port's
// settings rows (applied and saved at once, like F2). A category with no
// rows in a program is left out of that program's list.

#pragma once

#include <cstdint>

namespace cod3::options_pages {

enum class Category : uint8_t { kGameplay, kControls, kAim, kVideo, kAudio, kMovement };

// Where a row's value comes from.
enum class Source : uint8_t {
  kNative,         // the game's own option: `index` is the page's entry
  kFov,            // FIELD OF VIEW (index 0) or FOV SCALE (index 1)
  kVideo,          // a row of game/menu_video.h (`index` is its videomenu::Row)
  kBrightness,     // the game's brightness (campaign)
  kBattleActions,  // stick battle actions (campaign; game/qte_actions.h)
  kMovement,       // a row of game/movement/menu_movement.h
  kAim,            // a row of game/menu_aim.h
};

struct Row {
  Source source;
  int8_t index;
};

struct Page {
  Category category;
  const char* button_key;  // its item on the list ("GAMEPLAY")
  const char* title_key;   // the page's title ("OPTIONS / GAMEPLAY")
  const Row* rows;
  int count;
};

// The menus that show these pages.
enum class Menu : uint8_t {
  kMultiplayerPause,  // cod3mp: Pause > OPTIONS (InGameOptionsMenu, full-screen layout)
};

// The pages of `menu`, in the order its list shows them.
int PageCount(Menu menu);
const Page* PageAt(Menu menu, int i);  // nullptr past the end
// The most rows any page of `menu` has.
int MostRows(Menu menu);

// The English text of one of this table's keys (the list items and the page
// titles), or nullptr when `key` isn't one of them. Each program's Localize
// hook asks.
const char* Text(const char* key);

}  // namespace cod3::options_pages
