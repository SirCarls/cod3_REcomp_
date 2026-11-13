// cod2 / cod2mp - tests: the game's own dvars in the log (see dvar_peek.h). [f2-menus]

#include "game/dvar_peek.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(cod2_debug_dvars, "", "cod2/Debug",
                      "Debug: the game's own dvars to write to the log, \"name,name,...\" (each looked up with the "
                      "game's Dvar_FindVar; one it doesn't have shows as name=?)");
REXCVAR_DEFINE_INT32(cod2_debug_dvars_seconds, 10, "cod2/Debug",
                     "Debug: seconds between two lines of cod2_debug_dvars")
    .range(1, 600);

#if defined(COD2_MP)
REX_EXTERN(__imp__sub_823069E0);  // Dvar_FindVar(name r3) -> the dvar, or 0
#define COD2_DVAR_FIND __imp__sub_823069E0
#else
REX_EXTERN(__imp__sub_822E7CB0);
#define COD2_DVAR_FIND __imp__sub_822E7CB0
#endif

namespace cod2::dvar_peek {

namespace {

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

float LoadBeF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadBe32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

}  // namespace

// The game's Dvar_FindVar for `name`, from inside a hook on the game's own thread: the name goes on the guest stack
// below the current frame, with a fresh frame below it for the call; every register is put back afterwards.
uint32_t FindDvar(PPCContext& ctx, uint8_t* base, const std::string& name) {
  const PPCContext saved = ctx;
  const uint32_t buffer = (ctx.r1.u32 - 0x200 - uint32_t(name.size())) & ~0xFu;
  std::memcpy(base + buffer, name.c_str(), name.size() + 1);
  ctx.r1.u32 = buffer - 0x100;
  ctx.r3.u64 = buffer;
  COD2_DVAR_FIND(ctx, base);
  const uint32_t dvar = ctx.r3.u32;
  ctx = saved;
  ctx.fpscr.restoreGuestBits(saved.fpscr.csr);
  return dvar;
}

namespace {

std::string ValueText(const uint8_t* base, uint32_t dvar) {
  char text[96];
  const uint8_t type = base[dvar + 6];
  const uint32_t v = dvar + 8;
  switch (type) {
    case 0: return base[v] ? "1" : "0";
    case 1: std::snprintf(text, sizeof(text), "%g", double(LoadBeF32(base, v))); return text;
    case 2:
      std::snprintf(text, sizeof(text), "(%g %g)", double(LoadBeF32(base, v)), double(LoadBeF32(base, v + 4)));
      return text;
    case 3:
      std::snprintf(text, sizeof(text), "(%g %g %g)", double(LoadBeF32(base, v)), double(LoadBeF32(base, v + 4)),
                    double(LoadBeF32(base, v + 8)));
      return text;
    case 4:
      std::snprintf(text, sizeof(text), "(%g %g %g %g)", double(LoadBeF32(base, v)), double(LoadBeF32(base, v + 4)),
                    double(LoadBeF32(base, v + 8)), double(LoadBeF32(base, v + 12)));
      return text;
    case 5:
    case 6: return std::to_string(int32_t(LoadBe32(base, v)));
    case 7: {
      const uint32_t p = LoadBe32(base, v);
      if (p == 0) return "\"\"";
      std::string s = "\"";
      for (uint32_t i = 0; i < 40; ++i) {
        const char c = char(base[p + i]);
        if (c == 0) break;
        s += (c >= 32 && c < 127) ? c : '.';
      }
      return s + "\"";
    }
    case 8:
      std::snprintf(text, sizeof(text), "(%u %u %u %u)", base[v], base[v + 1], base[v + 2], base[v + 3]);
      return text;
    default: std::snprintf(text, sizeof(text), "type%u:%#x", unsigned(type), LoadBe32(base, v)); return text;
  }
}

}  // namespace

void OnGameFrame(PPCContext& ctx, uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static bool parsed = false;
  static std::vector<std::string> names;
  static Clock::time_point next{};
  if (!parsed) {
    parsed = true;
    const std::string all = REXCVAR_GET(cod2_debug_dvars);
    size_t at = 0;
    while (at <= all.size() && !all.empty()) {
      size_t end = all.find(',', at);
      if (end == std::string::npos) end = all.size();
      std::string name = all.substr(at, end - at);
      at = end + 1;
      while (!name.empty() && name.front() == ' ') name.erase(name.begin());
      while (!name.empty() && name.back() == ' ') name.pop_back();
      bool plain = !name.empty() && name.size() < 64;
      for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) plain = false;
      if (plain) names.push_back(name);
    }
    next = Clock::now() + std::chrono::seconds(REXCVAR_GET(cod2_debug_dvars_seconds));
  }
  if (names.empty()) return;
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(REXCVAR_GET(cod2_debug_dvars_seconds));
  std::string line;
  int on_line = 0;
  for (const std::string& name : names) {
    const uint32_t dvar = FindDvar(ctx, base, name);
    line += (line.empty() ? "" : " ") + name + "=" + (dvar ? ValueText(base, dvar) : std::string("?"));
    if (++on_line == 8) {
      REXLOG_INFO("cod2: dvars: {}", line);
      line.clear();
      on_line = 0;
    }
  }
  if (!line.empty()) REXLOG_INFO("cod2: dvars: {}", line);
}

}  // namespace cod2::dvar_peek
