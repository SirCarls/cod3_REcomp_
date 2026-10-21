// cod3 - keyboard and mouse keys that call the game's own actions (see binds.h).
//
// Game side, the same in both programs (default.xex / codmp_xenonf.xex; the
// addresses are addresses.h's addr::keybinds, cod3mp's from
// mp/config/campaign_map.tsv; notes in dev/agents/keybinds/):
//   the pad poll           once before every frame on the game's main thread
//                          (hooked here: sub_82522138 in the campaign,
//                          0x82543340 in cod3mp). It takes button presses
//                          only while in_joystick is on and no front-end
//                          menu blocks the player; releases always pass.
//                          The campaign: byte 0x82A2A17D must be 0 and
//                          *(g_FEManager+32) must not block (+28+cl set and
//                          +12 != 3). cod3mp: g_FEManager+50 must be set,
//                          the client's controller not busy (0x82B3EDB8+pad)
//                          and the client's front-end object
//                          (*(g_FEManager+208+cl*4)) must answer 0 to its
//                          vfunc at +92, and the client must be in a match
//                          (+184 == 2; otherwise its presses go to the menu
//                          keys). cod3mp's poll goes through the local
//                          clients, with each one's kbuttons copied in.
//   cmd_functions          the game's command list: {+0 next, +4 name, +8
//                          type (1 plain, 2 button), +12 func}. Each action's
//                          command is looked up there by name, so both
//                          programs need no table of handler addresses.
//                          Button commands are called (key, time), plain ones
//                          with nothing, as the pad path 0x82464248 does.
//   kbuttons               {down[2], downtime, msec, active, wasPressed}, 24
//                          bytes each: two holders per button (two keys, or a
//                          key and a controller button). cod3mp keeps them
//                          per local client (696 bytes each) and copies the
//                          client's set in around its pad poll and its frame;
//                          the keys do the same for local client 0.
//   keys                   qkey_t keys[local client][256] {+0 down, +4 char*
//                          binding}: the game's own bind table, controller
//                          buttons only (xenon_a..d.cfg)
//   pad keys               X 211, A 212, B 213, Y 214, RT 215, LT 216, RB 217,
//                          LB 218, RS 219, LS 220, D-pad 154-157 (up, down,
//                          left, right), START 27, BACK 9
//   input                  +28 a pad pressed Start, +24 which; the pad poll
//                          reads that pad, else the local client's controller
//   action words           "[HOLD_USE]" ... (table 0x82A8DFF8; button
//                          prompts, campaign only)

#include "game/keybinds/binds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiInputTextState: a box's text changed while it is active

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "game/addresses.h"
#include "game/cheat_guard.h"
#include "game/decomp_trace.h"
#include "game/guest.h"
#include "game/keybinds/model.h"
#include "game/keybinds/sdk_spelling.h"
#include "game/program.h"
#include "game/screenshot.h"
#include "ui/theme.h"

REXCVAR_DEFINE_STRING(cod3_binds_kbm_p1, "", "cod3/Binds",
                      "Keyboard and mouse keys of the game's own actions (fire, aim, use, reload, grenades, "
                      "stance, weapons ...), e.g. fire=MOUSE1;aim=MOUSE2;use=F;reload=R "
                      "(empty = the defaults; F2 > Controls or the console's bind command set it)");
REXCVAR_DEFINE_BOOL(cod3_binds_actions, true, "cod3/Binds",
                    "Keyboard and mouse keys call the game's own actions while you play (fire, aim, use, "
                    "reload ...); false = every key presses the controller buttons of its keybind_* row, as "
                    "before");
REXCVAR_DEFINE_BOOL(cod3_aim_toggle_kbm_p1, false, "cod3/Binds",
                    "The keyboard and mouse aim key: false = aims while held, true = on and off with each "
                    "press (the controller keeps the game's own TOGGLE ADS option)");
REXCVAR_DEFINE_BOOL(cod3_debug_binds, false, "cod3",
                    "Log each key bind action as it reaches the game, and the binoculars, hold breath and "
                    "sprint state (with a screenshot of the game's picture when the binoculars are up)");
REXCVAR_DEFINE_STRING(cod3_test_vehicle_hold, "", "cod3",
                      "Test switch, from the command line only: Vehicle actions (tank_gun, veh_gas ...) held as if "
                      "their keys were down while you drive; empty = none");
REXCVAR_DEFINE_INT32(cod3_binds_version, 0, "cod3",
                     "Which one-time changes of the key binds have run (set by the game)");
REXCVAR_DEFINE_INT32(cod3mp_binds_version, 0, "cod3",
                     "Multiplayer: which one-time changes of the key binds have run (set by the game)");

