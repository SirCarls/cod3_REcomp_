// cod2 - the PC copy's movement numbers in the campaign (see pc_movement.h). [f2-menus]

#include "game/pc_movement.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/dvar_peek.h"

#if !defined(COD2_MP)

REXCVAR_DEFINE_BOOL(cod2_pc_movement, true, "cod2/Game",
                    "Move as in the PC copy: sideways at 0.8 and backwards at 0.7 of the forward speed, the view "
                    "up and down to 85 degrees, binoculars stay up after a press; off: the Xbox 360 program's own "
                    "(full speed in every direction, 70 degrees, binoculars while held)");

namespace cod2::pc_movement {

namespace {

struct Number {
  const char* name;
  bool is_float;   // else a true/false dvar
  float x360;      // the Xbox 360 program's own
  float pc;        // the PC copy's
  uint32_t dvar;   // the game's dvar, once found
  bool complained;
};

Number g_numbers[] = {
    {"player_strafeSpeedScale", true, 1.0f, 0.8f, 0, false},
    {"player_backSpeedScale", true, 1.0f, 0.7f, 0, false},
    {"player_view_pitch_up", true, 70.0f, 85.0f, 0, false},
    {"player_view_pitch_down", true, 70.0f, 85.0f, 0, false},
    {"player_toggleBinoculars", false, 0.0f, 1.0f, 0, false},
};

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

float LoadBeF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadBe32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

void StoreBeF32(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  StoreBe32(base, address, bits);
}

// Writes each of the five that differs from `pc ? the PC copy's : the 360's`. Returns how many were written.
int Apply(PPCContext& ctx, uint8_t* base, bool pc) {
  int written = 0;
  for (Number& n : g_numbers) {
    if (n.dvar == 0) n.dvar = cod2::dvar_peek::FindDvar(ctx, base, n.name);
    if (n.dvar == 0) continue;  // not registered yet: the game makes these when a level starts
    // The dvar's type is the byte at +6 (1 a float, 0 true/false), its value at +8.
    const uint8_t type = base[n.dvar + 6];
    if (type != (n.is_float ? 1 : 0)) {
      if (!n.complained) {
        n.complained = true;
        REXLOG_WARN("cod2: pc movement: {} has type {}, not the one expected: left alone", n.name, unsigned(type));
      }
      continue;
    }
    const float want = pc ? n.pc : n.x360;
    if (n.is_float) {
      const float have = LoadBeF32(base, n.dvar + 8);
      if (std::fabs(have - want) < 0.0005f) continue;
      StoreBeF32(base, n.dvar + 8, want);
    } else {
      const bool have = base[n.dvar + 8] != 0;
      if (have == (want != 0.0f)) continue;
      base[n.dvar + 8] = want != 0.0f ? 1 : 0;
    }
    ++written;
  }
  return written;
}

}  // namespace

void OnGameFrame(PPCContext& ctx, uint8_t* base) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  static bool was_on = false;
  static int logged = 0;
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  const bool on = REXCVAR_GET(cod2_pc_movement);
  if (!on && !was_on) return;  // off and nothing of ours to take back
  const int written = Apply(ctx, base, on);
  if (written > 0 && logged < 6) {
    ++logged;
    REXLOG_INFO("cod2: pc movement: {} of the 5 movement numbers set to {}", written,
                on ? "the PC copy's" : "the Xbox 360 program's own");
  }
  was_on = on;
}

}  // namespace cod2::pc_movement

#endif  // !COD2_MP
