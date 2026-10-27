// cod3mp: [claude-9b] 2026-10-27 automated tests join a team and pick a class (owner 2026-10-27 04:50: "your
// mp tests aren't actually spawning in. They are getting stuck on the in-game menu, also there needs to be
// splitscreen"). No console command does it in this game: `mr <menu> <response>` reaches the server
// (Cmd_Mr_f 0x82562C00) and sends a "menuresponse" notify nothing waits on; the team and class pages are
// the client's own in-match pages, which call the game directly (dev/agents/claude-9b-think/FINDINGS-MPTEST.md).
// So with cod3_test_autojoin set, each local player's team page (InGameSwitchSides, page 10) and class page
// (WeaponSelectMenu, page 1) are answered with the page's own accept function, as a player pressing A on
// the chosen item would: the team (and the page closed), then the class (the player spawns). Off (empty)
// by default: tests only. Both pages also work for split-screen players 2 to 4.
// [claude-9b] 2026-10-27 cod3_debug_menu_pages: one log line whenever the front end's page, a message box or a
// player's in-match page changes, by its class name, so a scripted run (split screen: main menu, DOWN DOWN A,
// Splitscreen1st, CreateLocalSessionMenu) can be checked from the log (FINDINGS-MPTEST.md part C).

#include <chrono>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/mp/test_autojoin_mp.h"

REXCVAR_DEFINE_STRING(cod3_test_autojoin, "", "cod3",
                      "Tests only: each local player joins a team when the team page opens and picks a class "
                      "when the class page opens: allies, axis, or auto (players 1 and 3 allies, 2 and 4 "
                      "axis); empty: off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_test_autojoin_kit, 0, "cod3",
                     "Tests only: the class cod3_test_autojoin picks, 0 to 6 in the class page's order "
                     "(0: rifleman)")
    .range(0, 6)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_debug_menu_pages, false, "cod3",
                    "Tests: log the front end's page, message boxes and each player's in-match page by name "
                    "whenever one changes")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_8252E2E0);  // CL_Frame (cod3mp), once a frame per local player
REX_EXTERN(InGameSwitchSides__vfunc_36);  // 0x823AEC18, InGameSwitchSides: accept (vtable +0x90)
REX_EXTERN(WeaponSelectMenu__vfunc_36);   // 0x823B01C0, WeaponSelectMenu: accept (vtable +0x90)

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMenuSystems = 0x82C6B2CC;   // + 4 * local player: the in-match menu system
constexpr uint32_t kMenuPages = 4;              // menu system + 4: its page table, by page id
constexpr uint32_t kMenuCurrent = 0x1C;         // menu system + 0x1C: the open page's id
constexpr uint32_t kPageClass = 1;              // "weapon": WeaponSelectMenu
constexpr uint32_t kPageTeam = 10;              // "side_select": InGameSwitchSides
constexpr uint32_t kTeamVtable = 0x82084854;
constexpr uint32_t kClassVtable = 0x82084644;
constexpr uint32_t kPageTeamChoice = 0x110;     // int: 1 axis, 2 allies
constexpr uint32_t kPageClassChoice = 0x22;     // u16: the class index (0: rifleman)
constexpr uint32_t kGentities = 0x82BCC7FC;     // *(it) + 4 * (local player + 1): the player's entity
constexpr uint32_t kEntClient = 0x208;          // the entity's client; client + 4: its team
constexpr uint32_t kPauseFlags = 0x82C73024;    // + local player: 1 while a menu has the player paused
constexpr int kPlayers = 4;
constexpr auto kSettle = std::chrono::milliseconds(500);  // a page open this long is answered

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

struct Player {
  uint32_t seen_page = 0;   // the page found open, and since when
  Clock::time_point since{};
  uint32_t done_page = 0;   // the page last answered (once per opening)
  uint32_t logged_id = 0xFFFFFFFF;
};
Player g_players[kPlayers];

uint32_t TeamFor(int player, const std::string& want) {
  if (want == "allies") return 2;
  if (want == "axis") return 1;
  return (player % 2 == 0) ? 2 : 1;  // auto
}

uint32_t TeamNow(uint8_t* base, int player) {
  const uint32_t table = g::ReadU32(base, kGentities);
  if (!Valid(table)) return 0;
  const uint32_t ent = g::ReadU32(base, table + 4 * (player + 1));
  if (!Valid(ent)) return 0;
  const uint32_t client = g::ReadU32(base, ent + kEntClient);
  return Valid(client) ? g::ReadU32(base, client + 4) : 0;
}

