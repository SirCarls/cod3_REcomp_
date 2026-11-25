// inputkit - tests of include/inputkit/activity.h: which the player used last.

#include "inputkit/activity.h"

#include <cstdio>
#include <functional>

using inputkit::activity::Device;
using inputkit::activity::PadState;
using inputkit::activity::Tracker;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

static PadState Buttons(uint16_t b) {
  PadState p;
  p.buttons = b;
  return p;
}

// Runs `script` (what happens at a time) with pad reads every `read_ms` from 0 to `end_ms`; returns the tracker's
// answer at the end. The same script at any read rate must give the same answer (the frame rate rule).
static bool InUseAfter(int read_ms, int64_t end_ms,
                       const std::function<void(Tracker&, int64_t, PadState&)>& script) {
  Tracker t;
  PadState pad;
  for (int64_t now = 0; now <= end_ms; ++now) {
    script(t, now, pad);
    if (now % read_ms == 0) t.PadRead(pad, now);
  }
  return t.KeyboardMouseInUse();
}

int main() {
  // Nobody touched anything: the controller (vibration is never held back for a controller-only player).
  {
    Tracker t;
    CHECK(!t.KeyboardMouseInUse());
    CHECK(t.device() == Device::kController);
    CHECK(!t.PadRead(PadState{}, 0));
    CHECK(!t.KeyboardMouseInUse());
  }
  // A key: the keyboard and mouse. A controller button much later: the controller.
  {
    Tracker t;
    t.KeyboardMousePress(100);
    CHECK(t.KeyboardMouseInUse());
    t.PadRead(PadState{}, 116);
    CHECK(t.KeyboardMouseInUse());
    CHECK(t.PadRead(Buttons(0x1000), 1000));  // A, long after the key
    CHECK(!t.KeyboardMouseInUse());
  }
  // A pad button that a key explains (the keyboard driver pressed it): still the keyboard.
  {
    Tracker t;
    t.PadRead(PadState{}, 0);
    t.KeyboardMousePress(1000);
    CHECK(!t.PadRead(Buttons(0x1000), 1005));  // a press since the last read
    CHECK(t.KeyboardMouseInUse());
    t.PadRead(PadState{}, 1100);
    CHECK(!t.PadRead(Buttons(0x1000), 1200));  // within 250 ms of the key
    CHECK(t.KeyboardMouseInUse());
    t.PadRead(PadState{}, 1300);
    CHECK(t.PadRead(Buttons(0x1000), 1400));  // 400 ms after the key, no press since the last read
    CHECK(!t.KeyboardMouseInUse());
  }
  // A held button doesn't count again; a held stick or trigger doesn't either.
  {
    Tracker t;
    t.PadRead(Buttons(0x1000), 0);  // held from the start: a use (from nothing)
    t.KeyboardMousePress(1000);
    PadState held = Buttons(0x1000);
    held.triggers[1] = 255;
    held.axes[1] = 32767;
    t.PadRead(Buttons(0x1000), 1300);  // the game reads every frame; this read takes in the key
    t.PadRead(held, 1500);  // the trigger and the stick newly past their thresholds: a use
    CHECK(!t.KeyboardMouseInUse());
    t.KeyboardMousePress(2000);
    CHECK(!t.PadRead(held, 2500));  // nothing changed: the keyboard stays
    CHECK(t.KeyboardMouseInUse());
  }
  // Thresholds: a small trigger squeeze or stick drift is not a use; a full one is. -32768 is "far" (no overflow).
  {
    Tracker t;
    t.KeyboardMousePress(0);
    PadState p;
    p.triggers[0] = 30;
    p.axes[0] = 16000;
    CHECK(!t.PadRead(p, 1000));
    CHECK(t.KeyboardMouseInUse());
    p.axes[0] = -32768;
    CHECK(t.PadRead(p, 1100));
    CHECK(!t.KeyboardMouseInUse());
  }
  // The right stick while the mouse moves (a mouse-to-stick game): not the controller. After the mouse stops: it is.
  {
    Tracker t;
    t.KeyboardMousePress(0);
    t.MouseMotion(5, 0, 900);
    PadState p;
    p.axes[2] = 30000;
    CHECK(!t.PadRead(p, 1000));
    CHECK(t.KeyboardMouseInUse());
    t.PadRead(PadState{}, 1300);
    CHECK(t.PadRead(p, 1400));  // the mouse last moved 500 ms ago
    CHECK(!t.KeyboardMouseInUse());
  }
  // Mouse motion: a few counts (a desk bump) is not a use; 24 counts in total are, however they are split.
  {
    Tracker t;
    t.PadRead(Buttons(0x1000), 0);
    CHECK(!t.KeyboardMouseInUse());
    t.MouseMotion(3, -2, 10);
    CHECK(!t.KeyboardMouseInUse());
    for (int i = 0; i < 19; ++i) t.MouseMotion(1, 0, 20 + i);  // 5 + 19 = 24
    CHECK(t.KeyboardMouseInUse());
  }
  {
    Tracker a, b;
    a.MouseMotion(24, 0, 0);  // one batch
    for (int i = 0; i < 24; ++i) b.MouseMotion(0, i % 2 ? 1.0f : -1.0f, i);  // 24 events of 1, either way
    CHECK(a.KeyboardMouseInUse());
    CHECK(b.KeyboardMouseInUse());
  }
  // A controller use clears the mouse's sum: the next 24 counts start from nothing.
  {
    Tracker t;
    t.MouseMotion(20, 0, 0);
    t.PadRead(PadState{}, 300);
    t.PadRead(Buttons(0x0010), 400);
    t.MouseMotion(10, 0, 500);
    CHECK(!t.KeyboardMouseInUse());
    t.MouseMotion(14, 0, 510);
    CHECK(t.KeyboardMouseInUse());
  }
  // NaN motion is ignored.
  {
    Tracker t;
    t.MouseMotion(0.0f / 0.0f, 0, 0);
    CHECK(!t.KeyboardMouseInUse());
  }
  // Frame rate: the same hands give the same answer at 20, 60, 144 and 1000 reads a second.
  for (int read_ms : {50, 16, 7, 1}) {
    // Keys, then the controller's A 1 s later and held: the controller.
    CHECK(!InUseAfter(read_ms, 3000, [](Tracker& t, int64_t now, PadState& pad) {
      if (now == 500) t.KeyboardMousePress(now);
      if (now == 1500) pad.buttons = 0x1000;
    }));
    // The controller, then a key: the keyboard.
    CHECK(InUseAfter(read_ms, 3000, [](Tracker& t, int64_t now, PadState& pad) {
      if (now == 500) pad.buttons = 0x1000;
      if (now == 1500) t.KeyboardMousePress(now);
    }));
    // Typing with the keyboard driver pressing the pad's buttons too (each key 80 ms): the keyboard throughout.
    CHECK(InUseAfter(read_ms, 3000, [](Tracker& t, int64_t now, PadState& pad) {
      if (now % 200 == 0) {
        t.KeyboardMousePress(now);
        pad.buttons = 0x1000;
      }
      if (now % 200 == 80) pad.buttons = 0;
    }));
    // A slow mouse (1 count every 10 ms) after the controller: the keyboard and mouse after 240 ms.
    CHECK(InUseAfter(read_ms, 1500, [](Tracker& t, int64_t now, PadState& pad) {
      if (now == 100) pad.buttons = 0x1000;
      if (now >= 1000 && now % 10 == 0) t.MouseMotion(1, 0, now);
    }));
  }
  // Reset: nothing used.
  {
    Tracker t;
    t.KeyboardMousePress(0);
    t.Reset();
    CHECK(!t.KeyboardMouseInUse());
    CHECK(t.device() == Device::kController);
  }

  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("activity: all tests passed\n");
  return 0;
}
