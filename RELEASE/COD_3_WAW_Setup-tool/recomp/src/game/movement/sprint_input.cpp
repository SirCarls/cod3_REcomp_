// cod3 - the sprint button: hold or toggle (see sprint_input.h).

#include "game/movement/sprint_input.h"

#include "game/movement/program.h"

namespace cod3::movement {

namespace {
// The server hasn't sprinted for this long while the toggle holds the
// button: it can't now (out of breath, a mounted gun ...), so let go.
constexpr int kRefusedMs = 250;
}  // namespace

KeyAction SprintInput::ToggleDown(int time) {
  if (latched_) {
    latched_ = false;  // pressed again: stop
    return {KeyAction::kRelease, kKeyAux3, time};
  }
  // Only while moving forward, so a toggle never waits armed.
  if (last_forwardmove_ <= 0) return {};
  latched_ = true;
  refused_since_ = -1;
  return {KeyAction::kPress, kKeyAux3, time};
}

KeyAction SprintInput::OnSprintDown(int key, int time, SprintMode mode, bool enabled) {
  if (!enabled) return {};
  if (mode == SprintMode::kToggle) return ToggleDown(time);
  return {KeyAction::kPress, key, time};
}

KeyAction SprintInput::OnSprintUp(int key, int time, SprintMode mode, bool enabled) {
  (void)enabled;
  // The real key is let go in both modes: in toggle mode it holds nothing
  // (the latch is AUX3), unless it went down while the mode was hold. A
  // typed "-sprint" (key -1) would let go of every holder, the latch too.
  if (mode == SprintMode::kToggle && key == -1) return {};
  return {KeyAction::kRelease, key, time};
}

KeyAction SprintInput::OnSprintBreathDown(int key, int time, SprintMode mode, bool enabled, bool aiming) {
  if (aiming) {
    breath_held_ = true;
    breath_key_ = key;
    return {KeyAction::kHoldBreathPress, key, time};
  }
  return OnSprintDown(key, time, mode, enabled);
}

KeyAction SprintInput::OnSprintBreathUp(int key, int time, SprintMode mode, bool enabled) {
  if (breath_held_ && (key == breath_key_ || key == -1)) {
    breath_held_ = false;
    return {KeyAction::kHoldBreathRelease, key, time};
  }
  return OnSprintUp(key, time, mode, enabled);
}

KeyAction SprintInput::OnBinocularsDown(int key, int time, SprintMode mode, bool enabled,
                                        const PlayerView& view, bool* handled) {
  *handled = false;
  if (!enabled || key != kKeyPadL3) return {};
  if (view.in_binoculars) return {};  // looking through them: L3 is the game's (steady)
  if (last_forwardmove_ < kPadSprintForward) return {};
  *handled = true;
  l3_sprint_ = true;
  return OnSprintDown(key, time, mode, enabled);
}

KeyAction SprintInput::OnBinocularsUp(int key, int time, SprintMode mode, bool enabled, bool* handled) {
  *handled = false;
  if (!l3_sprint_ || key != kKeyPadL3) return {};
  *handled = true;
  l3_sprint_ = false;
  return OnSprintUp(key, time, mode, enabled);
}

KeyAction SprintInput::OnCommand(const CommandView& cmd, const PlayerView& view, SprintMode mode, bool enabled,
                                 int64_t now_ms, bool* clear_button) {
  *clear_button = false;
  const bool gap = last_command_ms_ != 0 && now_ms - last_command_ms_ > kCommandGapMs;
  last_command_ms_ = now_ms;
  last_forwardmove_ = cmd.forwardmove;
  last_time_ = cmd.server_time;
  if (!latched_) return {};

  bool end = !enabled || mode != SprintMode::kToggle || gap;
  end = end || cmd.forwardmove <= 0;                  // stopped or backing up
  end = end || (cmd.buttons & button::kAds) != 0;     // aiming
  end = end || (cmd.buttons & button::kAttack) != 0;  // firing
  if (view.valid) {
    end = end || (view.pm_flags & (pmf::kProne | pmf::kDucked)) != 0;
    end = end || view.pm_type != kPmNormal;            // dead, noclip, ufo
    end = end || (view.flags2 & kFlags2NoMove) != 0;   // a mounted gun or a vehicle
    // The server stopped sprinting (out of breath, or it can't now) for a
    // while although the button is held.
    if (view.pm_flags & pmf::kSprinting) {
      refused_since_ = -1;
    } else if (refused_since_ < 0) {
      refused_since_ = cmd.server_time;
    } else if (cmd.server_time - refused_since_ >= kRefusedMs) {
      end = true;
    }
  }
  if (!end) return {};
  latched_ = false;
  *clear_button = true;
  return {KeyAction::kRelease, kKeyAux3, cmd.server_time};
}

KeyAction SprintInput::Reset(int time) {
  l3_sprint_ = false;
  last_command_ms_ = 0;
  if (!latched_) return {};
  latched_ = false;
  return {KeyAction::kRelease, kKeyAux3, time};
}

}  // namespace cod3::movement
