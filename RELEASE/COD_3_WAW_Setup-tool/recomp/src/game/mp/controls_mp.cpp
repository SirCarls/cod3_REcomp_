// cod3mp: the player's keys, controls and crosshair as the campaign has them
// (controls_mp.h). Owner, 2026-10-19: "Crosshairs and controls are not being
// carried over / Grenade and special grenade's activations get switched in MP".
//
// Grenade keys
//   The keyboard presses controller buttons (the SDK's keybind_* rows), and
//   F2 names its rows for the campaign's layout: "Grenade (RB)" is G,
//   "Smoke grenade (LB)" is 4 (xenon_a.cfg binds the right shoulder to
//   +grenadeattack, the left one to +smokegrenade). Multiplayer runs the
//   same layout file and then its own mp_xenon_<layout>.cfg on top
//   (Controls_ExecLayoutFiles 0x82507AD8), which can give the frag and the
//   special grenade the other shoulder; then G threw the special grenade
//   and 4 the frag. After every
//   change to player 1's shoulder binds (Key_SetBinding), the commands are
//   read from the game's bind table and the two rows are pointed at the
//   buttons that hold +grenadeattack and +smokegrenade (SDK patch 0056:
//   only the keyboard's presses move; a controller keeps the layout).
//
// Controls
//   The campaign's Multiplayer item hands its 664-byte control settings
//   (invert, sensitivity, layout, crosshair ...) to this program as launch
//   data, and MP_ReadLaunchData copies them into the player's controller
//   profile (0x82C6B730 + profile * 6984). When that profile's own saved
//   settings are read afterwards (Profile_SettingsReadDone 0x824178E0), the
//   save would replace them; the campaign's are put back on top of the
//   first such read of the session, so a switch from the campaign keeps the
//   player's controls. Later reads (after a change in multiplayer's own
//   options) are left alone, and so is a read that failed (the game only
//   shows a message then and may read again). Both steps are logged with
//   the values.
//
// Crosshair
//   Multiplayer has no cg_drawCrosshair dvar: its crosshair is drawn while
//   byte +57 of the local player's profile is set (0x82452920). The
//   campaign keeps the player's last choice in its console config
//   (game/settings_sync.h); it is copied into this program's console config
//   at start, which creates the dvar in the game at the first frame, so the
//   same settings_sync keeps it and player 1's profile byte together here
//   (and applies it again at every map this machine loads as the server,
//   its SV_SpawnServer hook). Without a choice in either config, the first
//   value known is taken as it is: the campaign's handed-over byte, else
//   player 1's profile as first read. A profile read puts the choice back
//   on the byte it loads, so a load is never mistaken for the player's
//   choice; a change made in multiplayer's own options is written back to
//   the campaign's console config.

#include "game/mp/controls_mp.h"
#include "game/mp/controls_rules.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/guest.h"
#include "game/keybinds/binds.h"  // [claude-9b] the special grenade key follows the layout
#include "util.h"