namespace cod3::binds {

namespace {

using guest::Call;
using guest::ReadF32;
using guest::ReadS32;
using guest::ReadString;
using guest::ReadU32;
using guest::ReadU8;
using guest::WriteU16;
using guest::WriteU32;
using guest::WriteU8;
namespace kb = addr::keybinds;

constexpr int kKeyAux1 = 239;          // AUX1, AUX2: key ids no controller sends
constexpr int kPadX = 211;             // the controller's X (the frag throw-back quirk)
constexpr int32_t kBindsVersion = 3;   // cod3_binds_version after the moves from keybind_*
constexpr uint32_t kTypePlain = 1;     // cmd_function_t +8
constexpr uint32_t kTypeButton = 2;

// A controller input as the game's pad key and as the XInput state.
struct PadInput {
  int key;
  uint16_t button;  // XInput button bit, 0 for a trigger
  uint8_t trigger;  // 1 left, 2 right
  const char* name;
};
constexpr PadInput kPadInputs[] = {
    {220, 0x0040, 0, "LS"},     {219, 0x0080, 0, "RS"},    {211, 0x4000, 0, "X"},
    {212, 0x1000, 0, "A"},      {213, 0x2000, 0, "B"},     {214, 0x8000, 0, "Y"},
    {215, 0, 2, "RT"},          {216, 0, 1, "LT"},         {217, 0x0200, 0, "RB"},
    {218, 0x0100, 0, "LB"},     {154, 0x0001, 0, "D-pad up"}, {155, 0x0002, 0, "D-pad down"},
    {156, 0x0004, 0, "D-pad left"}, {157, 0x0008, 0, "D-pad right"}, {27, 0x0010, 0, "START"},
    {9, 0x0020, 0, "BACK"},
};

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
// A function in the game's image (0x82xxxxxx).
bool InImage(uint32_t f) { return (f >> 24) == 0x82 && (f & 3) == 0; }

bool SameText(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

bool Debug() { return REXCVAR_GET(cod3_debug_binds); }

uint32_t LocalClient(uint8_t* base) {
  const uint32_t cl = ReadU32(base, addr::kActiveLocalClient);
  return cl < 4 ? cl : 0;
}

// The local client the keyboard and mouse play: the campaign's one player.
// [mp-menus] cod3mp (its pad poll runs once per local client): the player on
// controller 0, which the keyboard and mouse share (the SDK feeds them to
// guest user 0 with the first controller), found the way the game finds a
// controller's player (0x82521BD0: the first local client in use, state
// != 0, with that controller); the others keep their own controllers.
// Player 1 when none (the front end; one player on another controller).
uint32_t KbmClient(uint8_t* base) {
  if (!cod3::kMultiplayer || !kb::kLocalClientState) return cod3::kMultiplayer ? 0 : LocalClient(base);
  for (uint32_t cl = 0; cl < 4; ++cl) {
    const uint32_t client = kb::kLocalClients + cl * kb::kLocalClientStride;
    if (ReadU32(base, client + kb::kLocalClientState) != 0 && ReadU32(base, client + kb::kLocalClientPad) == 0)
      return cl;
  }
  return 0;
}

// The game's binding of a controller key ("+binoculars"), "" when none.
std::string Binding(uint8_t* base, uint32_t cl, int pad_key) {
  if (pad_key <= 0 || pad_key > 255) return "";
  const uint32_t text = ReadU32(base, kb::kKeys + cl * kb::kKeysStride + uint32_t(pad_key) * 8 + 4);
  return Valid(text) ? ReadString(base, text, 64) : "";
}

// The game's action words ("[HOLD_USE]" -> USE; the table its hints use, NOTES.md
// (b)) and the command each one stands for. Prompts write the action's key
// instead of the controller button ("Press and hold [F]").
struct GameWord {
  const char* word;
  const char* command;
};
constexpr GameWord kGameWords[] = {
    {"USE", "+activate"},          {"ACTIVATE", "+activate"},
    {"RELOAD", "+reload"},         {"ATTACK", "+attack"},
    {"AIM", "+speed"},             {"TOGGLE_AIM", "+speed"},
    {"MELEE", "+melee"},           {"GRENADE", "+grenadeattack"},
    {"SMOKEGRENADE", "+smokegrenade"}, {"BINOCS", "+binoculars"},
    {"SPRINT", "+sprint"},         {"STANCE_UP", "+gostand"},
    {"STANCE_DOWN", "+stance"},    {"WEAPON_SWAP", "weapnext"},
    {"LEAN_LEFT", "+leanleftswitchnext"}, {"LEAN_RIGHT", "+leanrightswitchnext"},
    // The vehicles' alias words: the Vehicle actions (their command is their id).
    {"COAXIAL", "tank_gun"},       {"ALIGNTURRET", "tank_turret"},
    {"GAS", "veh_gas"},            {"REVERSE", "veh_reverse"},
    {"HANDBRAKE", "veh_handbrake"},
};

// --- the keys (any thread) ---------------------------------------------------------------
std::mutex g_mutex;          // g_set, g_last_text
KbmSet g_set = Defaults();
std::string g_last_text = "\x01";  // the cvar text g_set was read from
std::atomic<uint32_t> g_generation{1};
// KeyCode -> (action + 1) | slot << 8, 0 = no action. Read by the listener:
// the on-foot actions' keys, and the Vehicle actions' (model.h).
std::array<std::atomic<uint16_t>, kKeyCodeCount> g_lookup{};
std::array<std::atomic<uint16_t>, kKeyCodeCount> g_vehicle_lookup{};
std::once_flag g_started;
// The game has the action's command (set by the game thread's lookup): a key
// of an action it hasn't stays the keyboard row's.
std::array<std::atomic<bool>, kActionCount> g_action_ok{};
// The game takes presses for the keyboard's player now (no menu over the
// game; set by the game thread at every pad poll). Outside play the keys are
// the keyboard rows' (keybind_*): the game's menus, the pause menu.
std::atomic<bool> g_in_play{false};
// + 1 when the game thread lets every action go (a menu opened): keys still
// held are pressed again when play goes on (Listener::Update).
std::atomic<uint32_t> g_release_gen{0};

// --- vehicles (any thread) -----------------------------------------------------------------
// The jeep and the tank read their controls themselves, every server frame
// while the player drives: their button aliases (ButtonAlias_Value 0x82467710,
// hooked at the end of this file: the tank's machine gun FireCoax and its
// AlignTurret, the jeep's Gas, Reverse and HandBrake, which the controller
// layouts' "buttonalias" lines put on controller buttons) and the view switch
// (the right stick click's press, read in the camera select 0x82447D48). A key
// that calls a command never reaches those reads (reported 2026-10-20: the
// tank's machine gun in the Polish missions didn't fire with the keyboard), so
// the Vehicle actions (model.h) answer them where the game reads them; no
// controller button is pressed.
//
// While a vehicle reads an alias, the key of the Vehicle action for that alias
// is that action's (KeyAction), and Vehicle: change view's key while it reads
// any; otherwise the same keys keep their on-foot actions. Each alias of each
// context counts on its own (a tank that reads only AlignTurret and FireCoax
// leaves W and S to the movement rows). A read counts for kVehiclePolls pad
// polls in play and kVehicleMs, whichever is longer: menus and the pause stop
// the poll count, so a key pressed right after them still counts for the
// vehicle, and the time covers high frame rates. Play coming back after more
// than 3 s (a level load, a mission's end) clears the poll count.
constexpr int kVehicleContexts = 2;  // the game's alias contexts: 0 Vehicle (the jeep), 1 VehicleTank
constexpr int kAliasIds = 8;         // alias ids answered (0 Gas ... 4 FireCoax; cod3mp reads a 5 too)
constexpr int kVehiclePolls = 15;
constexpr int64_t kVehicleMs = 250;
// Per context * kAliasIds + alias: the last read's countdown and time.
std::array<std::atomic<int>, kVehicleContexts * kAliasIds> g_alias_polls{};
std::array<std::atomic<int64_t>, kVehicleContexts * kAliasIds> g_alias_until_ms{};
// The controller the last read of each context asked for (its alias table row).
std::array<std::atomic<uint32_t>, kVehicleContexts> g_vehicle_pad{};
// The aliases the Vehicle actions hold now, bit = alias id (the game thread
// sets it at every pad poll; ButtonAlias_Value's hook reads it).
std::atomic<uint32_t> g_alias_held{0};
// cod3_test_vehicle_hold's aliases (testing: held as if their keys were down).
std::atomic<uint32_t> g_test_hold{0};
// Vehicle: change view was pressed at this pad poll (0 = no press waiting).
std::atomic<uint32_t> g_view_press_poll{0};
std::array<uint32_t, kVehicleContexts> g_vehicle_was{};  // game thread: ReadingMask at the last pad poll
int64_t g_play_stopped_ms = 0;                            // game thread: when the game last stopped taking presses

const char* VehicleName(int context) { return context == 1 ? "tank" : "vehicle"; }

int64_t SteadyMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// A vehicle of `context` read `alias` lately.
bool AliasReading(int context, int alias) {
  if (context < 0 || context >= kVehicleContexts || alias < 0 || alias >= kAliasIds) return false;
  const size_t i = size_t(context * kAliasIds + alias);
  return g_alias_polls[i].load(std::memory_order_relaxed) > 0 ||
         SteadyMs() < g_alias_until_ms[i].load(std::memory_order_relaxed);
}

// The aliases a vehicle of `context` read lately (bit = alias id).
uint32_t ReadingMask(int context) {
  uint32_t mask = 0;
  for (int a = 0; a < kAliasIds; ++a)
    if (AliasReading(context, a)) mask |= 1u << a;
  return mask;
}

// A Vehicle action counts now: a vehicle reads its alias (Vehicle: change
// view: a vehicle reads any).
bool VehicleActionApplies(Action action) {
  const ActionDef& def = Def(action);
  if (def.group != Group::kVehicle) return false;
  for (int c = 0; c < kVehicleContexts; ++c)
    if (def.alias >= 0 ? AliasReading(c, def.alias) : ReadingMask(c) != 0) return true;
  return false;
}

// The action a key down goes to, as (action + 1) | slot << 8 (0 = none): its
// Vehicle action while that counts, else its on-foot action.
uint16_t KeyAction(KeyCode code) {
  if (code >= kKeyCodeCount) return 0;
  const uint16_t vehicle = g_vehicle_lookup[code].load(std::memory_order_relaxed);
  if (vehicle && VehicleActionApplies(static_cast<Action>((vehicle & 0xFF) - 1))) return vehicle;
  return g_lookup[code].load(std::memory_order_relaxed);
}

// The Vehicle action that answers `alias`; Action::kCount when none.
Action VehicleActionForAlias(int alias) {
  for (int a = 0; a < kActionCount; ++a)
    if (Def(static_cast<Action>(a)).delivery == Delivery::kAlias && Def(static_cast<Action>(a)).alias == alias)
      return static_cast<Action>(a);
  return Action::kCount;
}

// The alias bits of the actions named in `text` ("tank_gun,veh_gas").
uint32_t AliasBits(std::string_view text) {
  uint32_t bits = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string_view::npos) end = text.size();
    const Action a = ActionById(text.substr(start, end - start));
    if (a != Action::kCount && Def(a).delivery == Delivery::kAlias && Def(a).alias >= 0 && Def(a).alias < kAliasIds)
      bits |= 1u << Def(a).alias;
    start = end + 1;
  }
  return bits;
}

// cod3_test_vehicle_hold, from the command line only (left in cod3.toml it
// would hold a vehicle's button in every game).
uint32_t TestHoldBits() {
  const std::string text = rex::cvar::GetFlagByName("cod3_test_vehicle_hold");
  if (text.empty()) return 0;
  if (rex::cvar::GetFlagSource("cod3_test_vehicle_hold") != rex::cvar::Source::kCommandLine) {
    REXLOG_WARN("cod3: cod3_test_vehicle_hold is a command-line test switch; not used");
    return 0;
  }
  const uint32_t bits = AliasBits(text);
  REXLOG_WARN("cod3: cod3_test_vehicle_hold: {} held in vehicles as if their keys were down (a test){}", text,
              bits ? "" : "; none of them is a vehicle's alias action (tank_gun, tank_turret, veh_gas ...)");
  return bits;
}

void Load(std::string_view text) {
  std::vector<std::string> notes;
  const KbmSet set = Parse(text, &notes);
  {
    std::lock_guard lock(g_mutex);
    if (text == g_last_text) return;
    g_last_text = std::string(text);
    g_set = set;
    for (auto& v : g_lookup) v.store(0, std::memory_order_relaxed);
    for (auto& v : g_vehicle_lookup) v.store(0, std::memory_order_relaxed);
    for (int a = 0; a < kActionCount; ++a)
      for (int s = 0; s < kSlots; ++s)
        if (const KeyCode k = set.key[a][s]; k != kNoKey && k < kKeyCodeCount)
          (IsVehicleAction(static_cast<Action>(a)) ? g_vehicle_lookup : g_lookup)[k].store(
              static_cast<uint16_t>((a + 1) | (s << 8)), std::memory_order_relaxed);
  }
  for (const auto& n : notes) REXLOG_WARN("cod3: key binds: {}", n);
  g_generation.fetch_add(1, std::memory_order_release);
  std::string keys;
  for (int a = 0; a < kActionCount; ++a) {
    const std::string k = KeysText(set, static_cast<Action>(a));
    if (k.empty()) continue;
    keys += std::string(keys.empty() ? "" : ", ") + Def(static_cast<Action>(a)).id + " " + k;
  }
  REXLOG_INFO("cod3: key binds: {}", keys.empty() ? "none" : keys);
}

void EnsureStarted() {
  std::call_once(g_started, [] {
    for (auto& ok : g_action_ok) ok.store(true, std::memory_order_relaxed);  // until the game says otherwise
    rex::cvar::RegisterChangeCallback(kSettingName,
                                      [](std::string_view, std::string_view value) { Load(value); });
    Load(rex::cvar::GetFlagByName(kSettingName));
    g_test_hold.store(TestHoldBits(), std::memory_order_relaxed);
  });
}

KbmSet CurrentSet() {
  EnsureStarted();
  std::lock_guard lock(g_mutex);
  return g_set;
}

// Writes the keys to the setting (its change callback loads them).
void Store(const KbmSet& set) {
  rex::cvar::SetFlagByName(kSettingName, Serialize(set));
  Load(rex::cvar::GetFlagByName(kSettingName));
}

// "" when the key can be bound, else why not.
std::string ReservedReason(KeyCode key) {
  if (key == kKeyBacktick) return "` / ~ opens the console";
  if (key == kKeyEscape) return "ESCAPE is the pause menu";
  if (key == kKeyF1 + 1) return "F2 opens the settings window";
  for (const char* name : {"bind_debug_overlay", "bind_settings", "bind_achievements", "bind_console"}) {
    const std::string v = rex::cvar::GetFlagByName(name);
    if (!v.empty() && ParseKey(v) == key) return KeyName(key) + " opens an overlay (" + name + ")";
  }
  if (trace::kEnabled && key == kKeyF1 + 7) return "F8 is the trace build's marker key";
  return "";
}

// The keybind_* settings (the keyboard pressing controller buttons: in the
// game's menus, and in play for keys no action has) and what each button
// does in the campaign's default layout.
struct OldBindName {
  const char* name;
  const char* label;
  bool moved;  // version 1 moved its keys to an action (model.h Migrate)
};
constexpr OldBindName kOldBinds[] = {
    {"keybind_a", "Jump (A)", false},
    {"keybind_b", "Stance (B)", false},
    {"keybind_x", "Use/Reload (X)", false},
    {"keybind_y", "Switch weapon (Y)", false},
    {"keybind_left_trigger", "Aim (LT)", false},
    {"keybind_right_trigger", "Fire (RT)", false},
    {"keybind_left_shoulder", "Smoke grenade (LB)", false},
    {"keybind_right_shoulder", "Grenade (RB)", false},
    {"keybind_lstick_up", "Forward", false},
    {"keybind_lstick_down", "Back", false},
    {"keybind_lstick_left", "Strafe left", false},
    {"keybind_lstick_right", "Strafe right", false},
    {"keybind_lstick_press", "Left stick click (LS)", true},
    {"keybind_rstick_up", "Look up", false},
    {"keybind_rstick_down", "Look down", false},
    {"keybind_rstick_left", "Look left", false},
    {"keybind_rstick_right", "Look right", false},
    {"keybind_rstick_press", "Melee (RS)", false},
    {"keybind_dpad_up", "D-pad up", true},
    {"keybind_dpad_down", "D-pad down", true},
    {"keybind_dpad_left", "D-pad left", true},
    {"keybind_dpad_right", "D-pad right", true},
    {"keybind_back", "Objectives (Back)", false},
    {"keybind_start", "Pause (Start)", false},
    {"keybind_guide", "Guide", false},
};

// The controller row whose keys include `key` (the action takes the key in
// play, the row keeps it in the game's menus), or nullptr.
const OldBindName* ControllerRowWith(KeyCode key) {
  for (const OldBindName& b : kOldBinds) {
    const std::string v = rex::cvar::GetFlagByName(b.name);
    size_t start = 0;
    while (start <= v.size()) {
      size_t end = v.find(',', start);
      if (end == std::string::npos) end = v.size();
      const std::string_view token(v.data() + start, end - start);
      if (!IsCombination(token) && ParseKey(token) == key) return &b;
      start = end + 1;
    }
  }
  return nullptr;
}

// --- UI thread -> game thread ----------------------------------------------------------
enum class Kind : uint8_t { kDown, kUp, kTap };
struct Event {
  uint8_t action;
  uint8_t slot;
  Kind kind;
  uint16_t code;
};
// One producer (the UI thread), one consumer (the game thread).
constexpr uint32_t kRingSize = 128;
std::array<Event, kRingSize> g_ring;
std::atomic<uint32_t> g_ring_head{0}, g_ring_tail{0};
// A release that didn't fit in the ring: one bit per (action, slot), never lost.
constexpr int kSlotCount = kActionCount * kSlots;
std::array<std::atomic<uint32_t>, (kSlotCount + 31) / 32> g_release_overflow{};

void Push(const Event& e) {
  const uint32_t head = g_ring_head.load(std::memory_order_relaxed);
  if (head - g_ring_tail.load(std::memory_order_acquire) >= kRingSize) {
    if (e.kind == Kind::kUp) {
      const int i = e.action * kSlots + e.slot;
      g_release_overflow[i / 32].fetch_or(1u << (i % 32), std::memory_order_release);
    }
    return;  // a press is dropped when full
  }
  g_ring[head % kRingSize] = e;
  g_ring_head.store(head + 1, std::memory_order_release);
}

bool Pop(Event* out) {
  const uint32_t tail = g_ring_tail.load(std::memory_order_relaxed);
  if (tail == g_ring_head.load(std::memory_order_acquire)) return false;
  *out = g_ring[tail % kRingSize];
  g_ring_tail.store(tail + 1, std::memory_order_release);
  return true;
}

std::atomic<bool> g_mnk_mode{true};

// An action a key holds, with nothing done once on the press: pressed again
// when the game let go of it while the key stayed down (a menu flicker). A
// use, a grenade, a jump, a one-shot command or a toggle isn't pressed again,
// as a controller button held through it wouldn't be.
bool Continuous(Action action) {
  switch (action) {
    case Action::kFire:
    case Action::kSprintBreath:
    case Action::kHoldBreath:
    case Action::kSprint:
    case Action::kLeanLeft:
    case Action::kLeanRight:
    case Action::kCrouchHold:
    case Action::kProneHold:
    case Action::kBinoculars:
    case Action::kScores:
    case Action::kTankGun:
    case Action::kTankTurret:
    case Action::kVehGas:
    case Action::kVehReverse:
    case Action::kVehHandbrake:
      return true;
    case Action::kAim:
      return !REXCVAR_GET(cod3_aim_toggle_kbm_p1);
    default:
      return false;
  }
}

class Listener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  rex::ui::Window* window = nullptr;
  std::function<bool()> overlay_open;

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const uint32_t vk = static_cast<uint32_t>(e.virtual_key());
    if (vk == 0 || vk > 0xFF) return;
    if (Down(static_cast<KeyCode>(vk), e.prev_state())) e.set_handled(true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    const uint32_t vk = static_cast<uint32_t>(e.virtual_key());
    if (vk == 0 || vk > 0xFF) return;
    Up(static_cast<KeyCode>(vk));  // never taken: the others see every release
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (const KeyCode code = MouseCode(e.button()); code && Down(code, false)) e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (const KeyCode code = MouseCode(e.button())) Up(code);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (e.scroll_y() == 0 || !Acting()) return;
    const KeyCode code = e.scroll_y() > 0 ? kWheelUp : kWheelDown;
    const uint16_t v = KeyAction(code);
    if (!v || !g_action_ok[(v & 0xFF) - 1].load(std::memory_order_relaxed)) return;
    // One tap per wheel event (a slot takes one press per poll, as a key does).
    Push({uint8_t((v & 0xFF) - 1), uint8_t(v >> 8), Kind::kTap, code});
    e.set_handled(true);
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override { ReleaseAll("the window lost focus"); }

  // Every UI frame.
  void Update() {
    const bool blocked = Blocked() || (window && !window->HasFocus());
    if (blocked && !blocked_) ReleaseAll("a menu or an overlay opened, or the keys were turned off");
    blocked_ = blocked;
    // The game let every action go (a menu flickered up for a moment): keys
    // still held, now in play again, press their held actions again (the
    // others wait for the key to be pressed again, as a held controller
    // button would).
    const uint32_t gen = g_release_gen.load(std::memory_order_acquire);
    if (gen != release_gen_) {
      release_gen_ = gen;
      if (!blocked)
        for (size_t code = 0; code < kKeyCodeCount; ++code)
          if (const uint16_t v = pressed_[code]; v && Continuous(static_cast<Action>((v & 0xFF) - 1)))
            Push({uint8_t((v & 0xFF) - 1), uint8_t(v >> 8), Kind::kDown, uint16_t(code)});
    }
  }

  void ReleaseAll(const char* why) {
    bool any = false;
    for (size_t code = 0; code < kKeyCodeCount; ++code) {
      if (!pressed_[code]) continue;
      Push({uint8_t((pressed_[code] & 0xFF) - 1), uint8_t(pressed_[code] >> 8), Kind::kUp, uint16_t(code)});
      pressed_[code] = 0;
      any = true;
    }
    if (any && Debug()) REXLOG_INFO("cod3: binds: keys let go ({})", why);
  }

 private:
  static KeyCode MouseCode(rex::ui::MouseEvent::Button b) {
    switch (b) {
      case rex::ui::MouseEvent::Button::kLeft: return kMouse1;
      case rex::ui::MouseEvent::Button::kRight: return kMouse2;
      case rex::ui::MouseEvent::Button::kMiddle: return kMouse3;
      case rex::ui::MouseEvent::Button::kX1: return kMouse4;
      case rex::ui::MouseEvent::Button::kX2: return kMouse5;
      default: return kNoKey;
    }
  }

  // Keys aren't actions now: they are turned off, an overlay has the input,
  // or the game isn't taking the player's presses (a menu is open).
  bool Blocked() const {
    return !g_mnk_mode.load(std::memory_order_relaxed) || !REXCVAR_GET(cod3_binds_actions) ||
           (overlay_open && overlay_open()) || !g_in_play.load(std::memory_order_relaxed);
  }

  bool Acting() {
    const bool blocked = Blocked();
    if (blocked && !blocked_) ReleaseAll("a menu or an overlay opened");
    blocked_ = blocked || (window && !window->HasFocus());
    return !blocked_;
  }

  // True when the key is an action's (it is taken: nothing below sees it).
  bool Down(KeyCode code, bool repeat) {
    const uint16_t v = KeyAction(code);
    if (pressed_[code]) return true;  // held since play: still the action's (repeats too)
    if (!v || !Acting()) return false;
    if (!g_action_ok[(v & 0xFF) - 1].load(std::memory_order_relaxed)) return false;
    if (repeat) return true;
    pressed_[code] = v;
    Push({uint8_t((v & 0xFF) - 1), uint8_t(v >> 8), Kind::kDown, code});
    return true;
  }

  void Up(KeyCode code) {
    const uint16_t v = pressed_[code];
    if (!v) return;
    pressed_[code] = 0;
    Push({uint8_t((v & 0xFF) - 1), uint8_t(v >> 8), Kind::kUp, code});
  }

  uint16_t pressed_[kKeyCodeCount] = {};  // the action each held key pressed
  bool blocked_ = false;
  uint32_t release_gen_ = 0;              // the last g_release_gen seen
};
Listener g_listener;

// --- game thread: the pad poll ------------------------------------------------------------
// The game's own functions for an action, found by name in its command list.
struct Handlers {
  uint32_t down = 0, up = 0;
};
Handlers g_handlers[kActionCount];
uint32_t g_aim_toggle = 0;   // "toggle cl_run": the game's aim toggle
bool g_looked_up = false;
bool g_missing[kActionCount] = {};  // looked up again now and then (RetryMissing)

enum class Mode : uint8_t { kNone, kDirect, kPad };
struct Slot {
  Mode mode = Mode::kNone;
  uint16_t buttons = 0;  // kPad: XInput bits
  uint8_t triggers = 0;  // kPad: 1 left, 2 right
  uint32_t since_poll = 0;
  bool release_pending = false;  // kPad: let go after the poll that saw it
  bool tap = false;              // kDirect: let go on the next poll (a wheel detent, a very short press)
  bool silent = false;           // kDirect: the action was already down from its other key
  bool toggle = false;           // kDirect aim: pressed as a toggle (nothing on the release)
};
Slot g_slots[kActionCount][kSlots];
// The frag key's press made the game hold +activatereload with the
// controller's X (the throw-back, Deliver); its release lets that go.
bool g_frag_added_x[kSlots] = {};
uint32_t g_poll = 0;
bool g_was_in_play = false;

// A press or release to call after the pad poll (model.h's Delivery says how).
struct Pending {
  uint8_t action, slot;
  bool down;
};
// Every event of a full ring, the taps let go and the overflow releases fit.
std::array<Pending, 2 * kRingSize + kSlotCount> g_deliveries;
size_t g_delivery_count = 0;

std::atomic<uint32_t> g_inject_buttons{0};
std::atomic<uint32_t> g_inject_triggers{0};
std::atomic<int32_t> g_inject_user{-1};

// The game's handler for command `name` (its type in `type`), 0 when none.
uint32_t FindCommand(uint8_t* base, std::string_view name, uint32_t* type) {
  int guard = 0;
  for (uint32_t cmd = ReadU32(base, addr::kCmdFunctions); Valid(cmd) && guard < 4096;
       cmd = ReadU32(base, cmd), ++guard) {
    const uint32_t text = ReadU32(base, cmd + 4);
    if (!Valid(text) || !SameText(ReadString(base, text, 40), name)) continue;
    *type = ReadU32(base, cmd + 8);
    return ReadU32(base, cmd + 12);
  }
  return 0;
}

// One action's handlers: true when the game has them, of the kind the action
// calls (a button command's pair, a plain command's pair, one plain command).
bool LookUp(uint8_t* base, int a, std::string* why) {
  const ActionDef& def = Def(static_cast<Action>(a));
  Handlers h;
  uint32_t type = 0;
  switch (def.delivery) {
    case Delivery::kText:
      g_handlers[a] = {};
      return true;  // cgame commands: console text, nothing to look up
    case Delivery::kAlias:
    case Delivery::kView:
      g_handlers[a] = {};
      return true;  // the vehicle's own reads (the hooks at the end)
    case Delivery::kOnce:
      h.down = FindCommand(base, def.command, &type);
      if (!InImage(h.down) || type != kTypePlain)
        return *why = std::string(def.command) + " isn't a plain command here", false;
      break;
    case Delivery::kButton:
    case Delivery::kAim:
    case Delivery::kPlain: {
      const uint32_t want = def.delivery == Delivery::kPlain ? kTypePlain : kTypeButton;
      const std::string up = std::string("-") + (def.command + 1);
      uint32_t up_type = 0;
      h.down = FindCommand(base, def.command, &type);
      h.up = FindCommand(base, up, &up_type);
      if (!InImage(h.down) || !InImage(h.up) || type != want || up_type != want)
        return *why = std::string(def.command) + " / " + up + " aren't " +
                      (want == kTypePlain ? "plain" : "button") + " commands here",
               false;
      break;
    }
  }
  g_handlers[a] = h;
  return true;
}

void LookUpAll(uint8_t* base) {
  g_looked_up = true;
  int found = 0;
  for (int a = 0; a < kActionCount; ++a) {
    std::string why;
    const bool ok = LookUp(base, a, &why);
    g_missing[a] = !ok;
    g_action_ok[a].store(ok, std::memory_order_relaxed);
    if (ok)
      ++found;
    else
      REXLOG_WARN("cod3: key binds: {}: {}; its keys press their keyboard row's controller button instead",
                  Def(static_cast<Action>(a)).id, why);
  }
  uint32_t type = 0;
  g_aim_toggle = FindCommand(base, "toggle cl_run", &type);
  if (!InImage(g_aim_toggle) || type != kTypeButton) {
    g_aim_toggle = 0;
    REXLOG_WARN("cod3: key binds: the game's aim toggle (toggle cl_run) isn't here; the aim key holds");
  }
  REXLOG_INFO("cod3: key binds: {} of {} actions call the game's own commands ({})", found, kActionCount,
              cod3::kMultiplayer ? "multiplayer" : "campaign");
}

// Actions the game didn't have yet are looked up again now and then (a
// command registered late); their keys stay the keyboard rows' meanwhile.
void RetryMissing(uint8_t* base) {
  for (int a = 0; a < kActionCount; ++a) {
    if (!g_missing[a]) continue;
    std::string why;
    if (!LookUp(base, a, &why)) continue;
    g_missing[a] = false;
    g_action_ok[a].store(true, std::memory_order_relaxed);
    REXLOG_INFO("cod3: key binds: {} found now; its keys call it", Def(static_cast<Action>(a)).id);
  }
}

// The pad the pad poll reads for local client `cl`: the one that pressed
// Start, else pad 0 (the campaign) or the client's own controller (cod3mp).
int ActivePad(uint8_t* base, uint32_t cl) {
  const uint32_t input = ReadU32(base, kb::kInput);
  if (Valid(input) && ReadU8(base, input + 28)) {
    const uint32_t pad = ReadU32(base, input + 24);
    return pad < 4 ? int(pad) : 0;
  }
  if constexpr (!cod3::kMultiplayer) return 0;
  const uint32_t pad = ReadU32(base, kb::kLocalClients + kb::kLocalClientPad + cl * kb::kLocalClientStride);
  return pad < 4 ? int(pad) : 0;
}

// The pad poll's own rule for presses (see the top of this file).
bool PollTakesPresses(PPCContext& ctx, uint8_t* base, uint32_t cl, const char** why) {
  const uint32_t dvar = ReadU32(base, kb::kInJoystick);
  if (Valid(dvar) && ReadS32(base, dvar + 32) == 0) return *why = "in_joystick is 0", false;
  if (kb::kPadPollOff && ReadU8(base, kb::kPadPollOff)) return *why = "the pad poll is off", false;
  const uint32_t fe = addr::prompts::kFEManager;
  if constexpr (!cod3::kMultiplayer) {
    const uint32_t block = ReadU32(base, fe + kb::kFeBlock);
    if (Valid(block) && ReadU8(base, block + 28 + cl) && ReadS32(base, block + 12) != 3)
      return *why = "a game menu is open", false;
  } else {
    if (kb::kFeInputOn && !ReadU8(base, fe + kb::kFeInputOn)) return *why = "the game takes no pad input now", false;
    // Not in a match (the front end, a lobby, loading): the client's presses
    // are menu keys (the pad poll passes state == 2 to its press path).
    if (kb::kLocalClientState &&
        ReadU32(base, kb::kLocalClients + kb::kLocalClientState + cl * kb::kLocalClientStride) != 2)
      return *why = "the player isn't in a match", false;
    const uint32_t pad = ReadU32(base, kb::kLocalClients + kb::kLocalClientPad + cl * kb::kLocalClientStride);
    if (kb::kPadSkip && pad < 4 && ReadU8(base, kb::kPadSkip + pad))
      return *why = "the player's controller is busy", false;
    if (kb::kFePerClient) {
      const uint32_t object = ReadU32(base, fe + kb::kFePerClient + 4 * cl);
      if (Valid(object)) {
        const uint32_t vtable = ReadU32(base, object);
        const uint32_t blocked = Valid(vtable) ? ReadU32(base, vtable + kb::kFeBlockedVfunc) : 0;
        if (InImage(blocked)) {
          Call c(ctx, base);
          if (static_cast<uint8_t>(c.Invoke(blocked, {object}))) return *why = "a game menu is open", false;
        }
      }
    }
  }
  return true;
}

bool InteractionRunning(uint8_t* base, uint32_t cl) {
  const uint32_t it = ReadU32(base, addr::interact::kPerClient + 4 * cl);
  return Valid(it) && ReadU32(base, it + addr::interact::kState) != 0;
}

// Why keys press controller buttons now instead of calling the game's
// commands (an interaction runs: a mounted gun, a battle action); nullptr
// when they call the commands. Vehicles never make them do that: the Vehicle
// actions answer their reads.
const char* PadModeReason(uint8_t* base, uint32_t cl) {
  if (InteractionRunning(base, cl)) return "an interaction runs";
  return nullptr;
}

// ButtonAlias_Value's tables (*kb::kAliasContexts): per context int
// button[4 pads][17] (alias id or -1) at +pad*68, then the sticks.
constexpr uint32_t kAliasContextSize = 328;
constexpr uint32_t kAliasPadStride = 68;
constexpr const char* kAliasNames[] = {"Gas", "Reverse", "HandBrake", "AlignTurret", "FireCoax"};
// The controller input of each alias table entry (the order of 0x820E7E10,
// which reads an entry's value from the game's pad cache).
constexpr int kAliasEntryKeys[16] = {156, 155, 157, 154, 211, 212, 213, 214,
                                     215, 216, 217, 218, 219, 220, 27,  9};

std::string AliasName(int alias) {
  return alias >= 0 && alias < 5 ? kAliasNames[alias] : "alias " + std::to_string(alias);
}

// ButtonAlias_Value's hook (game thread): `table` is the alias context it
// reads (*kb::kAliasContexts + context * 328), `pad` the controller (the
// campaign: the local client's controller or the input's active pad; cod3mp:
// the input's active pad, input + 24), `alias` the alias id. Notes the read
// and returns its context; -1 for one that isn't a vehicle's (2 is OnFoot)
// or, in cod3mp, not the keyboard player's controller.
int NoteVehicleRead(uint8_t* base, uint32_t table, uint32_t pad, int32_t alias) {
  const uint32_t contexts = ReadU32(base, kb::kAliasContexts);
  if (!Valid(contexts) || table < contexts || (table - contexts) % kAliasContextSize) return -1;
  const uint32_t context = (table - contexts) / kAliasContextSize;
  if (context >= uint32_t(kVehicleContexts)) return -1;
  if constexpr (cod3::kMultiplayer) {
    // Only the keyboard player's vehicle (split-screen players have controllers of their own).
    const uint32_t client = kb::kLocalClients + KbmClient(base) * kb::kLocalClientStride;
    if (pad != ReadU32(base, client + kb::kLocalClientPad)) return -1;
  }
  g_vehicle_pad[context].store(pad, std::memory_order_relaxed);
  // Only the aliases a Vehicle action answers count as "the vehicle reads its
  // controls" (cod3mp reads a 5 too, which nothing answers).
  if (alias >= 0 && alias < kAliasIds && VehicleActionForAlias(alias) != Action::kCount) {
    const size_t i = size_t(context) * kAliasIds + size_t(alias);
    g_alias_polls[i].store(kVehiclePolls, std::memory_order_relaxed);
    g_alias_until_ms[i].store(SteadyMs() + kVehicleMs, std::memory_order_relaxed);
  }
  return int(context);
}

// After ButtonAlias_Value: `value` is what the controller gave; the Vehicle
// action held for the alias makes it 255 (every threshold the vehicles use:
// Gas > 150 is full, Reverse != 0, HandBrake, AlignTurret and FireCoax > 50).
void AnswerAlias(int context, int32_t alias, uint64_t* value) {
  if (context < 0 || context >= kVehicleContexts || alias < 0 || alias >= kAliasIds) return;
  const uint32_t bit = 1u << alias;
  const bool held = (g_alias_held.load(std::memory_order_relaxed) & bit) != 0;
  const uint64_t controller = *value;
  if (held) *value = 255;
  static uint32_t s_logged = 0;  // game thread; bit context * kAliasIds + alias
  const uint32_t log_bit = 1u << (context * kAliasIds + alias);
  if (Debug() && held != ((s_logged & log_bit) != 0)) {
    s_logged ^= log_bit;
    REXLOG_INFO("cod3: binds: the {}'s {} read: {} (the controller's {})", VehicleName(context), AliasName(alias),
                held ? "held by its key" : "let go", controller);
  }
}

// The Vehicle action whose alias a vehicle reading now maps the controller
// input `pad_key` to (its alias table row for the controller it reads), or
// Action::kCount. Game thread.
Action VehicleActionForPadKey(uint8_t* base, int pad_key) {
  int entry = -1;
  for (int e = 0; e < 16; ++e)
    if (kAliasEntryKeys[e] == pad_key) entry = e;
  if (entry < 0) return Action::kCount;
  const uint32_t contexts = ReadU32(base, kb::kAliasContexts);
  if (!Valid(contexts)) return Action::kCount;
  for (int c = 0; c < kVehicleContexts; ++c) {
    const uint32_t pad = g_vehicle_pad[c].load(std::memory_order_relaxed);
    if (pad >= 4) continue;
    const int32_t alias = ReadS32(base, contexts + uint32_t(c) * kAliasContextSize + pad * kAliasPadStride +
                                            4 * uint32_t(entry));
    if (!AliasReading(c, alias)) continue;  // a mapping the vehicle doesn't read (now)
    if (const Action a = VehicleActionForAlias(alias); a != Action::kCount) return a;
  }
  return Action::kCount;
}

// At every pad poll: a vehicle starts or stops reading its controls (the log
// says which keys count then).
void NoteVehicleChanges() {
  for (int c = 0; c < kVehicleContexts; ++c) {
    const uint32_t mask = ReadingMask(c);
    const uint32_t was = g_vehicle_was[c];
    g_vehicle_was[c] = mask;
    if (mask == was) continue;
    if (!mask) {
      REXLOG_INFO("cod3: binds: the {} doesn't read its controls anymore: its keys have their on-foot actions again",
                  VehicleName(c));
      continue;
    }
    if (was && (mask & ~was) == 0) continue;  // one alias less for now: nothing to say
    std::string read, keys;
    const KbmSet set = CurrentSet();
    for (int a = 0; a < kAliasIds; ++a)
      if (mask & (1u << a)) read += (read.empty() ? "" : ", ") + AliasName(a);
    for (int a = 0; a < kActionCount; ++a) {
      const ActionDef& def = Def(static_cast<Action>(a));
      if (def.group != Group::kVehicle || (def.alias >= 0 && !(mask & (1u << def.alias)))) continue;
      const std::string k = KeysText(set, static_cast<Action>(a));
      keys += std::string(keys.empty() ? "" : ", ") + def.label + " " + (k.empty() ? std::string("-") : k);
    }
    if (was) {
      if (Debug()) REXLOG_INFO("cod3: binds: the {} reads {} too: its keys now {}", VehicleName(c), read, keys);
      continue;
    }
    REXLOG_INFO("cod3: binds: the {} reads its controls ({}): its keys count now ({}); on foot the same keys keep "
                "their own actions{}",
                VehicleName(c), read, keys,
                REXCVAR_GET(cod3_binds_actions) ? "" : " (keys call no actions now: cod3_binds_actions is off)");
  }
}

// Cod3VehicleViewPressed (the hooks at the end): one Vehicle: change view press
// at the game's read of the right stick click's press. `r29` is cod3mp's
// camera select's player (its local client at +480).
bool TakeVehicleViewPress(uint8_t* base, uint32_t r29) {
  const uint32_t at = g_view_press_poll.load(std::memory_order_relaxed);
  if (!at) return false;
  if (g_poll - at > 2) {  // nothing read it (the view can't switch now)
    g_view_press_poll.store(0, std::memory_order_relaxed);
    return false;
  }
  if constexpr (cod3::kMultiplayer)
    if (!Valid(r29) || ReadU32(base, r29 + 480) != KbmClient(base)) return false;
  g_view_press_poll.store(0, std::memory_order_relaxed);
  if (Debug()) REXLOG_INFO("cod3: binds: veh_view: the vehicle's view switch read the press");
  return true;
}

// The controller input the game binds to the action (its command first,
// then an equivalent), or nullptr. `skip_vehicle`: never a button the vehicle
// being driven reads as one of its own controls (VehicleActionForPadKey).
const PadInput* PadInputFor(uint8_t* base, uint32_t cl, Action action, bool skip_vehicle = false) {
  const ActionDef& def = Def(action);
  auto usable = [&](const PadInput& p, const char* command) {
    return SameText(Binding(base, cl, p.key), command) &&
           (!skip_vehicle || VehicleActionForPadKey(base, p.key) == Action::kCount);
  };
  for (const PadInput& p : kPadInputs)
    if (usable(p, def.command)) return &p;
  for (const char* eq : def.equivalents) {
    if (!eq) break;
    for (const PadInput& p : kPadInputs)
      if (usable(p, eq)) return &p;
  }
  return nullptr;
}

const char* HolderName(int slot) { return slot == 0 ? "AUX1" : "AUX2"; }

void Queue(int a, int s, bool down) {
  if (g_delivery_count < g_deliveries.size()) g_deliveries[g_delivery_count++] = {uint8_t(a), uint8_t(s), down};
}

// A delivery kind with one game state per action (not two holders): the press
// goes out for the first key down, the release for the last key up.
bool OneState(Delivery d) { return d != Delivery::kButton && d != Delivery::kAim; }

void PressDirect(int a, int s, bool tap) {
  const ActionDef& def = Def(static_cast<Action>(a));
  Slot& slot = g_slots[a][s];
  const bool toggle = def.delivery == Delivery::kAim && REXCVAR_GET(cod3_aim_toggle_kbm_p1) && g_aim_toggle;
  const bool other_down = g_slots[a][1 - s].mode == Mode::kDirect;
  slot = {Mode::kDirect, 0, 0, g_poll, false, tap, (OneState(def.delivery) || toggle) && other_down, toggle};
  if (!slot.silent) Queue(a, s, true);
}

void ReleaseDirect(int a, int s) {
  const ActionDef& def = Def(static_cast<Action>(a));
  Slot& slot = g_slots[a][s];
  Slot& other = g_slots[a][1 - s];
  if ((OneState(def.delivery) || slot.toggle) && other.mode == Mode::kDirect) {
    // The other key still holds the action: it sends the release.
    if (!slot.silent) other.silent = false;
  } else if (!slot.silent && !slot.toggle && def.delivery != Delivery::kOnce) {
    Queue(a, s, false);
  }
  slot = {};
}

void Press(PPCContext& ctx, uint8_t* base, uint32_t cl, int a, int s, bool tap) {
  Slot& slot = g_slots[a][s];
  if (slot.mode != Mode::kNone) return;
  const Action action = static_cast<Action>(a);
  if (g_missing[a]) return;  // the game doesn't have it (RetryMissing)
  // A Vehicle action is answered at the vehicle's own read, inside an
  // interaction too (never a controller button).
  if (const char* pad_mode = IsVehicleAction(action) ? nullptr : PadModeReason(base, cl)) {
    // The interaction decides, as for a controller: press the button the
    // game binds to this action.
    // A button the vehicle being driven reads as one of its own controls (the
    // jeep's RT is its gas in layout A) is the Vehicle action's key's only:
    // never pressed for an on-foot action.
    if (const PadInput* p = PadInputFor(base, cl, action, /*skip_vehicle=*/true)) {
      slot = {Mode::kPad, p->button, p->trigger, g_poll, tap, false};
      if (Debug()) REXLOG_INFO("cod3: binds: {} down -> the controller's {} ({})", Def(action).id, p->name, pad_mode);
    } else if (Debug()) {
      const PadInput* own = PadInputFor(base, cl, action);
      const Action owner = own ? VehicleActionForPadKey(base, own->key) : Action::kCount;
      if (owner != Action::kCount)
        REXLOG_INFO("cod3: binds: {} not delivered (the controller's {} is the vehicle's {}: {})", Def(action).id,
                    own->name, Def(owner).label, KeysText(CurrentSet(), owner));
      else
        REXLOG_INFO("cod3: binds: {} not delivered ({}, no controller button has it)", Def(action).id, pad_mode);
    }
    return;
  }
  const char* why = "";
  if (!PollTakesPresses(ctx, base, cl, &why)) {
    if (Debug()) REXLOG_INFO("cod3: binds: {} not delivered ({})", Def(action).id, why);
    return;
  }
  PressDirect(a, s, tap);
}

void Release(int a, int s) {
  Slot& slot = g_slots[a][s];
  if (slot.mode == Mode::kDirect) {
    // Pressed in this same poll: let go on the next one, so the game sees
    // the press for at least one frame (a controller press that short would
    // be lost entirely).
    if (slot.since_poll == g_poll) {
      slot.tap = true;
      return;
    }
    ReleaseDirect(a, s);
  } else if (slot.mode == Mode::kPad) {
    slot.release_pending = true;
  }
}

// Every action held through the game's commands lets go (a menu opened);
// controller buttons pressed for an interaction go too.
void ReleaseEverything(const char* why) {
  bool any = false;
  for (int a = 0; a < kActionCount; ++a)
    for (int s = 0; s < kSlots; ++s) {
      Slot& slot = g_slots[a][s];
      if (slot.mode == Mode::kDirect) {
        ReleaseDirect(a, s);
        any = true;
      } else if (slot.mode == Mode::kPad) {
        slot = {};
        any = true;
      }
    }
  g_release_gen.fetch_add(1, std::memory_order_release);
  if (any && Debug()) REXLOG_INFO("cod3: binds: actions let go ({})", why);
}

void BeforePadPoll(PPCContext& ctx, uint8_t* base) {
  EnsureStarted();  // the keys and g_action_ok before the first lookup
  ++g_poll;
  g_delivery_count = 0;
  if (!g_looked_up)
    LookUpAll(base);
  else if (g_poll % 600 == 0)
    RetryMissing(base);
  const uint32_t cl = KbmClient(base);
  // Is the game taking the player's presses? The listener takes the keys
  // only then (otherwise they press the keyboard rows' controller buttons).
  const char* why = "turned off (cod3_binds_actions)";
  const bool in_play = REXCVAR_GET(cod3_binds_actions) && PollTakesPresses(ctx, base, cl, &why);
  g_in_play.store(in_play, std::memory_order_relaxed);
  if (g_was_in_play && !in_play) ReleaseEverything(why);
  if (in_play != g_was_in_play && Debug())
    REXLOG_INFO("cod3: binds: keys {} ({})", in_play ? "call the game's actions" : "press the controller rows",
                in_play ? "in play" : why);
  if (!in_play && g_was_in_play) g_play_stopped_ms = SteadyMs();
  const bool long_stop = in_play && !g_was_in_play && SteadyMs() - g_play_stopped_ms > 3000;
  g_was_in_play = in_play;
  // A vehicle's last read counts down in play only (NoteVehicleRead); a level
  // load or a long pause clears it (the vehicle reads again first).
  for (auto& polls : g_alias_polls) {
    if (long_stop) polls.store(0, std::memory_order_relaxed);
    if (in_play && polls.load(std::memory_order_relaxed) > 0) polls.fetch_sub(1, std::memory_order_relaxed);
  }
  NoteVehicleChanges();
  // Taps from the last poll, and controller buttons that were seen once.
  for (int a = 0; a < kActionCount; ++a)
    for (int s = 0; s < kSlots; ++s) {
      Slot& slot = g_slots[a][s];
      if (slot.mode == Mode::kDirect && slot.tap && slot.since_poll < g_poll) ReleaseDirect(a, s);
      if (slot.mode == Mode::kPad && slot.release_pending && slot.since_poll < g_poll) slot = {};
    }
  // A key held when an interaction (or a vehicle's reading) starts or ends goes on as a held
  // controller button would: it stays what it was (fire held onto a turret
  // keeps firing; X held off a mounted gun does nothing new) until let go.
  Event e;
  while (Pop(&e)) {
    if (e.action >= kActionCount || e.slot >= kSlots) continue;
    if (e.kind == Kind::kUp)
      Release(e.action, e.slot);
    else if (in_play)
      Press(ctx, base, cl, e.action, e.slot, e.kind == Kind::kTap);
  }
  // Releases that didn't fit in the ring, after it (their presses were in it).
  for (size_t w = 0; w < g_release_overflow.size(); ++w)
    if (uint32_t overflow = g_release_overflow[w].exchange(0, std::memory_order_acquire))
      for (int b = 0; b < 32; ++b)
        if (overflow & (1u << b)) {
          const int i = int(w) * 32 + b;
          if (i < kSlotCount) Release(i / kSlots, i % kSlots);
        }
  uint32_t buttons = 0, triggers = 0;
  for (auto& action : g_slots)
    for (const Slot& slot : action)
      if (slot.mode == Mode::kPad) buttons |= slot.buttons, triggers |= slot.triggers;
  g_inject_user.store(ActivePad(base, cl), std::memory_order_relaxed);
  g_inject_triggers.store(triggers, std::memory_order_relaxed);
  g_inject_buttons.store(buttons, std::memory_order_release);
  // The aliases the Vehicle actions hold, for ButtonAlias_Value's hook.
  uint32_t held = in_play ? g_test_hold.load(std::memory_order_relaxed) : 0;
  for (int a = 0; a < kActionCount; ++a) {
    const ActionDef& def = Def(static_cast<Action>(a));
    if (def.delivery != Delivery::kAlias || def.alias < 0 || def.alias >= kAliasIds) continue;
    for (const Slot& slot : g_slots[a])
      if (slot.mode == Mode::kDirect) held |= 1u << def.alias;
  }
  g_alias_held.store(held, std::memory_order_relaxed);
}

// cod3mp keeps the buttons of each local client apart and copies the client's
// set into the game's kbuttons around its pad poll and its frame. The keys'
// commands work on player 1's set the same way, and everything is as before
// for the game afterwards.
class PlayerButtons {
 public:
  PlayerButtons(uint8_t* base, uint32_t cl) : base_(base), cl_(cl) {
    if (!kb::kSavedKbuttons || kb::kSavedKbuttonsSize > sizeof(game_)) return;
    on_ = true;
    active_ = ReadU32(base, addr::kActiveLocalClient);
    WriteU32(base, addr::kActiveLocalClient, cl);
    std::memcpy(game_.data(), base + kb::kKbuttons, kb::kSavedKbuttonsSize);
    std::memcpy(base + kb::kKbuttons, base + Saved(), kb::kSavedKbuttonsSize);
  }
  ~PlayerButtons() {
    if (!on_) return;
    std::memcpy(base_ + Saved(), base_ + kb::kKbuttons, kb::kSavedKbuttonsSize);
    std::memcpy(base_ + kb::kKbuttons, game_.data(), kb::kSavedKbuttonsSize);
    WriteU32(base_, addr::kActiveLocalClient, active_);
  }
  PlayerButtons(const PlayerButtons&) = delete;
  PlayerButtons& operator=(const PlayerButtons&) = delete;

