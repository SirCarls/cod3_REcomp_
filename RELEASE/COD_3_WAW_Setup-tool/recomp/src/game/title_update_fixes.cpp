// cod3 - what Call of Duty 3's title update 3 fixed in the campaign, redone as our own hooks on the disc
// program. [rexglue-native] 2026-11-09.
//
// Owner 2026-11-09 17:28: "replicate the patches without actually needing them. Also without breaking all our
// progress." The update is never shipped, read or needed at run time: these hooks only do what a study of its
// code showed it changing (dev/agents/rexglue-native/TU3-STUDY.md), each behind its own setting. Every address
// here is the disc's default.xex; the update's own addresses appear only in that study.
//
// The update changed 53 functions of the campaign program. Most of the changes are the compiler's (registers,
// data that moved, functions placed elsewhere). What it really changed, and what this file does about it:
//
//   1. A melee struggle's start (InteractStateMeleeStart::Enter, 0x82505F78). When the struggle hands the
//      player a weapon he wasn't holding, 0x824709E8 gives that weapon a random amount of ammunition, and the
//      clip can come out empty. The update then raises the clip to 1, so the struggle's closing shot can always
//      be fired.                                                       -> cod3_tu_melee_struggle_round
//   2. Selecting a weapon (0x824694B0: the weapon keys, cycling, picking a weapon up, a struggle's start and
//      end, and the rest of its 19 callers). The update first calls CG_RegisterWeapon (0x82448C78) for the
//      weapon. That returns at once for a weapon that is already registered; otherwise it registers the
//      weapon's models and animations before the switch starts, not on its first drawn frame.
//                                                                      -> cod3_tu_register_selected_weapon
//   3. The sound device's constructor (0x82492D18, called once by Com_Init on an object from a tagged heap,
//      which isn't cleared). The update also sets two arrays the disc leaves as the heap gave them: 50 floats
//      at +24832 (8 bytes apart) to -1, and 16 floats at +25248 (20 bytes apart) to 0.
//                                                                      -> cod3_tu_sound_device_init
//   4. A block copy (0x824C2778, the first 408 bytes of the 7 KB copy at 0x824CB438). The disc copies every
//      field but the float at +400; the update copies that one too.    -> cod3_tu_copy_missing_field
//
// Not redone:
//   - The loading screen: the update draws it from a new routine with an animation step of 0.15 s and two
//     presents per update (for the console's rules on long loads), and stops drawing "Loading ..." while a flag
//     is set. That would add work for the emulated GPU and changes nothing in play.
//   - A string format at 0x8242AC08 ("%s_%03d" into a stack buffer) became a bounded one (200 bytes). The
//     names it formats are short asset names, far below that.
//   - Everything else is the compiler's (see the study): Com_Frame's frame timing, in particular, is unchanged.
//
// All four are on by default. Each one only acts inside the function the update changed and does exactly
// what the update does there; turning a setting off gives the disc's behavior back.

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_tu_melee_struggle_round, true, "cod3/Game",
                    "Title update 3's fix: a melee struggle that hands you a weapon with an empty clip loads one "
                    "round into it, so the struggle's closing shot can be fired")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_tu_register_selected_weapon, true, "cod3/Game",
                    "Title update 3's fix: a weapon's models and animations are registered when it is selected, "
                    "before the switch starts")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_tu_sound_device_init, true, "cod3/Game",
                    "Title update 3's fix: the sound device sets two arrays the disc version leaves uninitialized "
                    "(read once, at start-up)");
REXCVAR_DEFINE_BOOL(cod3_tu_copy_missing_field, true, "cod3/Game",
                    "Title update 3's fix: a block copy also copies the one field the disc version leaves out")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_tu_fixes, false, "cod3",
                    "Log what the title update 3 fixes do (game/title_update_fixes.cpp): the sound device's arrays "
                    "before they are set, and the first 64 weapon selections and block copies")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;

constexpr int kMaxLogged = 64;
std::atomic<int> g_selects_logged{0};
std::atomic<int> g_copies_logged{0};

bool LogOne(std::atomic<int>& count) {
  return REXCVAR_GET(cod3_debug_tu_fixes) && count.fetch_add(1, std::memory_order_relaxed) < kMaxLogged;
}

constexpr uint32_t kRegisterWeapon = 0x82448C78;  // CG_RegisterWeapon(weapon)

// The sound device's two arrays the update initializes (offsets from the object).
constexpr uint32_t kSoundFirstArray = 24832;  // 50 x {float, int}: the float set to -1
constexpr uint32_t kSoundFirstCount = 50;
constexpr uint32_t kSoundFirstStride = 8;
constexpr uint32_t kSoundSecondArray = 25248;  // 16 entries of 20 bytes: the float at +16 of each set to 0
constexpr uint32_t kSoundSecondCount = 16;
constexpr uint32_t kSoundSecondStride = 20;

constexpr uint32_t kMissingFieldOffset = 400;  // the float 0x824C2778 doesn't copy

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xF0000000u; }

// Set while a melee struggle's start runs on this thread: the ammunition it hands out is the one to check.
thread_local bool t_melee_struggle_start = false;

}  // namespace

// 1. A melee struggle's start. Only the call to 0x824709E8 made from it is changed (that function also gives
// ammunition to weapons that are picked up, at 0x8255E090, which the update left alone).
// [rexglue-native] 2026-11-09 22:40 (owner 22:33: "Just wanna make sure the TU3 works right"): a struggle's start and
// a saved game's load (4. below) are rare (a few struggles in three levels; a load when a profile signs in), and no
// scripted run reaches them yet, so both log every time, not only with cod3_debug_tu_fixes: a playthrough's log shows
// that they ran and what they did.
REX_EXTERN(__imp__InteractStateMeleeStart__vfunc_0);
REX_HOOK_RAW(InteractStateMeleeStart__vfunc_0) {
  REXLOG_INFO("cod3: tu fixes: a melee struggle starts");
  const bool outer = t_melee_struggle_start;
  t_melee_struggle_start = true;
  __imp__InteractStateMeleeStart__vfunc_0(ctx, base);
  t_melee_struggle_start = outer;
}