namespace {

namespace a = cod3::addr;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;

// --- The game's bind table (codmp_xenonf.xex) ---------------------------------------
// Key_SetBinding 0x82528550: keys 0x82C35030 + local client * 2052 + key * 8,
// the binding's text pointer at +4 (the setter 0x82526F30 frees and copies it).
constexpr uint32_t kBindTable = 0x82C35030;
constexpr uint32_t kBindTableStride = 2052;
constexpr uint32_t kBindEntrySize = 8;
constexpr uint32_t kBindText = 4;
constexpr uint32_t kBindKeys = 256;

// The game's key numbers for the controller's buttons (the pad poll's
// button -> key table, the same code in both programs: 0x8251DA00 in the
// campaign, 0x8253A7B8 here; dev/agents/keybinds/PLAN.md section 2).
constexpr uint32_t kKeyRightShoulder = 217;
constexpr uint32_t kKeyLeftShoulder = 218;
struct PadKey {
  uint32_t key;
  const char* name;
};
constexpr PadKey kPadKeys[] = {
    {215, "RT"}, {216, "LT"},     {217, "RB"},     {218, "LB"},     {212, "A"},      {213, "B"},
    {211, "X"},  {214, "Y"},      {219, "RS"},     {220, "LS"},     {27, "START"},   {9, "BACK"},
    {154, "D-pad 154"}, {155, "D-pad 155"}, {156, "D-pad 156"}, {157, "D-pad 157"},
};

// --- The controller profiles' settings ------------------------------------------
// One block per profile (addr::controls::kSettings, stride 6984); its first
// 664 bytes are the control settings both programs share (the launch data's
// size; MP_ReadLaunchData copies exactly those). Fields as in the campaign's
// block (addresses.h `controls`; layout +76 from dev/agents/keybinds/PLAN.md).
constexpr uint32_t kProfiles = 4;
constexpr uint32_t kProfileStride = 6984;
constexpr uint32_t kControlsSize = 664;
constexpr uint32_t kLayout = 76;
// Set by MP_ReadLaunchData when the launch data held the campaign's block.
constexpr uint32_t kLaunchedWithControls = 0x82B793DF;
// The profile read's request (Profile_SettingsReadDone's argument): its status (0 = read)
// and which profile it is for.
constexpr uint32_t kReadStatus = 8;
constexpr uint32_t kReadProfile = 0x154;

bool ValidText(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

uint32_t ProfileBlock(uint32_t profile) { return a::controls::kSettings + profile * kProfileStride; }

// The command bound to `key` for local client `client` ("" when none).
std::string Binding(uint8_t* base, uint32_t client, uint32_t key) {
  if (client >= kProfiles || key >= kBindKeys) return {};
  const uint32_t entry = kBindTable + client * kBindTableStride + key * kBindEntrySize;
  const uint32_t text = ReadU32(base, entry + kBindText);
  return ValidText(text) ? cod3::guest::ReadString(base, text, 96) : std::string();
}

// --- Grenade keys -----------------------------------------------------------------
using cod3::mpcontrols::GrenadeKeys;
std::atomic<GrenadeKeys> g_grenade_keys{GrenadeKeys::kUnknown};

// Game thread: player 1's shoulder binds -> the keyboard's two grenade rows.
// `quiet` while a layout file binds one key after another (its hook logs the
// result once it has run).
void UpdateGrenadeKeys(uint8_t* base, bool quiet) {
  const std::string rb = Binding(base, 0, kKeyRightShoulder);
  const std::string lb = Binding(base, 0, kKeyLeftShoulder);
  const GrenadeKeys now = cod3::mpcontrols::GrenadeKeysFor(rb, lb);
  // [claude-9b] The keyboard's special grenade key calls the other shoulder's command than the
  // frag grenade's (layout A: RB +class), its first command when it holds several.
  {
    using cod3::mpcontrols::BindingIsCommand;
    using cod3::mpcontrols::kFragCommand;
    const std::string& other = BindingIsCommand(rb, kFragCommand)   ? lb
                               : BindingIsCommand(lb, kFragCommand) ? rb
                                                                    : std::string();
    std::string first = other.substr(0, other.find_first_of("; "));
    if (first.size() < 2 || first[0] != '+') first.clear();
    for (char& c : first) c = char(std::tolower(static_cast<unsigned char>(c)));
    cod3::binds::SetMpSpecialCommand(first);
  }
  // Anything else (a bind being rewritten, a layout with the grenades
  // elsewhere): the rows press the buttons they are named for.
  const bool swap = now == GrenadeKeys::kSwapped;
  namespace mnk = rex::input::mnk;
  constexpr uint16_t kRight = rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  constexpr uint16_t kLeft = rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
  mnk::SetButtonRemap(kRight, swap ? kLeft : kRight);
  mnk::SetButtonRemap(kLeft, swap ? kRight : kLeft);
  if (quiet || g_grenade_keys.exchange(now) == now) return;
  switch (now) {
    case GrenadeKeys::kSwapped:
      REXLOG_INFO("cod3mp: controller layout: RB {}, LB {}: the keyboard's Grenade keys press LB and its "
                  "Smoke grenade keys press RB (frag and special grenade as in the campaign)",
                  rb, lb);
      break;
    case GrenadeKeys::kAsNamed:
      REXLOG_INFO("cod3mp: controller layout: RB {}, LB {}: the keyboard's grenade keys press the buttons "
                  "they are named for",
                  rb, lb);
      break;
    default:
      REXLOG_INFO("cod3mp: controller layout: RB '{}', LB '{}' (the grenades aren't on the shoulders "
                  "now): the keyboard's grenade keys press the buttons they are named for",
                  rb, lb);
      break;
  }
}

// One line with every controller button's command for local client `client`.
std::string LayoutText(uint8_t* base, uint32_t client) {
  std::string out;
  for (const auto& k : kPadKeys) {
    const std::string b = Binding(base, client, k.key);
    if (b.empty()) continue;
    if (!out.empty()) out += ", ";
    out += k.name;
    out += ' ';
    out += b;
  }
  return out.empty() ? "(no controller binds)" : out;
}

// --- Control settings handed over by the campaign ---------------------------------
std::mutex g_handover_mutex;
std::array<uint8_t, kControlsSize> g_handover{};
std::optional<uint32_t> g_handover_profile;  // set while the first saved read is still to come
thread_local int t_reading_profile = -1;    // inside the profile read's completion
thread_local bool t_in_layout_files = false;  // inside the layout files' run (Controls_ExecLayoutFiles)

std::string DescribeControls(uint8_t* base, uint32_t block) {
  const int32_t hsens = static_cast<int32_t>(ReadU32(base, block + a::controls::kHorizontalSens));
  const int32_t vsens = static_cast<int32_t>(ReadU32(base, block + a::controls::kVerticalSens));
  const uint32_t layout = ReadU32(base, block + kLayout);
  return fmt::format("crosshair {}, invert aim {}, sensitivity {}/{}, button layout {}",
                     ReadU8(base, block + a::controls::kCrosshair) ? "on" : "off",
                     ReadU8(base, block + a::controls::kInvertAim) ? "on" : "off", hsens, vsens,
                     layout < 4 ? std::string(1, char('A' + layout)) : std::to_string(layout));
}

// --- Crosshair --------------------------------------------------------------------
std::mutex g_crosshair_mutex;
std::filesystem::path g_campaign_config;  // the campaign's console config
std::string g_crosshair_seen;             // this program's archived value as last seen ("" = none)

// The choice in this program's console config: 0, 1, or none.
std::optional<uint8_t> CrosshairChoice() {
  const auto archived = cod3::Engine::Get().ArchivedCvars();
  const auto it = archived.find(std::string(cod3::mpcontrols::kCrosshairDvar));
  if (it == archived.end()) return std::nullopt;
  return uint8_t(it->second != "0" ? 1 : 0);
}

// No choice anywhere yet: `on` (the first value known) becomes it, as the
// game has it, so settings_sync can follow the player's later changes. The
// dvar is made at once too (the first frame may have passed already); the
// value isn't written back to the campaign, which has the same.
void SeedCrosshairChoice(uint8_t on, const char* from) {
  const std::string value = on ? "1" : "0";
  {
    std::lock_guard lock(g_crosshair_mutex);
    if (CrosshairChoice()) return;
    cod3::Engine::Get().SetArchived(std::string(cod3::mpcontrols::kCrosshairDvar), value);
    g_crosshair_seen = value;
  }
  cod3::Engine::Get().QueueGuestTask([value](PPCContext& ctx, uint8_t* base) {
    cod3::Engine::CvarSet(ctx, base, std::string(cod3::mpcontrols::kCrosshairDvar).c_str(), value.c_str(),
                          /*force=*/false);
  });
  REXLOG_INFO("cod3mp: crosshair {} ({}; neither program had a choice yet)", on ? "on" : "off", from);
}

// cg_drawCrosshair in a console config (controls_rules.h); nullopt when the
// file or the line isn't there.
constexpr uintmax_t kMaxConfigBytes = 256 * 1024;

std::optional<std::string> ConfigValue(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) > kMaxConfigBytes)
    return std::nullopt;
  std::ifstream in(path);
  return cod3::mpcontrols::CrosshairInConfig(in);
}

