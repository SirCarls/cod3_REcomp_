// waw - World at War: drawing problems of the port worked around in the game's own code. [waw-decomp]
//
// Each function here is named by a [[midasm_hook]] block in config/default.toml, which says where the codegen calls
// it. Addresses are the disc program's (default.xex: campaign, co-op and zombies).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/default/waw_pch.h"

// Owner, 2026-11-09 18:17 ("blue soldier.") and 18:27: soldiers, leaves and barbed wire come out blue or purple, with
// a fine grid of dark dots over trunks, sandbags and log walls. Turning the game's own shadow settings off one at a
// time on pel1a found the cause: with sm_sunEnable 0 (the sun's shadow map) every patch and dot is gone; nothing else
// changed it (spot shadows, shadow depth bias, shine/bump/color maps, and the GPU emulation's own settings tried:
// depth_float24_convert_in_pixel_shader, depth_float24_round, half_pixel_offset, native_2x_msaa, readback_resolve,
// texture_replacement_enabled, gpu_allow_invalid_fetch_constants, async_shader_compilation). So the sun shadow map
// is read wrong somewhere in the GPU emulation. Until that is found and fixed, the port leaves the sun's shadows off.
REXCVAR_DEFINE_BOOL(waw_sun_shadows, false, "waw",
    "The sun's shadows (the game's sm_sunEnable). Off by default for now: through the GPU emulation they draw as "
    "blue/purple patches and a dot grid on models and foliage. Set true to draw them anyway");

// 2026-11-09 night: the sun shadow map's own Direct3D objects, logged to find out how the receivers read it. The
// gpu_census run (X360-GAMES.md) shows the map drawn into a 1x depth-only D24FS8 target and copied out raw as 32-bit
// words; Call of Duty 4 does the same and is clean, but its receivers take 4 point samples placed by the shader's own
// arithmetic, while World at War's take 9 with the fetch instruction's own offsets plus getWeights2D. What the log
// should settle: the format, size, tiling, endianness, swizzle and filters the receivers sample the map with.
REXCVAR_DEFINE_INT32(waw_debug_sun_shadowmap, 0, "waw",
    "Debug: every this many seconds, log the sun shadow map's Direct3D objects (its record from the render target "
    "setup, and the first 16 words of each object it points to: the textures' fetch constants) (0: off)");

// 2026-11-10: the map's own content. Lit lines run through fully shadowed areas, which only taps reading a "lit" depth
// can give, so the map itself is saved once to look at (untiled on the PC by results/waw-decomp-tools/gfx/smdump.py).
// The copy is in guest memory only when the GPU emulation writes resolves there (--vulkan_resolve_into_textures=false).
REXCVAR_DEFINE_STRING(waw_debug_dump_sun_shadowmap, "", "waw",
    "Debug: save the sun shadow map's memory (8 MB, as the game's image record gives it: tiled, big-endian 32-bit "
    "words) once to this file, 70 s after the scene setup first runs (empty: off)");

namespace {

// R_InitRenderTargets 0x82430AF0 sets up the sun shadow map through 0x82430610 (1024 wide, two 1024-high halves) into
// a record at 0x84E82190 + 280: +0x0 the render target, +0x4 an A8R8G8B8 texture, +0x8 a texture in the shadow map
// depth format, +0xC width, +0x10 height. That depth format is a variable, the word at 0x84A84E10.
constexpr uint32_t kSunShadowRecord = 0x84E822A8u;
constexpr uint32_t kShadowmapFormat = 0x84A84E10u;

uint32_t LoadGuestWord(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + address));
}

// Addresses in the virtual heaps and the program image (0x40000000-0x9FFEFFFF) and in the physical-memory views
// (0xA0000000-0xFFFEFFFF, where the Direct3D objects of the render targets live) are read; the guest's whole 4 GB
// is mapped on the host. 2026-11-10: the first log (23:46) showed the record's +0x4 and +0x8 at 0xBF908xxx, which
// the first version did not follow.
bool IsReadableGuestAddress(uint32_t address) {
  const bool virtual_memory = address >= 0x40000000u && address < 0x9FFF0000u;
  const bool physical_view = address >= 0xA0000000u && address < 0xFFFF0000u;
  return (virtual_memory || physical_view) && (address & 3u) == 0;
}

