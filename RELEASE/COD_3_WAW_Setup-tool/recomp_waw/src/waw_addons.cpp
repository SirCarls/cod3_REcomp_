// waw - World at War's own Xbox 360 add-on packages (the map packs) in the disc program. [map-compat]
//
// Owner, 2026-11-09 17:28: the ports run the disc program and redo what the title updates did as our own code; no
// title update file is needed, loaded or shipped. Owner 2026-11-11 05:44: zombies stays in World at War, and every
// multiplayer map is shared; this file is the first part of both for World at War's own packs.
//
// What the disc program (default.xex, campaign/co-op/zombies) already does, read from the decompile
// (dev/waw-decomp/x360_sp; dev/agents/map-compat/NOTES.md 2026-11-11 "M1c"):
//   - Content_FindContentPacks 0x8229B5D8 and the enumerator 0x82293690 find the marketplace packages (title
//     4156081C, content type 2) that the SDK's content manager lists from userdata/0000000000000000/4156081C/00000002/.
//   - 0x82293238 mounts each with XContentCreate as "mappack%i" (a table of 6 at 0x83551588, 0x44 bytes each).
//   - 0x82293170 decides which pack it is from an id file on the drive: "%s:\mappack1.id" (4) or "%s:\mappack2.id"
//     (8). Anything else is "Unable to determine map pack identity" and 0x822934E0 closes the drive again.
//   - DB_TryLoadXFileInternal 0x821566D8 opens a zone only as "<zone folder>\<zone>.ff"; it never looks on a pack's
//     drive. 0x82156548 builds the same path for its one caller (0x82252340).
//   - The other files a level reads are opened only from the game folder too: streamed sounds "%s\%s.%s" (0x8234C318),
//     sound banks "%s\%s" (0x8234B498, at zone load), Bink movies "%s\%s.%s" (0x824110B0) and highmip textures
//     "%s\highmip\%s.hi" (0x82428C28).
// The Makin bonus map carries mappack1.id; Map Packs 1, 2 and 3 carry dlcmappack1.id, dlcmappack2.id and
// dlcmappack3.id, which only title update 7 knows ("dlcmappack1-3", "%s:\%s" are new strings there). So on the disc
// program the three map packs are closed as unknown and none of their zones (the MP maps, and the zombie maps
// Verrückt, Shi No Numa and Der Riese) can load.
//
// What this file adds, each behind waw_addons (on by default):
//   1. waw_addons_identify_pack_hook, at the entry of 0x82293170: a drive holding dlcmappackN.id (and neither of the
//      disc's own id files) is map pack N, given the identity bits 0x10, 0x20, 0x40. They are new bits, so the
//      "already mounted as a different drive" check 0x82293120 tells the packs apart, and 0x822934E0 keeps the drive
//      mounted (only 4 and 8 set the disc's mappack1/mappack2 settings; any other known pack still sets
//      xblive_anymappacks).
//   2. waw_addons_zone_path_hook, in DB_TryLoadXFileInternal at 0x821567FC (after it built the path, before it
//      opens it), and waw_addons_zone_path_exists_hook in 0x82156548 at 0x82156580 (after its Com_sprintf): when the
//      disc has no "<zone>.ff", the path is pointed at "<drive>:\<zone>.ff" on the first mounted pack that has it.
//      The disc's own zones always win; nothing changes for a zone the disc has.
//   3. The same for the other files a pack's level reads, each right after the disc built its path and before it
//      opens it: streamed sounds (0x8234C424; the packs keep them under SFX\, Stream\ and voiceovers\), sound banks
//      (0x8234B548), Bink movies (0x82411120; a zombie map's <map>_load.bik) and highmip textures (0x82428D10; the
//      packs' highmip files have no ".hi", as title update 7's "%s\highmip%s\%s" reads them).
//   4. waw_addons_zombie_start_save_hook, in SV_SpawnServer 0x822BC6C8 at 0x822BC768: the disc skips the save at the
//      start of a level (SV_SaveImmediately 0x822BFCB8; state 4 at 0x842F35D8) only for outro, credits, intro_pac
//      and nazi_zombie_prototype. A pack's zombie map would get one, and it would replace the campaign's resume save.
//      Every map whose name holds "nazi_zombie_" (the PC game's own zombie-mode test, 0x005AA180 there) now gets
//      what the prototype gets.
//   5. waw_addons_level_patch_zone_hook, at the two calls that load a level's zone, SV_SpawnServer 0x822BCAD8 and
//      CL_InitCGame 0x822C6178 (a co-op client's): the disc loads only the level's own zone; each pack's zombie map
//      comes with a "<map>_patch" zone that title update 7 loads with it (the PC game: Com_LoadLevelFastFiles). When
//      a mounted pack has one, the call gets a second entry for it, after the level's (DB_LoadXAssets 0x82158A68:
//      freeFlags 0, so it unloads nothing; the level's own allocFlags, so it goes with the level, and the later of
//      two zones with equal flags wins an asset both have, as DB_LinkXAssetEntry decides on the PC).
//   6. [acct2-mods 2026-11-14 23:45] The end of the list of packages. The enumerator's poll 0x82293690 walks the
//      packages it was given (XCONTENT_DATA, 308 bytes each, from 0x835540D8) until an entry whose content type is
//      0, and hands each to 0x82293238 to mount. Six entries fit before the next variable, and the word that would
//      be a seventh entry's content type is the count of drives mounted so far (0x83554814, the number in
//      "mappack%i"). With six packages installed (the three map packs, the Makin bonus map and the two weapons) that
//      count is 6 when the walk gets there, so the walk takes it for a seventh package (the log's "Found content
//      pack ????????"), finds no free drive among the table's six and raises "Couldn't get free content pack
//      index": the error box about 3 s after every start. The whole-function hook on 0x82293238 below leaves an
//      entry behind the list's six alone. With five packages or fewer the disc's walk ends by itself.
// Nothing in a pack is changed; the packages are installed as the console kept them (c3w/addons_waw.py).

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system.h>