// 0x824709E8(int* ammo, int* clip, int weapon): a random amount of ammunition for the weapon. The update's
// struggle start checks the clip right after this call: below 1, it becomes 1.
REX_EXTERN(__imp__sub_824709E8);
REX_HOOK_RAW(sub_824709E8) {
  const uint32_t clip = ctx.r4.u32;
  const uint32_t weapon = ctx.r5.u32;
  __imp__sub_824709E8(ctx, base);
  if (!t_melee_struggle_start || !Valid(clip)) return;
  const int32_t rounds = g::ReadS32(base, clip);
  REXLOG_INFO("cod3: tu fixes: melee struggle start: weapon {} got {} rounds in its clip", weapon, rounds);  // 22:40
  if (rounds >= 1 || !REXCVAR_GET(cod3_tu_melee_struggle_round)) return;
  g::WriteU32(base, clip, 1);
  REXLOG_INFO("cod3: tu fixes: melee struggle start: weapon {}'s clip was {}, now 1 (cod3_tu_melee_struggle_round)",
              weapon, rounds);
}

// 2. Selecting a weapon: 0x824694B0(weapon). The update calls CG_RegisterWeapon(weapon) first, every time.
REX_EXTERN(__imp__sub_824694B0);
REX_HOOK_RAW(sub_824694B0) {
  const uint32_t weapon = ctx.r3.u32;
  const bool register_first = REXCVAR_GET(cod3_tu_register_selected_weapon);
  if (register_first) {
    g::Call call(ctx, base);
    call.Invoke(kRegisterWeapon, {weapon});
  }
  if (LogOne(g_selects_logged)) {
    REXLOG_INFO("cod3: tu fixes: weapon {} selected{}", weapon, register_first ? " (registered first)" : "");
  }
  __imp__sub_824694B0(ctx, base);
}

// 3. The sound device's constructor: 0x82492D18(this). The update's extra stores come after everything the
// disc's constructor does, so doing them after it returns is the same.
REX_EXTERN(__imp__sub_82492D18);
REX_HOOK_RAW(sub_82492D18) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_82492D18(ctx, base);
  if (!Valid(device)) return;
  if (REXCVAR_GET(cod3_debug_tu_fixes)) {
    // What the disc's constructor left there: anything but 0 means the heap's old contents.
    int first_nonzero = 0, second_nonzero = 0;
    for (uint32_t i = 0; i < kSoundFirstCount; ++i) {
      first_nonzero += g::ReadU32(base, device + kSoundFirstArray + i * kSoundFirstStride) != 0;
    }
    for (uint32_t i = 0; i < kSoundSecondCount; ++i) {
      second_nonzero += g::ReadU32(base, device + kSoundSecondArray + i * kSoundSecondStride) != 0;
    }
    REXLOG_INFO("cod3: tu fixes: sound device {:08X}: {} of {} and {} of {} values not 0 before{}", device,
                first_nonzero, kSoundFirstCount, second_nonzero, kSoundSecondCount,
                REXCVAR_GET(cod3_tu_sound_device_init) ? "; set to -1 and 0" : "; left as they are");
  }
  if (!REXCVAR_GET(cod3_tu_sound_device_init)) return;
  for (uint32_t i = 0; i < kSoundFirstCount; ++i) {
    g::WriteF32(base, device + kSoundFirstArray + i * kSoundFirstStride, -1.0f);
  }
  for (uint32_t i = 0; i < kSoundSecondCount; ++i) {
    g::WriteF32(base, device + kSoundSecondArray + i * kSoundSecondStride, 0.0f);
  }
}

// 4. The block copy: 0x824C2778(destination, source). The update copies the float at +400 as its last field
// but one; nothing in between reads it, so copying it after the call is the same.
// [rexglue-native] 22:40: its only caller is 0x824CB438, whose only caller is 0x823FD260: the callback of a saved game's
// read ("COD3_SaveSPV1.0", 0x1B10 bytes into 0x82AAF2C8, checked against the build's version string), which copies it
// into the live progress at 0x82A8E610 and then loads the profile's settings. So it runs once per saved game loaded,
// and its first lines are logged without cod3_debug_tu_fixes too (above).
constexpr int kCopiesAlwaysLogged = 4;
REX_EXTERN(__imp__sub_824C2778);
REX_HOOK_RAW(sub_824C2778) {
  const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32;
  __imp__sub_824C2778(ctx, base);
  if (!Valid(destination) || !Valid(source)) return;
  const bool copy = REXCVAR_GET(cod3_tu_copy_missing_field);
  const int logged = g_copies_logged.fetch_add(1, std::memory_order_relaxed);
  if (logged < kCopiesAlwaysLogged || (REXCVAR_GET(cod3_debug_tu_fixes) && logged < kMaxLogged)) {
    REXLOG_INFO("cod3: tu fixes: block copy {:08X} -> {:08X}: +400 is {} there, {} here{}", source, destination,
                g::ReadF32(base, source + kMissingFieldOffset), g::ReadF32(base, destination + kMissingFieldOffset),
                copy ? "; copied" : "; left");
  }
  if (!copy) return;
  g::WriteU32(base, destination + kMissingFieldOffset, g::ReadU32(base, source + kMissingFieldOffset));
}