 private:
  uint32_t Saved() const { return kb::kSavedKbuttons + cl_ * kb::kSavedKbuttonsSize; }
  uint8_t* base_;
  uint32_t cl_;
  bool on_ = false;
  uint32_t active_ = 0;
  std::array<uint8_t, 1024> game_{};
};

// The +activatereload kbutton is held by the controller's X (211).
bool HoldsX(uint8_t* base) {
  const uint32_t button = kb::kKbuttons + kb::kActivateReloadButton;
  return ReadU32(base, button) == uint32_t(kPadX) || ReadU32(base, button + 4) == uint32_t(kPadX);
}

void Deliver(PPCContext& ctx, uint8_t* base, uint32_t now, const Pending& d) {
  const ActionDef& def = Def(static_cast<Action>(d.action));
  const Handlers& h = g_handlers[d.action];
  Slot& slot = g_slots[d.action][d.slot];
  const uint64_t key = uint64_t(kKeyAux1 + d.slot);
  const char* how = "";
  switch (def.delivery) {
    case Delivery::kButton:
    case Delivery::kAim: {
      // The aim toggle: the game's own (aimToggle[cl] = !aimToggle[cl]), once per press.
      const bool toggle = def.delivery == Delivery::kAim && d.down && slot.toggle;
      if (toggle) {
        Call(ctx, base).Invoke(g_aim_toggle, {key, now});
        how = ", toggle";
        break;
      }
      // +grenadeattack throws a grenade back by holding +activatereload with
      // the controller's X (211), which -grenadeattack never lets go: a
      // keyboard player would be left holding "use" after the throw.
      const bool frag = static_cast<Action>(d.action) == Action::kFrag;
      const bool held_x = frag && HoldsX(base);
      Call(ctx, base).Invoke(d.down ? h.down : h.up, {key, now});
      if (frag && d.down) g_frag_added_x[d.slot] = !held_x && HoldsX(base);
      if (frag && !d.down && g_frag_added_x[d.slot]) {
        g_frag_added_x[d.slot] = false;
        const uint32_t up = g_handlers[int(Action::kUseReload)].up;  // -activatereload
        if (HoldsX(base) && up) {
          Call(ctx, base).Invoke(up, {uint64_t(kPadX), now});
          how = ", and -activatereload 211 (the throw-back's X)";
        }
      }
      break;
    }
    case Delivery::kPlain:
      Call(ctx, base).Invoke(d.down ? h.down : h.up);
      break;
    case Delivery::kOnce:
      if (d.down) Call(ctx, base).Invoke(h.down);
      break;
    case Delivery::kAlias:
      how = ", the vehicle reads it";
      break;
    case Delivery::kView:
      if (d.down) g_view_press_poll.store(g_poll ? g_poll : 1, std::memory_order_relaxed);
      how = ", the vehicle reads it";
      break;
    case Delivery::kText: {
      std::string text;
      if (d.down)
        text = def.command;
      else if (def.command[0] == '+')
        text = std::string("-") + (def.command + 1);
      if (text.empty()) break;
      text += '\n';
      Call c(ctx, base);
      c.Invoke(addr::kCbufAddText, {c.PushString(text)});
      break;
    }
  }
  if (Debug()) {
    std::string what = def.command;
    if (!d.down && def.command[0] == '+') what = std::string("-") + (def.command + 1);
    REXLOG_INFO("cod3: binds: {} {} ({}, holder {}, direct{})", def.id, d.down ? "down" : "up", what,
                HolderName(d.slot), how);
  }
}

// --- debug: what the game did with it (campaign) -------------------------------------------
#if !defined(COD3_MP)
int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr uint32_t kEntityList = 0x82A2AB40;          // gentity*[] (index cl + 1)
constexpr uint32_t kSprintKbutton = 0x829C8E68 + 15 * 24;
constexpr uint32_t kBinocularsRequest = 0x82A578F4;   // bit 2 while +binoculars is held (not zoomed yet)
constexpr uint32_t kActionWordText = 0x824DD9A8;      // "[HOLD_BINOCS]" -> its button sentence

struct Watch {
  int zoom = -1, breath = -1, sprint = -1, request = -1;
  int64_t up_since = 0;
  bool binoculars_up = false;
  bool second_shot = false;
  uint32_t prompt_gen = 0;  // the key generation the prompt check last ran for
};
Watch g_watch;

void PromptCheck(PPCContext& ctx, uint8_t* base, uint32_t cl) {
  for (const char* word : {"[HOLD_BINOCS]", "[VERB_BINOCS]", "[HOLD_USE]", "[RELOAD]"}) {
    Call c(ctx, base);
    const uint32_t w = c.PushString(word);
    const uint32_t text = static_cast<uint32_t>(c.Invoke(kActionWordText, {w}));
    std::string out = Valid(text) ? ReadString(base, text, 160) : "";
    for (char& ch : out)
      if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7E) ch = '?';
    REXLOG_INFO("cod3: binds: prompt check: {} reads \"{}\"", word, out);
  }
  for (const PadInput& p : kPadInputs) {
    const std::string b = Binding(base, cl, p.key);
    const std::string key = KeyNameForPadKey(base, p.key);
    if (!key.empty())
      REXLOG_INFO("cod3: binds: prompt check: the controller's {} is bound to {}; with the keyboard its prompts show [{}]",
                  p.name, b, key);
  }
}