#include "generated/default/waw_pch.h"

REXCVAR_DEFINE_BOOL(waw_addons, true, "waw",
    "World at War's Xbox 360 add-on packages (the ones Set up adds): Map Packs 1 to 3 are known as map packs (the "
    "disc program only knows the Makin bonus map's), and a level's fastfile is looked for on each mounted pack when "
    "the disc doesn't have it. Title update 7 did both; this is our own code on the disc program");

REXCVAR_DEFINE_BOOL(waw_addons_patch_zones, true, "waw",
    "With waw_addons: a level whose pack has a \"<map>_patch\" zone loads it after the level's own (the three "
    "zombie maps of Map Packs 1-3 have one; title update 7 loaded them). Off to test a pack's level without it");

namespace {

// The packs the game mounted (0x82293238 fills it, 0x822934E0 marks an entry ready): 6 entries of 0x44 bytes.
constexpr uint32_t kMountTable = 0x83551588;
constexpr uint32_t kMountEntrySize = 0x44;
constexpr uint32_t kMountEntries = 6;
constexpr uint32_t kMountDrive = 0x2F;     // the drive's name, "mappack%i" (0x82293238's snprintf, 12 bytes)
constexpr uint32_t kMountReady = 0x40;     // 1 once the pack's identity is known and the drive stays mounted
constexpr uint32_t kMappackDriveMax = 12;

// The identity bits the disc program gives its own two id files, and ours for the three map packs.
constexpr uint32_t kFirstMapPackBit = 0x10;
constexpr int kMapPacks = 3;

constexpr size_t kZoneNameMax = 64;
constexpr size_t kAssetNameMax = 64;    // the sound and image names the disc copies are at most 0x40 bytes
constexpr uint32_t kBinkExtension = 0x820A6858;  // 0x824110B0's extension string ("bik")
constexpr uint32_t kBinkNoFolderFlag = 0x20;     // 0x824110B0 builds "bik/%s.%s" (no game folder) with this flag
constexpr uint32_t kStartSaveState = 0x842F35D8; // SV_SaveImmediately's level-start save state (big-endian int)
constexpr uint32_t kStartSaveSkip = 4;           // what SV_SpawnServer stores for the prototype: no level-start save
constexpr char kZombieMapPrefix[] = "nazi_zombie_";
constexpr uint32_t kZoneInfoSize = 12;           // XZoneInfo: name, allocFlags, freeFlags (big-endian)

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16); p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
}

std::string GuestString(const uint8_t* base, uint32_t address, size_t limit) {
  if (address == 0) return {};
  const char* s = reinterpret_cast<const char*>(base + address);
  size_t n = 0;
  while (n < limit && s[n] != '\0') ++n;
  if (n == limit) return {};  // no end inside the limit: not a name this file knows how to handle
  return std::string(s, n);
}

