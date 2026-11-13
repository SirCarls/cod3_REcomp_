// cod3mp: [claude-9b-think] 2026-11-02 UNTESTED Tests only: host or join a system link match from the front end
// (cod3_test_syslink host | join; off by default). For the tester's T2 (host + client: two cod3mp copies on one PC,
// SDK patch 0209 for the network side). cod3_test_autojoin only answers the in-match team and class pages.
//
// How: like cod3_test_autojoin, each front end page is answered with the page's own A function (vtable +0x90,
// called as (page, controller 0), the arguments the front end passes), after the page has been open 1 s, with
// the list row the step needs written first. Pages are recognized by their vtables (FINDINGS-SYSLINK.md part 1;
// slots from the RTTI dump), and each call checks the slot holds the expected function, so a different build
// fails with a log line instead of calling something else.
//
//   page (FE id)                  vtable      A (+0x90)   what is written first
//   MPMainMenuXBox                0x8207E95C  0x823D2978  rows +244/+248 = 1/0: SYSTEM LINK (menus_mp.cpp)
//   Splitscreen1st (4)            0x8207EA5C  0x823D1308  nothing: A joins pane 1, A again is ready
//   PlayLanMenu (11)              0x82083D3C  0x82393E28  rows +0xF0/+0xF4: 0 CREATE GAME, 1 FIND GAME
//     ([r2] 2026-11-02: +0xF0 / +0xF4 are its UIListBox's (page + 144) first shown row (+96) and cursor (+100). Its A,
//     PlayLanMenu vfunc 36 = 0x82393E28, is CREATE and FIND both: it adds the two and asks the front end for page 16,
//     the game list, on 1, page 1, the create page, on 0 (and nothing otherwise). ob29's joiner did reach the game
//     list and found the host's game; the create page came after its join: see the game list step.)
//   CreateLanSessionMenu (1)      0x82082F0C  0x823B0980  map combo (+0x11C) + 0x18 (u16) = map index
//   SessionLanListMenu (16)       0x8208423C  +0x68 = 0x823A4310 (join the selected row); rows +0xAC/+0xB0 = 0
//   message box (18)              0x82082CFC  (SEARCHING, JOINING, NO SESSIONS FOUND: waited out)
//
// Host: main menu -> SYSTEM LINK -> A, A on the join page -> CREATE GAME -> the map (cod3_test_syslink_map) -> A:
// the game creates the session (0x823FB048) and loads the map (0x823FD4F8). Join: the same to the system link
// page -> FIND GAME -> the list page searches 5 s (0x823A4248) -> when it lists a game, join row 0 -> JOINING ->
// the map. NO SESSIONS FOUND goes back to the system link page: FIND GAME again until the timeout (start the
// joiner before the host if you like). Done when the client plays in a match (0x82C2DAC8 == 2, session.cpp).
// Every step and every page change is logged ("cod3mp: test syslink: ..."); the end is one line, PASSED or
// FAILED with the reason, and after it the helper does nothing more. One local player (controller 1).
//
// [r2] [claude-9b-think] 2026-11-02 UNTESTED: each page's handlers (vtable slots) are logged when it opens, and its
// items (list box rows or FEMenu entries, by their texts, cursor *) when it opens and whenever they change, so a
// message box's text shows; the message box systems (DialogMenuSystem) are logged the same way. A joiner that
// joined and then lands on the create page fails with "the join didn't complete" and the last box's text.
// [r3] UNTESTED: cod3mp_demonware_log prints the network library's own messages (hook on its log sink).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>

#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/engine.h"
#include "game/guest.h"
#include "game/mp/test_autojoin_mp.h"

REXCVAR_DEFINE_STRING(cod3_test_syslink, "", "cod3",
                      "Tests only: from the front end, host a system link match (host) or join the first one "
                      "found (join); empty: off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_test_syslink_map, "", "cod3",
                      "Tests only: the map cod3_test_syslink host picks, by file name (mp_merv, poissons, "
                      "ederdam, mp_fuel, mpmaynne, rouen, forest, mp_hosta, verdun); empty: the page's own");
