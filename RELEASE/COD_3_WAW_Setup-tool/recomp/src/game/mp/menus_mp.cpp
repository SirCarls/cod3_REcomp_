// cod3mp: the multiplayer program's own menus (codmp_xenonf.xex addresses).
//
// Not the campaign's PC menu controls (game/pc_menu.cpp): multiplayer's menu
// classes have other fields and virtual slots, so that file isn't part of
// this program yet. What is here:
//  - whether a menu page is on screen, so raw mouse aim lets the pointer go
//    in the pause, side selection and scoreboard pages (MenuActive): a page
//    was drawn in this game frame or the last one (the game's frame counter,
//    as game/pc_menu.cpp counts it), so it holds at any frame rate;
//  - the main menu: XBOX LIVE does nothing (there is no Live service for the
//    PC version; A there stopped on a null pointer in the session code) and
//    says so; DOWNLOAD CONTENT becomes QUIT TO DESKTOP (its text through the
//    game's Localize; its A asks for a second A on it within 20 seconds, and
//    A on any other item cancels that);
//  - a warning when the game's voice and session system didn't start: it
//    starts with the sound system, and without an audio output device
//    creating or joining a match stops the game (a null read below MPPeer's
//    constructor, in 0x8229B420).

#include "game/mp/menus_mp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "ui/announcement_feed.h"

namespace {

using cod3::guest::ReadU32;
using Clock = std::chrono::steady_clock;

// FEMenuSystem (codmp_xenonf.xex; the campaign's draw reads its page at +12):
// pages at +4, the page shown at +20 and a page over it at +28 (< 0: none),
// as its draw (FEMenuSystem__vfunc_19, 0x824D5ED0) reads them.
constexpr uint32_t kSysPages = 4;
constexpr uint32_t kSysCurrent = 20;
constexpr uint32_t kSysOverlay = 28;

// MPMainMenuXBox's item handler (slot 36, 0x823D2978) switches on +244 plus
// +248: 0 XBOX LIVE, 1 SYSTEM LINK, 2 LOCAL SPLIT SCREEN, 3 OPTIONS,
// 4 DOWNLOAD CONTENT. Its labels come from the key table at 0x825D4738 and
// are localized when the page is built (sub_823C9A90 calls Localize).
constexpr uint32_t kMainMenuRow = 244;
constexpr uint32_t kMainMenuRowOffset = 248;
constexpr uint32_t kXboxLiveItem = 0;
constexpr uint32_t kQuitItem = 4;  // DOWNLOAD CONTENT on the console

// The voice / session singleton MPPeer's constructor reads (sub_8229B420:
// lwz r3,60 of it). It is made by an init the sound system runs (FMOD's
// init, 0x8238D990, after its own device check); without an audio device
// it stays null.
constexpr uint32_t kVoiceSystemPtr = 0x82A9ACB8;

// While the game thread is busy (a loading screen) the frame counter stands
// still: a page drawn longer ago than this no longer counts.
constexpr int64_t kLoadingLimitNs = 20'000'000'000;
constexpr int64_t kQuitConfirmNs = 20'000'000'000;  // the second A for QUIT TO DESKTOP
constexpr int64_t kVoiceCheckNs = 15'000'000'000;  // after the first menu, then the check

std::atomic<int64_t> g_menu_ns{0};
std::atomic<uint32_t> g_menu_frame{0};  // the game frame of that draw
std::atomic<int64_t> g_first_menu_ns{0};
std::atomic<int64_t> g_quit_asked_ns{0};
bool g_voice_checked = false;  // UI thread

std::mutex g_quit_mutex;
std::function<void()> g_quit;  // g_quit_mutex

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

int64_t NowNs() { return Clock::now().time_since_epoch().count(); }

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}

// The port's texts for the game's keys, as guest strings made once.
struct PortText {
  const char* key;
  const char* text;
  std::atomic<uint32_t> guest{0};
};
PortText g_texts[] = {
    {"MPFRONTEND_DOWNLOAD_CONTENT", "QUIT TO DESKTOP"},
    {"MPFRONTEND_DOWNLOAD_CONTENT_DESCRIPTION", "Close Call of Duty 3 and return to the desktop."},
    {"MPFRONTEND_XBOXLIVE_DESCRIPTION",
     "Xbox Live isn't part of the PC version: play over System Link on your network."},
};

uint32_t GuestString(uint8_t* base, const char* text) {
  auto* mem = rex::system::kernel_state()->memory();
  const uint32_t len = static_cast<uint32_t>(std::strlen(text)) + 1;
  const uint32_t addr = mem->SystemHeapAlloc(len);
  if (addr) std::memcpy(base + addr, text, len);
  return addr;
}

}  // namespace