bool GuestPathExists(const std::string& path) {
  auto* kernel = rex::system::kernel_state();
  auto* fs = kernel ? kernel->file_system() : nullptr;
  return fs != nullptr && fs->ResolvePath(path) != nullptr;
}

// A drive name the game made itself ("mappack" and a number), never anything else.
bool IsMappackDrive(const std::string& drive) {
  if (drive.size() < 8 || drive.size() >= kMappackDriveMax || drive.compare(0, 7, "mappack") != 0) return false;
  for (size_t i = 7; i < drive.size(); ++i) {
    if (drive[i] < '0' || drive[i] > '9') return false;
  }
  return true;
}

// A zone name as the game passes it: letters, digits and '_' only (no drive, no folder, no dots).
bool IsPlainZoneName(const std::string& zone) {
  if (zone.empty()) return false;
  for (char c : zone) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

// A path inside a pack as the disc names it: folders and a file name of letters, digits and _ - . ~ & $ #, with '\'
// or '/' between them; never a drive, an empty part or "..".
bool IsPlainRelativePath(const std::string& rel) {
  if (rel.empty() || rel.size() > 200) return false;
  size_t part = 0;
  for (size_t i = 0; i <= rel.size(); ++i) {
    const char c = i < rel.size() ? rel[i] : '\\';
    if (c == '\\' || c == '/') {
      const std::string name = rel.substr(part, i - part);
      if (name.empty() || name == "." || name == "..") return false;
      part = i + 1;
      continue;
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '-' || c == '.' || c == '~' || c == '&' || c == '$' || c == '#';
    if (!ok) return false;
  }
  return true;
}

std::mutex g_log_mutex;
std::set<std::string> g_logged;

void LogOnce(const std::string& key, const std::string& line) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_logged.insert(key).second) REXLOG_INFO("waw_addons: {}", line);
}

// The ready mounted packs, as one bit per mount table entry: a cached answer is only reused while it is the same.
uint32_t ReadyPacks(const uint8_t* base) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kMountEntries; ++i) {
    if (base[kMountTable + i * kMountEntrySize + kMountReady] != 0) mask |= 1u << i;
  }
  return mask;
}

std::mutex g_cache_mutex;
std::map<std::string, std::string> g_found;  // (ready packs, path) -> the pack's path, or "" when none has it

// "<drive>:\<rel>" on the first ready mounted pack that has `rel` ('/' read as '\'); empty when none has it.
std::string FindOnPacks(const uint8_t* base, const std::string& rel) {
  const uint32_t ready = ReadyPacks(base);
  if (ready == 0 || !IsPlainRelativePath(rel)) return {};
  std::string inside = rel;
  for (char& c : inside) {
    if (c == '/') c = '\\';
  }
  const std::string key = std::to_string(ready) + "|" + inside;
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_found.find(key);
    if (it != g_found.end()) return it->second;
  }
  std::string found;
  for (uint32_t i = 0; i < kMountEntries && found.empty(); ++i) {
    if ((ready & (1u << i)) == 0) continue;
    const std::string drive = GuestString(base, kMountTable + i * kMountEntrySize + kMountDrive, kMappackDriveMax);
    if (!IsMappackDrive(drive)) continue;
    const std::string candidate = drive + ":\\" + inside;
    if (GuestPathExists(candidate)) found = candidate;
  }
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_found.size() > 65536) g_found.clear();  // a bound, far above the packs' 5,500 files
  g_found[key] = found;
  return found;
}

// The disc built "<game folder>\<rel>" in `buffer` (its own, `size` bytes) and opens it next. When there is no such
// file and a mounted pack has `rel`, the buffer gets the pack's path instead. True when it did.
bool RedirectToPack(uint8_t* base, uint32_t buffer, uint32_t size, const std::string& rel, const char* kind) {
  if (buffer == 0 || size == 0 || rel.empty()) return false;
  const std::string path = GuestString(base, buffer, size);
  if (path.empty() || GuestPathExists(path)) return false;  // the disc has it (or the path is odd): leave it alone
  const std::string found = FindOnPacks(base, rel);
  if (found.empty() || found.size() + 1 > size) return false;
  std::memcpy(base + buffer, found.c_str(), found.size() + 1);
  LogOnce(std::string(kind) + ":" + rel, std::string("the ") + kind + " " + rel + " is read from " + found +
                                             " (the disc has none)");
  return true;
}

