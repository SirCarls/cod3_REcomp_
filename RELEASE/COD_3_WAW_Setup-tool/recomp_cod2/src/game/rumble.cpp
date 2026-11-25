// cod2 / cod2mp - no controller vibration while the keyboard and mouse are in use (game/rumble.h). [input-port]

#include "game/rumble.h"

#include <rex/cvar.h>
#include <rex/hook.h>

#include "inputkit/rex/rumble_hold.h"

REXCVAR_DEFINE_BOOL(cod2_rumble_with_keys, false, "cod2/Controls",
                    "Let the first controller vibrate while the keyboard and mouse are in use (off: it stays still "
                    "until one of its buttons, triggers or sticks is used; players on their own controllers always "
                    "feel the game's vibration)");

// The game's XamInputSetState wrapper (user r3, XINPUT_VIBRATION* r4). The second macro level makes the address
// name expand before the SDK's macros paste it (as game/test_start.cpp does).
#if defined(COD2_MP)
#define COD2_SET_STATE sub_822897C0
#else
#define COD2_SET_STATE sub_8228A6B0
#endif
#define COD2_RUMBLE_IMP_(name) __imp__##name
#define COD2_RUMBLE_IMP(name) COD2_RUMBLE_IMP_(name)
#define COD2_RUMBLE_EXTERN(name) REX_EXTERN(COD2_RUMBLE_IMP(name))
#define COD2_RUMBLE_HOOK_RAW(name) REX_HOOK_RAW(name)

COD2_RUMBLE_EXTERN(COD2_SET_STATE);

namespace {
inputkit::rexui::RumbleHold g_hold("cod2");
}  // namespace

namespace cod2::rumble {

void NoteKeyboardMousePress() { g_hold.NoteKeyboardMousePress(); }

void NoteMouseMotion(float dx, float dy) { g_hold.NoteMouseMotion(dx, dy); }

void OnPadRead(PPCContext& ctx, uint8_t* base, uint32_t result, uint32_t state) {
  g_hold.OnPadRead(ctx, base, result, state, REXCVAR_GET(cod2_rumble_with_keys), COD2_RUMBLE_IMP(COD2_SET_STATE));
}

}  // namespace cod2::rumble

// The game asks for new motor speeds: users 1-3 pass untouched; user 0 gets zero while the keyboard and mouse are in
// use (inputkit/rex/rumble_hold.h).
COD2_RUMBLE_HOOK_RAW(COD2_SET_STATE) {
  g_hold.OnSetState(ctx, base, REXCVAR_GET(cod2_rumble_with_keys), COD2_RUMBLE_IMP(COD2_SET_STATE));
}
