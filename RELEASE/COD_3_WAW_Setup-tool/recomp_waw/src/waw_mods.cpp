// waw - Call of Duty: World at War (Xbox 360) static recompilation: a converted PC mod's zones, loaded after a level's.
// [acct2-mods] 2026-11-14, the PC-mod converter's port side (dev/agents/acct2-mods/PLAN.md section 6; it carries on
// map-compat's plan 4.1.2). The owner, 2026-11-14 17:26: "start working on a conversion tool for PC mods to our copies".
//
// What it does, with waw_mod set (empty by default: then every hook here only calls the game's own function):
//   - the mod's converted folder, <mods root>/<waw_mod>/360/, becomes a read-only device of its own, "mods:";
//   - when the game loads a level's zone, the mod's zones (waw_mod_zones, "mod" by default) are loaded right after it,
//     with the level's flags, so they go when the level goes;
//   - only those zones' files are opened from "mods:"; every other file is the game's own, from the disc's folder.
// Nothing of the disc's is replaced or changed. The zones come from the converter (COD3-rea-work/acct2-mods/p0/
// zconvert.py for now): a PC zone's raw files, string tables and localized strings written as an Xbox 360 zone. A
// zone listed but not in the folder is left out (said once in the log): the game ends with an error on a zone file it
// can't open.
//
// The game's functions (dev/waw-decomp/x360_sp: names.tsv, calls.tsv, strings.tsv):
//   0x822BC6C8  SV_SpawnServer: builds {level, 2, 2} and calls DB_LoadXAssets with that one zone.
//   0x82158A68  DB_LoadXAssets(r3 the zones: 12 bytes each, {name, alloc flags, free flags}; r4 how many; r5 wait).
//               Ten callers; the one in SV_SpawnServer is told apart by a mark set around SV_SpawnServer.
//   0x821566D8  DB_TryLoadXFileInternal(r3 the zone's name, r4 its flags): opens "%s\%s.ff" (0x820133E8) with the folder
//               0x8227F2C8 gives, and copies the name; a zone file it can't open is Com_Error.
//   0x8227F2C8  the folder the game's files are read from (returns a constant string; twelve callers).
//   0x82158A38  DB_SyncXAssets: the loading screen kept drawn while every queued zone finishes loading, then
//               DB_PostLoadXZone 0x82158890 links the assets that take another zone's place ("Redundant asset - %s").
// SV_SpawnServer doesn't wait for its level's zones: it goes on and DB_FindXAssetHeader 0x82158FB8 waits for each
// asset it asks for ("Waited %d msec for asset"). So the level's script, which is in the level's own zone, would be
// compiled before the mod's zone has loaded and taken its place (runs m1-zm, m2-zm, 11-14 19:57 and 20:05: the mod's
// raw file linked at 20:05:20.00, the level's entities spawned at 20:05:20.05, our lines never ran). When a mod's
// zones were added, the hook on DB_LoadXAssets waits for them with DB_SyncXAssets, as SV_SpawnServer itself does
// before it loads (0x822BC840).
// Scripts are read from zones only (Scr_AddSourceBuffer 0x82318970, through DB_FindXAssetHeader), so a mod's scripts
// reach the game this way.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_STRING(waw_mod, "", "waw",
                      "A converted PC mod to load: its folder's name under waw_mods_root. Its zones (waw_mod_zones) "
                      "are loaded after each level's, from <waw_mods_root>/<waw_mod>/360/. Empty: no mod, the game as "
                      "it is. Needs a restart");
REXCVAR_DEFINE_STRING(waw_mods_root, "", "waw",
                      "The folder the converted mods are in. Empty: \"mods\" in the user data folder (user_data_root), "
                      "or beside the program when that isn't set. Needs a restart");
REXCVAR_DEFINE_STRING(waw_mod_zones, "mod", "waw",
                      "The mod's zones to load after a level's, in order, separated by commas: file names without "
                      "\".ff\" in the mod's 360 folder. Needs a restart");

