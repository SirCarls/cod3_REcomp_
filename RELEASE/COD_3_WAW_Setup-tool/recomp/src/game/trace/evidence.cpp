// cod3 - decomp trace build: evidence from the game's own functions, and
// what the game is doing (the context of each first call).
//
// A few game functions get a collector (EvidenceFor): their slot then takes
// the slow path on every call, and the collector notes what the caller asked
// for, keyed by the caller's return address (ctx.lr), so the writer can name
// the calling function. Only text that is the game's own goes in: constant
// strings from read-only image pages (dvar names, print formats, command
// names), file paths inside the game's folder, names hashed for lookups.
// A name the player typed in the console lives in a writable buffer and is
// left out.
//
// Trace build only (src/game/trace/decomp_trace.cmake).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game/addresses.h"
#include "game/decomp_trace.h"
#include "game/engine.h"
#include "game/game_api.h"
#include "game/guest.h"
#include "game/pc_menu.h"
#include "game/trace/trace_internal.h"

namespace cod3::trace {
namespace {

namespace a = cod3::addr;
using cod3::guest::ReadS32;
using cod3::guest::ReadU32;

// Game functions that addresses.h doesn't list (names from research/symbols.toml).
constexpr uint32_t kFsFOpenFileRead = 0x8252C330;     // FS_FOpenFileRead(qpath, fileHandle_t*, qboolean)
constexpr uint32_t kFsFOpenFileAppend = 0x8252C850;   // FS_FOpenFileAppend(qpath)
constexpr uint32_t kFsFOpenFileWrite = 0x8252C940;    // FS_FOpenFileWrite(qpath)
constexpr uint32_t kFsFileExists = 0x8252CA38;        // FS_FileExists(qpath)
constexpr uint32_t kFsFOpenFileByMode = 0x82531E10;   // FS_FOpenFileByMode(qpath, fileHandle_t*, mode)
constexpr uint32_t kFsReadFile = 0x82532350;          // FS_ReadFile(qpath, void** buffer)
constexpr uint32_t kCmdAddButtonCommand = 0x8246CAA0; // Cmd_AddButtonCommand(name, function)
constexpr uint32_t kCmdAddServerCommand = 0x8246CCB0; // Cmd_AddServerCommand(name, function)
constexpr uint32_t kNameHashInit = 0x820A0118;        // NameHash_Init(record*, const char* text)
constexpr uint32_t kLevelInterface = 0x82A2A2A0;      // levelInterface: the level library's table of engine functions

// The level interface table: about 1,000 slots (G_InitLevelInterface 0x824BB028
// stores up to +4268); read a little past that, page by page.
constexpr uint32_t kLevelSlotBytes = 0x1400;
constexpr uint32_t kPageSize = 0x1000;
// The level libraries all load here (generated/<level>/cod3_pch.h: REX_IMAGE_BASE 0x89000000).
constexpr uint32_t kLevelImageBase = 0x89000000;
constexpr uint32_t kLevelImageEnd = 0x8A000000;

// ---------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------

uint32_t PageProtect(uint32_t address) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* memory = kernel_state ? kernel_state->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  uint32_t protect = 0;
  if (!heap || !heap->QueryProtect(address & ~(kPageSize - 1), &protect)) return 0;
  return protect;
}

bool Readable(uint32_t address) { return (PageProtect(address) & rex::memory::kMemoryProtectRead) != 0; }

// Whether `address` is in a read-only page: the game's (or a level library's)
// own constant data. Pages are asked once; a level library loads at the same
// address as the one before, so its pages are asked again for each level.
bool ReadOnly(uint32_t address) {
  static std::mutex mutex;
  static std::unordered_map<uint32_t, bool> pages;  // page -> read-only
  static uint32_t pages_level = 0;                  // the level library the level range's pages are from
  const uint32_t page = address & ~(kPageSize - 1);
  const bool in_level = page >= kLevelImageBase && page < kLevelImageEnd;
  const uint32_t level = in_level ? CurrentLevelModule() : 0;
  {
    std::lock_guard lock(mutex);
    if (in_level && level != pages_level) {
      // Another level library: forget the old one's pages (the executable's stay).
      std::erase_if(pages, [](const auto& p) { return p.first >= kLevelImageBase && p.first < kLevelImageEnd; });
      pages_level = level;
    }
    if (auto it = pages.find(page); it != pages.end()) return it->second;
  }
  // Asked without holding the lock: the heap takes the emulator's global
  // lock, which game code can hold while it runs.
  const uint32_t protect = PageProtect(page);
  const bool read_only = (protect & rex::memory::kMemoryProtectRead) && !(protect & rex::memory::kMemoryProtectWrite);
  std::lock_guard lock(mutex);
  if (!in_level || level == pages_level) pages.emplace(page, read_only);
  return read_only;
}

std::string GuestText(uint8_t* base, uint32_t address, size_t max_chars) {
  if (address < 0x10000) return {};
  return cod3::guest::ReadString(base, address, max_chars);
}

// The game's constant text at `address` (read-only data), or "".
std::string ConstantText(uint8_t* base, uint32_t address, size_t max_chars) {
  if (address < 0x10000 || !ReadOnly(address)) return {};
  return GuestText(base, address, max_chars);
}

bool Printable(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F;
  });
}

