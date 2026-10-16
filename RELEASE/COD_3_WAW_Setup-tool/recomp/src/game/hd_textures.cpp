// cod3 - HD textures from the setup tool's pack (see hd_textures.h).

#include "game/hd_textures.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/multiplayer.h"
#include "util.h"

REXCVAR_DEFINE_BOOL(cod3_hd_textures, true, "cod3/Display",
                    "Use the HD texture pack when it is there: the game's textures upscaled 2x "
                    "with waifu2x, which the setup tool packs into data/cod3/hd_textures. Applies "
                    "after a restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(cod3_user_textures, true, "cod3/Display",
                    "Use the user texture pack when it is there: your own replacements for "
                    "chosen textures (for example the insignia), which the setup tool's \"Build "
                    "user texture pack\" makes into data/cod3/user_textures. Applies after a "
                    "restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace cod3 {

namespace {

// The SDK's settings (patch 0016): the pack folders, or empty for none. They
// belong to the GPU plugin, so they exist once that is loaded.
constexpr const char* kSdkSetting = "texture_replacement_path";
constexpr const char* kSdkUserSetting = "texture_replacement_user_path";
// Live: a file of the keys that may be replaced (empty = every key).
constexpr const char* kSdkAllowList = "texture_replacement_allow_list";

std::atomic<HdTextures> g_state{HdTextures::kUndecided};
std::atomic<HdTextures> g_user_state{HdTextures::kUndecided};

// The world insignia a user pack may still replace in a multiplayer match:
// flags, banners, aircraft and crate markings, a knife grip, a poster and two
// menu pictures. Never a texture a soldier or a vehicle wears (helmets,
// uniforms, tanks): a bright or see-through copy of those would show where
// the players are. The keys are the textures' hd_keys.tsv keys (XXH64 of the
// original data, so they can't be matched by another texture); the full
// list with what each shows is the setup tool's
// data/insignia_textures.txt (its "mp" column) and docs/TEXTURE_REPLACEMENT.md.
constexpr uint64_t kMultiplayerInsigniaKeys[] = {
    0xe26a0a1331259fd8,  // cloth_masked$of_flag_german: party flag (swastika)
    0x3d71bea5f9c2267d,  // decal$of_germanflag_dirty_c: hanging party banner (swastika)
    0xcc05fcb9517dacb0,  // metal$of_v_fw190: Fw 190 (tail swastika, crosses)
    0x78110e521ebffb27,  // wood$us_gersmallcrate_top_c: eagle ink stamp on a crate
    0x244cd5634547c2be,  // metal$p_ger_knife_c: Hitler Youth knife grip diamond
    0x43e8a8f26b93615f,  // decal$us_german_propaganda_: propaganda poster
    0xfe2665c6dacb3f45,  // sp_bs_veh_main_icon_02: menu picture, Iron Cross
    0x2beac860929168f5,  // sp_bs_wpn_main_icon_02: menu picture, Iron Cross
};

// The multiplayer policy: the allow list file the SDK reads, and the mode.
std::mutex g_mp_mutex;
std::filesystem::path g_user_pack;       // data/cod3/user_textures, when in use
std::filesystem::path g_mp_allow_list;   // userdata/texture_allow_list_mp.txt
std::atomic<bool> g_mp_mode{false};

// The keys in a pack's index.tsv (the first column of each line).
std::set<uint64_t> PackKeys(const std::filesystem::path& pack) {
  std::set<uint64_t> keys;
  std::ifstream index(pack / "index.tsv");
  std::string line;
  // A pack of every texture of the game has some thousands of lines.
  for (size_t lines = 0; lines < 1000000 && std::getline(index, line); ++lines) {
    if (line.size() < 17 || line[16] != '\t' || line[0] == '#') continue;
    try {
      size_t used = 0;
      uint64_t key = std::stoull(line.substr(0, 16), &used, 16);
      if (used == 16) keys.insert(key);
    } catch (const std::exception&) {
    }
  }
  return keys;
}

// The SDK's flag, unless the command line or environment chose it (a test).
bool ChosenByHand(const char* name) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  return info && (info->source == rex::cvar::Source::kCommandLine ||
                  info->source == rex::cvar::Source::kEnvironment);
}

// Decides the user pack (after the HD pack's decision).
void ApplyUserTextures(const std::filesystem::path& root) {
  const std::filesystem::path pack = root / "data" / "cod3" / "user_textures";
  const std::string where = PathToUtf8(pack);
  if (!rex::cvar::GetFlagInfo(kSdkUserSetting)) {
    g_user_state = HdTextures::kUnsupported;
    REXLOG_INFO("cod3: user textures not used: this build's GPU emulation has no "
                "texture_replacement_user_path (SDK patch 0016)");
    return;
  }
  if (ChosenByHand(kSdkUserSetting)) {
    g_user_state = HdTextures::kCommandLine;
    REXLOG_INFO("cod3: user textures from texture_replacement_user_path on the command line or "
                "environment (cod3_user_textures is not used)");
    return;
  }
  std::error_code ec;
  const bool have_pack = std::filesystem::is_regular_file(pack / "index.tsv", ec);
  std::string value;
  if (!REXCVAR_GET(cod3_user_textures)) {
    g_user_state = HdTextures::kOff;
    if (have_pack) REXLOG_INFO("cod3: user textures off (cod3_user_textures); the pack is in {}", where);
  } else if (!have_pack) {
    g_user_state = HdTextures::kNoPack;  // the usual case: nothing to say
  } else {
    g_user_state = HdTextures::kOn;
    value = PathToUtf8Generic(pack);
    std::lock_guard<std::mutex> lock(g_mp_mutex);
    g_user_pack = pack;
    REXLOG_INFO("cod3: user textures on: the pack in {} (read after the HD pack, so it wins)",
                where);
  }
  // Written in every case, like texture_replacement_path (see below).
  if (!rex::cvar::SetFlagByName(kSdkUserSetting, value) && !value.empty()) {
    g_user_state = HdTextures::kUnsupported;
    REXLOG_WARN("cod3: the GPU emulation refused {} = {}; user textures not used",
                kSdkUserSetting, value);
  }
}

}  // namespace

