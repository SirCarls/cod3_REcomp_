// cod3 - [acct2-main] the controller's own binds in game (see pad_binds.h).

#include "game/keybinds/pad_binds.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/keybinds/binds.h"
#include "game/keybinds/model.h"
#include "game/keybinds/pad_model.h"
#include "game/program.h"

REXCVAR_DEFINE_STRING(cod3_binds_pad_p1, "", "cod3/Binds",
                      "Player 1's controller buttons that differ from the controller layout, up to three "
                      "actions each, e.g. x=reload,melee;dpad_up= (empty = the layout; START stays the pause "
                      "menu). In the campaign the player is player 1");
REXCVAR_DEFINE_STRING(cod3_binds_pad_p2, "", "cod3/Binds",
                      "Multiplayer split screen: player 2's controller buttons (as cod3_binds_pad_p1)");
REXCVAR_DEFINE_STRING(cod3_binds_pad_p3, "", "cod3/Binds",
                      "Multiplayer split screen: player 3's controller buttons (as cod3_binds_pad_p1)");
REXCVAR_DEFINE_STRING(cod3_binds_pad_p4, "", "cod3/Binds",
                      "Multiplayer split screen: player 4's controller buttons (as cod3_binds_pad_p1)");

// [acct2-main] 2026-11-05 (owner 09:13: the stick dead zone per player). -1 = the game's joy_threshold
// (F2 > Controls > Controller), which every controller shares.
REXCVAR_DEFINE_INT32(cod3_deadzone_p1, -1, "cod3/Binds",
                     "Player 1's stick dead zone, in the game's units of 128 per axis (0-100; the game's own is 25): "
                     "a push smaller than this counts as nothing. -1 = joy_threshold, shared by every controller. "
                     "In the campaign the player is player 1");
REXCVAR_DEFINE_INT32(cod3_deadzone_p2, -1, "cod3/Binds",
                     "Multiplayer split screen: player 2's stick dead zone (as cod3_deadzone_p1)");
REXCVAR_DEFINE_INT32(cod3_deadzone_p3, -1, "cod3/Binds",
                     "Multiplayer split screen: player 3's stick dead zone (as cod3_deadzone_p1)");
REXCVAR_DEFINE_INT32(cod3_deadzone_p4, -1, "cod3/Binds",
                     "Multiplayer split screen: player 4's stick dead zone (as cod3_deadzone_p1)");

