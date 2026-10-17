// cod3 - the sprint button on the player's side: hold or toggle.
//
// Sprint is the game's own button: kbutton 15, which the game turns into
// usercmd bit 2 (CL_CmdButtons) and its movement code into the sprinting
// flag. The commands +sprint / -sprint press and release it (empty in the
// campaign's retail code; the port fills them in, as the multiplayer program
// has them). This class decides what each press does; the glue
// (hooks_sp.cpp) carries the answer out with the game's IN_KeyDown / IN_KeyUp.
//
// Hold: down while held, as in the multiplayer program (each key is its own
// holder, so a pad and a keyboard don't release each other).
// Toggle: one press starts sprinting (only while moving forward); it ends
// when the player stops moving forward, aims, fires, presses sprint again,
// crouches or goes prone, runs out of breath, dies, mounts a gun or uses a
// vehicle, or when the game stops making commands (a menu, a pause, a level
// change). The latch is one IN_KeyDown and one IN_KeyUp under its own key id
// (AUX3): nothing is ever pressed twice or repeated.
//
// Plain logic: no game address (program.h has those), no allocation.

#pragma once

#include <cstdint>

namespace cod3::movement {

enum class SprintMode { kHold, kToggle };

// One thing for the glue to do with the sprint kbutton.
struct KeyAction {
  enum Kind : uint8_t { kNone, kPress, kRelease, kHoldBreathPress, kHoldBreathRelease };
  Kind kind = kNone;
  int key = 0;
  int time = 0;
};

// What the player's machine knows about the player now (from the last
// movement step on this machine, or the last snapshot).
struct PlayerView {
  bool valid = false;
  uint32_t pm_flags = 0;
  int pm_type = 0;
  uint32_t flags2 = 0;
  bool in_binoculars = false;
};

// The finished usercmd, as the game built it.
struct CommandView {
  int forwardmove = 0;
  uint32_t buttons = 0;
  int server_time = 0;
};

class SprintInput {
 public:
  // +sprint / -sprint (key: the key or pad button, -1 when typed; time: ms).
  KeyAction OnSprintDown(int key, int time, SprintMode mode, bool enabled);
  KeyAction OnSprintUp(int key, int time, SprintMode mode, bool enabled);
  // +sprintbreath / -sprintbreath (World at War's Shift): hold breath while
  // aiming, sprint otherwise.
  KeyAction OnSprintBreathDown(int key, int time, SprintMode mode, bool enabled, bool aiming);
  KeyAction OnSprintBreathUp(int key, int time, SprintMode mode, bool enabled);
  // The pad's L3 (+binoculars in layouts A, B and D): sprint while the stick
  // pushes forward and the player isn't using the binoculars; binoculars as
  // before otherwise. `handled` is false when the game's own binoculars should run.
  KeyAction OnBinocularsDown(int key, int time, SprintMode mode, bool enabled, const PlayerView& view,
                             bool* handled);
  KeyAction OnBinocularsUp(int key, int time, SprintMode mode, bool enabled, bool* handled);

  // Once per usercmd, after the game finished it (CL_FinishMove). Ends the
  // toggle when one of its end conditions holds; `clear_button` then asks
  // the glue to clear bit 2 in this same command, so the end is immediate.
  // `now_ms` is the host's clock (a long gap between commands = a menu or a
  // pause).
  KeyAction OnCommand(const CommandView& cmd, const PlayerView& view, SprintMode mode, bool enabled,
                      int64_t now_ms, bool* clear_button);

  // A level starts, the player leaves the game, or sprint is switched off:
  // let go of the latch.
  KeyAction Reset(int time);

  bool latched() const { return latched_; }
  int last_forwardmove() const { return last_forwardmove_; }

 private:
  KeyAction ToggleDown(int time);

  bool latched_ = false;        // the toggle holds the button (key AUX3)
  bool breath_held_ = false;    // +sprintbreath became hold breath
  int breath_key_ = 0;
  bool l3_sprint_ = false;      // the pad's L3 press became sprint
  int last_forwardmove_ = 0;
  int last_time_ = 0;
  int64_t last_command_ms_ = 0;
  int refused_since_ = -1;      // serverTime the server stopped sprinting under the latch
};

// Forward on the stick needed for the pad's L3 to mean sprint (of 127).
inline constexpr int kPadSprintForward = 64;
// No command for this long: the game is in a menu, paused or loading. Long
// enough that a slow frame (an autosave, streaming, a slow PC) doesn't end a
// toggled sprint; a menu or a pause also lets go of the movement keys, which
// ends it on the next command anyway.
inline constexpr int64_t kCommandGapMs = 2000;

}  // namespace cod3::movement