// [rexglue-native] 2026-11-09: the create page's game mode, as its combo's index (the page's A, 0x823B0980, reads
// the mode combo's u16 at + 0x18 into the match settings, 0x82680CA8 + 89, and takes 0..8). A joiner plays the
// session's mode, so a match in another mode has to be created in it (hq1: the host's own mp_gametype hq and
// map_restart left the joiner in War). The names of the indexes: the log's create page line and the "mp_gametype
// given initial values" warning show them; rexglue-native's NOTES has the list once read.
REXCVAR_DEFINE_INT32(cod3_test_syslink_mode, -1, "cod3",
                     "Tests only: the game mode cod3_test_syslink host picks, as the create page's index (0-8); "
                     "-1: the page's own")
    .range(-1, 8);
REXCVAR_DEFINE_INT32(cod3_test_syslink_timeout, 120, "cod3",
                     "Tests only: seconds cod3_test_syslink may take to reach a match before it logs FAILED")
    .range(10, 900);
// [claude-9b-think] 2026-11-02 r3 UNTESTED: Demonware's own messages (its connections, joins and sessions).
REXCVAR_DEFINE_BOOL(cod3mp_demonware_log, false, "cod3",
                    "Tests only: log the network library's own messages (connections, join requests and "
                    "replies, sessions) as \"cod3mp: demonware: ...\"; at most 3000 lines a run");

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrontEnd = 0x82C6B218;     // g_FEManager + 28: the front end's menu system
constexpr uint32_t kSysPages = 4;              // + 4: pages by id; + 16: how many
constexpr uint32_t kSysCount = 16;
constexpr uint32_t kSysUnder = 20;             // + 20: the page under an overlay (< 0: none)
constexpr uint32_t kSysCurrent = 28;           // + 28: the page taking input
constexpr uint32_t kVoiceSystemPtr = 0x82A9ACB8;  // menus_mp.cpp: 0 until the voice and session system is up
constexpr uint32_t kClientState = 0x82C2DAC8;  // session.cpp: 2 while the client plays in a match

constexpr uint32_t kMainVt = 0x8207E95C, kMainA = 0x823D2978;
constexpr uint32_t kMainRow = 244, kMainRowOffset = 248, kMainSystemLink = 1;
constexpr uint32_t kJoinVt = 0x8207EA5C, kJoinA = 0x823D1308;
constexpr uint32_t kJoinPaneState = 0x80;      // + 4 pane: 4 joined (0x823C0740), 3 signing in
constexpr uint32_t kJoinPaneReady = 0xB4;      // + pane: 1 ready (0x823B7BD8)
constexpr uint32_t kLanVt = 0x82083D3C, kLanA = 0x82393E28;
// As the page's A, 0x82393E28, reads them ([r2]: its list box's first shown row and cursor, page + 144 + 96 / + 100;
// the sum 1 opens the game list, 0 the create page).
constexpr uint32_t kLanRow = 0xF0, kLanRowOffset = 0xF4;
constexpr uint32_t kCreateVt = 0x82082F0C, kCreateA = 0x823B0980;
constexpr uint32_t kCreatePlayers = 0x114, kCreateMode = 0x118, kCreateMap = 0x11C;  // combos; + 0x18: u16 index
constexpr uint32_t kComboIndex = 0x18;
constexpr uint32_t kListVt = 0x8208423C, kListJoinSlot = 0x68, kListJoin = 0x823A4310;
constexpr uint32_t kListRow = 0xAC, kListRowOffset = 0xB0;  // as 0x823A4310 reads them
constexpr uint32_t kListFound = 0xFC;          // games found (0x823B2388 stores the count)
constexpr uint32_t kListSession0 = 0x14C;      // (row + 0x53) * 4: the game on row 0 (< 0: none)
constexpr uint32_t kBoxVt = 0x82082CFC;
constexpr uint32_t kSlotA = 0x90;

// The create page's maps (0x823B0980 reads entry + 1 for the map's id): 0x72 bytes each, the file name at
// + 0x42, ended by id 0xFF (codmp_xenonf.xex 0x825D2240: mp_merv ... verdun, 9 maps).
constexpr uint32_t kMapTable = 0x825D2240, kMapStride = 0x72, kMapId = 1, kMapFile = 0x42;
constexpr int kMaxMaps = 32;