namespace cod3::binds::padbinds {
namespace {

using guest::Call;
using guest::ReadString;
using guest::ReadU16;
using guest::ReadU32;
using guest::ReadU8;
using guest::WriteU32;
namespace kb = addr::keybinds;

// The game's functions (the same code in both programs; codmp_xenonf.xex's
// from its names table, checked in the translated code 2026-11-05):
//   Key_SetBinding(r3 key, r4 unused, r5 text) for the active local client;
//     it copies the text and refreshes the pad poll's cached command.
//   Cbuf_ExecuteText(r3 when, r4 text): 0 = now (Cmd_ExecuteString).
#if defined(COD3_MP)
constexpr uint32_t kKeySetBinding = 0x82528550;
constexpr uint32_t kCbufExecuteText = 0x824A2900;
#else
constexpr uint32_t kKeySetBinding = 0x8250CC88;
constexpr uint32_t kCbufExecuteText = 0x8247E498;
#endif
constexpr uint32_t kExecNow = 0;
constexpr uint32_t kTypePlain = 1;   // cmd_function_t +8 (the pad poll's own test, 0x82464248)
constexpr uint32_t kTypeButton = 2;
constexpr int kPlayers = 4;
constexpr int kSyncEvery = 15;  // pad polls between two looks at the bind tables
constexpr uint8_t kTriggerPressed = 30;

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

// The pad state's bit for each pad::Button (X_INPUT_GAMEPAD buttons; the
// triggers above the 16 buttons).
constexpr uint32_t kBits[pad::kButtonCount] = {
    0x1000, 0x2000, 0x4000, 0x8000,  // A B X Y
    0x0100, 0x0200,                  // LB RB
    1u << 16, 1u << 17,              // LT RT
    0x0040, 0x0080,                  // LS RS
    0x0001, 0x0002, 0x0004, 0x0008,  // the D-pad
    0x0020, 0x0010,                  // BACK START
};

// --- the raw pad state, per controller (GetPadState hook) -------------------------------
std::array<uint32_t, 4> g_raw{};

// --- per player (game main thread) -----------------------------------------------------
struct Player {
  std::string text = "\x01";  // the setting text `set` was read from
  pad::PadSet set;
  // The layout's binding of each button, as the game ran it, and what the
  // port wrote over it (wrote = the table holds `written`).
  std::array<std::string, pad::kButtonCount> layout;
  std::array<std::string, pad::kButtonCount> written;
  std::array<bool, pad::kButtonCount> wrote{};
  bool have_layout = false;
  // Second and third actions held, per button and slot (kCount = none).
  std::array<std::array<Action, pad::kPadSlots>, pad::kButtonCount> held;
  uint32_t prev_bits = 0;
  bool active = false;  // in use at the last sync
  Player() {
    for (auto& h : held) h.fill(Action::kCount);
  }
};
std::array<Player, kPlayers> g_players;
uint32_t g_poll = 0;

std::string SettingText(int player) {
  switch (player) {
    case 0: return REXCVAR_GET(cod3_binds_pad_p1);
    case 1: return REXCVAR_GET(cod3_binds_pad_p2);
    case 2: return REXCVAR_GET(cod3_binds_pad_p3);
    default: return REXCVAR_GET(cod3_binds_pad_p4);
  }
}

// The local clients that play: the campaign's one; cod3mp's in use (state != 0).
std::vector<uint32_t> Clients(uint8_t* base) {
  std::vector<uint32_t> out;
  if (!cod3::kMultiplayer || !kb::kLocalClientState) {
    const uint32_t cl = ReadU32(base, addr::kActiveLocalClient);
    out.push_back(cl < 4 ? cl : 0);
    return out;
  }
  for (uint32_t cl = 0; cl < kPlayers; ++cl)
    if (ReadU32(base, kb::kLocalClients + cl * kb::kLocalClientStride + kb::kLocalClientState) != 0)
      out.push_back(cl);
  return out;
}

// The player number of a local client: cod3mp's client index (split screen
// player 1-4); the campaign's player is player 1.
int PlayerOf(uint32_t cl) { return cod3::kMultiplayer ? int(cl & 3) : 0; }

std::string Binding(uint8_t* base, uint32_t cl, int key) {
  const uint32_t text = ReadU32(base, kb::kKeys + cl * kb::kKeysStride + uint32_t(key) * 8 + 4);
  return Valid(text) ? ReadString(base, text, 96) : std::string();
}

void SetBinding(PPCContext& ctx, uint8_t* base, uint32_t cl, int key, const std::string& text) {
  const uint32_t active = ReadU32(base, addr::kActiveLocalClient);
  WriteU32(base, addr::kActiveLocalClient, cl);
  {
    Call c(ctx, base);
    c.Invoke(kKeySetBinding, {uint64_t(key), 0, c.PushString(text)});
  }
  WriteU32(base, addr::kActiveLocalClient, active);
}

pad::Layout LayoutActions(const Player& p) {
  pad::Layout l;
  for (int b = 0; b < pad::kButtonCount; ++b) l.binds[b].act[0] = ActionByCommand(p.layout[b]);
  return l;
}

// The first actions into the player's bind table; the layout taken again when
// the game ran it since.
void Sync(PPCContext& ctx, uint8_t* base, uint32_t cl, bool log) {
  Player& p = g_players[cl & 3];
  for (int b = 0; b < pad::kButtonCount; ++b) {
    const auto button = static_cast<pad::Button>(b);
    if (!pad::Changeable(button)) continue;
    const std::string cur = Binding(base, cl, pad::GameKey(button));
    if (!p.wrote[b] || cur != p.written[b]) {
      p.layout[b] = cur;  // the game's (its layout file, a menu, the console)
      p.wrote[b] = false;
    }
  }
  p.have_layout = true;
  const pad::Layout layout = LayoutActions(p);
  for (int b = 0; b < pad::kButtonCount; ++b) {
    const auto button = static_cast<pad::Button>(b);
    if (!pad::Changeable(button)) continue;
    const int key = pad::GameKey(button);
    if (p.set.custom[b]) {
      const Action first = pad::Effective(p.set, layout, button).act[0];
      const std::string want = first == Action::kCount ? std::string() : pad::BindText(first);
      if (!p.wrote[b] || p.written[b] != want) {
        SetBinding(ctx, base, cl, key, want);
        p.written[b] = Binding(base, cl, key);  // as the table holds it
        p.wrote[b] = true;
        if (log)
          REXLOG_INFO("cod3: controller binds: player {} {} = \"{}\" (layout \"{}\")", PlayerOf(cl) + 1,
                      pad::ButtonId(button), want, p.layout[b]);
      }
    } else if (p.wrote[b]) {
      SetBinding(ctx, base, cl, key, p.layout[b]);  // back to the layout
      p.wrote[b] = false;
      if (log)
        REXLOG_INFO("cod3: controller binds: player {} {} back to the layout (\"{}\")", PlayerOf(cl) + 1,
                    pad::ButtonId(button), p.layout[b]);
    }
  }
}

// A command of the game's command list: its handler and kind.
uint32_t FindCommand(uint8_t* base, std::string_view name, uint32_t* type) {
  int guard = 0;
  for (uint32_t cmd = ReadU32(base, addr::kCmdFunctions); Valid(cmd) && guard < 4096;
       cmd = ReadU32(base, cmd), ++guard) {
    const uint32_t text = ReadU32(base, cmd + 4);
    if (!Valid(text)) continue;
    const std::string n = ReadString(base, text, 48);
    if (n.size() != name.size()) continue;
    bool same = true;
    for (size_t i = 0; i < n.size() && same; ++i)
      same = (n[i] | 0x20) == (name[i] | 0x20);
    if (!same) continue;
    *type = ReadU32(base, cmd + 8);
    return ReadU32(base, cmd + 12);
  }
  return 0;
}

// One second or third action, as the pad poll calls a button's command
// (0x82464248: a button command gets the key and the time, a plain one
// nothing); console text at once for this player. Game thread, the player
// being the active local client (binds::AsPlayer).
// Returns how it ran (for the debug log; nullptr = nothing to do).
const char* Deliver(PPCContext& ctx, uint8_t* base, Action action, bool down, int key, uint32_t now) {
  const std::string text = pad::BindText(action);
  if (text.empty()) return nullptr;
  const bool plus = text[0] == '+';
  if (!down && !plus) return nullptr;  // only button commands have a release
  std::string name = text;
  const bool has_args = name.find(' ') != std::string::npos;
  if (!down) name[0] = '-';
  uint32_t type = 0;
  const uint32_t handler = has_args ? 0 : FindCommand(base, name, &type);
  if (handler && type == kTypeButton) {
    Call(ctx, base).Invoke(handler, {uint64_t(key), now});
    return "button command";
  }
  if (handler && type == kTypePlain) {
    Call(ctx, base).Invoke(handler);
    return "command";
  }
  std::string line = down ? text : (std::string("-") + text.substr(1));
  line += '\n';
  Call c(ctx, base);
  c.Invoke(kCbufExecuteText, {kExecNow, c.PushString(line)});
  return "console, at once";
}

// [acct2-main] One line per second / third action (debug log): which player,
// which button, and the local client the game had active while it ran, so a
// split-screen test can see each player's actions stay that player's.
void LogDelivery(uint8_t* base, uint32_t cl, pad::Button button, int slot, Action action, bool down,
                 const char* how) {
  if (!how) return;
  REXLOG_DEBUG("cod3: controller binds: player {} {} slot {} {} \"{}\" ({}; active client {})", PlayerOf(cl) + 1,
               pad::ButtonId(button), slot + 1, down ? "down" : "up", pad::BindText(action), how,
               ReadU32(base, addr::kActiveLocalClient));
}

void ReleaseHeld(PPCContext& ctx, uint8_t* base, uint32_t cl, uint32_t now) {
  Player& p = g_players[cl & 3];
  bool any = false;
  for (auto& h : p.held)
    for (Action a : h) any |= a != Action::kCount;
  if (!any) return;
  binds::AsPlayer(base, cl, [&]() {
    for (int b = 0; b < pad::kButtonCount; ++b)
      for (Action& a : p.held[b]) {
        if (a == Action::kCount) continue;
        Deliver(ctx, base, a, false, pad::GameKey(static_cast<pad::Button>(b)), now);  // a setting change
        a = Action::kCount;
      }
  });
}

// --- the stick dead zone per player -----------------------------------------------------
// The game's dead zone function (campaign 0x82519D98, cod3mp 0x82537B20; the same code) takes one
// stick axis (-128..127) and returns 0 below joy_threshold, else the rest stretched back to the full
// range. It reads the threshold from joy_threshold's cvar_t (value, a float at +28) on every call, and
// nothing else of it. Its callers: the pad poll, for each axis of each local player's sticks (cod3mp's
// poll makes each player the active local client first: stw r20 to 0x82BCBE9C), the jeep's and the
// tank's controls, and one more stick reader. So: while it runs, the player's own value is in that
// float, and the shared value goes back right after. Game main thread only, nothing else reads it in
// between. The player is the active local client (the campaign's: player 1).
#if defined(COD3_MP)
constexpr uint32_t kJoyThresholdCvar = 0x82B3ED8C;  // cvar_t* (the dead zone function: lwz -4724 of 0x82B4)
#else
constexpr uint32_t kJoyThresholdCvar = 0x829C2968;  // cvar_t* (config/symbols.toml: joy_threshold)
#endif
constexpr uint32_t kCvarValue = 28;  // float
constexpr int kDeadZoneMax = 100;    // of 128, as the F2 slider's

int DeadZoneSetting(int player) {
  switch (player) {
    case 0: return REXCVAR_GET(cod3_deadzone_p1);
    case 1: return REXCVAR_GET(cod3_deadzone_p2);
    case 2: return REXCVAR_GET(cod3_deadzone_p3);
    default: return REXCVAR_GET(cod3_deadzone_p4);
  }
}

std::array<int, kPlayers> g_dz_logged = {-2, -2, -2, -2};  // the value last announced
std::array<std::chrono::steady_clock::time_point, kPlayers> g_dz_debug{};

}  // namespace

// For one call of the dead zone function (the hooks at the end of this file).
class DeadZoneSwap {
 public:
  DeadZoneSwap(PPCContext& ctx, uint8_t* base) : ctx_(ctx), base_(base), in_(ctx.r3.s32) {
    const uint32_t active = cod3::kMultiplayer ? ReadU32(base, addr::kActiveLocalClient) : 0;
    player_ = active < uint32_t(kPlayers) ? int(active) : 0;
    const int set = DeadZoneSetting(player_);
    const int units = set < 0 ? -1 : std::min(set, kDeadZoneMax);
    if (units != g_dz_logged[player_]) {
      g_dz_logged[player_] = units;
      if (units < 0)
        REXLOG_INFO("cod3: stick dead zone: player {} uses joy_threshold (shared)", player_ + 1);
      else
        REXLOG_INFO("cod3: stick dead zone: player {} uses {} of 128 ({:.0f}% of a full push)", player_ + 1, units,
                    units * 100.0 / 128.0);
    }
    if (units < 0) return;
    cvar_ = ReadU32(base, kJoyThresholdCvar);
    if (!Valid(cvar_)) return;
    saved_ = ReadU32(base, cvar_ + kCvarValue);
    shared_ = std::bit_cast<float>(saved_);
    WriteU32(base, cvar_ + kCvarValue, std::bit_cast<uint32_t>(float(units)));
    units_ = units;
  }
  ~DeadZoneSwap() {
    if (units_ < 0) return;
    WriteU32(base_, cvar_ + kCvarValue, saved_);
    // The debug log: at most a line every 2 s per player, for a stick that is pushed.
    if (in_ == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_dz_debug[player_] < std::chrono::seconds(2)) return;
    g_dz_debug[player_] = now;
    REXLOG_DEBUG("cod3: stick dead zone: player {} {} of 128 (shared {:.0f}): axis {} -> {}", player_ + 1, units_,
                 shared_, in_, ctx_.r3.s32);
  }
  DeadZoneSwap(const DeadZoneSwap&) = delete;
  DeadZoneSwap& operator=(const DeadZoneSwap&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  int32_t in_;
  int player_ = 0;
  int units_ = -1;
  uint32_t cvar_ = 0;
  uint32_t saved_ = 0;
  float shared_ = 0;
};

void OnPadState(uint8_t* base, uint32_t user, uint32_t state) {
  if (user >= 4) return;
  uint32_t bits = ReadU16(base, state + 4);
  if (ReadU8(base, state + 6) >= kTriggerPressed) bits |= 1u << 16;
  if (ReadU8(base, state + 7) >= kTriggerPressed) bits |= 1u << 17;
  g_raw[user] = bits;
}

void BeforePoll(PPCContext& ctx, uint8_t* base) {
  ++g_poll;
  const std::vector<uint32_t> clients = Clients(base);
  std::array<bool, kPlayers> in_use{};
  for (uint32_t cl : clients) in_use[cl & 3] = true;
  for (uint32_t cl : clients) {
    Player& p = g_players[cl & 3];
    const std::string text = SettingText(PlayerOf(cl));
    const bool changed = text != p.text;
    if (changed) {
      // Actions held from the old binds let go first.
      ReleaseHeld(ctx, base, cl, static_cast<uint32_t>(Call(ctx, base).Invoke(kb::kSysMilliseconds)));
      std::vector<std::string> notes;
      p.set = pad::Parse(text, &notes);
      p.text = text;
      for (const std::string& n : notes) REXLOG_WARN("cod3: controller binds: player {}: {}", PlayerOf(cl) + 1, n);
    }
    if (changed || !p.active || g_poll % kSyncEvery == 0) Sync(ctx, base, cl, changed);
    p.active = true;
  }
  // A player that left (split screen) keeps nothing held, and starts again.
  for (int i = 0; i < kPlayers; ++i)
    if (!in_use[i] && g_players[i].active) {
      g_players[i].active = false;
      g_players[i].prev_bits = 0;
      for (auto& h : g_players[i].held) h.fill(Action::kCount);
    }
}

void AfterPoll(PPCContext& ctx, uint8_t* base) {
  const std::vector<uint32_t> clients = Clients(base);
  uint32_t now = 0;
  bool have_now = false;
  for (uint32_t cl : clients) {
    Player& p = g_players[cl & 3];
    if (!p.have_layout) continue;
    const uint32_t pad = binds::PadOfPlayer(base, cl);
    const uint32_t bits = pad < 4 ? g_raw[pad] : 0;
    const uint32_t changed = bits ^ p.prev_bits;
    p.prev_bits = bits;
    if (!changed) continue;
    bool extra = false;
    for (int b = 0; b < pad::kButtonCount && !extra; ++b)
      extra = (changed & kBits[b]) && p.set.custom[b] &&
              (p.set.binds[b].act[1] != Action::kCount || p.held[b][1] != Action::kCount ||
               p.held[b][2] != Action::kCount);
    if (!extra) continue;
    if (!have_now) {
      now = static_cast<uint32_t>(Call(ctx, base).Invoke(kb::kSysMilliseconds));
      have_now = true;
    }
    const bool takes = binds::PlayerTakesPresses(ctx, base, cl);
    binds::AsPlayer(base, cl, [&]() {
      for (int b = 0; b < pad::kButtonCount; ++b) {
        if (!(changed & kBits[b])) continue;
        const auto button = static_cast<pad::Button>(b);
        const int key = pad::GameKey(button);
        const bool down = bits & kBits[b];
        for (int s = 1; s < pad::kPadSlots; ++s) {
          Action& held = p.held[b][s];
          if (!down) {
            if (held != Action::kCount)
              LogDelivery(base, cl, button, s, held, false, Deliver(ctx, base, held, false, key, now));
            held = Action::kCount;
            continue;
          }
          const Action a = p.set.custom[b] ? p.set.binds[b].act[s] : Action::kCount;
          if (a == Action::kCount || held != Action::kCount) continue;
          if (!takes) {
            REXLOG_DEBUG("cod3: controller binds: player {} {} slot {} \"{}\" not sent (a menu, the chat or an "
                         "interaction has the presses)", PlayerOf(cl) + 1, pad::ButtonId(button), s + 1,
                         pad::BindText(a));
            continue;
          }
          LogDelivery(base, cl, button, s, a, true, Deliver(ctx, base, a, true, key, now));
          held = a;
        }
      }
    });
  }
}

}  // namespace cod3::binds::padbinds

// [acct2-main] The game's stick dead zone function (see DeadZoneSwap). Not in mp/config/campaign_map.tsv:
// each program hooks its own copy.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_82537B20);
REX_HOOK_RAW(sub_82537B20) {
  const cod3::binds::padbinds::DeadZoneSwap swap(ctx, base);
  __imp__sub_82537B20(ctx, base);
}
#else
REX_EXTERN(__imp__sub_82519D98);
REX_HOOK_RAW(sub_82519D98) {
  const cod3::binds::padbinds::DeadZoneSwap swap(ctx, base);
  __imp__sub_82519D98(ctx, base);
}
#endif
