// cod4mp - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation, the multiplayer program: game scripts read
// from a folder of the player's own (cod4_script_folder, off by default), for script mods such as Promod.
// [waw-decomp-cod4] 2026-11-11
//
// The console reads every game script (.gsc) from the fastfiles. In the multiplayer program (x360-family's decompile,
// dev/x360-family/cod4_360/mp; the names are IW3's, as KisakCOD has them for the PC game): Scr_LoadScriptInternal
// 0x82220518 makes "<name>.gsc" (the format at 0x8206D6D8) and calls Scr_AddSourceBuffer 0x8221EF90, which calls
// Scr_ReadFile_FastFile 0x8221EEF0 (r3 the name, r4 the name with ".gsc", r5 the place in the script that asked for
// it). That one looks the file up as a rawfile (DB_FindXAssetHeader 0x8229E9F0, asset type 0x20; the text at the
// rawfile's +8), registers it with Scr_AddSourceBufferInternal 0x8221EDB0(name with ".gsc", place, text, length, 1,
// 1) and returns the text, which the compiler then reads. The PC game can also read loose files from a mod's folder;
// the console has no such path.
//
// With cod4_script_folder set, the hook below looks for <folder>/<name with ".gsc"> first (for example
// <folder>/maps/mp/gametypes/_globallogic.gsc; the letter case of the folder's files doesn't matter). When the file is
// there, its text is copied into guest memory and handed on exactly as the fastfile's would be; when it isn't, the
// game's own reader runs. Nothing is written anywhere, and no mod's file comes with the port: the folder is the
// player's own. Multiplayer only (the campaign's loader is at other addresses and isn't hooked).

#ifdef COD4_MP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default_mp/cod4mp_pch.h"

REXCVAR_DEFINE_STRING(cod4_script_folder, "", "cod4",
                      "Multiplayer: a folder of game scripts (.gsc) that are read instead of the game's own where it "
                      "has them, at the same paths (for example <folder>/maps/mp/gametypes/_globallogic.gsc), for "
                      "script mods such as Promod; empty = off (the game's own scripts only)");

// Scr_AddSourceBufferInternal and the game's own Scr_ReadFile_FastFile (see the top of this file).
DECLARE_REX_FUNC(sub_8221EDB0);
REX_EXTERN(__imp__sub_8221EEF0);

namespace {

constexpr size_t kMaxNameLength = 256;
constexpr size_t kMaxScriptBytes = size_t{8} << 20;  // a larger file is taken for a mistake, not a script

// A script read from the folder, kept with its copy in guest memory, so a level that loads it again with the same
// text reuses the copy.
struct GuestText {
  std::string text;
  uint32_t address = 0;  // the copy, followed by a zero
};

std::mutex g_mutex;
std::unordered_map<std::string, GuestText> g_texts;  // by the name with ".gsc"
bool g_announced = false;

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// The name the game asks for as a path under the folder ('\' made '/'), or empty when it can't be one: empty, too
// long, with a control character, absolute, with a drive or with "..". The game's own names are never like that.
std::string RelativeName(const uint8_t* base, uint32_t address) {
  if (address < 0x10000u) return {};
  const char* text = reinterpret_cast<const char*>(base + address);
  const size_t length = strnlen(text, kMaxNameLength);
  if (length == 0 || length == kMaxNameLength) return {};
  std::string name(text, length);
  for (char& c : name) {
    if (static_cast<unsigned char>(c) < 0x20) return {};
    if (c == '\\') c = '/';
  }
  if (name.front() == '/' || name.find(':') != std::string::npos) return {};
  for (const std::filesystem::path& part : std::filesystem::path(name)) {
    if (part == "..") return {};
  }
  return name;
}

// The folder's file for `name`: the exact path, or else the one whose every part matches whatever the letter case
// (the game asks in lower case; a file saved as "_GlobalLogic.gsc" is still found where file names are
// case-sensitive). Empty when there is none.
std::filesystem::path FindFile(const std::filesystem::path& folder, const std::string& name) {
  std::error_code error;
  const std::filesystem::path exact = folder / std::filesystem::path(name);
  if (std::filesystem::is_regular_file(exact, error)) return exact;
  std::filesystem::path at = folder;
  for (const std::filesystem::path& part : std::filesystem::path(name)) {
    const std::string wanted = Lower(part.string());
    std::filesystem::path found;
    std::filesystem::directory_iterator entry(at, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
      if (Lower(entry->path().filename().string()) == wanted) {
        found = entry->path();
        break;
      }
    }
    if (found.empty()) return {};
    at = found;
  }
  return std::filesystem::is_regular_file(at, error) ? at : std::filesystem::path();
}

// The file's text (a UTF-8 mark at its start left out); false when it can't be read or is too large.
bool ReadText(const std::filesystem::path& path, std::string& text) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (file.bad() || text.size() > kMaxScriptBytes) return false;
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
  return true;
}