// [r2] 2026-11-02 What a page shows, logged when it opens and whenever it changes (a message box's text changes
// while it is open). The same fields pc_menu.cpp reads (mpui, pm): pages built on a UIListBox (mpui::kListPages:
// the box inside the page at + offset, its vtable 0x820711E4; +4 shown rows of 16 bytes, each *(+0) cells: +0 == 1
// in use, *(+12) elements, the first the row's FEText; +16 rows of 20 bytes, +16 u8 selectable; +80 last row,
// +84 rows shown, +96 first shown, +100 cursor), or a page of FEMenu entries (+4 the entries, +34 s16 selected,
// +38 s16 how many; an entry: +16 its FEText, +22 u8 disabled). An FEText (vtable 0x820702DC, multi-line
// 0x8207041C) has its String at +28, the characters at +12. The message boxes' own systems (DialogMenuSystem,
// vtable 0x82071614, among FEManager + 192 .. + 220) are watched the same way; their buttons are two FETexts in
// the display at system + 44: + 20 / + 24, or + 36 / + 40 when + 60 is set. Only pointers these fields hold are
// read, never a scan of the page.
struct ListAt {
  uint32_t vtable, offset;
};
constexpr ListAt kListPagesAt[] = {
    {0x8207E95C, 148}, {0x8207B874, 100}, {0x82083D3C, 144}, {0x8208423C, 76},
    {0x8208413C, 88},  {0x82082CFC, 136}, {0x82082E04, 96},
};
constexpr uint32_t kUiListVt = 0x820711E4;
constexpr uint32_t kListShown = 4, kListRows = 16, kListLast = 80, kListVisible = 84, kListTop = 96, kListCursor = 100;
constexpr uint32_t kListRowSize = 20, kListRowSelectable = 16, kListShownSize = 16, kListCellElements = 12;
constexpr uint32_t kTextVt = 0x820702DC, kMultiLineTextVt = 0x8207041C;
constexpr uint32_t kTextString = 28, kStringChars = 12;
constexpr uint32_t kPageItems = 4, kPageSelected = 34, kPageItemCount = 38, kEntryLabel = 16, kEntryDisabled = 22;
constexpr uint32_t kFEManager = 0x82C6B1FC, kFirstOtherSystem = 192, kLastOtherSystem = 220;
constexpr uint32_t kDialogSystemVt = 0x82071614, kDialogDisplay = 44;
constexpr uint32_t kDisplayButtonsA = 20, kDisplayButtonsB = 36, kDisplayLayoutB = 60;
// The page handlers logged with the items: HandleButton's up, down, left (pm::kPageMoveUp / MoveDown / OnLeft),
// then +0x8C (right), A, +0x94 (B), and the game list's join slot.
constexpr uint32_t kHandlerSlots[] = {0x80, 0x84, 0x88, 0x8C, 0x90, 0x94, 0x68};
constexpr const char* kHandlerNames[] = {"up", "down", "left", "+8C", "A", "+94", "+68"};

constexpr auto kSettle = std::chrono::milliseconds(1000);  // a page open this long is answered
constexpr auto kBetween = std::chrono::milliseconds(1000);  // presses at least this far apart
constexpr auto kBoxStuck = std::chrono::seconds(8);         // a message box open longer is pressed (joiner)

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

enum class Role { kNone, kHost, kJoin };
enum class End { kRunning, kPassed, kFailed };

struct State {
  Role role = Role::kNone;
  End end = End::kRunning;
  Clock::time_point start{}, page_since{}, last_press{};
  uint32_t page = 0;         // the page object taking input
  int32_t page_id = -1;
  int join_presses = 0;      // on the join page (A, A)
  int box_presses = 0;
  int finds = 0;             // FIND GAME presses (joiner)
  bool create_pressed = false, join_pressed = false;
  bool joined_once = false;  // [r2] the joiner pressed join on the game list at least once
  Clock::time_point joined_at{};  // [r2] when it last did
  bool join_wait_logged = false;  // [r2] "still JOINING" said for this join
  std::string page_items;    // [r2] the current page's items as last logged
  std::string last_box;      // [r2] the last message box's (page 18's) items
  uint32_t dialog_page[8] = {};       // [r2] the message box systems' pages as last logged
  std::string dialog_items[8];
  bool voice_logged = false;
  std::string step = "start";
};
State g_state;               // the game's main thread only (the queued task)
std::atomic<bool> g_queued{false};
std::atomic<bool> g_active{false};  // g_state.role != kNone, for SyslinkFrame's thread

