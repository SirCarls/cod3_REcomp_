// inputkit - no vibration on the first controller while the keyboard and mouse are in use, in the ports built on the
// ReXGlue SDK (Call of Duty 2, World at War, Call of Duty 4; header only, so inputkit itself needs no SDK).
// [input-port] 2026-11-24
//
// The owner asked for Call of Duty 3's controller module in the other games. Call of Duty 3 does this in
// game/rumble.cpp; this is the same logic, once, for every ReXGlue port.
//
// Every one of these games vibrates a controller through one function, its wrapper of XamInputSetState, called with
// the user in r3 and an XINPUT_VIBRATION* in r4 (two big-endian u16 motor speeds). Call of Duty 2's wrapper passes
// them straight on; the IW games' (World at War, Call of Duty 4) check the controller's capabilities first, then send
// the same speeds. Both read the speeds through r4, so pointing r4 at a copy changes what is sent and nothing else.
// The SDK gives the keyboard and mouse to guest user 0 together with the first controller, so a player on the
// keyboard and mouse would feel the controller lying next to them buzz.
//
// While the keyboard and mouse are in use (inputkit/activity.h), user 0's request is sent as zero speeds from 16
// bytes of guest memory the game never sees. The game's own speeds are kept, and sent once the controller is used
// again. The game's XINPUT_VIBRATION and its vibration option are never written. Users 1-3 (players on their own
// controllers) always get what the game asks.
//
// A game wires it with three calls:
//   - its window listener: NoteKeyboardMousePress() for a key, mouse button or wheel step; NoteMouseMotion(dx, dy).
//     Or ActivityListener below, added to the window above the game's own listeners: it only watches;
//   - a hook on its XamInputGetState wrapper, after the read, for user 0: OnPadRead(ctx, base, r3, state, send);
//   - a hook on its XamInputSetState wrapper: OnSetState(ctx, base, imp) in place of calling imp itself.
// `send` / `imp` is the wrapper's __imp__ function (the game's own code, not the hook).
//
// Frame rate: what is held back follows the last device used, not frames (inputkit/activity.h). A change takes
// effect at the next controller read, which these games make every frame.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window_listener.h>

#include "inputkit/activity.h"

namespace inputkit::rexui {

class RumbleHold {
 public:
  using GuestFn = void (*)(PPCContext&, uint8_t*);

  // `game` names it in the log ("cod2").
  explicit RumbleHold(const char* game) : game_(game) {}

  // Window thread.
  void NoteKeyboardMousePress() { activity_.KeyboardMousePress(NowMs()); }
  void NoteMouseMotion(float dx, float dy) { activity_.MouseMotion(dx, dy, NowMs()); }

  // Game thread, after each read of user 0's pad. result: the read's answer (0 = connected); state: the guest
  // XINPUT_STATE. allow: the game's "vibrate with the keyboard and mouse too" setting.
  void OnPadRead(PPCContext& ctx, uint8_t* base, uint32_t result, uint32_t state, bool allow, GuestFn send) {
    // XINPUT_STATE (big-endian): u32 packet number, then XINPUT_GAMEPAD: u16 buttons, u8 left and right trigger,
    // s16 LX, LY, RX, RY. A controller that isn't connected changes nothing.
    if (result == 0 && Valid(state)) {
      activity::PadState pad;
      pad.buttons = LoadBe16(base, state + 4);
      pad.triggers[0] = base[state + 6];
      pad.triggers[1] = base[state + 7];
      for (int i = 0; i < 4; ++i) pad.axes[i] = static_cast<int16_t>(LoadBe16(base, state + 8 + 2 * i));
      activity_.PadRead(pad, NowMs());
    }
    const bool hold = !allow && activity_.KeyboardMouseInUse();
    std::lock_guard lock(mutex_);
    if (hold == held_) return;
    const uint32_t scratch = Scratch(base);
    if (!scratch) return;
    SetHeld(hold);
    if (!left_ && !right_) return;  // still: nothing to stop or to send
    if (hold) {
      Send(ctx, base, scratch, send);  // zero
    } else {
      StoreBe16(base, scratch + 4, left_);
      StoreBe16(base, scratch + 6, right_);
      Send(ctx, base, scratch + 4, send);
    }
  }

