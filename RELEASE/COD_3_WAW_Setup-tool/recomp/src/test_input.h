// cod3 - scripted controller input for automated testing.
//
// --cod3_test_input="12:START;16:A;20:DOWN;21:A;30:LY=1:4;35:RT:0.5"
// presses buttons at the given times (seconds after start). Each entry is
//   <time>:<what>[:<hold seconds, default 0.15>]
// where <what> is one or more of A B X Y START BACK LB RB LT RT UP DOWN LEFT
// RIGHT LS RS, or a stick axis LX= LY= RX= RY= in [-1, 1], joined with '+'.
// The device shows up as a synthetic pad on player 1, alongside any real pad
// or the keyboard. Unused (and absent) unless the cvar is set.
// [mp-menus] An entry "pad=playstation" (or xbox, nintendo) says which kind
// of controller the scripted pad is, for the button prompts' per-controller
// style (SDK patch 0067's ControllerFamily); without it the SDK can't name it.
// [claude-9b] 2026-10-27 Split screen in tests (owner 2026-10-27: "your mp tests aren't actually spawning in
// ... also there needs to be splitscreen"): an entry "pads=N" (2 to 4) makes N scripted pads, each a
// connected controller of its own (players 1 to N, in order; a real pad plugged in may take a place first),
// and an entry may start with the pad it is for, "p2:20:A" (no prefix: pad 1). With one pad (the default)
// it stays the synthetic pad on player 1, as before.

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>

namespace cod3 {

class ScriptedInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptedInputDriver(const std::string& script);

  rex::X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId, rex::input::X_INPUT_VIBRATION*) override;
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

  size_t event_count() const { return events_.size(); }

  static constexpr int kMaxPads = 4;  // [claude-9b]

 private:
  struct Event {
    int pad = 0;  // [claude-9b] which scripted pad (0: the first)
    double start = 0, end = 0;
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    bool has_axis[4] = {};
    int16_t axis[4] = {};  // LX LY RX RY
  };
  void Update(int pad);
  int PadOf(rex::input::DeviceId id) const;  // [claude-9b] -1: not one of ours

  std::vector<Event> events_;
  std::chrono::steady_clock::time_point start_;
  std::mutex mutex_;
  // [claude-9b] one of each per scripted pad
  int pads_ = 1;
  uint32_t packet_[kMaxPads] = {};
  uint16_t last_buttons_[kMaxPads] = {};
  rex::input::X_INPUT_GAMEPAD pad_[kMaxPads] = {};
  std::queue<rex::input::X_INPUT_KEYSTROKE> keys_[kMaxPads];
  rex::input::ControllerFamily family_ = rex::input::ControllerFamily::kUnknown;  // [mp-menus]
};

}  // namespace cod3