void AutoJoin(PPCContext& ctx, uint8_t* base, const std::string& want) {
  const auto now = Clock::now();
  for (int c = 0; c < kPlayers; ++c) {
    Player& p = g_players[c];
    const uint32_t ms = g::ReadU32(base, kMenuSystems + 4 * c);
    if (!Valid(ms)) continue;
    const uint32_t id = g::ReadU32(base, ms + kMenuCurrent);
    if (id != p.logged_id) {  // the open page as it changes (the first runs check kMenuCurrent)
      p.logged_id = id;
      REXLOG_INFO("cod3mp: test autojoin: player {} page {} (pause flag {})", c + 1, id,
                  g::ReadU8(base, kPauseFlags + c));
    }
    if (id != kPageClass && id != kPageTeam) continue;
    const uint32_t pages = g::ReadU32(base, ms + kMenuPages);
    if (!Valid(pages)) continue;
    const uint32_t page = g::ReadU32(base, pages + 4 * id);
    if (!Valid(page) || g::ReadU32(base, page) != (id == kPageTeam ? kTeamVtable : kClassVtable)) continue;
    if (p.seen_page != page) {  // let it finish opening
      p.seen_page = page;
      p.since = now;
      p.done_page = 0;
      continue;
    }
    if (now - p.since < kSettle || p.done_page == page) continue;
    p.done_page = page;
    const PPCContext saved = ctx;
    ctx.r3.u64 = page;
    if (id == kPageTeam) {
      const uint32_t team = TeamFor(c, want);
      g::WriteU32(base, page + kPageTeamChoice, team);
      InGameSwitchSides__vfunc_36(ctx, base);
      REXLOG_INFO("cod3mp: test autojoin: player {} joined {} (team now {})", c + 1,
                  team == 2 ? "allies" : "axis", TeamNow(base, c));
    } else {
      const int kit = REXCVAR_GET(cod3_test_autojoin_kit);
      g::WriteU16(base, page + kPageClassChoice, static_cast<uint16_t>(kit));
      WeaponSelectMenu__vfunc_36(ctx, base);
      REXLOG_INFO("cod3mp: test autojoin: player {} picked class {} (team {})", c + 1, kit, TeamNow(base, c));
    }
    ctx = saved;
    // The same page object opens again later (a death, a team change): answer it then too.
    p.seen_page = 0;
  }
}

}  // namespace

REX_HOOK_RAW(sub_8252E2E0) {
  const std::string& want = REXCVAR_GET(cod3_test_autojoin);
  if (!want.empty()) AutoJoin(ctx, base, want);
  __imp__sub_8252E2E0(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------------
// [claude-9b] 2026-10-27 The menu page trace (FINDINGS-MPTEST.md C5).
namespace {

constexpr uint32_t kFeManager = 0x82C6B1FC;   // g_FEManager
constexpr uint32_t kFeFrontEnd = 28;          // + 28: the front end (its page stack)
constexpr uint32_t kFePages = 4;              // front end + 4: its pages
constexpr uint32_t kFeCount = 16;             //   + 16: how many
constexpr uint32_t kFeIndex = 28;             //   + 28: the open one
constexpr uint32_t kFeMessageBoxes = 192;     // g_FEManager + 192 + 4 i: message boxes
constexpr uint32_t kFeInMatch = 208;          // g_FEManager + 208 + 4 player: the in-match menu systems
constexpr int kTraceSlots = 1 + 4 + 4;        // front end, message boxes 0-3, players 1-4

// A page object's class name from its RTTI (vtable - 4: the complete object locator; + 12: the type
// descriptor; + 8: ".?AVName@@"); empty if anything on the way isn't a pointer.
std::string ClassName(uint8_t* base, uint32_t object) {
  if (!Valid(object)) return {};
  const uint32_t vt = g::ReadU32(base, object);
  if (!Valid(vt)) return {};
  const uint32_t col = g::ReadU32(base, vt - 4);
  if (!Valid(col)) return {};
  const uint32_t td = g::ReadU32(base, col + 12);
  if (!Valid(td)) return {};
  std::string name = g::ReadString(base, td + 8, 96);
  if (name.rfind(".?AV", 0) == 0) name = name.substr(4);
  if (name.size() > 2 && name.compare(name.size() - 2, 2, "@@") == 0) name.resize(name.size() - 2);
  return name;
}

uint32_t g_trace_seen[kTraceSlots] = {};

void Trace(uint8_t* base, int slot, uint32_t object, const char* what) {
  if (g_trace_seen[slot] == object) return;
  g_trace_seen[slot] = object;
  const std::string name = ClassName(base, object);
  REXLOG_INFO("cod3mp: menu pages: {} {}", what, object && !name.empty() ? name : std::string("(none)"));
}

}  // namespace

namespace cod3::mp_test {

void EachFrame(uint8_t* base) {
  if (!REXCVAR_GET(cod3_debug_menu_pages)) return;
  const uint32_t fe = g::ReadU32(base, kFeManager + kFeFrontEnd);
  uint32_t page = 0;
  if (Valid(fe)) {
    const int32_t index = g::ReadS32(base, fe + kFeIndex);
    const uint32_t count = g::ReadU32(base, fe + kFeCount);
    const uint32_t pages = g::ReadU32(base, fe + kFePages);
    if (index >= 0 && static_cast<uint32_t>(index) < count && count < 64 && Valid(pages)) {
      page = g::ReadU32(base, pages + 4 * static_cast<uint32_t>(index));
    }
  }
  Trace(base, 0, page, "front end page");
  for (int i = 0; i < 4; ++i) {
    static const char* kBox[4] = {"message box 1", "message box 2", "message box 3", "message box 4"};
    Trace(base, 1 + i, g::ReadU32(base, kFeManager + kFeMessageBoxes + 4 * i), kBox[i]);
  }
  for (int c = 0; c < 4; ++c) {
    static const char* kPlayer[4] = {"player 1's in-match menus", "player 2's in-match menus",
                                     "player 3's in-match menus", "player 4's in-match menus"};
    Trace(base, 5 + c, g::ReadU32(base, kFeManager + kFeInMatch + 4 * c), kPlayer[c]);
  }
}

}  // namespace cod3::mp_test
