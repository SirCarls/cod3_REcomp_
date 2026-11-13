// cod3mp - title update 3's Headquarters radio fix, redone as our own hooks on the disc's multiplayer program
// (codmp_xenonf.xex). [rexglue-native] 2026-11-09.
//
// Owner 2026-11-09 17:28: "replicate the patches without actually needing them. Also without breaking all our
// progress." The update is never shipped, read or needed at run time; dev/agents/rexglue-native/TU3-STUDY.md has
// what it changed. Every address here is the disc's codmp_xenonf.xex (generated names sub_821EDE60, sub_821F1B30).
// The campaign's fixes are game/title_update_fixes.cpp; this is the only multiplayer change the update made that
// applies to our multiplayer (its other changes are the console's online services and the compiler's).
//
// In Headquarters (_mp_hq) the HQ state, whose pointer is at 0x82A99494, keeps the radio's place at +300 (x, y, z)
// and a byte at +420. The host sets that byte when it picks the first radio (0x821F97D0) and sends it to clients
// (0x821FAD80); a client takes it from the host's game state message (CallbackGameStateHQ, 0x8220CC10, which then
// calls 0x821F1B30).
//   - AddRadioModel (0x821EDE60) makes the radio's model and, only when the byte is set, moves it to +300.
//   - SwitchToRadioOnly (0x821F1B30) clears the objectives and, only when the byte is set, adds the radio's
//     objective at +300; then it calls AddRadioModel.
//   - The captured radio's objectives (0x82205B38) begin the same way: the objectives cleared and, only when the
//     byte is set, the radio's objective added at +300; then each team's objectives.
// The update removed all three checks: the radio is placed and its objective added every time these run. Where
// the byte is still 0 when they run, the disc's code makes the radio's model without moving it to its place and
// adds no objective for it.
//
// The hooks below do what the update does by having the byte read as set during the call: when it is 0, it is set
// to 1 for the call and put back to 0 after it. None of the three reads the byte anywhere else or writes it (read
// in the generated code of each), so the call runs exactly as the update's does, and nothing outside the call sees
// the 1. cod3_tu_hq_radio (on) turned off gives the disc's behavior back.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_tu_hq_radio, true, "cod3/Game",
                    "Title update 3's fix (Headquarters): the radio is moved to its place and its objective added "
                    "even when the HQ state's \"radio placed\" byte is still clear")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;

constexpr uint32_t kHqStatePointer = 0x82A99494;  // the HQ state (null outside Headquarters)
constexpr uint32_t kRadioPlacedByte = 420;        // checked by both functions before using the place at +300

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xF0000000u; }

// For the length of one call: when the fix is on and the byte is clear, it reads as set; afterwards it is clear
// again (unless something in the call changed it, which nothing in these three functions does).
class RadioPlacedDuringCall {
 public:
  RadioPlacedDuringCall(uint8_t* base, const char* function) : base_(base) {
    state_ = g::ReadU32(base, kHqStatePointer);
    if (!Valid(state_)) return;
    const bool clear = g::ReadU8(base, state_ + kRadioPlacedByte) == 0;
    const bool fix = REXCVAR_GET(cod3_tu_hq_radio);
    // These run only when Headquarters' radio is placed, moved or taken (a few times a match), so every call is
    // logged: a match's log shows whether the byte was clear and what the fix did.
    REXLOG_INFO("cod3mp: tu fixes: HQ radio: {} ran with the byte {}{}", function, clear ? "clear" : "set",
                !clear ? "" : fix ? "; read as set (cod3_tu_hq_radio)" : "; left clear (cod3_tu_hq_radio off)");
    if (!clear || !fix) return;
    g::WriteU8(base, state_ + kRadioPlacedByte, 1);
    set_ = true;
  }
  ~RadioPlacedDuringCall() {
    if (set_ && g::ReadU8(base_, state_ + kRadioPlacedByte) == 1) g::WriteU8(base_, state_ + kRadioPlacedByte, 0);
  }
  RadioPlacedDuringCall(const RadioPlacedDuringCall&) = delete;
  RadioPlacedDuringCall& operator=(const RadioPlacedDuringCall&) = delete;

 private:
  uint8_t* base_;
  uint32_t state_ = 0;
  bool set_ = false;
};

}  // namespace

// AddRadioModel (0x821EDE60): the radio's model, moved to +300 when the byte is set.
REX_EXTERN(__imp__sub_821EDE60);
REX_HOOK_RAW(sub_821EDE60) {
  RadioPlacedDuringCall placed(base, "AddRadioModel");
  __imp__sub_821EDE60(ctx, base);
}

// SwitchToRadioOnly (0x821F1B30): the radio's objective at +300 when the byte is set, then AddRadioModel.
REX_EXTERN(__imp__sub_821F1B30);
REX_HOOK_RAW(sub_821F1B30) {
  RadioPlacedDuringCall placed(base, "SwitchToRadioOnly");
  __imp__sub_821F1B30(ctx, base);
}

// The captured radio's objectives (0x82205B38): the radio's objective at +300 when the byte is set, then each
// team's.
REX_EXTERN(__imp__sub_82205B38);
REX_HOOK_RAW(sub_82205B38) {
  RadioPlacedDuringCall placed(base, "the captured radio's objectives (0x82205B38)");
  __imp__sub_82205B38(ctx, base);
}

// CallbackGameStateHQ (0x8220CC10) only logged, not changed: it is how a console that doesn't host learns the HQ
// state, so its log line shows a client reaching the three functions above. Read in its generated code: r3 is the
// stage the host's message brings, the stage reached so far is the word at +456 (the call does nothing unless r3 is
// greater), and each stage between runs in turn: 1 stores the radio's place (r4-r7 to +300 and +544); 2 stores r8
// at +244 and "r10 is not 0" in the byte at +420, then calls SwitchToRadioOnly; 3 stores "r9 is not 0" at +452, then
// calls the captured radio's objectives. Logged only when it moves to a later stage (a few times a match).
REX_EXTERN(__imp__sub_8220CC10);
REX_HOOK_RAW(sub_8220CC10) {
  const uint32_t state = g::ReadU32(base, kHqStatePointer);
  if (Valid(state)) {
    const int32_t reached = static_cast<int32_t>(g::ReadU32(base, state + 456));
    const int32_t brought = static_cast<int32_t>(ctx.r3.u32);
    if (brought > reached)
      REXLOG_INFO("cod3mp: tu fixes: HQ game state: stage {} to {}; the message's radio placed {}", reached,
                  brought, ctx.r10.u32 != 0 ? 1 : 0);
  }
  __imp__sub_8220CC10(ctx, base);
}