// The text's copy in guest memory (the system heap), followed by a zero; 0 when there is no room. A copy made for
// an older text of the same file stays where it is (the game may still point into it), so memory grows only when the
// player changes a file between two loads.
uint32_t GuestCopy(uint8_t* base, const std::string& name, std::string&& text) {
  GuestText& kept = g_texts[name];
  if (kept.address != 0 && kept.text == text) return kept.address;
  const uint32_t address =
      rex::system::kernel_state()->memory()->SystemHeapAlloc(static_cast<uint32_t>(text.size() + 1));
  if (address == 0) return 0;
  std::memcpy(base + address, text.data(), text.size());
  base[address + text.size()] = 0;
  kept.text = std::move(text);
  kept.address = address;
  return address;
}

// The folder as a path (the setting is UTF-8 text).
std::filesystem::path FolderPath(const std::string& setting) {
  return std::filesystem::path(std::u8string(setting.begin(), setting.end()));
}

}  // namespace

// Scr_ReadFile_FastFile 0x8221EEF0, replaced: the folder's file when it has one, else the game's own reader.
REX_HOOK_RAW(sub_8221EEF0) {
  const std::string setting = REXCVAR_GET(cod4_script_folder);
  if (setting.empty()) {
    __imp__sub_8221EEF0(ctx, base);
    return;
  }
  const uint32_t name_address = ctx.r4.u32;  // the name with ".gsc"
  const uint32_t place = ctx.r5.u32;         // where the script that asked for it is
  const std::string name = RelativeName(base, name_address);
  uint32_t text_address = 0;
  size_t length = 0;
  {
    std::lock_guard lock(g_mutex);
    const std::filesystem::path folder = FolderPath(setting);
    if (!g_announced) {
      g_announced = true;
      std::error_code error;
      REXLOG_INFO("cod4mp: script folder {} ({}): the game's scripts are read from it where it has them; the first "
                  "asked for: \"{}\"",
                  folder.string(), std::filesystem::is_directory(folder, error) ? "found" : "NOT FOUND", name);
    }
    if (!name.empty()) {
      const std::filesystem::path path = FindFile(folder, name);
      std::string text;
      if (!path.empty() && ReadText(path, text)) {
        length = text.size();
        text_address = GuestCopy(base, name, std::move(text));
        if (text_address != 0) {
          REXLOG_INFO("cod4mp: script folder: {} read from the folder ({} bytes, guest {:08X})", name, length,
                      text_address);
        } else {
          REXLOG_WARN("cod4mp: script folder: {}: no guest memory for {} bytes; the game's own is used", name,
                      length);
        }
      } else if (!path.empty()) {
        REXLOG_WARN("cod4mp: script folder: {} could not be read (or is over {} bytes); the game's own is used",
                    path.string(), kMaxScriptBytes);
      }
    }
  }
  if (text_address == 0) {
    __imp__sub_8221EEF0(ctx, base);
    return;
  }
  // As the game's own reader does with a rawfile's text: register it, then return it for the compiler.
  ctx.r3.u64 = name_address;
  ctx.r4.u64 = place;
  ctx.r5.u64 = text_address;
  ctx.r6.u64 = length;
  ctx.r7.u64 = 1;
  ctx.r8.u64 = 1;
  sub_8221EDB0(ctx, base);
  ctx.r3.u64 = text_address;
}

#endif  // COD4_MP
