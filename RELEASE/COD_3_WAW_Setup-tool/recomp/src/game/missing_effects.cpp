// cod3 - [acct2-main] 2026-11-05 the effect events a level's script asks for: which the level lacks, and how it defines
// the ones looked at (debug only; no change in behavior).
//
// Owner 2026-11-05 17:40 "a missing effect when using the mortar ... a glowing reticle", 18:27 "it shows on the
// ground" (Forest's mortar). A level script plays an effect event with EffectEventPlay (0x824B3270); the request is
// queued and the effect group (0x82533978) looks the event up by name in the level's resources: 0x82472548(out r3,
// r4, r5, name r6, type r7 "event") sets out[1] to the resource or 0 (then the group plays nothing). The resource is
// {0x15, record}; the record is 21 words: [0..4] up to five .effect paths (one picked at random; the particle is the
// path's file name), the rest the group's other parts (sound, rumble, light, chained group; 0 when none) and flags.
// Forest's barricade bombing (forest.dll mortar_flare_func1-3) plays "flare_light_loop" on the three blockade targets.
// Run 15 (19:35) shows Forest does define it (map/nightdrop/flare_light_loop_red.effect, as Laison and Night Drop do;
// the definition isn't a plain string in forest.cod), so the lookup isn't what hides the flares.
//
// With cod3_debug_effects (effects_debug.cpp's cvar), once per name:
//   "cod3: effects: the level has no effect event 'NAME'"   (most are normal: footsteps etc. probe several names)
//   "cod3: effects: event 'NAME' resource R [..] record P: 24 words, [i]='text' for the words that point at text"
//     for the events below, looked up once in each set of the level's resources.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DECLARE(bool, cod3_debug_effects);

namespace {

namespace g = cod3::guest;

constexpr uint32_t kLookup = 0x82472548;
constexpr char kEventType[] = "event";
constexpr const char* kWatched[] = {"flare_light_loop"};
constexpr size_t kMaxLogged = 512;

std::mutex g_mutex;
std::set<std::string> g_logged;
std::set<uint64_t> g_probed;  // the (r4, r5) resource sets already looked in
thread_local bool t_inside = false;

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xF0000000u; }

std::string Text(uint8_t* base, uint32_t p) {
  if (!Valid(p)) return {};
  std::string s = g::ReadString(base, p, 64);
  if (s.size() < 3) return {};
  for (char c : s) {
    if (c < 0x20 || c > 0x7E) return {};
  }
  return s;
}

void DumpRecord(uint8_t* base, const std::string& name, uint32_t res) {
  const uint32_t rec = g::ReadU32(base, res + 4);
  std::string words, texts;
  char buf[96];
  for (uint32_t i = 0; i < 24; ++i) {
    const uint32_t w = Valid(rec) ? g::ReadU32(base, rec + 4 * i) : 0;
    std::snprintf(buf, sizeof(buf), " %08X", w);
    words += buf;
    const std::string s = Text(base, w);
    if (!s.empty()) {
      std::snprintf(buf, sizeof(buf), " [%u]='%s'", i, s.c_str());
      texts += buf;
    }
  }
  REXLOG_INFO("cod3: effects: event '{}' resource {:08X} [{:08X} {:08X}] record {:08X}:{}{}", name, res,
              g::ReadU32(base, res), g::ReadU32(base, res + 4), rec, words, texts);
}

}  // namespace

REX_EXTERN(__imp__sub_82472548);  // the resource lookup (not in config/names.toml)
REX_HOOK_RAW(sub_82472548) {
  const uint32_t out = ctx.r3.u32, set_a = ctx.r4.u32, set_b = ctx.r5.u32;
  const uint32_t name_ptr = ctx.r6.u32, type_ptr = ctx.r7.u32;
  __imp__sub_82472548(ctx, base);
  if (t_inside || !Valid(out) || !Valid(name_ptr) || !Valid(type_ptr)) return;
  if (!REXCVAR_GET(cod3_debug_effects)) return;
  if (std::memcmp(base + type_ptr, kEventType, sizeof(kEventType)) != 0) return;
  const uint32_t res = g::ReadU32(base, out + 4);

  const std::string name = g::ReadString(base, name_ptr, 64);
  bool probe = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logged.size() < kMaxLogged) {
      if (res == 0) {
        if (g_logged.insert("no:" + name).second)
          REXLOG_INFO("cod3: effects: the level has no effect event '{}'", name);
      } else {
        for (const char* w : kWatched) {
          if (name == w && g_logged.insert(std::string("dump:") + w).second) DumpRecord(base, name, res);
        }
      }
    }
    if (g_probed.size() < 64) probe = g_probed.insert((uint64_t(set_a) << 32) | set_b).second;
  }
  if (!probe) return;
  t_inside = true;
  for (const char* w : kWatched) {
    g::Call c(ctx, base);
    const uint32_t result = c.PushString("................");  // scratch for the two result words
    const uint32_t wanted = c.PushString(w);
    c.Invoke(kLookup, {result, set_a, set_b, wanted, type_ptr});
    const uint32_t found = g::ReadU32(base, result + 4);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Valid(found) && g_logged.insert(std::string("dump:") + w).second) DumpRecord(base, w, found);
  }
  t_inside = false;
}