// Writes the choice into a console config, every other line kept: a new
// file next to it, then renamed over it.
bool WriteConfigValue(const std::filesystem::path& path, const std::string& value) {
  std::string text;
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    if (std::filesystem::file_size(path, ec) > kMaxConfigBytes) return false;
    std::ifstream in(path);
    text = cod3::mpcontrols::ConfigWithCrosshair(in, value);
  } else {
    std::istringstream none;
    text = cod3::mpcontrols::ConfigWithCrosshair(none, value);
  }
  std::filesystem::path temp = path;
  temp += ".cod3mp-new";
  bool written = false;
  {
    std::ofstream out(temp, std::ios::trunc);  // text mode, as Engine::SaveConfig writes it
    if (out) {
      out << text;
      out.flush();
      written = static_cast<bool>(out);
    }
  }
  if (!written) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace

namespace cod3::mpcontrols {

void TakeCampaignCrosshair(const std::filesystem::path& campaign_config) {
  std::lock_guard lock(g_crosshair_mutex);
  g_campaign_config = campaign_config;
  const auto campaign = ConfigValue(campaign_config);
  const auto own = CrosshairChoice();
  if (campaign) {
    if (!own || *own != uint8_t(*campaign == "1"))
      Engine::Get().SetArchived(std::string(kCrosshairDvar), *campaign);
    REXLOG_INFO("cod3mp: crosshair {} (the campaign's choice, {})", *campaign == "1" ? "on" : "off",
                cod3::PathToUtf8(campaign_config));
    g_crosshair_seen = *campaign;
  } else {
    // The campaign has no choice yet: this program's own, if it has one
    // (Tick hands it to the campaign).
    if (own) REXLOG_INFO("cod3mp: crosshair {} (this program's choice; the campaign has none yet)",
                         *own ? "on" : "off");
    g_crosshair_seen.clear();
  }
}

namespace {
// A new choice in this program's console config -> the campaign's. "Seen"
// moves only once the campaign has it, so a failed write is tried again.
void WriteBack() {
  std::lock_guard lock(g_crosshair_mutex);
  if (g_campaign_config.empty()) return;
  const auto choice = CrosshairChoice();
  const std::string value = choice ? (*choice ? "1" : "0") : "";
  if (value == g_crosshair_seen) return;
  if (value.empty() || ConfigValue(g_campaign_config) == value) {
    g_crosshair_seen = value;
    return;
  }
  static std::string warned;  // one warning per value
  if (WriteConfigValue(g_campaign_config, value)) {
    g_crosshair_seen = value;
    warned.clear();
    REXLOG_INFO("cod3mp: crosshair {} chosen in multiplayer; the campaign's console config follows ({})",
                value == "1" ? "on" : "off", cod3::PathToUtf8(g_campaign_config));
  } else if (warned != value) {
    warned = value;
    REXLOG_WARN("cod3mp: couldn't write the crosshair choice into {} (trying again)",
                cod3::PathToUtf8(g_campaign_config));
  }
}
}  // namespace

void Tick() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next;
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::milliseconds(500);
  WriteBack();
}

