// f2 - the F2 window with a controller (see f2_pad.h). [f2-menus]

#include "f2/f2_pad.h"

#include <algorithm>
#include <iterator>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace f2 {

namespace {

// One direction of SDL's analog range (-32768 to 32767) as Dear ImGui's 0-1, past a dead zone (the stick's is the
// 8000 of 32767 Dear ImGui's own SDL 3 backend uses).
float Analog(int value, int dead_zone, int full) {
  const float v = float(value - dead_zone) / float(full - dead_zone);
  return std::clamp(v, 0.0f, 1.0f);
}

// The Dear ImGui keys fed while the window is open, in the order of PadShortcut::sent_.
constexpr ImGuiKey kKeys[] = {
    ImGuiKey_GamepadStart,     ImGuiKey_GamepadBack,      ImGuiKey_GamepadFaceDown,  ImGuiKey_GamepadFaceRight,
    ImGuiKey_GamepadFaceLeft,  ImGuiKey_GamepadFaceUp,    ImGuiKey_GamepadDpadUp,    ImGuiKey_GamepadDpadDown,
    ImGuiKey_GamepadDpadLeft,  ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadL1,        ImGuiKey_GamepadR1,
    ImGuiKey_GamepadL2,        ImGuiKey_GamepadR2,        ImGuiKey_GamepadLStickUp,  ImGuiKey_GamepadLStickDown,
    ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadL3,     ImGuiKey_GamepadR3,
};
static_assert(std::size(kKeys) == 20, "PadShortcut::sent_ holds one value per key");

}  // namespace

void PadShortcut::RefreshPadList(std::chrono::steady_clock::time_point now) {
  // SDL hands the list over as a new array, so it is asked for once a second, not every frame.
  if (now < next_refresh_) return;
  next_refresh_ = now + std::chrono::seconds(1);
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  std::array<Pad, kMaxPads> next{};
  for (int i = 0; ids && i < count && i < kMaxPads; ++i) {
    next[i].id = ids[i];
    // A controller still connected keeps its hold in progress.
    for (const Pad& old : pads_)
      if (old.id == ids[i]) next[i].hold = old.hold;
  }
  SDL_free(ids);
  pads_ = next;
}

bool PadShortcut::Poll(bool enabled) {
  const auto now = std::chrono::steady_clock::now();
  RefreshPadList(now);
  bool fired = false;
  progress_ = 0.0f;
  for (Pad& pad : pads_) {
    if (!pad.id) continue;
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(pad.id);  // opened by the SDK's SDL driver; null if not (yet)
    const bool held = enabled && gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) &&
                      SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START);
    fired |= pad.hold.Update(held, now);
    progress_ = std::max(progress_, pad.hold.Progress(now));
  }
  return fired;
}

float PadShortcut::Progress() const { return progress_; }

void PadShortcut::FeedNavigation(ImGuiIO& io, bool window_open) {
  std::array<float, 20> now{};
  if (window_open) {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    constexpr int kStickDeadZone = 8000;
    constexpr int kTriggerDeadZone = 2000;
    constexpr int kFull = 32767;
    for (const Pad& pad : pads_) {
      SDL_Gamepad* g = pad.id ? SDL_GetGamepadFromID(pad.id) : nullptr;
      if (!g) continue;
      auto button = [&](int slot, SDL_GamepadButton b) {
        if (SDL_GetGamepadButton(g, b)) now[slot] = 1.0f;
      };
      auto analog = [&](int slot, int value, int dead_zone) {
        now[slot] = std::max(now[slot], Analog(value, dead_zone, kFull));
      };
      button(0, SDL_GAMEPAD_BUTTON_START);
      button(1, SDL_GAMEPAD_BUTTON_BACK);
      button(2, SDL_GAMEPAD_BUTTON_SOUTH);  // A: choose
      button(3, SDL_GAMEPAD_BUTTON_EAST);   // B: back
      button(4, SDL_GAMEPAD_BUTTON_WEST);
      button(5, SDL_GAMEPAD_BUTTON_NORTH);
      button(6, SDL_GAMEPAD_BUTTON_DPAD_UP);
      button(7, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
      button(8, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
      button(9, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
      button(10, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
      button(11, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
      analog(12, SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), kTriggerDeadZone);
      analog(13, SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), kTriggerDeadZone);
      // Up and left are the negative halves of SDL's axes.
      const int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
      const int ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
      analog(14, -ly, kStickDeadZone);
      analog(15, ly, kStickDeadZone);
      analog(16, -lx, kStickDeadZone);
      analog(17, lx, kStickDeadZone);
      button(18, SDL_GAMEPAD_BUTTON_LEFT_STICK);
      button(19, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    }
  }
  for (size_t i = 0; i < std::size(kKeys); ++i) {
    if (now[i] == sent_[i]) continue;
    const bool analog = (i >= 12 && i <= 17);
    if (analog)
      io.AddKeyAnalogEvent(kKeys[i], now[i] > 0.1f, now[i]);
    else
      io.AddKeyEvent(kKeys[i], now[i] > 0.0f);
    sent_[i] = now[i];
  }
  // LB / RB change the page (a press, not a hold).
  const bool left = now[10] > 0.0f, right = now[11] > 0.0f;
  if (left && !shoulder_left_) page_step_ = -1;
  if (right && !shoulder_right_) page_step_ = +1;
  shoulder_left_ = left;
  shoulder_right_ = right;
}

int PadShortcut::TakePageStep() {
  const int step = page_step_;
  page_step_ = 0;
  return step;
}

}  // namespace f2
