// cod3 - raw mouse aim with World at War style sensitivity.
//
// The console game only knows analog sticks, and the SDK's keyboard/mouse
// driver turns the mouse into a right-stick deflection, which the game then
// runs through its own turn-rate and acceleration curves. That never feels
// like a PC shooter. Instead we take the raw relative mouse motion (SDL
// relative mode: raw input on Windows, XInput2 raw events on X11, pointer
// lock on Wayland) and add it straight to the client view angles, using the
// same maths as World at War / Quake III:
//     yaw   -= dx * sensitivity * m_yaw
//     pitch += dy * sensitivity * m_pitch
// with sensitivity scaled by (current FOV / base FOV) while zoomed, like
// WaW's zoom sensitivity.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace cod3 {

class MouseLook {
 public:
  static MouseLook& Get();

  // UI thread: raw relative motion in device counts.
  void AddDelta(float dx, float dy);
  // UI thread: drop motion not yet applied (when the mouse is taken or let go).
  void ClearPending();
  // UI thread: whether the window should currently hold the mouse.
  bool WantsCapture() const;
  // True if raw mouse aim is enabled in the settings.
  static bool Enabled();

  // Guest thread, from the CL_AdjustAngles hook.
  void ApplyToGuest(uint8_t* base);
  // Guest thread, from the CG_GetViewFov hook.
  void SetViewFov(float fov, float base_fov);

 private:
  using Clock = std::chrono::steady_clock;
  std::mutex mutex_;
  float dx_ = 0.0f;
  float dy_ = 0.0f;
  std::atomic<int64_t> last_guest_tick_ns_{0};
  std::atomic<float> zoom_ratio_{1.0f};
};

}  // namespace cod3
