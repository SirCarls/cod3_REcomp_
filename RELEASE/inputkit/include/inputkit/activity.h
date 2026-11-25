// inputkit - which the player used last: the keyboard and mouse, or the first controller. Shared by every game.
//
// [input-port] 2026-11-24, taken out of Call of Duty 3's game/button_prompts.cpp (WatchPadValues, KeyboardMouseInUse),
// where it picks the button prompts and holds back the first controller's vibration (game/rumble.h). The owner asked
// for Call of Duty 3's controller module in the other games; their vibration hold-back needs the same answer.
//
// Why it is not simply "the last event wins": the platform's keyboard and mouse driver feeds the same pad state the
// game reads (keys press pad buttons, the mouse can push the right stick), so a pad change that a key or the mouse
// explains is not the controller. The window thread notes each key or mouse button press and each mouse motion; the
// game thread hands over the pad state each time it reads it. A pad change counts as the controller only if no key
// or mouse press came since the last read and none in the last 250 ms.
//
// What counts as a controller use (a change since the last read, so a stick or trigger held still doesn't count
// again): a button going down, a trigger passing 30 of 255, a stick passing 16000 of 32767 from center (well off
// center: an old stick's drift never gets there). The right stick is ignored while the mouse moved in the last
// 250 ms (a game that turns the mouse into the right stick moves it).
//
// What counts as a keyboard and mouse use: a key or mouse button going down, a wheel step, or 24 counts of mouse
// motion (added up since the controller was last used, so a slow, steady push counts as much as a flick, and a
// desk bump of a few counts doesn't count).
//
// Frame rate: nothing here counts frames. A read compares the pad with the last read, and the 250 ms windows are
// real time from the caller's clock, so the answer is the same at 20 or 1000 reads a second (tests/
// test_activity.cpp runs the same input at both).
//
// Thread-safe: the notes are atomics, a pad read takes one mutex. No allocation.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace inputkit::activity {

enum class Device : uint8_t { kKeyboardMouse, kController };

// XInput's pad state (host order): buttons, triggers 0..255, sticks LX, LY, RX, RY -32768..32767.
struct PadState {
  uint16_t buttons = 0;
  uint8_t triggers[2] = {0, 0};
  int16_t axes[4] = {0, 0, 0, 0};
};

constexpr int kTriggerThreshold = 30;     // of 255
constexpr int kStickThreshold = 16000;    // of 32767
constexpr int64_t kExplainMs = 250;       // a key or mouse press this recent explains a pad change
constexpr float kMouseUseCounts = 24.0f;  // mouse motion that counts as a use

class Tracker {
 public:
  // Window thread. now_ms: a steady clock, the same one PadRead gets.
  void KeyboardMousePress(int64_t now_ms);  // a key or mouse button going down, a wheel step
  void MouseMotion(float dx, float dy, int64_t now_ms);

  // Game thread, each time the game reads the first controller. Returns true when this read made it the controller.
  bool PadRead(const PadState& pad, int64_t now_ms);

  Device device() const { return device_.load(std::memory_order_relaxed); }
  // The keyboard or mouse was used, and not the controller since. False before any key or mouse use, so a player
  // who never touches the keyboard always counts as on the controller.
  bool KeyboardMouseInUse() const {
    return presses_.load(std::memory_order_relaxed) != 0 && device() == Device::kKeyboardMouse;
  }
  // Starts over: no use of either yet.
  void Reset();

 private:
  void NotePress(int64_t now_ms);

  std::atomic<Device> device_{Device::kController};
  std::atomic<uint32_t> presses_{0};
  std::atomic<int64_t> press_ms_{INT64_MIN / 2};
  std::atomic<int64_t> motion_ms_{INT64_MIN / 2};
  std::atomic<float> motion_sum_{0.0f};  // counts since the controller was last used

  std::mutex mutex_;  // the last read, below
  PadState last_{};
  uint32_t last_presses_ = 0;
};

}  // namespace inputkit::activity