// A path inside the game's own folder ("maps/saint_lo.bsp", "game:\..."), or "".
std::string GamePath(std::string path) {
  if (!Printable(path) || path.size() > 160) return {};
  for (const char* device : {"game:\\", "game:/", "d:\\", "D:\\"}) {
    if (path.rfind(device, 0) == 0) {
      path.erase(0, std::char_traits<char>::length(device));
      return path;
    }
  }
  // Anything else on a device ("cache:", "hdd:", "save:") or absolute isn't game data.
  if (path.find(':') != std::string::npos || path[0] == '/' || path[0] == '\\') return {};
  return path;
}

// The command name at the start of a console line ("map", "+attack"), or "".
std::string CommandName(std::string_view line) {
  size_t start = line.find_first_not_of(" \t");
  if (start == std::string_view::npos) return {};
  size_t end = start;
  while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_' ||
                               line[end] == '+' || line[end] == '-'))
    ++end;
  if (end == start || end - start > 48) return {};
  std::string name(line.substr(start, end - start));
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name;
}

// ---------------------------------------------------------------------------
// Collectors (one per game function; ctx.lr is the caller)
// ---------------------------------------------------------------------------

// A call the port itself makes (guest::Call) has no guest caller (lr 0): the
// collectors below skip it before reading anything.

void OnDvarFind(Slot&, PPCContext& ctx, uint8_t* base) {
  if (!ctx.lr) return;
  NoteEvidence(Evidence::kDvar, CallSite(uint32_t(ctx.lr)), 0, ConstantText(base, ctx.r3.u32, 64));
}

void OnDvarRegister(Slot&, PPCContext& ctx, uint8_t* base) {
  if (!ctx.lr) return;
  const std::string name = ConstantText(base, ctx.r3.u32, 64);
  if (name.empty()) return;
  const std::string value = ConstantText(base, ctx.r4.u32, 64);
  NoteEvidence(Evidence::kDvarRegister, CallSite(uint32_t(ctx.lr)), 0,
               ctx.r4.u32 && Printable(value) ? fmt::format("{} = \"{}\"", name, value) : name);
}

void OnDvarSet(Slot&, PPCContext& ctx, uint8_t* base) {
  if (!ctx.lr) return;
  NoteEvidence(Evidence::kDvarSet, CallSite(uint32_t(ctx.lr)), 0, ConstantText(base, ctx.r3.u32, 64));
}

void OnFile(Slot&, PPCContext& ctx, uint8_t* base) {
  if (!ctx.lr) return;
  NoteEvidence(Evidence::kFile, CallSite(uint32_t(ctx.lr)), 0, GamePath(GuestText(base, ctx.r3.u32, 200)));
}

void OnCommandRun(Slot&, PPCContext& ctx, uint8_t* base) {
  if (!ctx.lr) return;
  // Only the command's name; whatever follows it can be the player's text.
  NoteEvidence(Evidence::kCommandRun, CallSite(uint32_t(ctx.lr)), 0, CommandName(GuestText(base, ctx.r3.u32, 64)));
}

void OnCommandAdd(Slot&, PPCContext& ctx, uint8_t* base) {
  const uint32_t handler = ctx.r4.u32;
  const std::string name = ConstantText(base, ctx.r3.u32, 64);
  if (handler && Printable(name)) NoteEvidence(Evidence::kCommandHandler, CallSite(uint32_t(ctx.lr)), handler, name);
}

void OnNameHash(Slot&, PPCContext& ctx, uint8_t* base) {
  if (!ctx.lr) return;
  std::string text = GuestText(base, ctx.r4.u32, 64);
  if (Printable(text)) NoteEvidence(Evidence::kNameHash, CallSite(uint32_t(ctx.lr)), 0, text);
}

// ---------------------------------------------------------------------------
// What the game is doing (the context of first calls)
// ---------------------------------------------------------------------------

// Whether the local player's interaction (vehicle, turret, rowboat, QTE ...)
// runs (the same test as game/fov.cpp).
bool InteractionRunning(uint8_t* base) {
  constexpr uint32_t kMaxLocalClients = 4;
  const uint32_t client = ReadU32(base, a::kActiveLocalClient);
  if (client >= kMaxLocalClients) return false;
  const uint32_t interaction = ReadU32(base, a::interact::kPerClient + client * 4);
  if (interaction < 0x10000 || interaction >= 0xC0000000u || (interaction & 3)) return false;
  return ReadU32(base, interaction + a::interact::kState) != 0;
}

// The functions behind the level interface table's slots, once per level.
void ScanLevelSlots(uint8_t* base) {
  static uint32_t scanned_table = 0;
  static std::string scanned_level;
  const uint32_t table = ReadU32(base, kLevelInterface);
  const std::string level = CurrentLevel();
  if (!table || (table == scanned_table && level == scanned_level)) return;
  scanned_table = table;
  scanned_level = level;
  for (uint32_t offset = 0; offset < kLevelSlotBytes; offset += 4) {
    const uint32_t slot = table + offset;
    if ((slot & (kPageSize - 1)) == 0 || offset == 0) {
      if (!Readable(slot)) break;
    }
    const uint32_t function = ReadU32(base, slot);
    if (IsMainFunction(function)) NoteEvidence(Evidence::kLevelSlot, 0, function, fmt::format("slot 0x{:X}", offset));
  }
}