std::string GuestWords(const uint8_t* base, uint32_t address, uint32_t count) {
  if (!IsReadableGuestAddress(address) || !IsReadableGuestAddress(address + 4u * (count - 1))) return " (not read)";
  std::string out;
  for (uint32_t i = 0; i < count; ++i) {
    char word[12];
    std::snprintf(word, sizeof(word), " %08X", LoadGuestWord(base, address + 4u * i));
    out += word;
  }
  return out;
}

// Saves the map's memory once: the image record (the record's +0x0) gives the address (+0x18) and the size (+0xC).
void DumpSunShadowmap(const uint8_t* base, const std::string& file) {
  static const auto first_call = std::chrono::steady_clock::now();
  static bool done = false;
  if (done || std::chrono::steady_clock::now() - first_call < std::chrono::seconds(70)) return;
  done = true;
  const uint32_t image = LoadGuestWord(base, kSunShadowRecord);
  if (!IsReadableGuestAddress(image)) {
    REXLOG_INFO("waw: sun shadow map dump: no image record ({:08X})", image);
    return;
  }
  const uint32_t memory = LoadGuestWord(base, image + 0x18u);
  const uint32_t size = LoadGuestWord(base, image + 0xCu);
  if (size == 0 || size > 0x2000000u || !IsReadableGuestAddress(memory) || !IsReadableGuestAddress(memory + size - 4u)) {
    REXLOG_INFO("waw: sun shadow map dump: memory {:08X} size {:X} not read", memory, size);
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
  FILE* f = std::fopen(file.c_str(), "wb");
  const bool ok = f && std::fwrite(base + memory, 1, size, f) == size;
  if (f) std::fclose(f);
  REXLOG_INFO("waw: sun shadow map dump: {:08X} ({} bytes) to {}: {}", memory, size, file, ok ? "saved" : "failed");
}

void LogSunShadowmap(const uint8_t* base) {
  static auto next_log = std::chrono::steady_clock::time_point{};
  const int32_t every = REXCVAR_GET(waw_debug_sun_shadowmap);
  const auto now = std::chrono::steady_clock::now();
  if (now < next_log) return;
  next_log = now + std::chrono::seconds(every);
  REXLOG_INFO("waw: sun shadow map: depth format word {:08X}; record {:08X}:{}", LoadGuestWord(base, kShadowmapFormat),
              kSunShadowRecord, GuestWords(base, kSunShadowRecord, 6));
  for (uint32_t slot = 0; slot < 3; ++slot) {
    const uint32_t object = LoadGuestWord(base, kSunShadowRecord + 4u * slot);
    REXLOG_INFO("waw: sun shadow map: record +0x{:X} -> {:08X}:{}", 4u * slot, object, GuestWords(base, object, 16));
    // 2026-11-10: +0x0 points at the game's image record (+0x4 its Direct3D texture, +0x10 1024 and 2048, +0x18 the
    // map's memory), so that texture is logged too: the fetch constant the receivers sample the map with.
    if (slot == 0 && IsReadableGuestAddress(object)) {
      const uint32_t texture = LoadGuestWord(base, object + 4u);
      REXLOG_INFO("waw: sun shadow map: image +0x4 -> texture {:08X}:{}", texture, GuestWords(base, texture, 16));
    }
  }
}

}  // namespace

// The scene setup 0x82403400, at 0x824039C0 (`cmplwi cr6, r9, 0`): r9 has just been loaded with sm_sunEnable's
// value (its dvar at 0x84E4AF74, the value byte at +0xC). When it is 0 the game skips the sun shadow for this frame,
// exactly as with `sm_sunEnable 0` typed in the console. Not a branch target.
void waw_sun_shadows_hook(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(waw_debug_sun_shadowmap) > 0) LogSunShadowmap(base);
  const std::string dump_file = REXCVAR_GET(waw_debug_dump_sun_shadowmap);
  if (!dump_file.empty()) DumpSunShadowmap(base, dump_file);
  if (!REXCVAR_GET(waw_sun_shadows)) ctx.r9.u64 = 0;
}
