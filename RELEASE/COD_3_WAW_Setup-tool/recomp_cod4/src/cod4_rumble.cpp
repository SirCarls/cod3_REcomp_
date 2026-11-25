// cod4 / cod4mp - no controller vibration while the keyboard and mouse are in use (cod4_rumble.h). [input-port]

#include "cod4_rumble.h"

#include <rex/cvar.h>
#include <rex/hook.h>

#ifdef COD4_MP
#include "generated/default_mp/cod4mp_pch.h"
#else
#include "generated/default/cod4_pch.h"
#endif
#include "inputkit/rex/rumble_hold.h"

REXCVAR_DEFINE_BOOL(cod4_rumble_with_keys, false, "cod4",
                    "Let the first controller vibrate while the keyboard and mouse are in use (off: it stays still "
                    "until one of its buttons, triggers or sticks is used; players on their own controllers always "
                    "feel the game's vibration)");

// The second macro level makes the address names expand before the SDK's macros paste them.
#ifdef COD4_MP
#define COD4_GET_STATE sub_82103318
#define COD4_SET_STATE sub_82103328
#else
#define COD4_GET_STATE sub_820E38A0
#define COD4_SET_STATE sub_820E38B0
#endif
#define COD4_RUMBLE_IMP_(name) __imp__##name
#define COD4_RUMBLE_IMP(name) COD4_RUMBLE_IMP_(name)
#define COD4_RUMBLE_EXTERN(name) REX_EXTERN(COD4_RUMBLE_IMP(name))
#define COD4_RUMBLE_HOOK_RAW(name) REX_HOOK_RAW(name)

COD4_RUMBLE_EXTERN(COD4_GET_STATE);  // XamInputGetState wrapper (user r3, XINPUT_STATE* r4) -> error code r3
COD4_RUMBLE_EXTERN(COD4_SET_STATE);  // XamInputSetState wrapper (user r3, XINPUT_VIBRATION* r4)

namespace {
inputkit::rexui::RumbleHold g_hold("cod4");
inputkit::rexui::ActivityListener g_watcher(g_hold);
}  // namespace

namespace cod4::rumble {
rex::ui::WindowInputListener* Watcher() { return &g_watcher; }
}  // namespace cod4::rumble

// The game reads a controller: after user 0's read, note which device is in use (and stop or restart controller 1's
// vibration when that changed).
COD4_RUMBLE_HOOK_RAW(COD4_GET_STATE) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  COD4_RUMBLE_IMP(COD4_GET_STATE)(ctx, base);
  if (user == 0)
    g_hold.OnPadRead(ctx, base, ctx.r3.u32, state, REXCVAR_GET(cod4_rumble_with_keys), COD4_RUMBLE_IMP(COD4_SET_STATE));
}

// The game asks for new motor speeds: users 1-3 pass untouched; user 0 gets zero while the keyboard and mouse are in
// use.
COD4_RUMBLE_HOOK_RAW(COD4_SET_STATE) {
  g_hold.OnSetState(ctx, base, REXCVAR_GET(cod4_rumble_with_keys), COD4_RUMBLE_IMP(COD4_SET_STATE));
}
