// Checks rex::input::PressLatch (SDK patches 0073 and 0323, include/rex/input/press_latch.h): a press
// or trigger pull that starts and ends between two reads of a pad is shown on the next read, and
// (0323) on every read within 25 ms of that first one, so a second reader in the same frame sees it too.
// Was 0073-press-latch-test.cpp (the 4 ms window from the press); the timings below are 0323's.
//
//   clang++ -std=c++17 -Wall -Wextra -I <sdk>/include 0323-press-latch-test.cpp -o press_latch_test
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
  // 0323: 33 ms is within 25 ms of the first read (16): still shown, one continuous press over two frames
  r = ReadAt(l, 0, 0, 0, 33 * kMs);
  CHECK(r.buttons == kA && r.added);
  r = ReadAt(l, 0, 0, 0, 50 * kMs);
  CHECK(r.buttons == 0 && !r.added);
  CHECK(!l.Pending());
}

static void TestSecondReaderSameFrame() {
  // 0323: at 20 fps (reads 50 ms apart) a 2 ms tap ends long before the next frame. That frame has two readers 3 ms
  // apart (the port's own and the game's): both see it, the next frame doesn't. (0073 showed it to the first only.)
  PressLatch l;
  CHECK(ReadAt(l, 0, 0, 0, 0).buttons == 0);
  l.Buttons(0, kA, 10 * kMs);
  l.Buttons(kA, 0, 12 * kMs);
  CHECK(ReadAt(l, 0, 0, 0, 50 * kMs).buttons == kA);
  CHECK(ReadAt(l, 0, 0, 0, 53 * kMs).buttons == kA);
  CHECK(ReadAt(l, 0, 0, 0, 100 * kMs).buttons == 0);
  CHECK(!l.Pending());
}

static void TestTriggerSecondReader() {
  // The same for a trigger tap (LT pulled to 200 and let go between frames).
  PressLatch l;
  l.Trigger(0, 0, 200, 10 * kMs);
  Read r = ReadAt(l, 0, 0, 0, 50 * kMs);
  CHECK(r.left == 200 && r.added);
  r = ReadAt(l, 0, 0, 0, 54 * kMs);
  CHECK(r.left == 200 && r.added);
  r = ReadAt(l, 0, 0, 0, 100 * kMs);
  CHECK(r.left == 0 && !r.added);
}

static void TestTwoReadsInOneFrame() {
  // The game reads twice, 1 ms apart, right after the tap: both see it.
  PressLatch l;
  l.Buttons(0, kB, 10 * kMs);
  l.Buttons(kB, 0, 11 * kMs);
  CHECK(ReadAt(l, 0, 0, 0, 12 * kMs).buttons == kB);
  CHECK(ReadAt(l, 0, 0, 0, 13 * kMs).buttons == kB);
  // 0323: 25 ms from the first read (12): 29 ms still does, 45 ms doesn't.
  CHECK(ReadAt(l, 0, 0, 0, 29 * kMs).buttons == kB);
  CHECK(ReadAt(l, 0, 0, 0, 45 * kMs).buttons == 0);
}

static void TestHeld() {
  // Held across the read: the pad shows it, nothing is added. A hold longer than the window (0323: 8 ms from the
  // first read that saw it held) is not lengthened: seen at 16, let go at 60, the read at 66 shows it up.
  PressLatch l;
  l.Buttons(0, kA, 1 * kMs);
  Read r = ReadAt(l, kA, 0, 0, 16 * kMs);
  CHECK(r.buttons == kA && !r.added);
  CHECK(ReadAt(l, kA, 0, 0, 33 * kMs).buttons == kA);
  CHECK(ReadAt(l, kA, 0, 0, 50 * kMs).buttons == kA);
  l.Buttons(kA, 0, 60 * kMs);
  CHECK(ReadAt(l, 0, 0, 0, 66 * kMs).buttons == 0);
  CHECK(!l.Pending());
}

static void TestHeldAtFirstReaderOnly() {
  // 0323: at 20 fps a 50 ms press is held at the frame's first read and let go before its second read 2 ms later
  // (Call of Duty 3 reads the pad twice in a frame; the test pad's runs showed "read LT" then "read nothing"): the
  // second read still shows it; the next frame doesn't.
  PressLatch l;
  l.Buttons(0, kA, 10 * kMs);
  CHECK(ReadAt(l, kA, 0, 0, 50 * kMs).buttons == kA);  // the pad shows it held
  l.Buttons(kA, 0, 51 * kMs);
  Read r = ReadAt(l, 0, 0, 0, 52 * kMs);
  CHECK(r.buttons == kA && r.added);
  CHECK(ReadAt(l, 0, 0, 0, 100 * kMs).buttons == 0);
  // the same for a trigger
  l.Trigger(0, 0, 255, 110 * kMs);
  CHECK(ReadAt(l, 0, 255, 0, 150 * kMs).left == 255);
  l.Trigger(0, 255, 0, 151 * kMs);
  r = ReadAt(l, 0, 0, 0, 152 * kMs);
  CHECK(r.left == 255 && r.added);
  CHECK(ReadAt(l, 0, 0, 0, 200 * kMs).left == 0);
}

static void TestPressReleasePress() {
  // Down, up, down again before the read: the pad shows it down.
  PressLatch l;
  l.Buttons(0, kA, 1 * kMs);
  l.Buttons(kA, 0, 2 * kMs);
  l.Buttons(0, kA, 3 * kMs);
  Read r = ReadAt(l, kA, 0, 0, 16 * kMs);
  CHECK(r.buttons == kA && !r.added);
  // 0323: seen held, it stays pending for its short window (8 ms), then the pad alone shows it
  CHECK(ReadAt(l, kA, 0, 0, 30 * kMs).buttons == kA);
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
  CHECK(ReadAt(l, 0, 0, 0, 33 * kMs).right == 200);  // 0323: within 25 ms of the first read (16)
  CHECK(ReadAt(l, 0, 0, 0, 42 * kMs).right == 0);    // and gone after it
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
  // (0323: 25 ms from the first read that shows it, reads 101..125), never longer.
  PressLatch l;
  l.Buttons(0, kA, 100 * kMs);
  l.Buttons(kA, 0, 100 * kMs + kMs / 2);
  int shown = 0;
  for (uint64_t t = 101; t < 160; ++t) shown += ReadAt(l, 0, 0, 0, t * kMs).buttons == kA;
  CHECK(shown == 25);
}

int main() {
  TestTap();
  TestSecondReaderSameFrame();  // 0323
  TestTriggerSecondReader();    // 0323
  TestHeldAtFirstReaderOnly();  // 0323
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
