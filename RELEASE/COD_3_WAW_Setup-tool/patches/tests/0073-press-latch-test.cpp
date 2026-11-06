// Checks rex::input::PressLatch (SDK patch 0073, include/rex/input/press_latch.h): a press
// or trigger pull that starts and ends between two reads of a pad is shown on the next read.
//
//   clang++ -std=c++17 -Wall -Wextra -I <sdk>/include 0073-press-latch-test.cpp -o press_latch_test
//   ./press_latch_test
#include <rex/input/press_latch.h>

#include <cstdio>

using rex::input::PressLatch;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(x)                                               \
  do {                                                         \
    ++g_checks;                                                \
    if (!(x)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

constexpr uint64_t kMs = 1'000'000;
constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;

struct Read {
  uint16_t buttons;
  uint8_t left;
  uint8_t right;
  bool added;
};

static Read ReadAt(PressLatch& l, uint16_t buttons, uint8_t left, uint8_t right, uint64_t now) {
  Read r{buttons, left, right, false};
  r.added = l.Apply(r.buttons, r.left, r.right, now);
  return r;
}

static void TestTap() {
  // A pressed at 1 ms, released at 3 ms; the game reads at 0, 16.7, 33.3 ms.
  PressLatch l;
  CHECK(ReadAt(l, 0, 0, 0, 0).buttons == 0);
  l.Buttons(0, kA, 1 * kMs);
  l.Buttons(kA, 0, 3 * kMs);
  Read r = ReadAt(l, 0, 0, 0, 16 * kMs);
  CHECK(r.buttons == kA && r.added);
  r = ReadAt(l, 0, 0, 0, 33 * kMs);
  CHECK(r.buttons == 0 && !r.added);
  CHECK(!l.Pending());
}

static void TestTwoReadsInOneFrame() {
  // The game reads twice, 1 ms apart, right after the tap: both see it.
  PressLatch l;
  l.Buttons(0, kB, 10 * kMs);
  l.Buttons(kB, 0, 11 * kMs);
  CHECK(ReadAt(l, 0, 0, 0, 12 * kMs).buttons == kB);
  CHECK(ReadAt(l, 0, 0, 0, 13 * kMs).buttons == kB);
  // The next frame does not.
  CHECK(ReadAt(l, 0, 0, 0, 29 * kMs).buttons == 0);
}

static void TestHeld() {
  // Held across the read: the pad shows it, nothing is added, and the
  // release is not delayed.
  PressLatch l;
  l.Buttons(0, kA, 1 * kMs);
  Read r = ReadAt(l, kA, 0, 0, 16 * kMs);
  CHECK(r.buttons == kA && !r.added);
  l.Buttons(kA, 0, 20 * kMs);
  CHECK(ReadAt(l, 0, 0, 0, 33 * kMs).buttons == 0);
}

static void TestPressReleasePress() {
  // Down, up, down again before the read: the pad shows it down.
  PressLatch l;
  l.Buttons(0, kA, 1 * kMs);
  l.Buttons(kA, 0, 2 * kMs);
  l.Buttons(0, kA, 3 * kMs);
  Read r = ReadAt(l, kA, 0, 0, 16 * kMs);
  CHECK(r.buttons == kA && !r.added);
  CHECK(!l.Pending());
}

static void TestOtherButtonsKept() {
  PressLatch l;
  l.Buttons(0, kA, 1 * kMs);
  l.Buttons(kA, 0, 2 * kMs);
  Read r = ReadAt(l, kB, 0, 0, 16 * kMs);  // B held now, A was tapped
  CHECK(r.buttons == (kA | kB));
}

static void TestTriggerPull() {
  // Right trigger 0 -> 200 -> 0 between reads: the read sees 200.
  PressLatch l;
  l.Trigger(1, 0, 90, 1 * kMs);
  l.Trigger(1, 90, 200, 2 * kMs);
  l.Trigger(1, 200, 40, 3 * kMs);
  l.Trigger(1, 40, 0, 4 * kMs);
  Read r = ReadAt(l, 0, 0, 0, 16 * kMs);
  CHECK(r.right == 200 && r.left == 0 && r.added);
  CHECK(ReadAt(l, 0, 0, 0, 33 * kMs).right == 0);
  // A light pull below XInput's threshold is still kept (the game may use its own).
  l.Trigger(0, 0, 20, 40 * kMs);
  l.Trigger(0, 20, 0, 41 * kMs);
  CHECK(ReadAt(l, 0, 0, 0, 50 * kMs).left == 20);
}

static void TestTriggerHeld() {
  // Held past the threshold at the read, then pushed further and let go:
  // that is not a new pull, so the release is not delayed.
  PressLatch l;
  l.Trigger(0, 0, 150, 1 * kMs);
  CHECK(ReadAt(l, 0, 150, 0, 16 * kMs).left == 150);
  l.Trigger(0, 150, 255, 20 * kMs);
  l.Trigger(0, 255, 0, 25 * kMs);
  CHECK(ReadAt(l, 0, 0, 0, 33 * kMs).left == 0);
  // The pad now shows at least the peak: nothing added.
  l.Trigger(0, 0, 100, 40 * kMs);
  Read r = ReadAt(l, 0, 120, 0, 50 * kMs);
  CHECK(r.left == 120 && !r.added);
}

static void TestClear() {
  PressLatch l;
  l.Buttons(0, kA, 1 * kMs);
  l.Buttons(kA, 0, 2 * kMs);
  l.Trigger(0, 0, 255, 1 * kMs);
  l.Clear();
  Read r = ReadAt(l, 0, 0, 0, 16 * kMs);
  CHECK(r.buttons == 0 && r.left == 0 && !r.added);
}

static void TestHighRateReads() {
  // A game reading 1000 times a second: a 1 ms tap is shown for the hold
  // (4 ms), never longer.
  PressLatch l;
  l.Buttons(0, kA, 100 * kMs);
  l.Buttons(kA, 0, 100 * kMs + kMs / 2);
  int shown = 0;
  for (uint64_t t = 101; t < 120; ++t) shown += ReadAt(l, 0, 0, 0, t * kMs).buttons == kA;
  CHECK(shown >= 1 && shown <= 4);
}

int main() {
  TestTap();
  TestTwoReadsInOneFrame();
  TestHeld();
  TestPressReleasePress();
  TestOtherButtonsKept();
  TestTriggerPull();
  TestTriggerHeld();
  TestClear();
  TestHighRateReads();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