// Cbuf_Execute, once a frame on the game's main thread: the context, four
// times a second.
void OnFrame(Slot&, PPCContext& ctx, uint8_t* base) {
  static uint32_t last_ms = 0;
  static uint32_t mapname = 0, intro = 0;
  const uint32_t now = NowMs();
  if (last_ms && now - last_ms < 250) return;
  last_ms = now;
  if (ReadU32(base, a::kCvarVars) == 0) return;  // before Cvar_Init
  if (!mapname) mapname = Engine::CvarFind(ctx, base, "mapname");
  if (!intro) intro = Engine::CvarFind(ctx, base, "introScreen");

  const uint32_t running_cvar = ReadU32(base, a::crossref::kSvRunningPtr);
  const bool running = running_cvar && ReadS32(base, running_cvar + a::cvar::kInteger) != 0;
  const bool movie = ReadU32(base, a::movie::kHandle) != 0;
  const bool menu = cod3::pcmenu::MenuActive();
  if (!running) {
    SetContext("front end", movie ? "movie" : menu ? "menu" : "front end");
    return;
  }
  std::string level = mapname ? game::CvarString(base, mapname, 64) : std::string();
  if (level.empty() || !Printable(level)) level = "?";
  std::string state = movie ? "cutscene" : (intro && ReadS32(base, intro + a::cvar::kInteger) != 0) ? "intro" : "playing";
  if (InteractionRunning(base)) state += "+vehicle";
  if (menu) state += "+menu";
  SetContext(level, state);
  ScanLevelSlots(base);
}

// SV_SpawnServer(mapname, ...): a level load starts.
void OnLevelLoad(Slot&, PPCContext& ctx, uint8_t* base) {
  std::string map = GuestText(base, ctx.r3.u32, 64);
  if (!Printable(map)) map = "?";
  SetCurrentLevelModule(map);
  SetContext(map, "loading");
}

// The loading screen and the briefing drawn as it (called while a level loads).
void OnLoadingScreen(Slot&, PPCContext&, uint8_t*) {
  static uint32_t last_ms = 0;
  const uint32_t now = NowMs();
  if (last_ms && now - last_ms < 250) return;
  last_ms = now;
  SetContext("", "loading");
}

// The briefing played on after the load (0x824EEBA0, inside SV_SpawnServer).
void OnBriefing(Slot&, PPCContext&, uint8_t*) { SetContext("", "cutscene"); }

}  // namespace

const char* EvidenceKindName(Evidence kind) {
  switch (kind) {
    case Evidence::kPrint: return "print";
    case Evidence::kDvar: return "dvar";
    case Evidence::kDvarRegister: return "dvar_register";
    case Evidence::kDvarSet: return "dvar_set";
    case Evidence::kFile: return "file";
    case Evidence::kCommandRun: return "command";
    case Evidence::kCommandHandler: return "command_handler";
    case Evidence::kNameHash: return "name";
    case Evidence::kLevelSlot: return "level_slot";
  }
  return "?";
}

EvidenceFn EvidenceFor(uint32_t address) {
  switch (address) {
    case a::kCbufExecute: return &OnFrame;
    case a::cheats::kSpawnServer: return &OnLevelLoad;
    case a::movie::kLoadingScreen:
    case a::movie::kLoadingMovie: return &OnLoadingScreen;
    case a::movie::kFinishLevelMovie: return &OnBriefing;
    case a::kCvarFindVar: return &OnDvarFind;
    case a::kCvarGet: return &OnDvarRegister;
    case a::kCvarSet2: return &OnDvarSet;
    case kFsFOpenFileRead:
    case kFsFOpenFileAppend:
    case kFsFOpenFileWrite:
    case kFsFileExists:
    case kFsFOpenFileByMode:
    case kFsReadFile: return &OnFile;
    case a::kCmdExecuteString: return &OnCommandRun;
    case a::kCmdAddCommand:
    case kCmdAddButtonCommand:
    case kCmdAddServerCommand: return &OnCommandAdd;
    case kNameHashInit: return &OnNameHash;
    default: return nullptr;
  }
}

// game/print_hooks.cpp: Com_Printf / Com_DPrintf / Com_Error, which the port
// replaces (their retail bodies are empty), so their slots never run.
void NotePrint(PPCContext& ctx, uint8_t* base, uint32_t format, bool constant) {
  if (!constant || !format) return;
  std::string text = GuestText(base, format, 200);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  if (!text.empty() && static_cast<unsigned char>(text[0]) < 0x20) text.erase(0, 1);  // Com_Error's code byte
  NoteEvidence(Evidence::kPrint, CallSite(uint32_t(ctx.lr)), 0, text);
}

}  // namespace cod3::trace