void WatchState(PPCContext& ctx, uint8_t* base, uint32_t cl) {
  const uint32_t ents = ReadU32(base, kEntityList);
  const uint32_t ent = Valid(ents) ? ReadU32(base, ents + 4 * (cl + 1)) : 0;
  const uint32_t ps = Valid(ent) ? ReadU32(base, ent + 528) : 0;
  if (!Valid(ps)) return;
  // Once in a level, and again after the keys change.
  if (g_watch.prompt_gen != Generation()) {
    g_watch.prompt_gen = Generation();
    PromptCheck(ctx, base, cl);
  }
  const float frac = ReadF32(base, ps + 140);
  const int zoom = !std::isfinite(frac) || frac <= 0.0f ? 0 : frac >= 1.0f ? 2 : 1;
  const int request = (ReadU32(base, kBinocularsRequest) & 2) ? 1 : 0;
  const int breath = int(ReadU32(base, ps + 1340) & 1);
  const int sprint = ReadS32(base, kSprintKbutton + 16) ? 1 : 0;
  static const char* kZoomNames[] = {"out", "moving", "in"};
  if (zoom != g_watch.zoom || request != g_watch.request)
    REXLOG_INFO("cod3: binds: state: zoom {} ({:.2f}; the sights or the binoculars), binoculars button {}",
                kZoomNames[zoom], std::isfinite(frac) ? frac : 0.0f, request ? "held" : "not held");
  // The binoculars are up: zoomed in while their button is held (aiming down
  // the sights zooms in too, without it). Pictures of the game's own frame:
  // at once and 25 s later (frames are slow in the cloud; the picture comes
  // after the game's state).
  const bool binoculars_up = zoom == 2 && request;
  if (binoculars_up && !g_watch.binoculars_up) {
    g_watch.up_since = NowMs();
    g_watch.second_shot = false;
    REXLOG_INFO("cod3: binds: state: binoculars up");
    const std::string file = shot::Request(shot::Kind::kAsked, "the binoculars are up (cod3_debug_binds)");
    if (!file.empty()) REXLOG_INFO("cod3: binds: screenshot of the binoculars up: {}", file);
  } else if (!binoculars_up && g_watch.binoculars_up) {
    REXLOG_INFO("cod3: binds: state: binoculars lowered");
  }
  if (binoculars_up && !g_watch.second_shot && NowMs() - g_watch.up_since > 25000) {
    g_watch.second_shot = true;
    const std::string file = shot::Request(shot::Kind::kAsked, "the binoculars are still up (cod3_debug_binds)");
    if (!file.empty()) REXLOG_INFO("cod3: binds: screenshot of the binoculars still up: {}", file);
  }
  if (breath != g_watch.breath) REXLOG_INFO("cod3: binds: state: hold breath {}", breath ? "on" : "off");
  if (sprint != g_watch.sprint) REXLOG_INFO("cod3: binds: state: sprint button {}", sprint ? "held" : "not held");
  g_watch.zoom = zoom;
  g_watch.binoculars_up = binoculars_up;
  g_watch.request = request;
  g_watch.breath = breath;
  g_watch.sprint = sprint;
}
#endif  // !COD3_MP