void Flush() { WriteBack(); }

}  // namespace cod3::mpcontrols

// ---------------------------------------------------------------------------
// Hooks (codmp_xenonf.xex names; this file is cod3mp's only)
// ---------------------------------------------------------------------------

// Key_SetBinding(key, -, text): every bind, from a layout file, the console
// or a menu, for the active local client.
REX_EXTERN(__imp__Key_SetBinding);
REX_HOOK_RAW(Key_SetBinding) {
  const uint32_t key = ctx.r3.u32 & 0xFF;
  __imp__Key_SetBinding(ctx, base);
  if ((key == kKeyRightShoulder || key == kKeyLeftShoulder) && ReadU32(base, a::kActiveLocalClient) == 0)
    UpdateGrenadeKeys(base, t_in_layout_files);
}

// The controller layout's files for the active local client (layout 0-3 =
// A-D): xenon_<x>.cfg, then multiplayer's mp_xenon_<x>.cfg, run at once.
REX_EXTERN(__imp__Controls_ExecLayoutFiles);
REX_HOOK_RAW(Controls_ExecLayoutFiles) {
  const uint32_t layout = ctx.r3.u32;
  t_in_layout_files = true;
  __imp__Controls_ExecLayoutFiles(ctx, base);
  t_in_layout_files = false;
  const uint32_t client = ReadU32(base, a::kActiveLocalClient);
  if (client >= kProfiles) return;
  if (client == 0) UpdateGrenadeKeys(base, /*quiet=*/false);
  static std::mutex mutex;
  static std::array<std::string, kProfiles> logged;
  std::string text = LayoutText(base, client);
  {
    std::lock_guard lock(mutex);
    if (logged[client] == text) return;
    logged[client] = text;
  }
  REXLOG_INFO("cod3mp: player {}'s controller layout {}: {}", client + 1,
              layout >= 1 && layout <= 3 ? char('A' + layout) : 'A', text);
}

