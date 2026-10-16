// cod3 - HD textures: the waifu2x-upscaled texture pack in the playable game.
//
// With "Upscale the extracted textures" ticked, the setup tool upscales the
// ripped textures 2x with waifu2x and packs them into
// <game folder>/data/cod3/hd_textures/: each one in the same compressed GPU
// format as the game's own texture (DXT1/3/5, ATI2 or 32-bit), at twice the
// size with all its mip levels, and index.tsv saying which game texture
// (a hash of its data) each file replaces (c3w/upscale.py, `rip
// cod3-hd-pack`). The SDK's texture replacement (patch 0016) loads the file
// instead of the game's texture whenever the game uses one in the index; it
// is on when its setting texture_replacement_path names the pack folder.
//
// This sets that setting from the game setting cod3_hd_textures (F2 > Video,
// on by default), once, after the GPU plugin is loaded (its settings only
// exist from then on) and before the GPU starts (the pack is read then).
//
// The user texture pack: the player's own replacements for chosen textures
// (for example the insignia: docs/TEXTURE_REPLACEMENT.md), which the setup
// tool's "Build user texture pack" makes from PNGs (`rip cod3-user-pack`)
// into <game folder>/data/cod3/user_textures/, the same layout as the HD
// pack. It is read after the HD pack, so a texture in both comes from it,
// and the SDK checks its images against the original's alpha (an opaque
// texture stays opaque). Used when cod3_user_textures is on (default) and
// the pack is there (SDK setting texture_replacement_user_path).
//
// Multiplayer (hd::SetMultiplayerMode): while a match runs, only the world
// insignia on a fixed list (kMultiplayerInsigniaKeys in hd_textures.cpp:
// flags, banners, aircraft markings ... never textures a soldier or a
// vehicle wears) may be replaced, and only from the user pack. Every other
// replacement, HD textures included, is turned off for the match, because a
// replaced texture could be made see-through or bright to spot players. The
// SDK applies the change live (texture_replacement_allow_list): textures
// already made are made again on their next use.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace cod3 {

enum class HdTextures {
  kUndecided,    // before the GPU starts
  kOn,           // the SDK replaces textures from the pack
  kOff,          // cod3_hd_textures is off
  kNoPack,       // no data/cod3/hd_textures/index.tsv
  kUnsupported,  // no GPU emulation, or an SDK without texture replacement
  kCommandLine,  // texture_replacement_path given on the command line or in the environment
};

// Decides once (later calls do nothing) and logs one "cod3: HD textures" line.
// `root` is the game folder (the one holding assets/, data/ and userdata/);
// `gpu` says whether a GPU plugin is loaded.
void ApplyHdTextures(const std::filesystem::path& root, bool gpu);

HdTextures GetHdTextures();

// A few words for the state, for F2 and the diagnostics ("in use", "off" ...).
const char* DescribeHdTextures(HdTextures state);

namespace hd {

// The user texture pack (decided by ApplyHdTextures, with the same states as
// the HD pack's: kOn, kOff = cod3_user_textures off, kNoPack ...).
HdTextures GetUserTextures();

// The keys (hd_keys.tsv) of the world insignia textures a user pack may
// still replace during a multiplayer match.
const uint64_t* MultiplayerInsigniaKeys(size_t& count_out);

// Turns the multiplayer texture policy on (a match started) or off (it
// ended). Any thread; repeated calls with the same value do nothing. The
// multiplayer code calls it, or SyncMultiplayerMode does once per frame.
void SetMultiplayerMode(bool match_running);
bool IsMultiplayerMode();

// Follows cod3::mp::IsMatchRunning() (game/multiplayer.h): cheap, for once
// per game frame.
void SyncMultiplayerMode();

}  // namespace hd

}  // namespace cod3