void AfterPadPoll(PPCContext& ctx, uint8_t* base) {
  if (g_delivery_count) {
    const uint32_t cl = KbmClient(base);
    PlayerButtons player(base, cl);  // cod3mp: player 1's buttons
    const uint32_t now = static_cast<uint32_t>(Call(ctx, base).Invoke(kb::kSysMilliseconds));
    for (size_t i = 0; i < g_delivery_count; ++i) {
      Deliver(ctx, base, now, g_deliveries[i]);
    }
    g_delivery_count = 0;
  }
#if !defined(COD3_MP)
  if (Debug()) WatchState(ctx, base, LocalClient(base));
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// The public side (binds.h)
// ---------------------------------------------------------------------------

uint32_t KeyboardClient(uint8_t* base) { return base ? KbmClient(base) : 0; }

uint32_t ClientPad(uint8_t* base, uint32_t client) {
  if (!base || !cod3::kMultiplayer || !kb::kLocalClientState || client >= 4) return 0;
  const uint32_t pad = ReadU32(base, kb::kLocalClients + client * kb::kLocalClientStride + kb::kLocalClientPad);
  return pad < 4 ? pad : 0;
}

void MigrateSettings(const std::filesystem::path& config_path) {
  EnsureStarted();
  const int32_t version = cod3::kMultiplayer ? REXCVAR_GET(cod3mp_binds_version) : REXCVAR_GET(cod3_binds_version);
  if (version >= kBindsVersion) return;
  // A row the player set: in the settings file (cod3mp: also the campaign's
  // cod3.toml, which cod3mp applies before this as runtime values).
  auto old_bind = [](const OldBindName& b) {
    const auto* info = rex::cvar::GetFlagInfo(b.name);
    const rex::cvar::Source source = rex::cvar::GetFlagSource(b.name);
    const bool players = source == rex::cvar::Source::kConfig ||
                         (cod3::kMultiplayer && source == rex::cvar::Source::kRuntime);
    return OldBind{b.name, rex::cvar::GetFlagByName(b.name), players, info ? info->default_value : std::string()};
  };
  size_t changes = 0;
  auto apply = [&changes](const Migration& m) {
    for (const auto& [name, value] : m.new_values) {
      if (value.empty())
        rex::cvar::ResetToDefault(name);
      else
        rex::cvar::SetFlagByName(name, value);
    }
    if (m.set_changed) Store(m.set);
    for (const auto& line : m.log) REXLOG_INFO("cod3: key binds: {}", line);
    changes += m.new_values.size() + (m.set_changed ? 1 : 0);
  };
  // Version 1: the left stick click and the D-pad (binoculars, hold breath);
  // the campaign's only (multiplayer's left stick click is +sprintbreath).
  if (!cod3::kMultiplayer && version < 1) {
    std::vector<OldBind> moved, others;
    for (const OldBindName& b : kOldBinds) {
      if (!rex::cvar::GetFlagInfo(b.name)) continue;
      (b.moved ? moved : others).push_back(old_bind(b));
    }
    apply(Migrate(CurrentSet(), moved, others));
  }
  // Version 2: every action has a key of its own; the player's own changes to
  // the controller rows are kept (model.h MigrateRows). Multiplayer's layouts
  // aren't the campaign's layout A, so there every changed row keeps its keys.
  std::vector<OldBind> rows;
  for (const OldBindName& b : kOldBinds)
    if (rex::cvar::GetFlagInfo(b.name)) rows.push_back(old_bind(b));
  if (version < 2) apply(MigrateRows(CurrentSet(), rows, !cod3::kMultiplayer));
  // Version 3: the Vehicle actions follow the player's own movement rows
  // (model.h MigrateVehicleRows).
  if (version < 3) apply(MigrateVehicleRows(CurrentSet(), rows, !cod3::kMultiplayer));
  const KbmSet now = CurrentSet();
  std::string keys;
  for (const Action a : {Action::kFire, Action::kAim, Action::kUse, Action::kReload, Action::kJump, Action::kCrouch,
                         Action::kProne, Action::kFrag, Action::kBinoculars})
    keys += std::string(keys.empty() ? "" : ", ") + Def(a).id + " " +
            (KeysText(now, a).empty() ? std::string("-") : KeysText(now, a));
  REXLOG_INFO("cod3: key binds call the game's own actions now ({}); in the game's menus keys still press the "
              "controller buttons of their keybind_* rows ({} change(s))",
              keys, changes);
  if constexpr (cod3::kMultiplayer)
    REXCVAR_SET(cod3mp_binds_version, kBindsVersion);
  else
    REXCVAR_SET(cod3_binds_version, kBindsVersion);
  if (!changes) return;  // the version goes into cod3.toml with the next save
  try {
    rex::cvar::SaveConfig(config_path);
  } catch (const std::exception& e) {
    REXLOG_WARN("cod3: key binds: couldn't save the settings: {}", e.what());
  }
}

void Attach(rex::ui::Window* window, size_t z_order, std::function<bool()> overlay_open) {
  if (!window) return;
  EnsureStarted();
  g_mnk_mode.store(rex::cvar::GetFlagByName("mnk_mode") == "true", std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("mnk_mode", [](std::string_view, std::string_view v) {
    g_mnk_mode.store(v == "true" || v == "1", std::memory_order_relaxed);
  });
  g_listener.window = window;
  g_listener.overlay_open = std::move(overlay_open);
  window->AddInputListener(&g_listener, z_order);
  window->AddListener(&g_listener);
}

void Detach(rex::ui::Window* window) {
  if (!window || g_listener.window != window) return;
  window->RemoveInputListener(&g_listener);
  window->RemoveListener(&g_listener);
  g_listener.window = nullptr;
}

void OnUiFrame() {
  if (g_listener.window) g_listener.Update();
}

bool InPlay() { return g_in_play.load(std::memory_order_relaxed); }

void ApplyToPad(uint8_t* base, uint32_t user, uint32_t state) {
  const uint32_t buttons = g_inject_buttons.load(std::memory_order_acquire);
  const uint32_t triggers = g_inject_triggers.load(std::memory_order_relaxed);
  if ((!buttons && !triggers) || int32_t(user) != g_inject_user.load(std::memory_order_relaxed)) return;
  // X_INPUT_STATE (big-endian): u32 packet, u16 buttons, u8 LT, u8 RT, sticks.
  WriteU16(base, state + 4, uint16_t(guest::ReadU16(base, state + 4) | buttons));
  if (triggers & 1) WriteU8(base, state + 6, 255);
  if (triggers & 2) WriteU8(base, state + 7, 255);
}

std::string KeyNameForPadKey(uint8_t* base, int pad_key) {
  // In a vehicle, a button it reads as one of its controls is that Vehicle
  // action's key (the tank's LB: its machine gun, MOUSE2).
  if (const Action a = VehicleActionForPadKey(base, pad_key); a != Action::kCount) {
    if (!REXCVAR_GET(cod3_binds_actions)) return "";
    const KbmSet set = CurrentSet();
    const KeyCode key = set.key[static_cast<int>(a)][0];
    return key != kNoKey ? PromptName(key) : "";
  }
  return KeyNameForCommand(Binding(base, LocalClient(base), pad_key));
}

bool IsActionWord(std::string_view word) {
  for (const GameWord& w : kGameWords)
    if (SameText(word, w.word)) return true;
  return false;
}

std::string KeyNameForActionWord(std::string_view word) {
  for (const GameWord& w : kGameWords)
    if (SameText(word, w.word)) return KeyNameForCommand(w.command);
  return "";
}

std::string KeyNameForCommand(std::string_view binding) {
  if (binding.empty() || !REXCVAR_GET(cod3_binds_actions)) return "";
  const KbmSet set = CurrentSet();
  // The action whose command it is, then one it counts for.
  for (int pass = 0; pass < 2; ++pass)
    for (int a = 0; a < kActionCount; ++a) {
      if (!g_action_ok[a].load(std::memory_order_relaxed)) continue;
      const ActionDef& def = Def(static_cast<Action>(a));
      bool match = pass == 0 && SameText(binding, def.command);
      for (const char* eq : def.equivalents) {
        if (!eq || pass == 0) break;
        match |= SameText(binding, eq);
      }
      if (match && set.key[a][0] != kNoKey) return PromptName(set.key[a][0]);
    }
  return "";
}

uint32_t Generation() { return g_generation.load(std::memory_order_acquire); }

bool HandleConsoleCommand(const std::vector<std::string>& args, cheats::Source source,
                          const std::function<void(bool error, const std::string& text)>& print,
                          bool* changed) {
  *changed = false;
  if (args.size() < 2) return false;
  const bool bind = SameText(args[0], "bind");
  if (!bind && !SameText(args[0], "unbind")) return false;
  // The game's names for controller buttons stay the game's (its bind table
  // is the controller's): L1 ... R3, SQUARE, CIRCLE, TRIANGLE, JOY*, AUX*,
  // the D-pad's UPARROW ..., and TAB / ESCAPE / ENTER / PAUSE (the pad's
  // BACK and START). Keyboard arrows are Up / Down / Left / Right here.
  const std::string& name = args[1];
  for (const char* pad : {"L1", "R1", "L2", "R2", "L3", "R3", "SQUARE", "CIRCLE", "TRIANGLE", "UPARROW",
                          "DOWNARROW", "LEFTARROW", "RIGHTARROW", "TAB", "ESCAPE", "ENTER", "PAUSE", "START",
                          "SELECT"})
    if (SameText(name, pad)) return false;
  if ((name.size() > 3 && SameText(name.substr(0, 3), "JOY")) || (name.size() > 3 && SameText(name.substr(0, 3), "AUX")))
    return false;
  const KeyCode key = ParseKey(name);
  if (key == kNoKey) return false;
  KbmSet set = CurrentSet();
  int had_slot = 0, vehicle_slot = 0;
  const Action had = Find(set, key, &had_slot);                         // on foot
  const Action had_vehicle = Find(set, key, &vehicle_slot, /*vehicle=*/true);  // while driving
  const std::string key_name = KeyName(key);

  if (bind && args.size() == 2) {  // "bind B": what it does
    if (had == Action::kCount && had_vehicle == Action::kCount) return false;
    std::string text = "\"" + key_name + "\"";
    if (had != Action::kCount)
      text += " = \"" + std::string(Def(had).command) + "\" (keyboard and mouse: " + Def(had).label + ")";
    if (had_vehicle != Action::kCount)
      text += std::string(had != Action::kCount ? "; " : " ") + "while driving: " + Def(had_vehicle).label;
    print(false, text + "\n");
    return true;
  }
  if (source == cheats::Source::kNetwork) return false;
  std::string command;
  for (size_t i = 2; i < args.size(); ++i) command += (i > 2 ? " " : "") + args[i];
  Action action = bind ? ActionByCommand(command) : Action::kCount;
  if (bind && action == Action::kCount) action = ActionById(command);  // "bind K reload" works too

  if (action != Action::kCount) {
    if (const std::string why = ReservedReason(key); !why.empty()) {
      print(true, "^1bind: " + key_name + " can't be bound (" + why + ")\n");
      return true;
    }
    if ((IsVehicleAction(action) ? had_vehicle : had) == action) {
      print(false, key_name + " is already " + Def(action).label + "\n");
      return true;
    }
    // A free slot, else the second key is replaced.
    const int slot = set.key[int(action)][0] == kNoKey ? 0 : 1;
    const std::string note = Assign(set, action, slot, key);
    Store(set);
    *changed = true;
    std::string text = key_name + " = " + Def(action).label + " (keyboard and mouse; saved)";
    if (!note.empty()) text += "; " + note;
    if (const OldBindName* row = ControllerRowWith(key))
      text += "; in the game's menus it still presses the controller's " + std::string(row->label);
    print(false, text + "\n");
    REXLOG_INFO("cod3: key binds: {} from the console", text);
    return true;
  }
  if (had == Action::kCount && had_vehicle == Action::kCount) return false;
  // unbind B, or B bound to a command that isn't one of the actions: B leaves
  // its actions, on foot and while driving (the game's own bind of the key
  // does nothing: its keyboard keys never reach the game).
  std::string left;
  if (had != Action::kCount) {
    Assign(set, had, had_slot, kNoKey);
    left = Def(had).label;
  }
  if (had_vehicle != Action::kCount) {
    Assign(set, had_vehicle, vehicle_slot, kNoKey);
    left += std::string(left.empty() ? "" : " or ") + Def(had_vehicle).label;
  }
  Store(set);
  *changed = true;
  std::string text = key_name + " is no longer " + left + " (saved)";
  if (bind)
    text += "; keyboard keys call only the actions in F2 > Controls > Key bindings (bind <key> <action>, "
            "e.g. bind K +reload or bind K reload)";
  print(false, text + "\n");
  REXLOG_INFO("cod3: key binds: {} from the console", text);
  return !bind;
}

namespace {

// F2's key rows keep what is typed in them until Enter, leaving the box (a
// click anywhere else, Save included) or Save / closing F2 applies it
// (owner 2026-10-20 20:24: "It should interperet the value when the user says
// save or enter"). Typed letters come out as capitals, as the names are shown.
char g_f2_text[kActionCount][64];
std::string g_f2_notes[kActionCount];
bool g_f2_edited[kActionCount] = {};
uint32_t g_f2_text_gen = 0;
// Controller rows (keybind_*): cvar -> the text typed and not applied yet;
// and the rows F2 has shown.
std::map<std::string, std::string, std::less<>> g_pad_edits;
std::set<std::string, std::less<>> g_pad_rows;

std::string UpperText(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// Call right before the box's InputText. A box that is active and that nobody
// typed in, whose text changed here meanwhile (a key just moved away from its
// row while the click on it was made, a reset): ImGui keeps its own copy of
// what the box showed when it was clicked and would put that back as an edit,
// so it takes the new text, and Escape goes back to it.
void SyncActiveBox(const char* label, const char* text) {
  const ImGuiID id = ImGui::GetID(label);
  // [claude-9b] Through GetCurrentContext(), not imgui_internal.h's inline GetInputTextState /
  // GetActiveID: those read the global GImGui, which a native Windows (MSVC ABI) link can't
  // import from the runtime DLL ("undefined symbol: struct ImGuiContext *GImGui").
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g || id == 0 || g->InputTextState.ID != id || g->ActiveId != id) return;
  ImGuiInputTextState* state = &g->InputTextState;
  const std::string_view shown(state->TextA.Data ? state->TextA.Data : "",
                               state->TextA.Data ? static_cast<size_t>(state->TextLen) : 0);
  if (shown == text) return;
  state->ReloadUserBufAndSelectAll();  // the old text is gone: typing replaces the new one
  const size_t n = std::strlen(text);
  state->TextToRevertTo.resize(static_cast<int>(n + 1));
  std::memcpy(state->TextToRevertTo.Data, text, n + 1);
}

// The stored keys into every action row nobody is typing in.
void RefreshF2Texts() {
  g_f2_text_gen = Generation();
  const KbmSet set = CurrentSet();
  for (int a = 0; a < kActionCount; ++a)
    if (!g_f2_edited[a])  // typed and not applied yet: kept
      std::snprintf(g_f2_text[a], sizeof(g_f2_text[a]), "%s", KeysText(set, static_cast<Action>(a)).c_str());
}

// Applies action row `a`'s typed keys; true when the keys changed.
bool ApplyF2Text(int a) {
  g_f2_edited[a] = false;
  const Action action = static_cast<Action>(a);
  const KbmSet set = CurrentSet();
  KbmSet next = set;
  std::string note;
  bool ok = AssignText(next, action, g_f2_text[a], &note);
  for (int s = 0; ok && s < kSlots; ++s)
    if (const std::string why = ReservedReason(next.key[a][s]); next.key[a][s] && !why.empty()) {
      note = KeyName(next.key[a][s]) + " can't be bound (" + why + ").";
      ok = false;
    }
  g_f2_notes[a] = note;
  const bool changed = ok && next != set;
  if (changed) {
    Store(next);
    REXLOG_INFO("cod3: key binds: {} = {} (F2){}", Def(action).id, KeysText(next, action),
                note.empty() ? "" : " " + note);
  }
  // At once, not at the next frame: a row clicked further down this frame
  // starts from what is stored now (a key that moved away from it is gone).
  RefreshF2Texts();
  return changed;
}

// Applies a controller row's typed keys in the SDK's spelling; true when the
// row changed.
bool ApplyPadText(const std::string& cvar, std::string_view typed) {
  const std::string current = rex::cvar::GetFlagByName(cvar);
  std::string value = sdk::Spelled(typed);
  if (sdk::SameName(typed, current)) {
    // The keys that are stored (Escape, or typed again): kept as written,
    // unless a name in them is one the SDK can't read in that spelling.
    value = sdk::Spelled(current);
  }
  if (value == current) return false;
  rex::cvar::SetFlagByName(cvar, value);
  REXLOG_INFO("cod3: key binds: {} = {} (F2)", cvar, value);
  return true;
}

}  // namespace

bool CommitF2Edits() {
  bool changed = false;
  for (int a = 0; a < kActionCount; ++a)
    if (g_f2_edited[a]) changed |= ApplyF2Text(a);
  for (const auto& [cvar, text] : g_pad_edits) changed |= ApplyPadText(cvar, text);
  g_pad_edits.clear();
  // A stored name the SDK can't read as written (space, UPARROW) but can in
  // its own spelling: F2 shows it in capitals like the rest, so it looks
  // right; it is written in that spelling now.
  for (const std::string& cvar : g_pad_rows) {
    const std::string stored = rex::cvar::GetFlagByName(cvar);
    const std::string spelled = sdk::Spelled(stored);
    if (spelled == stored) continue;
    rex::cvar::SetFlagByName(cvar, spelled);
    REXLOG_INFO("cod3: key binds: {} = {} (F2, was {})", cvar, spelled, stored);
    changed = true;
  }
  return changed;
}

bool DrawPadKeyRow(const char* cvar) {
  if (g_pad_rows.find(std::string_view(cvar)) == g_pad_rows.end()) g_pad_rows.emplace(cvar);
  auto edit = g_pad_edits.find(std::string_view(cvar));
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s",
                edit != g_pad_edits.end() ? edit->second.c_str() : UpperText(rex::cvar::GetFlagByName(cvar)).c_str());
  ImGui::PushID(cvar);
  ImGui::SetNextItemWidth(-1);
  if (edit == g_pad_edits.end()) SyncActiveBox("##v", buf);
  const bool entered = ImGui::InputText("##v", buf, sizeof(buf),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
  if (ImGui::IsItemEdited()) g_pad_edits[cvar] = buf;
  bool changed = false;
  if (entered || ImGui::IsItemDeactivated()) {
    if (auto it = g_pad_edits.find(std::string_view(cvar)); it != g_pad_edits.end()) {
      changed = ApplyPadText(it->first, it->second);
      g_pad_edits.erase(it);
    }
  }
  ImGui::PopID();
  return changed;
}

bool DrawF2Rows() {
  EnsureStarted();
  bool changed = false;
  if (g_f2_text_gen != Generation()) RefreshF2Texts();
  auto help = [](const char* line) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(line);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  };
  ImGui::TextUnformatted("Actions");
  bool actions = REXCVAR_GET(cod3_binds_actions);
  if (ImGui::Checkbox("Keys call the game's actions", &actions)) {
    rex::cvar::SetFlagByName("cod3_binds_actions", actions ? "true" : "false");
    changed = true;
  }
  help("On: while you play, these keys call the game's own actions directly (not a controller button), "
       "so every action can have a key of its own and a controller keeps working at the same time; the "
       "movement keys are further down. The game's menus take the keys as they always have. Off: the "
       "keyboard plays as a controller (every key presses a controller button), and the controller "
       "button rows show further down to change them.");
  bool toggle = REXCVAR_GET(cod3_aim_toggle_kbm_p1);
  if (ImGui::Checkbox("Aim toggle (keyboard and mouse)", &toggle)) {
    rex::cvar::SetFlagByName("cod3_aim_toggle_kbm_p1", toggle ? "true" : "false");
    changed = true;
  }
  help("Off: the aim key aims while held. On: each press aims or stops aiming. The controller has its own "
       "TOGGLE ADS in the game's Options > Controls.");
  help("Vehicle: keys that count only while you drive (the tank, a jeep); the vehicle reads them itself. On foot "
       "the same keys keep their own actions (MOUSE2 aims, W walks).");
  help("Up to two keys each, separated by a comma (B, MOUSE4); empty = none. What you type applies on Enter, "
       "when you click anywhere else (Save too) or close F2. Key names: "
       "A-Z, 0-9, SHIFT, CTRL, ALT, SPACE, TAB, ENTER, UPARROW ..., MOUSE1-MOUSE5, MWHEELUP, MWHEELDOWN. "
       "No key combinations (Shift+key). The console's bind K +reload (or bind K reload) works too.");
  if (ImGui::BeginTable("##bind_actions", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    Group group = Group::kCount;
    for (int a = 0; a < kActionCount; ++a) {
      const Action action = static_cast<Action>(a);
      const ActionDef& def = Def(action);
      if (def.group != group) {
        group = def.group;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ui::CurrentPalette().accent_text, "%s", GroupName(group));
        ImGui::TableNextColumn();
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (g_action_ok[a].load(std::memory_order_relaxed)) {
        ImGui::TextUnformatted(def.label);
      } else {
        ImGui::TextDisabled("%s (not in this game)", def.label);
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", def.help);
      ImGui::TableNextColumn();
      ImGui::PushID(def.id);
      ImGui::SetNextItemWidth(-1);
      if (!g_f2_edited[a]) SyncActiveBox("##keys", g_f2_text[a]);
      const bool entered = ImGui::InputText("##keys", g_f2_text[a], sizeof(g_f2_text[a]),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
      if (ImGui::IsItemEdited()) g_f2_edited[a] = true;
      if ((entered || ImGui::IsItemDeactivated()) && g_f2_edited[a]) changed |= ApplyF2Text(a);
      ImGui::PopID();
      if (!g_f2_notes[a].empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::CurrentPalette().warn, "%s", g_f2_notes[a].c_str());
        ImGui::PopTextWrapPos();
      }
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  // hud_overlay.cpp shows only the movement rows while the keys call the actions.
  ImGui::TextUnformatted(REXCVAR_GET(cod3_binds_actions) ? "Movement" : "Controller buttons");
  return changed;
}

}  // namespace cod3::binds

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// The pad poll (once before every frame, the game's main thread; cod3mp's
// through game/mp/mp_aliases.h): the keys' presses go in first (controller
// buttons inside an interaction), the game reads its pads, then the direct
// calls.
REX_EXTERN(__imp__sub_82522138);
REX_HOOK_RAW(sub_82522138) {
  cod3::binds::BeforePadPoll(ctx, base);
  __imp__sub_82522138(ctx, base);
  cod3::binds::AfterPadPoll(ctx, base);
}

// ButtonAlias_Value (r3 the alias context: *keybinds::kAliasContexts +
// context * 328, r4 the controller, r5 the alias id -> r3 0..255; cod3mp's
// copy 0x824829C0 through mp/config/campaign_map.tsv): the jeep and the tank
// read their controls through it every frame while the player drives. A
// Vehicle action held for the alias answers it (AnswerAlias).
REX_EXTERN(__imp__sub_82467710);
REX_HOOK_RAW(sub_82467710) {
  const int32_t alias = ctx.r5.s32;
  const int context = cod3::binds::NoteVehicleRead(base, ctx.r3.u32, ctx.r4.u32, alias);
  __imp__sub_82467710(ctx, base);
  if (context >= 0) cod3::binds::AnswerAlias(context, alias, &ctx.r3.u64);
}

// The vehicle view switch (config/default.toml's [[midasm_hook]]; cod3mp's
// through mp/config/campaign_map.tsv): right after the camera select's read of
// the right stick click's press (campaign 0x82448084 after bl 0x820E86A8 in
// 0x82447D48; cod3mp 0x82462F48 in 0x82462C68), r3 is that press; a
// Vehicle: change view key press adds one.
void Cod3VehicleViewPressed(PPCRegister& r3, PPCRegister& r29, uint8_t* base) {
  if (cod3::binds::TakeVehicleViewPress(base, r29.u32)) r3.u64 = 1;
}