namespace {

// Decides the HD pack.
void ApplyHdPack(const std::filesystem::path& root, bool gpu) {
  const std::filesystem::path pack = root / "data" / "cod3" / "hd_textures";
  const std::string where = PathToUtf8(pack);

  const auto* info = rex::cvar::GetFlagInfo(kSdkSetting);
  if (!gpu || !info) {
    g_state = HdTextures::kUnsupported;
    const char* why = gpu ? "this build's GPU emulation has no texture replacement "
                            "(texture_replacement_path, SDK patch 0016)"
                          : "no GPU emulation";
    REXLOG_INFO("cod3: HD textures not used: {}", why);
    return;
  }
  // A pack chosen by hand for testing (--texture_replacement_path=...) wins.
  if (info->source == rex::cvar::Source::kCommandLine ||
      info->source == rex::cvar::Source::kEnvironment) {
    g_state = HdTextures::kCommandLine;
    const std::string chosen = rex::cvar::GetFlagByName(kSdkSetting);
    REXLOG_INFO("cod3: HD textures {} (texture_replacement_path from the command line or "
                "environment; cod3_hd_textures is not used)",
                chosen.empty() ? std::string("off") : "on: the pack in " + chosen);
    return;
  }

  std::error_code ec;
  const bool have_pack = std::filesystem::is_regular_file(pack / "index.tsv", ec);
  std::string value;  // empty: no replacement
  if (!REXCVAR_GET(cod3_hd_textures)) {
    g_state = HdTextures::kOff;
    REXLOG_INFO("cod3: HD textures off (cod3_hd_textures, F2 > Video){}",
                have_pack ? "; the pack is in " + where : std::string());
  } else if (!have_pack) {
    g_state = HdTextures::kNoPack;
    REXLOG_INFO("cod3: HD textures not used: no pack in {} (the setup tool makes it when \"Upscale "
                "the extracted textures\" is ticked)",
                where);
  } else {
    g_state = HdTextures::kOn;
    value = PathToUtf8Generic(pack);
    REXLOG_INFO("cod3: HD textures on: the pack in {} (F2 > Video > HD textures turns it off)",
                where);
  }
  // Written in every case: F2's Save stores every changed setting in
  // cod3.toml, so an earlier start's path comes back from there and would
  // keep the pack on after the setting was turned off, or point at the old
  // place after the game folder moved. (So a path typed into cod3.toml by
  // hand is replaced too; the command line is the way to test another pack.)
  if (!rex::cvar::SetFlagByName(kSdkSetting, value) && !value.empty()) {
    g_state = HdTextures::kUnsupported;
    REXLOG_WARN("cod3: the GPU emulation refused texture_replacement_path = {}; HD textures not used",
                value);
  }
}

}  // namespace