// Points `buffer` at the zone on a mounted pack when the disc doesn't have it.
void RedirectZonePath(uint8_t* base, uint32_t zone_address, uint32_t buffer, uint32_t size) {
  const std::string zone = GuestString(base, zone_address, kZoneNameMax);
  if (!IsPlainZoneName(zone)) return;
  RedirectToPack(base, buffer, size, zone + ".ff", "zone");
}

}  // namespace

// 0x82293238 (the user's index, the package's XCONTENT_DATA, its display name): mounts one package the enumerator
// found. See 6 above: an entry behind the list's six is not a package.
REX_EXTERN(__imp__sub_82293238);
REX_HOOK_RAW(sub_82293238) {
  constexpr uint32_t kContentList = 0x835540D8;
  constexpr uint32_t kContentDataSize = 308;
  constexpr uint32_t kContentListRoom = 6;
  constexpr uint32_t kContentListEnd = kContentList + kContentListRoom * kContentDataSize;
  const uint32_t entry = ctx.r4.u32;
  // only the walk's own overrun is turned away: the entries right behind the list, where it would go on
  if (REXCVAR_GET(waw_addons) && entry >= kContentListEnd && entry < kContentListEnd + 16 * kContentDataSize) {
    LogOnce("contentlistend", "the list of packages ends after its six entries; what lies behind it is not mounted "
                              "(the disc takes the count of mounted drives there for a seventh package)");
    return;
  }
  __imp__sub_82293238(ctx, base);
}

// 0x82293170, at its entry (r3 = the drive's name, "mappack%i"). Returns true, with the identity in r3, for a drive
// holding one of the three map packs' id files; false leaves the disc's own check to run (the Makin bonus map's
// mappack1.id stays the disc's).
bool waw_addons_identify_pack_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons)) return false;
  const std::string drive = GuestString(base, ctx.r3.u32, kMappackDriveMax);
  if (!IsMappackDrive(drive)) return false;
  if (GuestPathExists(drive + ":\\mappack1.id") || GuestPathExists(drive + ":\\mappack2.id")) return false;
  for (int n = 1; n <= kMapPacks; ++n) {
    if (!GuestPathExists(drive + ":\\dlcmappack" + std::to_string(n) + ".id")) continue;
    ctx.r3.u64 = kFirstMapPackBit << (n - 1);
    LogOnce("pack:" + drive, "drive " + drive + " is map pack " + std::to_string(n));
    return true;
  }
  return false;
}

// DB_TryLoadXFileInternal 0x821566D8, at 0x821567FC: its own path is built in r1+80 (256 bytes) for the zone in r31,
// and the next call opens it.
void waw_addons_zone_path_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons)) return;
  RedirectZonePath(base, ctx.r31.u32, ctx.r1.u32 + 80, 256);
}

// 0x82156548 (its one caller is 0x82252340), at 0x82156580 after its Com_sprintf: r29 the path, r30 its size, r31 the
// zone.
void waw_addons_zone_path_exists_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons)) return;
  RedirectZonePath(base, ctx.r31.u32, ctx.r29.u32, ctx.r30.u32);
}

// 0x8234C318 (a streamed sound), at 0x8234C424 after its Com_sprintf: the path in r1+352 (256 bytes) is
// "<game folder>\<r23>.<r30>" (the alias' file and its extension); the next call opens it.
void waw_addons_stream_path_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons)) return;
  const std::string name = GuestString(base, ctx.r23.u32, kAssetNameMax);
  const std::string ext = GuestString(base, ctx.r30.u32, 8);
  if (name.empty() || ext.empty()) return;
  RedirectToPack(base, ctx.r1.u32 + 352, 256, name + "." + ext, "sound");
}

// 0x8234B498 (a sound bank, at zone load), at 0x8234B548 after its Com_sprintf: the path in r1+112 (256 bytes) is
// "<game folder>\<r26>".
void waw_addons_sound_bank_path_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons)) return;
  RedirectToPack(base, ctx.r1.u32 + 112, 256, GuestString(base, ctx.r26.u32, kAssetNameMax), "sound bank");
}

