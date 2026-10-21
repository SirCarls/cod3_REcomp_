// cod3 - no controller vibration while the keyboard and mouse are in use
// (game/rumble.h).

#include "game/rumble.h"

#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/button_prompts.h"
#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_rumble_with_keys, false, "cod3/Mouse",
                    "Let the first controller vibrate while the keyboard and mouse are in use "
                    "(off: it stays still until one of its buttons, triggers or sticks is used; "
                    "players 2-4 on their own controllers always feel the game's vibration; "
                    "game/rumble.h)");

// The game's XamInputSetState wrapper (user r3, XINPUT_VIBRATION* r4).
REX_EXTERN(__imp__sub_82344FA8);

namespace {

using cod3::guest::ReadU16;
using cod3::guest::WriteU16;

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

// The guest user the keyboard and mouse share: 0 (XamInputSetState takes
// user 0xFF as user 0 too, the SDK's xam_input.cpp).
bool FirstUser(uint32_t user) { return user == 0 || (user & 0xFF) == 0xFF; }

std::mutex g_mutex;
uint32_t g_scratch = 0;            // guest: 4 zero bytes, then the speeds OnPadRead sends
uint16_t g_left = 0, g_right = 0;  // what the game last asked for, user 0
bool g_held = false;               // user 0's vibration held back now
bool g_logged = false;             // the first request since the last change is in the log

// Under g_mutex.
uint32_t Scratch(uint8_t* base) {
  if (!g_scratch) {
    auto* state = rex::system::kernel_state();
    if (!state || !state->memory()) return 0;
    const uint32_t p = state->memory()->SystemHeapAlloc(16);
    if (!Valid(p)) return 0;
    std::memset(base + p, 0, 16);
    g_scratch = p;
  }
  return g_scratch;
}

bool HoldBack() {
  return !REXCVAR_GET(cod3_rumble_with_keys) && cod3::prompts::KeyboardMouseInUse();
}

// Under g_mutex: a change of what user 0 gets, in the log once.
void SetHeld(bool held) {
  if (held == g_held) return;
  g_held = held;
  g_logged = false;
  if (held)
    REXLOG_INFO("cod3: controller 1 vibration held back (the keyboard and mouse are in use)");
  else
    REXLOG_INFO("cod3: controller 1 vibration back (the controller is in use; left {}, right {})",
                g_left, g_right);
}

// Under g_mutex: user 0 gets the speeds at guest `vib` through the game's
// wrapper's own code (not the hook below), on a frame of its own.
void Send(PPCContext& parent, uint8_t* base, uint32_t vib) {
  cod3::guest::Call call(parent, base);
  PPCContext& c = call.ctx();
  c.r1.u32 -= 0x70;
  c.r3.u64 = 0;
  c.r4.u64 = vib;
  __imp__sub_82344FA8(c, base);
}

}  // namespace

namespace cod3::rumble {

void OnPadRead(PPCContext& ctx, uint8_t* base) {
  const bool hold = HoldBack();
  std::lock_guard lock(g_mutex);
  if (hold == g_held) return;
  const uint32_t scratch = Scratch(base);
  if (!scratch) return;
  SetHeld(hold);
  if (!g_left && !g_right) return;  // still: nothing to stop or to send
  if (hold) {
    Send(ctx, base, scratch);  // zero
  } else {
    WriteU16(base, scratch + 4, g_left);
    WriteU16(base, scratch + 6, g_right);
    Send(ctx, base, scratch + 4);
  }
}

}  // namespace cod3::rumble

// The game asks for new motor speeds. Users 1-3 pass untouched; user 0's are
// kept, and while the keyboard and mouse are in use it gets zero instead (a
// copy of ours: the game's own stays as it wrote it).
REX_HOOK_RAW(sub_82344FA8) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t vib = ctx.r4.u32;
  if (!FirstUser(user) || !Valid(vib)) {
    __imp__sub_82344FA8(ctx, base);
    return;
  }
  const bool hold = HoldBack();
  std::lock_guard lock(g_mutex);
  g_left = ReadU16(base, vib);
  g_right = ReadU16(base, vib + 2);
  const uint32_t scratch = hold ? Scratch(base) : 0;
  const bool held = hold && scratch;
  SetHeld(held);
  if ((g_left || g_right) && !g_logged) {
    g_logged = true;
    REXLOG_INFO("cod3: controller 1 vibration asked for (left {}, right {}): {}", g_left, g_right,
                held ? "held back" : "sent");
  }
  if (held) ctx.r4.u64 = scratch;
  __imp__sub_82344FA8(ctx, base);
}