namespace {

constexpr uint32_t kZoneInfoSize = 12;      // {name, alloc flags, free flags}
constexpr uint32_t kLevelFlags = 2;         // SV_SpawnServer's level zone: alloc 2, free 2
constexpr size_t kMaxZones = 8;
constexpr const char* kDevice = "\\Device\\Acct2Mods";
constexpr const char* kLink = "mods:";

struct State {
  bool tried = false;                     // set up once, on the first level
  std::atomic<bool> active{false};
  std::vector<std::string> zones;         // the zones found in the mod's folder, in order
  uint32_t folder = 0;                    // guest "mods:"
  std::vector<uint32_t> names;            // guest copies of the zones' names
  uint32_t list = 0;                      // guest room for the level's entry and the mod's
};

std::mutex g_mutex;
State g_state;
std::atomic<int> g_spawning{0};           // inside SV_SpawnServer
thread_local bool t_from_mods = false;    // DB_TryLoadXFileInternal is opening one of the mod's zones

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(base + address, value);
}

// A guest string (bounded), or "" for a null address.
std::string GuestString(const uint8_t* base, uint32_t address, size_t limit = 64) {
  if (!address) return {};
  const char* p = reinterpret_cast<const char*>(base + address);
  return std::string(p, strnlen(p, limit));
}

std::vector<std::string> SplitZones(const std::string& text) {
  std::vector<std::string> out;
  std::string item;
  for (char c : text + ",") {
    if (c == ',') {
      item.erase(0, item.find_first_not_of(" \t"));
      item.erase(item.find_last_not_of(" \t") + 1);
      // plain names only: no folders, no device, no ".ff"
      const bool plain = !item.empty() && item.find_first_of("\\/:.") == std::string::npos;
      if (plain && out.size() < kMaxZones) out.push_back(item);
      item.clear();
    } else {
      item += c;
    }
  }
  return out;
}

std::filesystem::path ModsRoot() {
  const std::string root = REXCVAR_GET(waw_mods_root);
  if (!root.empty()) return root;
  const std::string user = rex::cvar::GetFlagByName("user_data_root");
  if (!user.empty()) return std::filesystem::path(user) / "mods";
  return rex::filesystem::GetExecutableFolder() / "userdata" / "mods";
}

// Once: the device, the zones that are there, the guest strings and room. Under g_mutex.
void SetUp(uint8_t* base) {
  g_state.tried = true;
  const std::string mod = REXCVAR_GET(waw_mod);
  if (mod.empty()) return;
  if (mod.find_first_of("\\/:") != std::string::npos || mod == "." || mod == "..") {
    REXLOG_WARN("waw: mod \"{}\" left out: waw_mod is a folder's name, not a path", mod);
    return;
  }
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* vfs = kernel ? kernel->file_system() : nullptr;
  if (!memory || !vfs) {
    REXLOG_WARN("waw: mod \"{}\" left out: the kernel isn't up", mod);
    return;
  }
  const std::filesystem::path folder = ModsRoot() / mod / "360";
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) {
    REXLOG_WARN("waw: mod \"{}\" left out: no folder {}", mod, folder.string());
    return;
  }
  for (const std::string& zone : SplitZones(REXCVAR_GET(waw_mod_zones))) {
    if (std::filesystem::is_regular_file(folder / (zone + ".ff"), ec))
      g_state.zones.push_back(zone);
    else
      REXLOG_WARN("waw: mod \"{}\": zone {} left out, no {}.ff in its folder", mod, zone, zone);
  }
  if (g_state.zones.empty()) {
    REXLOG_WARN("waw: mod \"{}\" left out: none of its zones is in {}", mod, folder.string());
    return;
  }
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kDevice, folder, /*read_only=*/true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("waw: mod \"{}\" left out: its folder could not be mounted", mod);
    g_state.zones.clear();
    return;
  }
  vfs->RegisterSymbolicLink(kLink, kDevice);
  auto guest_copy = [&](const std::string& text) {
    const uint32_t address = memory->SystemHeapAlloc(static_cast<uint32_t>(text.size() + 1));
    if (address) std::memcpy(base + address, text.c_str(), text.size() + 1);
    return address;
  };
  g_state.folder = guest_copy(kLink);
  for (const std::string& zone : g_state.zones) g_state.names.push_back(guest_copy(zone));
  g_state.list = memory->SystemHeapAlloc(kZoneInfoSize * static_cast<uint32_t>(1 + g_state.zones.size()));
  const bool copied = g_state.folder && g_state.list &&
                      std::none_of(g_state.names.begin(), g_state.names.end(), [](uint32_t a) { return a == 0; });
  if (!copied) {
    REXLOG_WARN("waw: mod \"{}\" left out: no guest memory for its zone list", mod);
    g_state.zones.clear();
    return;
  }
  g_state.active = true;
  std::string list;
  for (const std::string& zone : g_state.zones) list += (list.empty() ? "" : ", ") + zone;
  REXLOG_INFO("waw: mod \"{}\": zones {} after each level's, from {}", mod, list, folder.string());
}