// The launch data (the campaign's control settings, see the top). The game
// copies the block into one profile (by its XUID or its profile index) and
// its +92 into all four, so the profile is the one whose block now equals
// the launch data (the SDK keeps a copy). The flag is MP_ReadLaunchData's
// own (its caller sets it to 1 again right after the call).
REX_EXTERN(__imp__MP_ReadLaunchData);
REX_HOOK_RAW(MP_ReadLaunchData) {
  __imp__MP_ReadLaunchData(ctx, base);
  if (!ReadU8(base, kLaunchedWithControls)) return;
  std::vector<uint8_t> launch;
  if (auto* kernel = REX_KERNEL_STATE())
    if (auto xam = kernel->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex"))
      if (xam->loader_data().launch_data_present) launch = xam->loader_data().launch_data;
  if (launch.size() < kControlsSize) return;
  std::optional<uint32_t> profile;
  for (uint32_t p = 0; p < kProfiles && !profile; ++p)
    if (std::memcmp(base + ProfileBlock(p), launch.data(), kControlsSize) == 0) profile = p;
  if (!profile) {
    REXLOG_WARN("cod3mp: launched by the campaign, but no controller profile holds its control settings");
    return;
  }
  {
    std::lock_guard lock(g_handover_mutex);
    std::memcpy(g_handover.data(), base + ProfileBlock(*profile), kControlsSize);
    g_handover_profile = *profile;
  }
  REXLOG_INFO("cod3mp: the campaign's control settings for profile {}: {}", *profile + 1,
              DescribeControls(base, ProfileBlock(*profile)));
  if (*profile == 0)
    SeedCrosshairChoice(ReadU8(base, ProfileBlock(0) + a::controls::kCrosshair),
                        "the campaign's handed-over settings");
}

// The campaign's control settings put back over `profile`'s block once, the
// first time the profile's own settings (or the defaults) are loaded after
// the hand-over; the crosshair choice over player 1's.
namespace {
void KeepCampaignControls(uint8_t* base, uint32_t profile, const char* what) {
  const uint32_t block = ProfileBlock(profile);
  {
    std::lock_guard lock(g_handover_mutex);
    if (g_handover_profile && *g_handover_profile == profile) {
      if (std::memcmp(base + block, g_handover.data(), kControlsSize) != 0) {
        const std::string loaded = DescribeControls(base, block);
        std::memcpy(base + block, g_handover.data(), kControlsSize);
        REXLOG_INFO("cod3mp: profile {}'s {} ({}) would replace the campaign's control settings; the "
                    "campaign's are kept for this session",
                    profile + 1, what, loaded);
      }
      g_handover_profile.reset();
    }
  }
  if (profile != 0) return;
  if (const auto choice = CrosshairChoice())
    cod3::guest::WriteU8(base, block + a::controls::kCrosshair, *choice);
  else
    SeedCrosshairChoice(ReadU8(base, block + a::controls::kCrosshair), "profile 1's settings as read");
}
}  // namespace

// A profile's settings were read (the read's completion; r3 = the request).
// Read: the whole block is assigned with ProfileSettings__Assign below, then
// copied as the "current" settings and handed to the read's callback. Not
// read (status not 0): the game leaves the block alone and shows a message
// (0x824169F8 for 1167, else 0x824176C8), whose retry reads again; nothing
// is done here then, so the hand-over waits for that read.
REX_EXTERN(__imp__Profile_SettingsReadDone);
REX_HOOK_RAW(Profile_SettingsReadDone) {
  const uint32_t request = ctx.r3.u32;
  const bool valid = request >= 0x10000 && (request & 3) == 0;
  const uint32_t profile = valid ? ReadU32(base, request + kReadProfile) : kProfiles;
  const uint32_t status = valid ? ReadU32(base, request + kReadStatus) : 0;
  t_reading_profile = profile < kProfiles && status == 0 ? int(profile) : -1;
  __imp__Profile_SettingsReadDone(ctx, base);
  t_reading_profile = -1;
  if (profile >= kProfiles) return;
  if (status == 0)
    REXLOG_INFO("cod3mp: profile {}'s settings read from its save: {}", profile + 1,
                DescribeControls(base, ProfileBlock(profile)));
  else
    REXLOG_INFO("cod3mp: profile {}'s settings not read (status {:#x}); kept: {}", profile + 1, status,
                DescribeControls(base, ProfileBlock(profile)));
}

// The profile settings' assignment (r3 = destination, r4 = source), inside a
// profile read only, before the game copies and hands on what it loaded.
REX_EXTERN(__imp__ProfileSettings__Assign);
REX_HOOK_RAW(ProfileSettings__Assign) {
  const uint32_t dest = ctx.r3.u32;
  __imp__ProfileSettings__Assign(ctx, base);
  const int profile = t_reading_profile;
  if (profile >= 0 && dest == ProfileBlock(uint32_t(profile)))
    KeepCampaignControls(base, uint32_t(profile), "saved settings");
}
