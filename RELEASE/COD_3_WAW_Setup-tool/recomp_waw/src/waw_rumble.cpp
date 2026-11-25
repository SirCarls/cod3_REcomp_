// waw - no controller vibration while the keyboard and mouse are in use (waw_rumble.h). [input-port]

#include "waw_rumble.h"

#include <rex/cvar.h>
#include <rex/hook.h>

#include "generated/default/waw_pch.h"
#include "inputkit/rex/rumble_hold.h"

REXCVAR_DEFINE_BOOL(waw_rumble_with_keys, false, "waw",
                    "Let the first controller vibrate while the keyboard and mouse are in use (off: it stays still "
                    "until one of its buttons, triggers or sticks is used; players on their own controllers always "
                    "feel the game's vibration)");

REX_EXTERN(__imp__sub_8237C120);  // XamInputGetState wrapper (user r3, XINPUT_STATE* r4) -> error code r3
REX_EXTERN(__imp__sub_8237C130);  // XamInputSetState wrapper (user r3, XINPUT_VIBRATION* r4)

namespace {
inputkit::rexui::RumbleHold g_hold("waw");
inputkit::rexui::ActivityListener g_watcher(g_hold);
}  // namespace

namespace waw::rumble {
rex::ui::WindowInputListener* Watcher() { return &g_watcher; }
}  // namespace waw::rumble

// The game reads a controller: after user 0's read, note which device is in use (and stop or restart controller 1's
// vibration when that changed).
REX_HOOK_RAW(sub_8237C120) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_8237C120(ctx, base);
  if (user == 0)
    g_hold.OnPadRead(ctx, base, ctx.r3.u32, state, REXCVAR_GET(waw_rumble_with_keys), __imp__sub_8237C130);
}

// The game asks for new motor speeds: users 1-3 pass untouched; user 0 gets zero while the keyboard and mouse are in
// use.
REX_HOOK_RAW(sub_8237C130) { g_hold.OnSetState(ctx, base, REXCVAR_GET(waw_rumble_with_keys), __imp__sub_8237C130); }