bool Active(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_state.tried) SetUp(base);
  return g_state.active;
}

bool IsModZone(const uint8_t* base, uint32_t name) {
  const std::string text = GuestString(base, name);
  return std::find(g_state.zones.begin(), g_state.zones.end(), text) != g_state.zones.end();
}

}  // namespace

REX_EXTERN(__imp__sub_822BC6C8);
REX_HOOK_RAW(sub_822BC6C8) {  // SV_SpawnServer: marked, so that its level load is told apart
  g_spawning.fetch_add(1);
  __imp__sub_822BC6C8(ctx, base);
  g_spawning.fetch_sub(1);
}

REX_EXTERN(sub_82158A38);  // DB_SyncXAssets

namespace {
// Calls DB_SyncXAssets from inside a translated function, with its frame below the current one, and every register
// put back afterwards (as waw_hooks.cpp's QueueConsoleCommand calls Cbuf_AddText).
void SyncXAssets(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kRoom = 0x400;
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - kRoom) & ~0xFu;
  sub_82158A38(ctx, base);
  ctx = saved;
}
}  // namespace

REX_EXTERN(__imp__sub_82158A68);
REX_HOOK_RAW(sub_82158A68) {  // DB_LoadXAssets: the level's zone, then the mod's
  const uint32_t zones = ctx.r3.u32;
  const bool level = g_spawning.load() > 0 && ctx.r4.u32 == 1 && zones &&
                     LoadU32(base, zones + 4) == kLevelFlags && LoadU32(base, zones + 8) == kLevelFlags;
  if (!level || !Active(base)) {
    __imp__sub_82158A68(ctx, base);
    return;
  }
  const uint32_t list = g_state.list;
  std::memcpy(base + list, base + zones, kZoneInfoSize);  // the level's own entry first, as it was
  for (size_t i = 0; i < g_state.names.size(); ++i) {
    const uint32_t entry = list + kZoneInfoSize * static_cast<uint32_t>(1 + i);
    StoreU32(base, entry + 0, g_state.names[i]);
    StoreU32(base, entry + 4, kLevelFlags);
    StoreU32(base, entry + 8, kLevelFlags);
  }
  REXLOG_INFO("waw: level {}: {} mod zone(s) after it", GuestString(base, LoadU32(base, zones)), g_state.names.size());
  ctx.r3.u64 = list;
  ctx.r4.u64 = 1 + g_state.names.size();
  __imp__sub_82158A68(ctx, base);
  // Wait for the level's zone and the mod's before SV_SpawnServer goes on to compile the level's scripts.
  const auto started = std::chrono::steady_clock::now();
  SyncXAssets(ctx, base);
  REXLOG_INFO("waw: level loaded with its mod zone(s), {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                  std::chrono::steady_clock::now() - started)
                                                                  .count());
}

REX_EXTERN(__imp__sub_821566D8);
REX_HOOK_RAW(sub_821566D8) {  // DB_TryLoadXFileInternal: the mod's zones are opened from "mods:"
  const bool ours = g_state.active && IsModZone(base, ctx.r3.u32);
  t_from_mods = ours;
  __imp__sub_821566D8(ctx, base);
  t_from_mods = false;
}

REX_EXTERN(__imp__sub_8227F2C8);
REX_HOOK_RAW(sub_8227F2C8) {  // the folder the game's files are read from
  if (t_from_mods) {
    ctx.r3.u64 = g_state.folder;
    return;
  }
  __imp__sub_8227F2C8(ctx, base);
}
