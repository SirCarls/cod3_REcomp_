// [acct2-bro] 2026-11-22 cod3 - the camo layer on the first-person gun (Camo lane; CAMO-PACK-PLAN.md).
//
// The owner, 2026-11-22: 03:17 "we need to adapt it [the camo pack] to ours"; 03:18 "there's camos that move/crawl
// that will also need to function"; 08:17 "the camo settings/gloves changes should be on a per-player and per gun
// basis, account for mid-game switches. Can be within f2 within game and a sub-tab."; 08:36 "the camo swapping
// needs to be extremely safe".
//
// The setup tool turns the player's own pack into checked camo files (c3w/camo_pack.py: <game folder>/data/cod3/
// camos/<id>.c3camo and index.tsv); this reads only those. Nothing of the game is changed: the native renderer asks
// LayerFor() at a first-person gun draw and, when it gets a layer, draws the gun's own pass with the pattern over it;
// no answer (no choice, kill switch off, anything missing or failed) means the gun is drawn exactly as before.
//
// Safety (the plan's rules 2-5): every camo file is checked again here by the tool's rules before use (sizes,
// offsets, counts, caps, both checksums); reading and decoding run on a thread of their own, never in a frame; a
// texture is made on the drawing thread at most one a frame, before it can be chosen; a choice changes only at the
// start of a frame (BeginFrame takes a snapshot), so a swap mid-frame can't happen; a file that fails is logged once
// and never retried in that session; nothing here throws.
#pragma once

#include <cstdint>
#include <filesystem>  // [acct2-bro] 2026-11-22 x549: ReadColourLevels
#include <string>
#include <string_view>
#include <vector>

namespace cod3::ngl_camo {

// One camo of the index (index.tsv), for F2's list.
struct Entry {
  std::string id;    // "camo_" + 12 hex, the file's name
  std::string name;  // the pack's folder path, printable
};

// The camos the setup tool imported (read once; Rescan() reads the folder again, e.g. after an import).
const std::vector<Entry>& List();
void Rescan();

// What the renderer needs to draw one gun with its camo. Textures are ngl_vk texture numbers (0: no such map).
struct Layer {
  uint32_t color = 0;
  uint32_t specular = 0;
  uint32_t env = 0;
  float mean[3] = {0.5f, 0.5f, 0.5f};  // the colour map's mean colour (the layer keeps the gun's own brightness)
  float scale = 1.0f;                  // pattern repeats per unit of the gun's texture coordinates
  float offset[2] = {0.0f, 0.0f};      // the crawl, in pattern widths, by real time (the same at any frame rate)
  float strength = 1.0f;               // 0..1, cod3_camo_strength
};

// On the drawing thread, once a frame before the first gun draw: takes the frame's snapshot of the choices
// (cod3_camo_choices), hands new files to the reading thread and makes at most one waiting texture.
void BeginFrame();

// The layer for the gun `weapon` (as its textures name it, e.g. "springfield" from w_springfield_c: ngl_side.cpp's
// CamoGunKey, [acct2-bro] 2026-11-22 c519) held by split-screen player `player`
// (1-4; 1 outside split screen), or false: draw the gun as the game does. Only after BeginFrame, same thread.
bool LayerFor(int player, std::string_view weapon, Layer* out);

// After ngl_vk_forget (a new level): every texture number here is gone; they are made again when chosen.
void Forget();

// F2 (the sub-tab): the choice for one player and gun ("*" = all guns; "" = none), kept in cod3_camo_choices.
std::string Choice(int player, std::string_view weapon);
void SetChoice(int player, std::string_view weapon, std::string_view camo_id);

// F2's thread: the guns LayerFor was asked about this session (their game names), for the per-gun rows. A gun shows
// here once it has been drawn in first person.
std::vector<std::string> GunsSeen();

// For tests and logs: how many files were refused and textures made this session.
struct Counts {
  uint32_t refused = 0;
  uint32_t made = 0;
  uint32_t waiting = 0;
};
Counts GetCounts();

// [acct2-bro] 2026-11-23 Camo lane (the gloves): the folder the camos are read from (data/cod3/camos under the game's
// folder); the gloves' arms and skins sit beside it (data/cod3/arms, data/cod3/gloves). Any thread.
std::filesystem::path Folder();

// [acct2-bro] 2026-11-22 Camo lane x549 (the gloves, a probe): one .c3camo file's colour map as BGRA levels, largest
// first, by the camo reader's own checks (ngl_camo.cpp); false and why.
bool ReadColourLevels(const std::filesystem::path& path, std::vector<std::vector<uint8_t>>* levels, uint32_t* width,
                      uint32_t* height, std::string* why);

}  // namespace cod3::ngl_camo