// 0x824110B0 (a Bink movie), at 0x82411120 where its two __snprintf calls meet: the path in r1+80 (256 bytes) is
// "<game folder>\<r30>.bik" unless flag 0x20 (r31) asked for "bik/<r30>.bik", which this leaves alone.
void waw_addons_bink_path_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons) || (ctx.r31.u32 & kBinkNoFolderFlag) != 0) return;
  const std::string name = GuestString(base, ctx.r30.u32, kAssetNameMax);
  const std::string ext = GuestString(base, kBinkExtension, 8);
  if (name.empty() || ext.empty()) return;
  RedirectToPack(base, ctx.r1.u32 + 80, 256, name + "." + ext, "movie");
}

// 0x82428C28 (a highmip texture), at 0x82428D10 after its Com_sprintf: the path in r1+96 (1024 bytes) is
// "<game folder>\highmip\<r30>.hi". The packs keep theirs without ".hi".
void waw_addons_highmip_path_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons)) return;
  const std::string image = GuestString(base, ctx.r30.u32, kAssetNameMax);
  if (image.empty()) return;
  if (!RedirectToPack(base, ctx.r1.u32 + 96, 1024, "highmip\\" + image + ".hi", "highmip")) {
    RedirectToPack(base, ctx.r1.u32 + 96, 1024, "highmip\\" + image, "highmip");
  }
}

// SV_SpawnServer 0x822BC6C8, at 0x822BC768 right after it stored the level-start save state (0, or 4 for outro,
// credits, intro_pac and nazi_zombie_prototype): r24 the map, r18 its second argument (0 for a new level, as the
// disc's own check needs). A zombie map from a pack (or any "nazi_zombie_*" map) gets the prototype's 4, so starting
// it never writes a level-start save over the campaign's.
void waw_addons_zombie_start_save_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons) || ctx.r18.u32 != 0) return;
  const std::string map = GuestString(base, ctx.r24.u32, kZoneNameMax);
  if (map.find(kZombieMapPrefix) == std::string::npos) return;
  if (LoadBe32(base, kStartSaveState) != 0) return;  // the disc already chose (4 for the prototype)
  StoreBe32(base, kStartSaveState, kStartSaveSkip);
  LogOnce("startsave:" + map,
          "no level-start save for the zombie map " + map + " (as the disc does for the prototype)");
}

// The two calls that load a level's zone: SV_SpawnServer 0x822BC6C8 at 0x822BCAD8 (r3 = r1+96: the map, allocFlags 2,
// freeFlags 2) and CL_InitCGame 0x822C6048 at 0x822C6178 (a co-op client's, r3 = r1+80: the same); r4 1, r5 0. When
// a mounted pack has "<map>_patch.ff", the call gets a two-entry list instead: the level's entry as the disc made it,
// then the patch zone with the same allocFlags and freeFlags 0. The block is reused: DB_LoadXAssets copies the names
// before it returns.
void waw_addons_level_patch_zone_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_addons) || !REXCVAR_GET(waw_addons_patch_zones)) return;
  if (ctx.r4.u32 != 1 || ctx.r3.u32 == 0) return;
  const uint32_t level = ctx.r3.u32;
  const std::string map = GuestString(base, LoadBe32(base, level), kZoneNameMax);
  if (!IsPlainZoneName(map)) return;
  const std::string patch = map + "_patch";
  if (patch.size() + 1 > kZoneNameMax || FindOnPacks(base, patch + ".ff").empty()) return;
  static uint32_t block = 0;  // two entries and the name, kept for the whole run (the main thread only comes here)
  if (block == 0) {
    auto* kernel = rex::system::kernel_state();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (memory) block = memory->SystemHeapAlloc(2 * kZoneInfoSize + kZoneNameMax);
    if (block == 0) return;
  }
  const uint32_t name = block + 2 * kZoneInfoSize;
  std::memcpy(base + block, base + level, kZoneInfoSize);
  std::memcpy(base + name, patch.c_str(), patch.size() + 1);
  StoreBe32(base, block + kZoneInfoSize + 0, name);
  StoreBe32(base, block + kZoneInfoSize + 4, LoadBe32(base, level + 4));
  StoreBe32(base, block + kZoneInfoSize + 8, 0);
  ctx.r3.u64 = block;
  ctx.r4.u64 = 2;
  LogOnce("patchzone:" + map, "the level " + map + " also loads " + patch + " (from its pack, as title update 7 did)");
}