void ApplyHdTextures(const std::filesystem::path& root, bool gpu) {
  if (g_state.load() != HdTextures::kUndecided) return;
  ApplyHdPack(root, gpu);
  if (!gpu || !rex::cvar::GetFlagInfo(kSdkSetting)) {
    // No GPU emulation, or one without texture replacement (said above).
    g_user_state = HdTextures::kUnsupported;
    return;
  }
  ApplyUserTextures(root);
  {
    std::lock_guard<std::mutex> lock(g_mp_mutex);
    g_mp_allow_list = root / "userdata" / "texture_allow_list_mp.txt";
  }
  // A match never runs at the start, so no allow list: one saved in
  // cod3.toml by F2 during a match must not limit single player.
  if (!ChosenByHand(kSdkAllowList) && rex::cvar::GetFlagInfo(kSdkAllowList)) {
    rex::cvar::SetFlagByName(kSdkAllowList, "");
  }
}

HdTextures GetHdTextures() { return g_state.load(); }

const char* DescribeHdTextures(HdTextures state) {
  switch (state) {
    case HdTextures::kOn:
      return "in use";
    case HdTextures::kOff:
      return "off";
    case HdTextures::kNoPack:
      return "no pack (data/cod3/hd_textures)";
    case HdTextures::kUnsupported:
      return "not supported by this build";
    case HdTextures::kCommandLine:
      return "set on the command line";
    case HdTextures::kUndecided:
      break;
  }
  return "not decided yet";
}

namespace hd {

HdTextures GetUserTextures() { return g_user_state.load(); }

const uint64_t* MultiplayerInsigniaKeys(size_t& count_out) {
  count_out = std::size(kMultiplayerInsigniaKeys);
  return kMultiplayerInsigniaKeys;
}

void SetMultiplayerMode(bool match_running) {
  std::lock_guard<std::mutex> lock(g_mp_mutex);
  if (g_mp_mode.load() == match_running) return;
  g_mp_mode = match_running;
  if (!rex::cvar::GetFlagInfo(kSdkAllowList)) {
    // No texture replacement in this build: nothing to limit.
    return;
  }
  if (!match_running) {
    rex::cvar::SetFlagByName(kSdkAllowList, "");
    REXLOG_INFO("cod3: multiplayer match over: texture replacement back to normal");
    return;
  }
  // Allowed: the world insignia that the user pack replaces. Nothing else,
  // HD textures included (the SDK takes a key from the user pack when both
  // packs have it, so these come from the user pack).
  std::set<uint64_t> user_keys;
  if (!g_user_pack.empty()) user_keys = PackKeys(g_user_pack);
  std::string text =
      "# Written by the game for a multiplayer match (game/hd_textures.cpp): the textures that "
      "may be replaced.\n";
  size_t allowed = 0;
  for (uint64_t key : kMultiplayerInsigniaKeys) {
    if (!user_keys.count(key)) continue;
    char line[20];
    std::snprintf(line, sizeof(line), "%016llx\n", static_cast<unsigned long long>(key));
    text += line;
    ++allowed;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_mp_allow_list.parent_path(), ec);
  {
    std::ofstream file(g_mp_allow_list, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) {
      // The SDK allows nothing when it can't read the list, so the match
      // still has no replaced textures.
      REXLOG_WARN("cod3: couldn't write {}; no texture is replaced during the match",
                  PathToUtf8(g_mp_allow_list));
    }
  }
  rex::cvar::SetFlagByName(kSdkAllowList, PathToUtf8Generic(g_mp_allow_list));
  REXLOG_INFO("cod3: multiplayer match: texture replacement limited to {} insignia texture(s) "
              "from the user pack; HD and other replaced textures are off until the match ends",
              allowed);
}

bool IsMultiplayerMode() { return g_mp_mode.load(); }

void SyncMultiplayerMode() {
  const bool running = mp::IsMatchRunning();
  if (running != g_mp_mode.load()) SetMultiplayerMode(running);
}

}  // namespace hd

}  // namespace cod3