namespace cod3::mpmenu {

bool MenuActive() {
  const int64_t when = g_menu_ns.load(std::memory_order_relaxed);
  if (when == 0 || NowNs() - when >= kLoadingLimitNs) return false;
  uint8_t* base = GuestBase();
  if (!base) return false;
  const uint32_t frame = ReadU32(base, cod3::addr::pc_menu::kFrameCount);
  return frame - g_menu_frame.load(std::memory_order_relaxed) <= 1;
}

void SetQuitHandler(std::function<void()> quit) {
  std::lock_guard lock(g_quit_mutex);
  g_quit = std::move(quit);
}

void Tick() {
  if (g_voice_checked) return;
  const int64_t first = g_first_menu_ns.load(std::memory_order_relaxed);
  if (first == 0 || NowNs() - first < kVoiceCheckNs) return;
  uint8_t* base = GuestBase();
  if (!base) return;
  g_voice_checked = true;
  const uint32_t voice = ReadU32(base, kVoiceSystemPtr);
  if (voice) {
    REXLOG_INFO("cod3mp: the game's voice and session system is up ({:08X})", voice);
    return;
  }
  REXLOG_WARN("cod3mp: the game's voice and session system didn't start (it starts with the sound "
              "system: is there an audio output device?); creating or joining a match will stop "
              "the game");
  cod3::hud::AnnouncementFeed::Push(
      "No sound device: matches can't start. Connect speakers or headphones and restart.");
}

}  // namespace cod3::mpmenu

// Every menu system's draw (front end, in game, message boxes) ends here.
REX_EXTERN(__imp__FEMenuSystem__vfunc_19);
REX_HOOK_RAW(FEMenuSystem__vfunc_19) {
  const uint32_t system = ctx.r3.u32;
  if (Valid(system) && Valid(ReadU32(base, system + kSysPages)) &&
      (static_cast<int32_t>(ReadU32(base, system + kSysCurrent)) >= 0 ||
       static_cast<int32_t>(ReadU32(base, system + kSysOverlay)) >= 0)) {
    const int64_t now = NowNs();
    g_menu_frame.store(ReadU32(base, cod3::addr::pc_menu::kFrameCount), std::memory_order_relaxed);
    g_menu_ns.store(now, std::memory_order_relaxed);
    int64_t none = 0;
    g_first_menu_ns.compare_exchange_strong(none, now, std::memory_order_relaxed);
  }
  __imp__FEMenuSystem__vfunc_19(ctx, base);
}

// The main menu's A (this, player).
REX_EXTERN(__imp__MPMainMenuXBox__vfunc_36);
REX_HOOK_RAW(MPMainMenuXBox__vfunc_36) {
  const uint32_t page = ctx.r3.u32;
  if (Valid(page)) {
    const uint32_t item = ReadU32(base, page + kMainMenuRow) + ReadU32(base, page + kMainMenuRowOffset);
    if (item != kQuitItem) g_quit_asked_ns.store(0, std::memory_order_relaxed);  // asked, then changed course
    if (item == kXboxLiveItem) {
      REXLOG_INFO("cod3mp: main menu: XBOX LIVE isn't part of the PC version; not opened");
      cod3::hud::AnnouncementFeed::Push("XBOX LIVE isn't part of the PC version: play over SYSTEM LINK");
      return;
    }
    if (item == kQuitItem) {
      const int64_t now = NowNs();
      const int64_t asked = g_quit_asked_ns.load(std::memory_order_relaxed);
      if (asked != 0 && now - asked < kQuitConfirmNs) {
        g_quit_asked_ns.store(0, std::memory_order_relaxed);
        std::function<void()> quit;
        {
          std::lock_guard lock(g_quit_mutex);
          quit = g_quit;
        }
        REXLOG_INFO("cod3mp: quit to desktop (main menu, confirmed with a second A)");
        if (quit) quit();
        return;
      }
      g_quit_asked_ns.store(now, std::memory_order_relaxed);
      REXLOG_INFO("cod3mp: main menu: QUIT TO DESKTOP asks for a second A");
      cod3::hud::AnnouncementFeed::Push("Press A again to quit to the desktop");
      return;
    }
  }
  __imp__MPMainMenuXBox__vfunc_36(ctx, base);
}

// const char* Localize(StringTable*, const char* key): the port's texts for
// the main menu's changed items. (The campaign's own hook of this function
// is in game/menu_fov.cpp, which isn't part of cod3mp.)
REX_EXTERN(__imp__Localize);
REX_HOOK_RAW(Localize) {
  const uint32_t key = ctx.r4.u32;
  if (key) {
    const char* k = reinterpret_cast<const char*>(base + key);
    for (auto& t : g_texts) {
      if (std::strcmp(k, t.key) != 0) continue;
      uint32_t text = t.guest.load(std::memory_order_acquire);
      if (!text) {
        text = GuestString(base, t.text);
        uint32_t none = 0;
        if (!t.guest.compare_exchange_strong(none, text, std::memory_order_acq_rel)) text = none;
      }
      if (text) {
        ctx.r3.u64 = text;
        return;
      }
      break;
    }
  }
  __imp__Localize(ctx, base);
}