  // The game's XamInputSetState wrapper was called (r3 user, r4 XINPUT_VIBRATION*): runs it, with zero speeds for
  // user 0 while held back. allow: as above.
  void OnSetState(PPCContext& ctx, uint8_t* base, bool allow, GuestFn imp) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t vib = ctx.r4.u32;
    if (!FirstUser(user) || !Valid(vib)) {
      imp(ctx, base);
      return;
    }
    const bool hold = !allow && activity_.KeyboardMouseInUse();
    std::lock_guard lock(mutex_);
    left_ = LoadBe16(base, vib);
    right_ = LoadBe16(base, vib + 2);
    const uint32_t scratch = hold ? Scratch(base) : 0;
    const bool held = hold && scratch;
    SetHeld(held);
    if ((left_ || right_) && !logged_) {
      logged_ = true;
      REXLOG_INFO("{}: controller 1 vibration asked for (left {}, right {}): {}", game_, left_, right_,
                  held ? "held back" : "sent");
    }
    if (held) ctx.r4.u64 = scratch;  // the wrapper reads the speeds through r4: it sends our zeros
    imp(ctx, base);
  }

  bool KeyboardMouseInUse() const { return activity_.KeyboardMouseInUse(); }

 private:
  static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  static bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
  // The guest user the keyboard and mouse share: 0 (XamInputSetState takes user 0xFF as user 0 too).
  static bool FirstUser(uint32_t user) { return user == 0 || (user & 0xFF) == 0xFF; }
  static uint16_t LoadBe16(const uint8_t* base, uint32_t address) {
    uint16_t v;
    std::memcpy(&v, base + address, sizeof(v));
    return __builtin_bswap16(v);
  }
  static void StoreBe16(uint8_t* base, uint32_t address, uint16_t value) {
    value = __builtin_bswap16(value);
    std::memcpy(base + address, &value, sizeof(value));
  }

  // Under mutex_: 4 zero bytes, then the speeds OnPadRead sends.
  uint32_t Scratch(uint8_t* base) {
    if (!scratch_) {
      auto* state = rex::system::kernel_state();
      if (!state || !state->memory()) return 0;
      const uint32_t p = state->memory()->SystemHeapAlloc(16);
      if (!Valid(p)) return 0;
      std::memset(base + p, 0, 16);
      scratch_ = p;
    }
    return scratch_;
  }

  // Under mutex_: a change of what user 0 gets, in the log once.
  void SetHeld(bool held) {
    if (held == held_) return;
    held_ = held;
    logged_ = false;
    if (held)
      REXLOG_INFO("{}: controller 1 vibration held back (the keyboard and mouse are in use)", game_);
    else
      REXLOG_INFO("{}: controller 1 vibration back (the controller is in use; left {}, right {})", game_, left_,
                  right_);
  }

  // Under mutex_: user 0 gets the speeds at guest `vib` through the wrapper's own code, on a register file and stack
  // frame of its own, so the controller read hook that calls this keeps its state.
  static void Send(PPCContext& parent, uint8_t* base, uint32_t vib, GuestFn send) {
    rex::CallFrame frame(parent);
    PPCContext& c = frame.ctx;
    c.r1.u32 = (c.r1.u32 - 0x100) & ~0xFu;  // below the caller's red zone, 16-byte aligned
    c.r3.u64 = 0;
    c.r4.u64 = vib;
    send(c, base);
  }

  const char* game_;
  activity::Tracker activity_;
  std::mutex mutex_;
  uint32_t scratch_ = 0;
  uint16_t left_ = 0, right_ = 0;  // what the game last asked for, user 0
  bool held_ = false;              // user 0's vibration held back now
  bool logged_ = false;            // the first request since the last change is in the log
};

// A window listener that only watches: every key, mouse button, wheel step and mouse motion goes to a RumbleHold,
// and nothing is marked handled, so the game's own listeners below it get every event as before. Add it above them
// (a higher z order: the window asks the highest first and stops at the first that handles an event).
class ActivityListener : public rex::ui::WindowInputListener {
 public:
  explicit ActivityListener(RumbleHold& hold) : hold_(hold) {}

  void OnKeyDown(rex::ui::KeyEvent&) override { hold_.NoteKeyboardMousePress(); }
  void OnMouseDown(rex::ui::MouseEvent&) override { hold_.NoteKeyboardMousePress(); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (e.scroll_y() != 0) hold_.NoteKeyboardMousePress();
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    // Relative mode (the pointer held for mouse look) gives the motion itself; otherwise the pointer's movement since
    // the last event. Only its size matters here (activity.h adds up 24 counts).
    float dx = float(e.dx()), dy = float(e.dy());
    if (dx == 0.0f && dy == 0.0f && have_last_) {
      dx = float(e.x() - last_x_);
      dy = float(e.y() - last_y_);
    }
    last_x_ = e.x();
    last_y_ = e.y();
    have_last_ = true;
    hold_.NoteMouseMotion(dx, dy);
  }

 private:
  RumbleHold& hold_;
  int32_t last_x_ = 0, last_y_ = 0;  // window thread only
  bool have_last_ = false;
};

}  // namespace inputkit::rexui