double Seconds(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

const char* RoleName(Role r) { return r == Role::kHost ? "host" : "join"; }

void Finish(End end, const std::string& why) {
  State& s = g_state;
  s.end = end;
  const double t = Seconds(s.start, Clock::now());
  if (end == End::kPassed) {
    REXLOG_INFO("cod3mp: test syslink: PASSED ({}): {} after {:.1f} s", RoleName(s.role), why, t);
  } else {
    REXLOG_ERROR("cod3mp: test syslink: FAILED ({}): {} after {:.1f} s (step: {}; page {} {:08X})",
                 RoleName(s.role), why, t, s.step, s.page_id, s.page);
  }
}

// How many maps the create page's table holds (9 on the disc).
int MapCount(uint8_t* base) {
  int n = 0;
  while (n < kMaxMaps && g::ReadU8(base, kMapTable + static_cast<uint32_t>(n) * kMapStride + kMapId) != 0xFF) ++n;
  return n;
}

// The file name of the create page's map `i`, or empty past the table's end.
std::string MapFile(uint8_t* base, int i) {
  if (i < 0 || i >= MapCount(base)) return {};
  return g::ReadString(base, kMapTable + static_cast<uint32_t>(i) * kMapStride + kMapFile, 16);
}

// [r2] An FEText's characters (control characters as spaces), or "<vtable ...>" for another element.
std::string TextOf(uint8_t* base, uint32_t text) {
  if (!Valid(text)) return "<none>";
  const uint32_t vt = g::ReadU32(base, text);
  if (vt != kTextVt && vt != kMultiLineTextVt) return fmt::format("<vtable {:08X}>", vt);
  const uint32_t string = g::ReadU32(base, text + kTextString);
  if (!Valid(string)) return "";
  std::string s = g::ReadString(base, string + kStringChars, 80);
  for (char& c : s)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
  return s;
}

// [r2] The page's list box (inside the page), or 0 for a page of FEMenu entries.
uint32_t PageListBox(uint8_t* base, uint32_t page) {
  const uint32_t vt = g::ReadU32(base, page);
  for (const auto& p : kListPagesAt)
    if (p.vtable == vt) return g::ReadU32(base, page + p.offset) == kUiListVt ? page + p.offset : 0;
  return 0;
}

// [r2] What the page shows: its list box's shown rows (the cursor's marked *, rows that can't be selected -) or its
// FEMenu entries (the selected one *, disabled ones -), by their texts.
std::string PageItems(uint8_t* base, uint32_t page) {
  std::string out;
  if (const uint32_t list = PageListBox(base, page)) {
    const int32_t last = g::ReadS32(base, list + kListLast);
    const int32_t top = g::ReadS32(base, list + kListTop);
    const int32_t cursor = g::ReadS32(base, list + kListCursor);
    const int32_t shown = g::ReadS32(base, list + kListVisible);
    out = fmt::format("list box +{}: rows {} (first shown {}, cursor {}, shown {}):", list - page, last + 1, top,
                      cursor, shown);
    const uint32_t records = g::ReadU32(base, list + kListShown);
    const uint32_t rows = g::ReadU32(base, list + kListRows);
    for (int32_t k = 0; Valid(records) && top >= 0 && k < shown && k < 16 && top + k <= last; ++k) {
      const uint32_t cells = g::ReadU32(base, records + kListShownSize * static_cast<uint32_t>(k));
      std::string text = "<no cells>";
      if (Valid(cells) && g::ReadU32(base, cells) == 1) {
        const uint32_t elements = g::ReadU32(base, cells + kListCellElements);
        text = Valid(elements) ? TextOf(base, g::ReadU32(base, elements)) : "<no elements>";
      }
      const bool selectable =
          Valid(rows) && g::ReadU8(base, rows + kListRowSize * static_cast<uint32_t>(top + k) + kListRowSelectable);
      out += fmt::format(" [{}{}{}] '{}'", top + k, k == cursor ? "*" : "", selectable ? "" : "-", text);
    }
    return out;
  }
  const int count = static_cast<int16_t>(g::ReadU16(base, page + kPageItemCount));
  const int selected = static_cast<int16_t>(g::ReadU16(base, page + kPageSelected));
  const uint32_t items = g::ReadU32(base, page + kPageItems);
  out = fmt::format("entries {}:", count);
  for (int i = 0; Valid(items) && i < count && i < 24; ++i) {
    const uint32_t entry = g::ReadU32(base, items + 4 * static_cast<uint32_t>(i));
    if (!Valid(entry)) {
      out += fmt::format(" [{}] <none>", i);
      continue;
    }
    out += fmt::format(" [{}{}{}] '{}'", i, i == selected ? "*" : "", g::ReadU8(base, entry + kEntryDisabled) ? "-" : "",
                       TextOf(base, g::ReadU32(base, entry + kEntryLabel)));
  }
  return out;
}

// [r2] The page's handlers (vtable slots), for the log.
std::string PageHandlers(uint8_t* base, uint32_t page) {
  std::string out;
  for (size_t i = 0; i < std::size(kHandlerSlots); ++i)
    out += fmt::format("{}{} {:08X}", i ? ", " : "", kHandlerNames[i], g::VirtualSlot(base, page, kHandlerSlots[i]));
  return out;
}

// [r2] Logs the front end page's items when they differ from the last logged (and, on a new page, its handlers);
// keeps a message box's for the failure line.
void LogPageItems(uint8_t* base, uint32_t page, bool new_page) {
  State& s = g_state;
  const std::string items = PageItems(base, page);
  if (new_page) {
    REXLOG_INFO("cod3mp: test syslink: page {} handlers: {}", s.page_id, PageHandlers(base, page));
  } else if (items == s.page_items) {
    return;
  }
  s.page_items = items;
  if (g::ReadU32(base, page) == kBoxVt) s.last_box = items;
  REXLOG_INFO("cod3mp: test syslink: page {} {}", s.page_id, items);
}

// [r2] Logs the message box systems' pages (their entries and the two buttons the box draws) as they change.
void LogDialogs(uint8_t* base) {
  State& s = g_state;
  for (uint32_t off = kFirstOtherSystem, n = 0; off <= kLastOtherSystem && n < 8; off += 4, ++n) {
    const uint32_t system = g::ReadU32(base, kFEManager + off);
    if (!Valid(system) || g::ReadU32(base, system) != kDialogSystemVt) continue;
    uint32_t page = 0;
    const int32_t id = g::ReadS32(base, system + kSysCurrent);
    const uint32_t count = g::ReadU32(base, system + kSysCount);
    const uint32_t pages = g::ReadU32(base, system + kSysPages);
    if (id >= 0 && static_cast<uint32_t>(id) < count && count <= 64 && Valid(pages))
      page = g::ReadU32(base, pages + 4 * static_cast<uint32_t>(id));
    if (!Valid(page)) {
      if (s.dialog_page[n]) REXLOG_INFO("cod3mp: test syslink: message box (FEManager + {}) closed", off);
      s.dialog_page[n] = 0;
      s.dialog_items[n].clear();
      continue;
    }
    std::string items = PageItems(base, page);
    const uint32_t display = g::ReadU32(base, system + kDialogDisplay);
    if (Valid(display)) {
      const uint32_t slots = display + (g::ReadU32(base, display + kDisplayLayoutB) ? kDisplayButtonsB : kDisplayButtonsA);
      for (uint32_t b = 0; b < 2; ++b) {
        const uint32_t text = g::ReadU32(base, slots + 4 * b);
        if (Valid(text)) items += fmt::format(" button {} '{}'", b, TextOf(base, text));
      }
    }
    if (page == s.dialog_page[n] && items == s.dialog_items[n]) continue;
    s.dialog_page[n] = page;
    s.dialog_items[n] = items;
    s.last_box = items;
    REXLOG_INFO("cod3mp: test syslink: message box (FEManager + {}) page {} (vtable {:08X}) {}", off, id,
                g::ReadU32(base, page), items);
  }
}

// Calls the page's own function in `slot` when it holds `expect`: (page, controller 0).
bool Press(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t slot, uint32_t expect, const char* what) {
  const uint32_t fn = g::VirtualSlot(base, page, slot);
  if (fn != expect) {
    Finish(End::kFailed, std::string("unexpected function on the page for ") + what);
    return false;
  }
  REXLOG_INFO("cod3mp: test syslink: {} (page {:08X}, function {:08X})", what, page, fn);
  g::Call c(ctx, base);
  c.Invoke(fn, {page, 0});
  g_state.last_press = Clock::now();
  return true;
}

void Step(PPCContext& ctx, uint8_t* base) {
  State& s = g_state;
  const std::string& want = REXCVAR_GET(cod3_test_syslink);
  const Role role = want == "host" ? Role::kHost : want == "join" ? Role::kJoin : Role::kNone;
  if (role != s.role) {  // set, changed or cleared: start again
    if (role != Role::kNone && !want.empty()) {
      s = State{};
      s.role = role;
      s.start = Clock::now();
      REXLOG_INFO("cod3mp: test syslink: start ({}; map '{}'; timeout {} s)", RoleName(role),
                  REXCVAR_GET(cod3_test_syslink_map), REXCVAR_GET(cod3_test_syslink_timeout));
    } else {
      if (s.role != Role::kNone && s.end == End::kRunning) REXLOG_INFO("cod3mp: test syslink: turned off");
      s = State{};
      if (!want.empty()) REXLOG_WARN("cod3mp: test syslink: '{}' isn't host or join; off", want);
    }
    g_active = s.role != Role::kNone;
    return;
  }
  if (s.role == Role::kNone || s.end != End::kRunning) return;
  const auto now = Clock::now();

  if (g::ReadU32(base, kClientState) == 2) {
    Finish(End::kPassed, s.role == Role::kHost ? "hosting: this console plays in the match it created"
                                               : "joined: this console plays in the match it found");
    return;
  }
  if (Seconds(s.start, now) > REXCVAR_GET(cod3_test_syslink_timeout)) {
    Finish(End::kFailed, "timed out");
    return;
  }
  if (g::ReadU32(base, kVoiceSystemPtr) == 0) {  // creating or joining stops the game without it
    s.step = "waiting for the voice and session system";
    return;
  }
  if (!s.voice_logged) {
    s.voice_logged = true;
    REXLOG_INFO("cod3mp: test syslink: the voice and session system is up");
  }

  LogDialogs(base);  // [r2]
  const uint32_t fe = g::ReadU32(base, kFrontEnd);
  if (!Valid(fe)) return;
  const int32_t id = g::ReadS32(base, fe + kSysCurrent);
  const uint32_t count = g::ReadU32(base, fe + kSysCount);
  const uint32_t pages = g::ReadU32(base, fe + kSysPages);
  if (id < 0 || static_cast<uint32_t>(id) >= count || count > 64 || !Valid(pages)) return;
  const uint32_t page = g::ReadU32(base, pages + 4 * static_cast<uint32_t>(id));
  if (!Valid(page)) return;
  if (page != s.page) {
    s.page = page;
    s.page_id = id;
    s.page_since = now;
    REXLOG_INFO("cod3mp: test syslink: front end page {} (vtable {:08X}, under it {})", id,
                g::ReadU32(base, page), g::ReadS32(base, fe + kSysUnder));
    LogPageItems(base, page, true);  // [r2]
    return;
  }
  LogPageItems(base, page, false);  // [r2] only when they changed
  if (now - s.page_since < kSettle || now - s.last_press < kBetween) return;

  const uint32_t vt = g::ReadU32(base, page);
  if (vt == kMainVt) {
    s.step = "main menu";
    g::WriteU32(base, page + kMainRow, kMainSystemLink);
    g::WriteU32(base, page + kMainRowOffset, 0);
    Press(ctx, base, page, kSlotA, kMainA, "main menu: SYSTEM LINK");
  } else if (vt == kJoinVt) {
    s.step = "join page";
    if (s.join_presses >= 6) {
      Finish(End::kFailed, "the join page didn't move on after 6 presses of A");
      return;
    }
    REXLOG_INFO("cod3mp: test syslink: join page: pane 1 state {}, ready {}", g::ReadS32(base, page + kJoinPaneState),
                g::ReadU8(base, page + kJoinPaneReady));
    ++s.join_presses;
    Press(ctx, base, page, kSlotA, kJoinA, "join page: A for controller 1");
  } else if (vt == kLanVt) {
    s.step = "system link page";
    if (s.role == Role::kHost && s.create_pressed) {
      Finish(End::kFailed, "back on the system link page after CREATE GAME (the session wasn't created?)");
      return;
    }
    const uint32_t row = s.role == Role::kHost ? 0 : 1;
    REXLOG_INFO("cod3mp: test syslink: system link page rows were {} + {}; writing {}", g::ReadS32(base, page + kLanRow),
                g::ReadS32(base, page + kLanRowOffset), row);
    g::WriteU32(base, page + kLanRow, row);
    g::WriteU32(base, page + kLanRowOffset, 0);
    if (s.role == Role::kJoin) {
      ++s.finds;
      s.join_pressed = false;
      s.box_presses = 0;
    }
    Press(ctx, base, page, kSlotA, kLanA,
          s.role == Role::kHost ? "system link page: CREATE GAME" : "system link page: FIND GAME");
    if (s.role == Role::kJoin) REXLOG_INFO("cod3mp: test syslink: search {}", s.finds);
  } else if (vt == kCreateVt) {
    s.step = "create page";
    if (s.role != Role::kHost) {
      // [r2] ob29: FIND GAME did open the game list (page 16) and the host's game was found and joined; the JOINING
      // box stayed 214 s, then the game went back to the list, searched again and opened this page by itself.
      if (s.joined_once) {
        Finish(End::kFailed, fmt::format("the join didn't complete: {:.0f} s after join on the game list the game "
                                         "left it for the create page (last message box: {})",
                                         Seconds(s.joined_at, now), s.last_box.empty() ? "none" : s.last_box));
      } else {
        Finish(End::kFailed, "the joiner is on the create page without having joined (FIND GAME opened it?)");
      }
      return;
    }
    if (s.create_pressed) return;  // the map loads from here
    const uint32_t map_combo = g::ReadU32(base, page + kCreateMap);
    const uint32_t mode_combo = g::ReadU32(base, page + kCreateMode);
    const uint32_t players_combo = g::ReadU32(base, page + kCreatePlayers);
    if (!Valid(map_combo)) {
      Finish(End::kFailed, "the create page has no map list");
      return;
    }
    const std::string& map = REXCVAR_GET(cod3_test_syslink_map);
    if (!map.empty()) {
      int found = -1;
      const int maps = MapCount(base);
      for (int i = 0; i < maps; ++i) {
        if (MapFile(base, i) == map) {
          found = i;
          break;
        }
      }
      if (found < 0) {
        Finish(End::kFailed, "unknown map '" + map + "'");
        return;
      }
      g::WriteU16(base, map_combo + kComboIndex, static_cast<uint16_t>(found));
    }
    // [rexglue-native] 2026-11-09 cod3_test_syslink_mode: the mode combo's index, as the map's above.
    const int32_t mode = REXCVAR_GET(cod3_test_syslink_mode);
    if (mode >= 0) {
      if (!Valid(mode_combo)) {
        Finish(End::kFailed, "the create page has no game mode list");
        return;
      }
      g::WriteU16(base, mode_combo + kComboIndex, static_cast<uint16_t>(mode));
    }
    const int map_index = g::ReadU16(base, map_combo + kComboIndex);
    REXLOG_INFO("cod3mp: test syslink: create page: map {} ({}), mode {}, max players choice {}", map_index,
                MapFile(base, map_index), Valid(mode_combo) ? g::ReadU16(base, mode_combo + kComboIndex) : -1,
                Valid(players_combo) ? g::ReadU16(base, players_combo + kComboIndex) : -1);
    if (Press(ctx, base, page, kSlotA, kCreateA, "create page: A (create the session and load the map)"))
      s.create_pressed = true;
  } else if (vt == kListVt) {
    s.step = "game list";
    if (s.role != Role::kJoin) {
      Finish(End::kFailed, "the host is on the game list (CREATE GAME opened it?)");
      return;
    }
    if (s.join_pressed) {
      if (now - s.page_since < std::chrono::seconds(10)) return;
      REXLOG_WARN("cod3mp: test syslink: still on the game list 10 s after joining; joining again");
      s.join_pressed = false;
    }
    const uint32_t found = g::ReadU32(base, page + kListFound);
    const int32_t session = g::ReadS32(base, page + kListSession0);
    if (found == 0 || found > 64 || session < 0) return;  // still searching (or about to say none)
    REXLOG_INFO("cod3mp: test syslink: game list: {} game(s) found; joining row 1 (session {})", found, session);
    g::WriteU32(base, page + kListRow, 0);
    g::WriteU32(base, page + kListRowOffset, 0);
    if (Press(ctx, base, page, kListJoinSlot, kListJoin, "game list: join row 1")) {
      s.join_pressed = true;
      s.joined_once = true;  // [r2]
      s.joined_at = now;
      s.join_wait_logged = false;
    }
  } else if (vt == kBoxVt) {
    s.step = "message box";
    // [r2] ob29's JOINING box stayed 214 s with nothing in either log: say so once at 30 s.
    if (s.role == Role::kJoin && s.join_pressed && !s.join_wait_logged && now - s.joined_at > std::chrono::seconds(30)) {
      s.join_wait_logged = true;
      REXLOG_WARN("cod3mp: test syslink: the join hasn't finished after 30 s (box: {})", s.last_box);
    }
    // SEARCHING closes itself after 5 s; JOINING ends in the map. Only the joiner, between searches, presses A
    // on a box that stays (NO SESSIONS FOUND, if it waits for a button): its next page is the system link page.
    if (s.role != Role::kJoin || s.join_pressed || now - s.page_since < kBoxStuck) return;
    if (s.box_presses >= 3) {
      Finish(End::kFailed, "a message box stayed open after 3 presses of A");
      return;
    }
    ++s.box_presses;
    const uint32_t fn = g::VirtualSlot(base, page, kSlotA);
    Press(ctx, base, page, kSlotA, fn, "message box open 8 s: A");
  }
  // Other pages: wait (the log names them as they open).
}

// [claude-9b-think] r3: Demonware's log sink, 0x821ADB30 (its only caller, the message formatter at
// 0x821ADD48, passes r4..r6 = the place (+0, +4, +8 of the record 0x821ADA48 fills: file, function and line,
// as handleJoinRequest's calls give them), r7 = a header buffer the sink reads from + 3, r8 = the formatted
// message). In a release build the sink may print nothing; this prints what reaches it. Strings are read only
// from valid guest addresses, at most 200 characters; a repeat of the last message is counted, not printed.
std::string GuestText(uint8_t* base, uint32_t addr, size_t max_len) {
  if (addr < 0x10000 || addr >= 0xC0000000u) return {};  // (strings needn't be aligned)
  std::string s = g::ReadString(base, addr, max_len);
  for (char& c : s)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
  return s;
}

void LogDemonware(uint8_t* base, uint32_t file, uint32_t function, uint32_t line, uint32_t header,
                  uint32_t message) {
  static std::mutex mutex;
  static int lines = 0, repeats = 0;
  static std::string last;
  std::lock_guard<std::mutex> lock(mutex);
  if (lines >= 3000) return;
  const std::string text = GuestText(base, message, 200);
  if (text == last) {
    ++repeats;
    return;
  }
  if (repeats) REXLOG_INFO("cod3mp: demonware: (the message above {} more time(s))", repeats);
  repeats = 0;
  last = text;
  ++lines;
  const std::string where = GuestText(base, function, 64);
  REXLOG_INFO("cod3mp: demonware: {}{}{} ({}{}{})", GuestText(base, header + 3, 32),
              header ? " " : "", text, where.empty() ? GuestText(base, file, 64) : where,
              line < 100000 ? ":" : "", line < 100000 ? std::to_string(line) : std::string());
  if (lines == 3000) REXLOG_INFO("cod3mp: demonware: 3000 lines; no more this run");
}

}  // namespace

REX_EXTERN(__imp__sub_821ADB30);
REX_HOOK_RAW(sub_821ADB30) {
  if (REXCVAR_GET(cod3mp_demonware_log))
    LogDemonware(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
  __imp__sub_821ADB30(ctx, base);
}

namespace cod3::mp_test {

void SyslinkFrame(uint8_t* base) {
  (void)base;
  if (REXCVAR_GET(cod3_test_syslink).empty() && !g_active.load()) return;
  if (g_queued.exchange(true)) return;  // one step per game frame at most
  cod3::Engine::Get().QueueGuestTask([](PPCContext& ctx, uint8_t* guest) {
    g_queued = false;
    Step(ctx, guest);
  });
}

}  // namespace cod3::mp_test
