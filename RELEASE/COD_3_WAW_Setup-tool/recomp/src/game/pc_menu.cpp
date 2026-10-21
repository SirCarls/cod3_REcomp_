// cod3 - PC controls for the game's own menus (see pc_menu.h).
//
// Both programs: the campaign (default.xex) and, since the [mp-menus] pass,
// multiplayer (cod3mp, codmp_xenonf.xex). The multiplayer program has the
// same menu framework with other fields and virtual slots: addresses.h gives
// the numbers for each program, and the few things that are built
// differently there are in `#if defined(COD3_MP)` blocks below (which menu
// system takes the input, where a system keeps its page, message box
// buttons, pages whose rows are a list box). The campaign's own pages (its
// pause menu, main menu, brightness page, storage buttons) are in
// `#if !defined(COD3_MP)` blocks; multiplayer's are in game/mp/menus_mp.cpp.

#include "game/pc_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/xvideo.h>
#include <rex/ui/flags.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/button_prompts.h"
#include "game/keybinds/binds.h"
#if !defined(COD3_MP)
#include "game/menu_slider.h"  // [claude-9b] the slider rows' number boxes
#include "game/menu_video.h"
#endif
#include "game/ultrawide.h"

REXCVAR_DEFINE_BOOL(cod3_pc_menus, true, "cod3/Mouse",
                    "Mouse and keyboard in the game's menus: the pointer selects, click to choose, "
                    "right click or Escape to go back, wheel and arrow keys to move, Enter to "
                    "choose (game/pc_menu.h)");

REXCVAR_DEFINE_BOOL(cod3_menu_trace, false, "cod3/Mouse",
                    "Log every change of a game menu's highlight and what made it (for bug reports)");

// [mp-menus] Automated tests only (like cod3_test_input): mouse and keyboard
// events for the game's menus at given times, "<seconds>:<what>;...", where
// <what> is key=<Up|Down|Left|Right|Enter|Escape|Backspace> (pressed and let
// go), move=<x>,<y> or click=<x>,<y> (the pointer in the menus' 640x480
// space), rclick, or wheel=<detents> (+ up). Seconds count from the start.
REXCVAR_DEFINE_STRING(cod3_test_menu_input, "", "cod3/Debug",
                      "Tests only: scripted mouse and keyboard events for the game's menus "
                      "(game/pc_menu.cpp)");

REXCVAR_DECLARE(bool, cod3_ultrawide_hud);

namespace {

namespace pm = cod3::addr::pc_menu;
namespace st = cod3::addr::storage;
using cod3::guest::Call;
using cod3::guest::ReadF32;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;
using cod3::guest::VirtualSlot;
using cod3::guest::WriteU32;
using Clock = std::chrono::steady_clock;

#if defined(COD3_MP)
// ---------------------------------------------------------------------------
// [mp-menus] cod3mp's menu framework (codmp_xenonf.xex), read at lined-up
// instructions of both programs (docs/MULTIPLAYER.md, "Menus"):
//  * A menu system's page that takes the input is one s32 at +28 (-1 none;
//    +20 is a page under an overlay: drawn first, no input), its pages at
//    +4, their count at +16 (HandleButton 0x824D5A08, the page switch
//    0x8250A148). The campaign keeps an array indexed by a slot instead.
//  * One system object per local player for the pause menus
//    (InGameMenuSystem, its local client at +48) and the message boxes
//    (DialogMenuSystem, client at +52), one front end, one after-action
//    report: FEManager +28, +192 + 4i, +208 + 4i, +224.
//  * Every system's update runs ProcessInput while it has a page, also a
//    system not on screen; vfunc +92 says whether it is up (FEManager draws
//    only those: 0x820BC990 reads byte +42; the message boxes' 0x820BD410
//    is "+28 >= 0"). On the split-screen sign-in page the front end runs
//    ProcessInput four times a frame (0x824D5FC0): the PC input acts once.
//  * Message box buttons: a DialogMenuDisplay at system +44 draws two FEText
//    slots, +20 / +24, or +36 / +40 in the split-screen layout (+60 set),
//    the highlighted slot at +56; button i is in slot i (0x824E85A8).
//  * Many front-end pages have no FEMenu items: their rows are a UIListBox
//    inside the page (below), selected = top (+96) + cursor (+100).
//  * Option rows are entries that hold their value (FEComboBox, FESlider):
//    FEMenu's own left / right step the selected entry.
// ---------------------------------------------------------------------------
namespace mpui {
inline constexpr uint32_t kFEManager = 0x82C6B1FC;      // = msgbox::kFrontEnd
inline constexpr uint32_t kFrontEndSystem = 28;         // FEManager +28
inline constexpr uint32_t kSysCurrentPage = 28;         // s32
inline constexpr uint32_t kSysPageCount = 16;           // u32
inline constexpr uint32_t kSysUp = 92;                  // vtbl: () -> u8, the system is on screen
inline constexpr uint32_t kInGameSystemVtable = 0x8207146C;
inline constexpr uint32_t kInGameClient = 48;           // u32: the local client
inline constexpr uint32_t kDialogClient = 52;           // u32 (DialogMenuSystem)
inline constexpr uint32_t kDialogDisplay = 44;          // DialogMenuDisplay*
inline constexpr uint32_t kDisplayButtonsA = 20;        // FEText*[2]
inline constexpr uint32_t kDisplayButtonsB = 36;        // FEText*[2], the split-screen layout
inline constexpr uint32_t kDisplayLayoutB = 60;         // u32: != 0 draws layout B
inline constexpr uint32_t kDisplaySelected = 56;        // s32: highlighted slot, -1 none
inline constexpr int kDisplayMaxButtons = 2;
// UIListBox (vtable 0x820711E4; SetRow 0x824E6520, Up 0x82509B18): rows
// (entries) of 20 bytes at *(+16), +16 u8 = selectable; the highest row set
// at +80, rows shown +84, the first shown +96, the cursor in them +100 (-1
// none). Shown row k: cells = *(*(+4) + 16k), column c at cells + 24c: +0 = 1
// when in use, *(+12) = its elements, the first is the row's text.
inline constexpr uint32_t kListVtable = 0x820711E4;
inline constexpr uint32_t kListShown = 4;
inline constexpr uint32_t kListRows = 16;
inline constexpr uint32_t kListLast = 80;
inline constexpr uint32_t kListVisible = 84;
inline constexpr uint32_t kListTop = 96;
inline constexpr uint32_t kListCursor = 100;
inline constexpr uint32_t kListRowSize = 20;
inline constexpr uint32_t kListRowSelectable = 16;
inline constexpr uint32_t kListShownSize = 16;
inline constexpr uint32_t kListCellSize = 24;
inline constexpr uint32_t kListCellElements = 12;
// The pages built on a list box, and where it is in them (their Up / Down
// and A read it there).
struct ListPage {
  uint32_t vtable, list;
};
inline constexpr ListPage kListPages[] = {
    {0x8207E95C, 148},  // MPMainMenuXBox (Up 0x823B7CE0)
    {0x8207B874, 100},  // MPProfileEditMenu, Options (Up 0x823EE220)
    {0x82083D3C, 144},  // PlayLanMenu, SYSTEM LINK
    {0x8208423C, 76},   // SessionLanListMenu, the game browser
    {0x8208413C, 88},   // SessionListMenu
    {0x82082CFC, 136},  // OverlayMenu
    {0x82082E04, 96},   // InGameOverlay
};
// Entries holding a value (left / right change it).
inline constexpr uint32_t kComboVtable = 0x820709EC;    // FEComboBox
inline constexpr uint32_t kSliderVtable = 0x82070AC4;   // FESlider
// The 85 page classes (RTTI: derived from FEMenu) lie in these runs of
// vtables, which hold no other class's.
inline constexpr uint32_t kPageVtables[][2] = {
    {0x82070774, 0x82070870}, {0x82070D4C, 0x82070E48}, {0x820712D4, 0x820713D0},
    {0x82071504, 0x82071600}, {0x820716DC, 0x820723FC}, {0x8207B374, 0x8207F958},
    {0x82082A5C, 0x82082B58}, {0x82082BF4, 0x82085270},
};
}  // namespace mpui
#endif

int64_t Now() { return Clock::now().time_since_epoch().count(); }
constexpr int64_t Ms(int64_t ms) { return ms * 1'000'000; }

int16_t ReadS16(uint8_t* base, uint32_t addr) { return static_cast<int16_t>(ReadU16(base, addr)); }
void WriteU16(uint8_t* base, uint32_t addr, uint16_t value) {  // big-endian like all guest data
  base[addr] = uint8_t(value >> 8);
  base[addr + 1] = uint8_t(value);
}

// A guest heap or image pointer (never null, never unaligned).
bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

// ---------------------------------------------------------------------------
// When a menu is up: a menu page took input in this game frame or the last
// one (the game's frame counter, pm::kFrameCount). Counting frames, not
// time, keeps it right at any frame rate, also at the cloud's 1-5 s software
// rendered frames. While the game thread is busy (a loading screen) the
// counter stands still, so a menu older than kLoadingLimit no longer counts.
// ---------------------------------------------------------------------------
constexpr int64_t kLoadingLimit = Ms(20000);

std::atomic<uint8_t*> g_base{nullptr};      // guest memory, for the UI thread's reads
std::atomic<uint32_t> g_menu_frame{0};      // frame in which a menu page last took input
std::atomic<int64_t> g_menu_time{0};        // and when (0 = never)
std::atomic<uint32_t> g_dialog_frame{0};    // same for a message box
std::atomic<bool> g_dialog_seen{false};

uint32_t FrameNow() {
  uint8_t* base = g_base.load(std::memory_order_relaxed);
  return base ? ReadU32(base, pm::kFrameCount) : 0;
}

// ---------------------------------------------------------------------------
// What the mouse and keys ask for, from the UI thread to the game thread.
// ---------------------------------------------------------------------------
enum class Ask {
  kButton,   // a button code (pm::kCode*), pressed and let go
  kHold,     // an arrow key pressed and held: the page repeats it like a held D-pad
  kRelease,  // that arrow key let go
  kAccept,   // A, or Start on the title screen
  kClick,    // left click at (x, y): the item there, or its value
};

struct Request {
  Ask ask = Ask::kButton;
  uint32_t code = 0;
  float x = 0.0f, y = 0.0f;
  uint32_t frame = 0;  // the game frame it was asked in
};

struct Pointer {
  std::mutex mutex;
  bool over = false;        // over the picture
  float x = 0.0f, y = 0.0f; // in the game's 640x480 menu space
  uint32_t moves = 0;       // counts motion events
  bool left_down = false;   // [menus-brightness] the left button is held (dragging a gauge)
  std::deque<Request> requests;
};
Pointer g_input;
constexpr size_t kMaxRequests = 8;

void PushLocked(Request r) {  // g_input.mutex held
  r.frame = FrameNow();
  if (g_input.requests.size() < kMaxRequests) g_input.requests.push_back(r);
}

void Push(Request r) {
  std::lock_guard lock(g_input.mutex);
  PushLocked(r);
}

// The arrow key held down (UI thread writes, game thread may let it go).
// The game repeats a held direction itself (the page keeps it until the
// release), so an arrow key is a press when it goes down and a release when
// it comes up, exactly like the D-pad; the OS's own key repeat is not used.
// If no key-up or OS repeat is seen for kHeldTimeout (focus lost on the
// way), the game thread lets it go.
struct HeldArrow {
  uint32_t code = 0;
  int64_t last_seen = 0;  // the key-down or its last OS repeat
};
HeldArrow g_held;  // g_input.mutex
constexpr int64_t kHeldTimeout = Ms(1500);

// ---------------------------------------------------------------------------
// Window pixels -> the game's 640x480 menu space.
// The presenter shows the picture at the guest display's shape with black
// bars around it (or stretched, or cropped, by its present_* settings). That
// shape is what VdQueryVideoMode reports: video_mode_width x _height (which
// game/ultrawide.cpp sets for wide pictures), else window_width x _height or
// the resolution preset when those are set, else 1280x720. On a picture
// wider than 16:9 the menus are drawn squeezed into its centered 16:9 part
// (ultrawide.cpp MapX).
// ---------------------------------------------------------------------------
std::atomic<uint32_t> g_display_width{1280}, g_display_height{720};

void RefreshPictureAspect() {
  rex::system::X_VIDEO_MODE mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  g_display_width.store(std::max<uint32_t>(1, mode.display_width), std::memory_order_relaxed);
  g_display_height.store(std::max<uint32_t>(1, mode.display_height), std::memory_order_relaxed);
}

// Where the picture lands in a window of ww x wh pixels: the same sums as
// the SDK's Presenter::GetGuestOutputPaintFlow.
struct PictureRect {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

PictureRect PictureInWindow(float ww, float wh) {
  const float dw = float(g_display_width.load(std::memory_order_relaxed));
  const float dh = float(g_display_height.load(std::memory_order_relaxed));
  const bool letterbox = REXCVAR_GET(present_letterbox);
  const bool overscan = REXCVAR_GET(present_allow_overscan_cutoff);
  auto safe_area = [overscan](int32_t percent) {
    return overscan && percent > 0 && percent < 100 ? float(percent) : 100.0f;
  };
  PictureRect r;
  if (ww * dh > wh * dw) {
    // A wider window: full width, cropped top and bottom at most to the safe
    // area, then bars at the sides (or stretched).
    const float safe = safe_area(REXCVAR_GET(present_safe_area_y));
    r.h = ww * dh / dw;
    bool bars = false;
    if (r.h * safe > wh * 100.0f) r.h = wh * 100.0f / safe, bars = true;
    r.w = bars && letterbox ? wh * 100.0f * dw / (dh * safe) : ww;
  } else {
    const float safe = safe_area(REXCVAR_GET(present_safe_area_x));
    r.w = wh * dw / dh;
    bool bars = false;
    if (r.w * safe > ww * 100.0f) r.w = ww * 100.0f / safe, bars = true;
    r.h = bars && letterbox ? ww * 100.0f * dh / (dw * safe) : wh;
  }
  r.x = (ww - r.w) * 0.5f;
  r.y = (wh - r.h) * 0.5f;
  return r;
}

bool ToMenuSpace(rex::ui::Window* window, int32_t px, int32_t py, float* vx, float* vy) {
  const float ww = float(window->GetActualPhysicalWidth());
  const float wh = float(window->GetActualPhysicalHeight());
  if (ww < 1.0f || wh < 1.0f) return false;
  const PictureRect pic = PictureInWindow(ww, wh);
  if (pic.w < 1.0f || pic.h < 1.0f) return false;
  const float u = (float(px) - pic.x) / pic.w;
  const float v = (float(py) - pic.y) / pic.h;
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
  float x = u * 640.0f;
  const float shape = cod3::DisplayAspect();
  if (shape > cod3::kSixteenByNine + 0.002f && REXCVAR_GET(cod3_ultrawide_hud))
    x = 320.0f + (x - 320.0f) * shape / cod3::kSixteenByNine;
  *vx = x;
  *vy = v * 480.0f;
  return true;
}

// ---------------------------------------------------------------------------
// UI thread: the window's mouse and keyboard while a menu is up.
// ---------------------------------------------------------------------------
class MenuInput final : public rex::ui::WindowInputListener {
 public:
  rex::ui::Window* window = nullptr;
  std::function<bool()> overlay_open;

  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;  // [menus-brightness]
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

 private:
  bool Usable() const {
    return cod3::pcmenu::MenuActive() && !(overlay_open && overlay_open());
  }
  int wheel_ = 0;
};
MenuInput g_listener;

void MenuInput::OnKeyDown(rex::ui::KeyEvent& e) {
  using rex::ui::VirtualKey;
  if (e.is_alt_pressed() || e.is_ctrl_pressed() || !Usable()) return;
#if !defined(COD3_MP)
  // [claude-9b] A slider row's number box (game/menu_slider.h) takes digits on
  // its row, and Enter, Esc and Backspace while a number is typed.
  if (cod3::menuslider::OnKey(static_cast<uint16_t>(e.virtual_key()), e.is_shift_pressed())) {
    e.set_handled(true);
    return;
  }
#endif
  Request r;
  bool repeats = false;
  switch (e.virtual_key()) {
    case VirtualKey::kUp: r.code = pm::kCodeUp, repeats = true; break;
    case VirtualKey::kDown: r.code = pm::kCodeDown, repeats = true; break;
    case VirtualKey::kLeft: r.code = pm::kCodeLeft, repeats = true; break;
    case VirtualKey::kRight: r.code = pm::kCodeRight, repeats = true; break;
    case VirtualKey::kReturn: r.ask = Ask::kAccept; break;
    case VirtualKey::kEscape:
    case VirtualKey::kBack: r.code = pm::kCodeB; break;
    default: return;
  }
  // Ours alone: for the keyboard/mouse driver Enter and Escape are Start and
  // the arrows the right stick.
  e.set_handled(true);
  std::lock_guard lock(g_input.mutex);
  if (repeats) {
    // A held arrow: the game's own repeat (see HeldArrow); the OS's repeats
    // only say it is still held.
    if (e.prev_state()) {
      if (g_held.code == r.code) g_held.last_seen = Now();
      return;
    }
    if (g_held.code && g_held.code != r.code) PushLocked({Ask::kRelease, g_held.code});
    r.ask = Ask::kHold;
    g_held = {r.code, Now()};
    PushLocked(r);
    return;
  }
  // A held Enter or Escape (the Escape that just opened the pause menu) acts
  // once.
  if (!e.prev_state()) PushLocked(r);
}

void MenuInput::OnKeyUp(rex::ui::KeyEvent& e) {
  using rex::ui::VirtualKey;
  uint32_t code = 0;
  switch (e.virtual_key()) {
    case VirtualKey::kUp: code = pm::kCodeUp; break;
    case VirtualKey::kDown: code = pm::kCodeDown; break;
    case VirtualKey::kLeft: code = pm::kCodeLeft; break;
    case VirtualKey::kRight: code = pm::kCodeRight; break;
    default: return;
  }
  // Not handled: whoever else saw the key go down sees it come up too.
  std::lock_guard lock(g_input.mutex);
  if (g_held.code != code) return;
  g_held.code = 0;
  PushLocked({Ask::kRelease, code});
}

void MenuInput::OnMouseMove(rex::ui::MouseEvent& e) {
  // Over F2 or the console the pointer isn't pointing at the menu behind.
  if (!window || (overlay_open && overlay_open())) return;
  float x = 0.0f, y = 0.0f;
  const bool over = ToMenuSpace(window, e.x(), e.y(), &x, &y);
  std::lock_guard lock(g_input.mutex);
  g_input.over = over;
  if (over) g_input.x = x, g_input.y = y;
  ++g_input.moves;
}

void MenuInput::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!window || !Usable()) return;
  using Button = rex::ui::MouseEvent::Button;
  if (e.button() == Button::kLeft) {
    Request r;
    r.ask = Ask::kClick;
    std::lock_guard lock(g_input.mutex);
    g_input.left_down = true;  // [menus-brightness]
    if (ToMenuSpace(window, e.x(), e.y(), &r.x, &r.y)) PushLocked(r);
  } else if (e.button() == Button::kRight) {
    Request r;
    r.code = pm::kCodeB;
    Push(r);
  } else {
    return;
  }
  // Not also a trigger pull for the keyboard/mouse driver.
  e.set_handled(true);
}

// [menus-brightness] Ends a gauge drag. Not handled: others see it too.
void MenuInput::OnMouseUp(rex::ui::MouseEvent& e) {
  if (e.button() != rex::ui::MouseEvent::Button::kLeft) return;
  std::lock_guard lock(g_input.mutex);
  g_input.left_down = false;
}

void MenuInput::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!Usable()) return;
  wheel_ += e.scroll_y();
  const int detent = rex::ui::MouseEvent::kScrollPerDetent;
  for (; wheel_ >= detent; wheel_ -= detent) Push({Ask::kButton, pm::kCodeUp});
  for (; wheel_ <= -detent; wheel_ += detent) Push({Ask::kButton, pm::kCodeDown});
  e.set_handled(true);
}

// ---------------------------------------------------------------------------
// Game thread: the page on screen and its items.
// ---------------------------------------------------------------------------
// obj->method(a1, a2) through the object's vtable.
uint64_t VCall(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint64_t a1 = 0,
               uint64_t a2 = 0) {
  Call c(ctx, base);
  return c.Invoke(VirtualSlot(base, obj, slot), {obj, a1, a2});
}

// Every menu class's vtable lies in this range (FEMenu 0x82070EDC ...
// ChapterSelectMenu 0x82072A8C); anything else isn't a page.
#if !defined(COD3_MP)
bool IsPage(uint8_t* base, uint32_t page) {
  if (!Valid(page)) return false;
  const uint32_t vt = ReadU32(base, page);
  return vt >= 0x82070E00 && vt < 0x82072C00;
}

// The page the menu system shows now, or 0.
uint32_t CurrentPage(PPCContext& ctx, uint8_t* base, uint32_t system) {
  const uint32_t slot = uint32_t(VCall(ctx, base, system, pm::kSysSlot));
  if (slot > 3) return 0;
  const int32_t index = static_cast<int32_t>(ReadU32(base, system + pm::kSysCurrent + 4 * slot));
  if (index < 0 || index > 64) return 0;
  const uint32_t pages = ReadU32(base, system + pm::kSysPages);
  if (!Valid(pages)) return 0;
  const uint32_t page = ReadU32(base, pages + 4 * uint32_t(index));
  return IsPage(base, page) ? page : 0;
}
#else
// [mp-menus] cod3mp: the page classes' vtables (see mpui).
bool IsPage(uint8_t* base, uint32_t page) {
  if (!Valid(page)) return false;
  const uint32_t vt = ReadU32(base, page);
  for (const auto& r : mpui::kPageVtables)
    if (vt >= r[0] && vt < r[1]) return true;
  return false;
}

// The page taking the system's input (+28), or 0.
uint32_t CurrentPage(PPCContext&, uint8_t* base, uint32_t system) {
  const int32_t index = static_cast<int32_t>(ReadU32(base, system + mpui::kSysCurrentPage));
  const int32_t count = static_cast<int32_t>(ReadU32(base, system + mpui::kSysPageCount));
  if (index < 0 || index >= count || count > 64) return 0;
  const uint32_t pages = ReadU32(base, system + pm::kSysPages);
  if (!Valid(pages)) return 0;
  const uint32_t page = ReadU32(base, pages + 4 * uint32_t(index));
  return IsPage(base, page) ? page : 0;
}

// The page's list box (the rows of MPMainMenuXBox, Options, SYSTEM LINK, the
// game browser ...), or 0 for a page of FEMenu items.
uint32_t PageList(uint8_t* base, uint32_t page) {
  const uint32_t vt = ReadU32(base, page);
  for (const auto& p : mpui::kListPages)
    if (p.vtable == vt) return ReadU32(base, page + p.list) == mpui::kListVtable ? page + p.list : 0;
  return 0;
}
#endif

struct Box {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float cx() const { return (x0 + x1) * 0.5f; }
  float cy() const { return (y0 + y1) * 0.5f; }
  bool Contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

// [menus-brightness] The arrows and the gauge drawn on a row (or a page's own,
// not on a row: Options > SCREEN), from the pictures the menu draws (see
// "Arrows and gauges" below).
struct RowControls {
  bool left = false, right = false, gauge = false, fill = false;
  Box l, r;  // the left and right arrow
  Box g, f;  // the gauge's frame (its whole range) and its fill
};

struct Item {
  int index = 0;
  Box ink;                    // the label's letters as drawn
  Box hit;                    // what the pointer selects it in
  bool left_aligned = false;  // label on the left, a value to its right
  RowControls ctl;            // [menus-brightness] its arrows and gauge
};

// ---------------------------------------------------------------------------
// [menus-brightness] Arrows and gauges. Every picture drawn inside a menu
// system's draw goes through the picture draw (PanelQuad__vfunc_1, hooked at
// the end); the ones named as a left / right arrow (arrowL1, arrowR6,
// slot_05_arrow_left, gauge_arrow_right ...) or a gauge (slot_01_gauge_back,
// slot_01_gauge_fill, HSensProgbar1 / 2, MusicVolProgbar ...) are noted with
// where they were drawn, like the footer prompts. The port's own rows draw
// copies of the game's pictures, with their names, so they are found too.
// A row gets the ones drawn on its line and its hit area grows to cover
// them (the arrows lie right of the labels' column on most pages).
// ---------------------------------------------------------------------------
enum class ControlKind : uint8_t { kLeft, kRight, kGauge, kFill };

struct Control {
  uint32_t system = 0;  // the menu system that drew it
  uint32_t quad = 0;
  uint32_t frame = 0;   // the game frame it was drawn in
  ControlKind kind = ControlKind::kLeft;
  Box box;
};

std::mutex g_control_mutex;
std::vector<Control> g_controls;  // g_control_mutex
constexpr float kControlMargin = 4.0f;  // menu units a click may miss a picture by

// What a picture's name says it is; false: neither an arrow nor a gauge.
bool ControlKindOf(std::string name, ControlKind* kind) {
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  auto has = [&name](const char* s) { return name.find(s) != std::string::npos; };
  auto numbered = [&name](const char* prefix) {  // arrowL1 .. arrowR7
    const size_t n = std::strlen(prefix);
    return name.size() > n && name.compare(0, n, prefix) == 0 && std::isdigit(static_cast<unsigned char>(name[n]));
  };
  if (has("arrow")) {
    if (has("left") || numbered("arrowl")) return *kind = ControlKind::kLeft, true;
    if (has("right") || numbered("arrowr")) return *kind = ControlKind::kRight, true;
    return false;  // up / down and the HUD's arrows
  }
  if (has("gauge") || has("progbar")) {
    const bool fill = has("fill") || (name.size() >= 8 && name.compare(name.size() - 8, 8, "progbar2") == 0);
    *kind = fill ? ControlKind::kFill : ControlKind::kGauge;
    return true;
  }
  return false;
}

// The rectangle a picture covers (its parts' vertices), in menu space.
bool QuadBox(uint8_t* base, uint32_t quad, Box* out) {
  namespace fe = cod3::addr::fe;
  const uint32_t parts = ReadU32(base, quad + fe::kQuadParts), n = ReadU32(base, quad + fe::kQuadPartCount);
  if (!Valid(parts) || n == 0 || n > 8) return false;
  Box b{1e9f, 1e9f, -1e9f, -1e9f};
  for (uint32_t p = 0; p < n; ++p) {
    const uint32_t part = ReadU32(base, parts + 4 * p);
    for (uint32_t v = 0; Valid(part) && v < 4; ++v) {
      const uint32_t at = part + fe::kQuadPartVerts + v * fe::kQuadVertStride;
      const float x = ReadF32(base, at), y = ReadF32(base, at + 4);
      if (!std::isfinite(x) || !std::isfinite(y)) return false;
      b.x0 = std::min(b.x0, x), b.x1 = std::max(b.x1, x);
      b.y0 = std::min(b.y0, y), b.y1 = std::max(b.y1, y);
    }
  }
  if (b.x0 > b.x1 || b.y0 > b.y1 || b.x1 - b.x0 > 640.0f || b.y1 - b.y0 > 480.0f) return false;
  *out = b;
  return true;
}

// What `system` drew in the last two frames.
std::vector<Control> DrawnControls(uint32_t system) {
  const uint32_t frame = FrameNow();
  std::vector<Control> out;
  std::lock_guard lock(g_control_mutex);
  for (const auto& c : g_controls)
    if (c.system == system && frame - c.frame <= 2) out.push_back(c);
  return out;
}

Box Union(const Box& a, const Box& b) {
  return Box{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

void AddControl(RowControls& rc, const Control& c) {
  auto add = [&c](bool& has, Box& box) {
    box = has ? Union(box, c.box) : c.box;
    has = true;
  };
  switch (c.kind) {
    case ControlKind::kLeft: add(rc.left, rc.l); break;
    case ControlKind::kRight: add(rc.right, rc.r); break;
    case ControlKind::kGauge: add(rc.gauge, rc.g); break;
    case ControlKind::kFill: add(rc.fill, rc.f); break;
  }
}

// Each control goes to the row whose hit area holds its middle (the nearest
// row's letters if two do), else to `loose` (the page's own).
void AttachControls(std::vector<Item>& items, const std::vector<Control>& controls, RowControls* loose) {
  for (const Control& c : controls) {
    Item* row = nullptr;
    float best = 1e9f;
    for (auto& it : items) {
      if (c.box.cy() < it.hit.y0 || c.box.cy() > it.hit.y1) continue;
      const float d = std::abs(c.box.cy() - it.ink.cy());
      if (d < best) best = d, row = &it;
    }
    if (!row) {
      if (loose) AddControl(*loose, c);
      continue;
    }
    AddControl(row->ctl, c);
    row->hit.x0 = std::min(row->hit.x0, c.box.x0 - kControlMargin);
    row->hit.x1 = std::max(row->hit.x1, c.box.x1 + kControlMargin);
  }
}

bool NearBox(const Box& b, float x, float y, float margin) {
  // Thin pictures (a gauge's bar) get at least a row's height around them.
  const float grow_y = std::max(margin, (12.0f - (b.y1 - b.y0)) * 0.5f);
  return x >= b.x0 - margin && x <= b.x1 + margin && y >= b.y0 - grow_y && y <= b.y1 + grow_y;
}

// The arrow under (x, y): its button code (left / right), else 0.
uint32_t ArrowAt(const RowControls& rc, float x, float y) {
  const bool l = rc.left && NearBox(rc.l, x, y, kControlMargin);
  const bool r = rc.right && NearBox(rc.r, x, y, kControlMargin);
  if (l && r) return std::abs(x - rc.l.cx()) <= std::abs(x - rc.r.cx()) ? pm::kCodeLeft : pm::kCodeRight;
  return l ? pm::kCodeLeft : r ? pm::kCodeRight : 0;
}

bool GaugeAt(const RowControls& rc, float x, float y) {
  return rc.gauge && rc.g.x1 - rc.g.x0 >= 4.0f && NearBox(rc.g, x, y, 3.0f);
}

// For the page log: " L x0,y0-x1,y1 R ... G ... F ..." (arrows, gauge, fill).
std::string DescribeControls(const RowControls& rc) {
  std::string s;
  auto add = [&s](bool has, const char* tag, const Box& b) {
    if (has) s += fmt::format(" {} {:.0f},{:.0f}-{:.0f},{:.0f}", tag, b.x0, b.y0, b.x1, b.y1);
  };
  add(rc.left, "L", rc.l);
  add(rc.right, "R", rc.r);
  add(rc.gauge, "G", rc.g);
  add(rc.fill, "F", rc.f);
  return s;
}

bool IsText(uint8_t* base, uint32_t elem) {
  const uint32_t vt = ReadU32(base, elem);
  return vt == pm::kTextVtable || vt == pm::kMultiLineTextVtable;
}

// The font a text draws with (FEText::Draw), or 0.
uint32_t TextFont(uint8_t* base, int32_t index) {
  if (index < 0 || index >= int32_t(cod3::addr::prompts::kFontCount)) return 0;
  const uint32_t font = ReadU32(base, cod3::addr::prompts::kFonts + 4 * uint32_t(index));
  return Valid(font) && ReadU32(base, font + pm::kFontTexture) ? font : 0;
}

// A glyph of the font (clamped to its first / last, as the game does).
uint32_t Glyph(uint8_t* base, uint32_t font, uint8_t c) {
  const int32_t first = int32_t(ReadU32(base, font + pm::kFontFirstChar));
  const int32_t count = int32_t(ReadU32(base, font + pm::kFontGlyphCount));
  const uint32_t glyphs = ReadU32(base, font + pm::kFontGlyphs);
  if (count <= 0 || !Valid(glyphs)) return 0;
  const int32_t i = std::clamp<int32_t>(int32_t(c) - first, 0, count - 1);
  return glyphs + pm::kGlyphSize * uint32_t(i);
}

int32_t GlyphField(uint8_t* base, uint32_t glyph, int field) {
  return int32_t(ReadU32(base, glyph + 4 * uint32_t(field)));
}

// One line of text laid out like the game's measure (0x82134B90) and draw:
// its width at scale `sx` (to the last letter's right edge) and the top and
// bottom of its letters at scale `sy`, below the line's top. False for text
// with control codes (colors, line breaks) or nothing to draw.
struct TextMetrics {
  float width = 0, height = 0;    // what the game measures (alignment)
  float ink_top = 0, ink_bottom = 0;
};

bool MeasureText(uint8_t* base, uint32_t font, const std::string& text, float sx, float sy,
                 TextMetrics* out) {
  if (!font || text.empty()) return false;
  float pen = 0.0f, top = 1e9f, bottom = -1e9f;
  uint32_t last = 0;
  for (char ch : text) {
    const uint8_t c = uint8_t(ch);
    if (c < 32) return false;
#if defined(COD3_MP)
    // [mp-menus] cod3mp's text draw shows "~X" as a button picture (X of the
    // button font): the game's own measure counts it.
    if (c == '~') return false;
#endif
    // A space moves on like an 'a' and draws nothing.
    const uint32_t g = Glyph(base, font, c == ' ' ? uint8_t('a') : c);
    if (!g) return false;
    pen += float(GlyphField(base, g, 6)) * sx;
    last = g;
    if (c == ' ') continue;
    const float y_off = float(GlyphField(base, g, 5)), h = float(GlyphField(base, g, 3));
    top = std::min(top, y_off * sy);
    bottom = std::max(bottom, (y_off + h) * sy);
  }
  if (last) pen += float(GlyphField(base, last, 4) + GlyphField(base, last, 2) - GlyphField(base, last, 6)) * sx;
  out->width = std::trunc(pen);
  out->height = std::trunc(float(int32_t(ReadU32(base, font + pm::kFontLineHeight))) * sx);
  if (top > bottom) top = 0.0f, bottom = out->height;
  out->ink_top = top, out->ink_bottom = bottom;
  return out->width > 0.0f;
}

std::string GuestString(uint8_t* base, uint32_t string, size_t max_len = 256) {
  return Valid(string) ? cod3::guest::ReadString(base, string + pm::kStringChars, max_len) : std::string();
}

// Where a text element's letters are drawn (FEText::Draw 0x824CFD58): at its
// x / y moved by its alignment (0x824BF4A8) with the size the game measures,
// at the scale of its state (selected or not). False when it isn't drawn.
bool DrawnText(PPCContext& ctx, uint8_t* base, uint32_t text, bool selected, Box* out) {
  if (!Valid(text) || !IsText(base, text)) return false;
  if (!(ReadU8(base, text + pm::kTextVisibility) & 0x4)) return false;
  float x = ReadF32(base, text + pm::kTextX);
  float y = ReadF32(base, text + pm::kTextY);
  const uint32_t scale = text + (selected ? pm::kTextScaleSelected : pm::kTextScaleNormal);
  const float sx = ReadF32(base, scale), sy = ReadF32(base, scale + 4);
  if (!std::isfinite(x) || !std::isfinite(y) || !(sx > 0.0f && sx < 20.0f) || !(sy > 0.0f && sy < 20.0f))
    return false;
  TextMetrics m;
  const uint32_t font = TextFont(base, int32_t(ReadU32(base, text + pm::kTextFont)));
  if (!MeasureText(base, font, GuestString(base, ReadU32(base, text + pm::kTextString)), sx, sy, &m)) {
    // Not a plain line (a multi-line text, control codes): the game's own
    // measure of the box (at the selected scale).
    Call c(ctx, base);
    c.Invoke(VirtualSlot(base, text, pm::kTextWidth), {text, 0});
    m.width = float(c.ResultF1());
    c.Invoke(VirtualSlot(base, text, pm::kTextHeight), {text, 0});
    m.height = float(c.ResultF1());
    m.ink_top = 0.0f, m.ink_bottom = m.height;
  }
  if (!(m.width > 0.0f && m.width < 2000.0f && m.height > 0.0f && m.height < 1000.0f)) return false;
  const uint16_t align = ReadU16(base, text + pm::kTextAlign);
  if (align & pm::kAlignRight)
    x -= m.width;
  else if (!(align & pm::kAlignLeft))
    x -= m.width * 0.5f;
  if (align & pm::kAlignBottom)
    y -= m.height;
  else if (!(align & pm::kAlignTop))
    y -= m.height * 0.5f;
  *out = Box{x, y + m.ink_top, x + m.width, y + m.ink_bottom};
  return true;
}

// A message box's button list (msgbox::kSysButtons of its DialogMenuSystem)
// and the slot item `i` of `count` is drawn in (see addresses.h).
uint32_t DialogButtons(uint8_t* base, uint32_t system) {
  if (!Valid(system) || ReadU32(base, system) != pm::kDialogSystemVtable) return 0;
#if defined(COD3_MP)
  const uint32_t buttons = ReadU32(base, system + mpui::kDialogDisplay);  // [mp-menus] its display
#else
  const uint32_t buttons = ReadU32(base, system + cod3::addr::msgbox::kSysButtons);
#endif
  return Valid(buttons) ? buttons : 0;
}

#if !defined(COD3_MP)
int DialogSlot(uint8_t* base, uint32_t buttons, int i) {
  const uint32_t count = ReadU32(base, buttons + pm::kButtonCount);
  if (count == 1) return 2;
  if (count == 4) return i;
  const bool middle = ReadU32(base, buttons + pm::kButtonSlots + 4) && ReadU32(base, buttons + pm::kButtonSlots + 8);
  return middle ? i + 1 : i;
}
#endif

// The text a message box draws for its button `i` and whether it is drawn
// highlighted; 0 when it has no such button.
uint32_t DialogButton(uint8_t* base, uint32_t buttons, int i, bool* selected) {
#if defined(COD3_MP)
  // [mp-menus] Two slots, button i in slot i, in the layout the box draws.
  if (i < 0 || i >= mpui::kDisplayMaxButtons) return 0;
  const uint32_t slots =
      buttons + (ReadU32(base, buttons + mpui::kDisplayLayoutB) ? mpui::kDisplayButtonsB : mpui::kDisplayButtonsA);
  *selected = static_cast<int32_t>(ReadU32(base, buttons + mpui::kDisplaySelected)) == i;
  return ReadU32(base, slots + 4 * uint32_t(i));
#else
  const int slot = DialogSlot(base, buttons, i);
  if (slot < 0 || slot > 3) return 0;
  *selected = ReadU32(base, buttons + pm::kButtonSelectedSlot) == uint32_t(slot);
  return ReadU32(base, buttons + pm::kButtonSlots + 4 * uint32_t(slot));
#endif
}

#if defined(COD3_MP)
// [mp-menus] A list box's rows: how many, which is selected (-1 none), and
// the text a shown row draws (0 if none).
int ListCount(uint8_t* base, uint32_t list) {
  const int32_t last = static_cast<int32_t>(ReadU32(base, list + mpui::kListLast));
  return last < 0 || last > 255 ? 0 : last + 1;
}

int ListSelected(uint8_t* base, uint32_t list) {
  const int32_t cursor = static_cast<int32_t>(ReadU32(base, list + mpui::kListCursor));
  if (cursor < 0) return -1;
  return static_cast<int32_t>(ReadU32(base, list + mpui::kListTop)) + cursor;
}

bool ListRowSelectable(uint8_t* base, uint32_t list, int row) {
  const uint32_t rows = ReadU32(base, list + mpui::kListRows);
  return Valid(rows) && ReadU8(base, rows + mpui::kListRowSize * uint32_t(row) + mpui::kListRowSelectable) != 0;
}

uint32_t ListRowText(uint8_t* base, uint32_t list, int shown) {
  const uint32_t records = ReadU32(base, list + mpui::kListShown);
  if (!Valid(records)) return 0;
  const uint32_t cells = ReadU32(base, records + mpui::kListShownSize * uint32_t(shown));
  if (!Valid(cells) || ReadU32(base, cells) != 1) return 0;  // column 0
  const uint32_t elements = ReadU32(base, cells + mpui::kListCellElements);
  return Valid(elements) ? ReadU32(base, elements) : 0;
}
#endif

// The page's items: its FEMenu items, or (cod3mp) its list box's rows.
int ItemCount(uint8_t* base, uint32_t page) {
#if defined(COD3_MP)
  if (const uint32_t list = PageList(base, page)) return ListCount(base, list);
#endif
  return ReadS16(base, page + pm::kPageCount);
}

// Hit areas from the drawn letters. Items on one line (a message box's
// buttons side by side) split it half way between them; rows of a list span
// the list's column (on a page of settings a left-aligned column gets its
// mirror image on the right, where the values are) and reach half way to the
// next row, at most about a row's height.
void LayOutHitAreas(std::vector<Item>& items, bool has_values) {
  if (items.empty()) return;
  float left = 640.0f, right = 0.0f, line = 0.0f;
  bool any_left = false;
  for (const auto& it : items) {
    left = std::min(left, it.ink.x0);
    right = std::max(right, it.ink.x1);
    line = std::max(line, it.ink.y1 - it.ink.y0);
    any_left |= it.left_aligned;
  }
  if (any_left && has_values) right = std::max(right, 640.0f - left);
  // Short words ("Yes", "No") get a column of a usable width around them.
  constexpr float kMinColumn = 80.0f;
  if (right - left < kMinColumn) {
    const float grow = (kMinColumn - (right - left)) * 0.5f;
    left -= any_left ? 0.0f : grow;
    right += any_left ? 2.0f * grow : grow;
  }
  const float reach = std::max(line, 8.0f);  // farthest a hit area reaches past its letters
  auto same_line = [line](const Item& a, const Item& b) {
    return std::abs(a.ink.cy() - b.ink.cy()) < line * 0.5f;
  };
  for (auto& it : items) {
    float top = it.ink.y0 - reach * 0.5f, bottom = it.ink.y1 + reach * 0.5f;
    float x0 = left - 6.0f, x1 = right + 6.0f;
    bool shares_line = false;
    for (const auto& o : items) {
      if (&o == &it) continue;
      if (same_line(o, it)) {
        // Side by side: up to half way to the neighbor.
        if (!shares_line) x0 = it.ink.x0 - reach, x1 = it.ink.x1 + reach;
        shares_line = true;
        const float mid = o.ink.cx() > it.ink.cx() ? (it.ink.x1 + o.ink.x0) * 0.5f : (o.ink.x1 + it.ink.x0) * 0.5f;
        if (o.ink.cx() > it.ink.cx()) x1 = std::min(x1, mid);
        else x0 = std::max(x0, mid);
        continue;
      }
      const float mid = (o.ink.cy() + it.ink.cy()) * 0.5f;
      if (o.ink.cy() > it.ink.cy()) bottom = std::min(bottom, mid);
      else top = std::max(top, mid);
    }
    it.hit = Box{x0, top, x1, bottom};
  }
}

// A page of settings: it takes left / right (its rows have values).
bool HasValues(uint8_t* base, uint32_t page) {
#if defined(COD3_MP)
  // [mp-menus] cod3mp's option rows are entries holding their value
  // (FEMenu's own left / right step them); a list box page has none.
  if (PageList(base, page)) return false;
  const int count = ReadS16(base, page + pm::kPageCount);
  const uint32_t list = ReadU32(base, page + pm::kPageItems);
  for (int i = 0; Valid(list) && i < count && i < 64; ++i) {
    const uint32_t entry = ReadU32(base, list + 4 * uint32_t(i));
    const uint32_t vt = Valid(entry) ? ReadU32(base, entry) : 0;
    if (vt == mpui::kComboVtable || vt == mpui::kSliderVtable) return true;
  }
#endif
  const uint32_t on_left = VirtualSlot(base, page, pm::kPageOnLeft);
  return on_left != pm::kFEMenuOnLeft && on_left != pm::kNullsub;
}

// Where each selectable item of the page is drawn and can be pointed at, in
// menu space. `system` is the page's menu system (a message box draws its
// buttons from its system's button list, not from the page). [menus-brightness]
// With the arrows and gauges drawn on each row; `loose` gets the page's own
// (drawn on no row).
std::vector<Item> ItemRects(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t system,
                            RowControls* loose = nullptr) {
  std::vector<Item> items;
#if defined(COD3_MP)
  // [mp-menus] A list box page: its shown rows' texts (the panel draws them,
  // not the page; the highlight is the panel's band).
  if (const uint32_t rows = PageList(base, page)) {
    const int count = ListCount(base, rows);
    const int top = static_cast<int32_t>(ReadU32(base, rows + mpui::kListTop));
    const int shown = static_cast<int32_t>(ReadU32(base, rows + mpui::kListVisible));
    for (int k = 0; top >= 0 && k < shown && k < 32; ++k) {
      const int i = top + k;
      if (i >= count || !ListRowSelectable(base, rows, i)) continue;
      Item it;
      const uint32_t label = ListRowText(base, rows, k);
      if (!DrawnText(ctx, base, label, false, &it.ink)) continue;
      it.index = i;
      it.left_aligned = (ReadU16(base, label + pm::kTextAlign) & pm::kAlignLeft) != 0;
      items.push_back(it);
    }
    LayOutHitAreas(items, false);
    return items;
  }
#endif
  const int count = ReadS16(base, page + pm::kPageCount);
  const uint32_t list = ReadU32(base, page + pm::kPageItems);
  if (count <= 0 || count > 64 || !Valid(list)) {
    // [menus-brightness] A page's own slider (only a page that takes left /
    // right: a page sliding out may still draw its arrows for a moment).
    if (loose && HasValues(base, page)) AttachControls(items, DrawnControls(system), loose);
    return items;
  }
  // A scrolling list draws only a window of its items.
  int first = 0, shown = count;
  if (ReadU16(base, page + pm::kPageFlags) & 1) {
    first = std::clamp<int>(ReadS16(base, page + pm::kPageScrollTop), 0, count - 1);
    shown = std::clamp<int>(ReadS16(base, page + pm::kPageScrollShown), 0, count);
  }
  const uint32_t buttons =
      ReadU32(base, page) == cod3::addr::msgbox::kPageVtable ? DialogButtons(base, system) : 0;
  for (int k = 0; k < shown; ++k) {
    const int i = (first + k) % count;
    const uint32_t entry = ReadU32(base, list + 4 * uint32_t(i));
    if (!Valid(entry) || ReadU8(base, entry + pm::kEntryDisabled)) continue;
    uint32_t label = ReadU32(base, entry + pm::kEntryLabel);
    bool selected = ReadU8(base, entry + pm::kEntryDrawnSelected) != 0;
    if (buttons) {
      label = DialogButton(base, buttons, i, &selected);
      if (!label) continue;
    }
    Item it;
    if (!DrawnText(ctx, base, label, selected, &it.ink)) continue;
    it.index = i;
    it.left_aligned = (ReadU16(base, label + pm::kTextAlign) & pm::kAlignLeft) != 0;
    items.push_back(it);
  }
  const bool values = HasValues(base, page);
  LayOutHitAreas(items, values);
  if (!buttons && values) AttachControls(items, DrawnControls(system), loose);  // [menus-brightness]
  return items;
}

const Item* ItemAt(const std::vector<Item>& items, float x, float y) {
  const Item* best = nullptr;
  float best_d = 1e9f;
  for (const auto& it : items) {
    if (!it.hit.Contains(x, y)) continue;
    const float dx = x - it.ink.cx(), dy = y - it.ink.cy();
    const float d = dx * dx * 0.01f + dy * dy;  // mostly the nearest row
    if (d < best_d) best = &it, best_d = d;
  }
  return best;
}

const Item* FindItem(const std::vector<Item>& items, int index) {
  for (const auto& it : items)
    if (it.index == index) return &it;
  return nullptr;
}

int Selected(uint8_t* base, uint32_t page) {
#if defined(COD3_MP)
  if (const uint32_t list = PageList(base, page)) return ListSelected(base, list);  // [mp-menus]
#endif
  return ReadS16(base, page + pm::kPageSelected);
}

// Moves the highlight to `target` with the page's own MoveUp / MoveDown, the
// way the D-pad does (so the page updates its titles, help text and sound),
// stepping the way the target lies on screen.
void SelectItem(PPCContext& ctx, uint8_t* base, uint32_t page, const std::vector<Item>& items,
                int target) {
  const Item* to = FindItem(items, target);
  if (!to) return;
  const int count = ItemCount(base, page);
  for (int n = 0; n < count; ++n) {
    const int sel = Selected(base, page);
    if (sel == target) return;
    const Item* from = FindItem(items, sel);
    const float line = std::max(1.0f, (to->ink.y1 - to->ink.y0) * 0.5f);
    const bool down = from ? (to->ink.cy() > from->ink.cy() + line ||
                              (std::abs(to->ink.cy() - from->ink.cy()) <= line && to->ink.cx() > from->ink.cx()))
                           : target > sel;
    VCall(ctx, base, page, down ? pm::kPageMoveDown : pm::kPageMoveUp);
    if (Selected(base, page) == sel) return;  // the page didn't move
  }
}

// One button event, through the menu system's own handler (as from the pad).
// The handler gets the pad that pressed; the game reads the active one first
// ("none" before Start is pressed on the title screen) and then whichever pad
// pressed. The keyboard and mouse are player 1 (pad 0).
uint64_t MenuPlayer(PPCContext& ctx, uint8_t* base, uint32_t system) {
  uint64_t player = VCall(ctx, base, system, pm::kSysPlayer) & 0xFFFFFFFFu;
  return player > 3 ? 0 : player;
}

bool IsDirection(uint32_t code) {
  return code == pm::kCodeUp || code == pm::kCodeDown || code == pm::kCodeLeft || code == pm::kCodeRight;
}

// [menus-brightness] Game thread: the code our Press is sending right now, and
// the arrow key held into the game (pressed with hold, not yet released), for
// FromPcInput.
uint32_t g_pressing_code = 0;
uint32_t g_holding_code = 0;

// The release the pad's input sends when a direction is let go: the page
// stops repeating it. Without it a page repeats the direction for ever
// (the "endless scrolling" of mouse wheel and arrow keys before).
void Release(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t code) {
  if (g_holding_code == code) g_holding_code = 0;  // [menus-brightness]
  VCall(ctx, base, system, pm::kSysRelease, code, MenuPlayer(ctx, base, system));
}

// A button: pressed, and let go at once unless `hold` (a held arrow key).
void Press(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t code, bool hold = false) {
  const uint64_t player = MenuPlayer(ctx, base, system);
  REXLOG_DEBUG("cod3: menu input {} (player {})", code, player);
  g_pressing_code = code;  // [menus-brightness]
  if (hold && IsDirection(code)) g_holding_code = code;
  VCall(ctx, base, system, pm::kSysButton, code, player);
  g_pressing_code = 0;
  if (IsDirection(code) && !hold) Release(ctx, base, system, code);
}

bool IsTitlePage(uint8_t* base, uint32_t page) {
  return ReadU32(base, page) == pm::kTitleMenuVtable;
}

// ---------------------------------------------------------------------------
// The pointer and the keys share the highlight the way a PC game does: the
// item under the pointer is highlighted when the mouse moves onto it; a key,
// the wheel or the pad then takes over, and the pointer only takes the
// highlight back once the mouse moves a few pixels away from where it was
// (or onto another item). A resting or jittering pointer never pulls the
// highlight back, so it can't fight the arrow keys. The pointer moves the
// highlight with the page's own moves, a step whenever the page lets it (it
// ignores moves while the highlight animates), and gives up after a bounded
// number of frames, so nothing can keep scrolling.
// ---------------------------------------------------------------------------
constexpr float kTakeBackDistance = 6.0f;  // menu units (640x480): ~13 px at 1080p
constexpr int kClickWaitFrames = 120;       // a click's action waits this long at most

struct Hover {
  uint32_t page = 0;
  int last_selected = -1;  // the selection when the last frame's input was done
  int pointed = -1;        // the item under the pointer at its last move
  int target = -1;         // the item the pointer asks for, not reached yet
  int tries = 0;
  bool keys = false;       // keys / wheel / pad have the highlight
  float key_x = 0.0f, key_y = 0.0f;  // where the pointer was when they took it
  // A click on an item that wasn't highlighted yet: its action follows once
  // the highlight has settled on it (the page ignores input while it moves).
  uint32_t click_code = 0;
  int click_item = -1;
  int click_frames = 0;
};
Hover g_hover;  // game thread only

void KeysTakeOver(Hover& h, float x, float y) {
  h.keys = true;
  h.key_x = x, h.key_y = y;
  h.target = -1;
  h.click_item = -1;
}

bool PageLocked(uint8_t* base, uint32_t page) { return ReadU8(base, page + pm::kPageLocked) != 0; }

// ---------------------------------------------------------------------------
// [menus-brightness] A gauge follows the pointer: a click on it, and the
// pointer while the button stays down. The game's brightness is set directly
// (menu_video.h SetSlider); any other gauge moves by its row's own left /
// right steps, at most one a frame, toward the pointer, and stops on the
// step that reaches or passes it (where its fill ends shows where it is).
// It never keeps going on its own: it waits for the pointer to move, ends
// when the button is up and it has arrived, and gives up after a bounded
// number of steps or when the page changes.
// ---------------------------------------------------------------------------
constexpr int kMaxDragSteps = 400;

struct Drag {
  bool active = false;
  uint32_t page = 0;
  int item = -1;           // the row, or -1: the page's own slider
  bool released = false;   // the button is up: finish at the last place
  float x = 0.0f;          // where the pointer wants the gauge (menu x)
  float stop_x = -1.0f;    // arrived (or passed) with the pointer here
  int last_dir = 0;        // the last step's direction
  float last_frac = -1.0f; // where the fill was before it
  float step = 0.0f;       // how far one step moved the fill
  int steps = 0, still = 0, wait = 0;
  bool direct = false;     // set directly (the game's brightness)
};
Drag g_drag;  // game thread only

void StartDrag(uint32_t page, int item, float x) {
  g_drag = Drag{};
  g_drag.active = true;
  g_drag.page = page;
  g_drag.item = item;
  g_drag.x = x;
}

void EndDrag(const char* why) {
  if (g_drag.active && REXCVAR_GET(cod3_menu_trace))
    REXLOG_INFO("cod3: menu trace: gauge of item {} let go ({}, {} steps)", g_drag.item, why, g_drag.steps);
  g_drag = Drag{};
}

void UpdateDrag(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t page, float px, bool down) {
  Drag& d = g_drag;
  if (!d.active) return;
  if (d.page != page) return EndDrag("page changed");
  if (!d.released) d.x = px;  // the pointer's latest place: where it was let go too
  if (!down) d.released = true;
  RowControls loose;
  const std::vector<Item> items = ItemRects(ctx, base, page, system, &loose);
  const RowControls* rc = &loose;
  if (d.item >= 0) {
    const Item* it = FindItem(items, d.item);
    // Wait for the highlight to settle on the row (a click on another row).
    if (!it || Selected(base, page) != d.item || PageLocked(base, page)) {
      if (++d.wait > kClickWaitFrames) EndDrag("row not reached");
      return;
    }
    rc = &it->ctl;
  }
  const float width = rc->g.x1 - rc->g.x0;
  if (!rc->gauge || width < 4.0f) return EndDrag("no gauge");
  const float target = std::clamp((d.x - rc->g.x0) / width, 0.0f, 1.0f);
#if defined(COD3_MP)
  const bool set = false;  // [mp-menus] every cod3mp gauge moves by its row's steps
#else
  const bool set = cod3::videomenu::SetSlider(base, page, d.item, target);
#endif
  if (set) {
    if (!d.direct || d.released) REXLOG_INFO("cod3: menu slider set to {:.2f} of its range{}", target, d.released ? "" : " (held)");
    d.direct = true;
    if (d.released) EndDrag("set");
    return;
  }
  if (!rc->fill) return EndDrag("no fill");
  const float frac = std::clamp((rc->f.x1 - rc->g.x0) / width, 0.0f, 1.0f);
  if (d.stop_x >= 0.0f) {  // arrived: wait for the pointer to move on
    if (d.released) return EndDrag("arrived");
    if (std::abs(d.x - d.stop_x) < 1.0f) return;
    d.stop_x = -1.0f, d.last_dir = 0, d.last_frac = -1.0f, d.still = 0;
  }
  if (d.last_dir != 0 && d.last_frac >= 0.0f) {
    const float moved = std::abs(frac - d.last_frac);
    if (moved < 1e-4f) {
      if (++d.still >= 3) d.stop_x = d.x, d.last_dir = 0;  // at an end, or the row doesn't move
      return;
    }
    d.still = 0, d.step = moved;
  }
  const float diff = target - frac;
  const int dir = std::abs(diff) <= std::max(0.004f, d.step * 0.5f) ? 0 : diff > 0.0f ? 1 : -1;
  if (dir == 0 || (d.last_dir != 0 && dir != d.last_dir)) {  // there, or one step past it
    d.stop_x = d.x, d.last_dir = 0;
    if (d.released) EndDrag("arrived");
    return;
  }
  if (++d.steps > kMaxDragSteps) return EndDrag("too many steps");
  Press(ctx, base, system, dir > 0 ? pm::kCodeRight : pm::kCodeLeft);
  d.last_dir = dir, d.last_frac = frac;
}

// ---------------------------------------------------------------------------
// Footer prompts ("[ENTER] SELECT [ESC] BACK", "(A) SELECT (B) BACK") are
// buttons too: a click on one presses what it names. Their places come from
// the game's own line layout (addresses.h, FEMultiLineText lines), noted as
// each line is drawn (game/button_prompts.cpp calls NoteLineDrawn) inside a
// menu system's draw, so a click only ever reaches the prompts of the menu
// that takes the input, never a hint of the game behind it.
// ---------------------------------------------------------------------------
struct Prompt {
  uint32_t system = 0;  // the menu system that drew it
  uint32_t line = 0;    // the text line it is on
  uint32_t frame = 0;   // the game frame it was drawn in
  Box area;             // its button and the words after it
  uint32_t code = 0;    // the button (pm::kCode*), or 0 for Accept (Enter)
};

std::mutex g_prompt_mutex;
std::vector<Prompt> g_prompts;          // g_prompt_mutex
thread_local uint32_t t_drawing_system = 0;  // the menu system drawing right now (game thread)

// What a prompt's button stands for: the game's button picture (a character
// of the button font) or a name in brackets (button_prompts.cpp: the key the
// PC menus take, or a controller style's name). Only buttons a menu takes;
// 0 = none (d-pad and stick pictures, triggers).
bool PromptButton(const std::string& glyph, uint32_t* code) {
  struct Name {
    const char* text;
    uint32_t code;  // 0: Accept, like the Enter key
  };
  static constexpr Name kNames[] = {
      {"\"", pm::kCodeA},        {"%", pm::kCodeB},           {"#", pm::kCodeX},
      {"$", pm::kCodeY},         {"-", pm::kCodeStart},       {"1", pm::kCodeBack},
      {"[ENTER]", 0},            {"[ESC]", pm::kCodeB},       {"[A]", pm::kCodeA},
      {"[CROSS]", pm::kCodeA},   {"[B]", pm::kCodeB},         {"[CIRCLE]", pm::kCodeB},
      {"[X]", pm::kCodeX},       {"[SQUARE]", pm::kCodeX},    {"[Y]", pm::kCodeY},
      {"[TRIANGLE]", pm::kCodeY}, {"[START]", pm::kCodeStart}, {"[BACK]", pm::kCodeBack},
      {"[SELECT]", pm::kCodeBack},
  };
  for (const auto& n : kNames)
    if (glyph == n.text) return *code = n.code, true;
  return false;
}

void NoteLine(uint8_t* base, uint32_t line, float sy) {
  const uint32_t system = t_drawing_system;
  if (!system || !Valid(line)) return;
  const int32_t n = int32_t(ReadU32(base, line + pm::kLineSegmentCount));
  const uint32_t segments = ReadU32(base, line + pm::kLineSegments);
  if (n <= 0 || n > 64 || !Valid(segments)) return;  // no buttons
  const uint32_t string = ReadU32(base, line + pm::kLineString);
  if (!Valid(string)) return;
  const std::string text = GuestString(base, string, 512);
  const float x = ReadF32(base, line + pm::kLineX), y = ReadF32(base, line + pm::kLineY);
  const float width = ReadF32(base, line + pm::kLineWidth);
  const uint32_t font = TextFont(base, int32_t(ReadU32(base, line + 0)));
  if (!font || !std::isfinite(x) || !std::isfinite(y) || !(width > 0.0f && width < 2000.0f)) return;
  const float height = float(int32_t(ReadU32(base, font + pm::kFontLineHeight))) * sy;
  auto entry = [&](int i, int field) {  // field 0: string index, 1: x offset
    return int32_t(int16_t(ReadU16(base, segments + 4 * uint32_t(i) + 2 * uint32_t(field))));
  };
  const uint32_t frame = ReadU32(base, pm::kFrameCount);
  std::lock_guard lock(g_prompt_mutex);
  // This line's prompts as drawn now; those of older frames are gone (a
  // page's footer is drawn every frame).
  std::erase_if(g_prompts, [frame, line](const Prompt& p) { return p.line == line || frame - p.frame > 2; });
  for (int k = 0; k < n; k += 2) {
    const int32_t start = entry(k, 0);
    const int32_t end = k + 1 < n ? entry(k + 1, 0) : int32_t(text.size());
    if (start < 0 || end <= start || end > int32_t(text.size())) continue;
    Prompt p;
    if (!PromptButton(text.substr(size_t(start), size_t(end - start)), &p.code)) continue;
    p.system = system;
    p.line = line;
    p.frame = frame;
    const float x0 = x + float(entry(k, 1));
    const float x1 = k + 2 < n ? x + float(entry(k + 2, 1)) : x + width;
    p.area = Box{x0 - 3.0f, y - 3.0f, std::max(x0 + 4.0f, x1 - 3.0f), y + height + 3.0f};
    if (g_prompts.size() < 64) g_prompts.push_back(p);
  }
}

// [menus-brightness] A picture drawn inside a menu system's draw: noted when
// it is an arrow or a gauge (see "Arrows and gauges" above).
void NoteControl(uint8_t* base, uint32_t quad) {
  namespace fe = cod3::addr::fe;
  const uint32_t system = t_drawing_system;
  if (!system || !Valid(quad)) return;
  if (!(ReadU8(base, quad + pm::kTextVisibility) & 0x4) || !(ReadF32(base, quad + fe::kElemOpacity) > 0.05f)) return;
  Control c;
  if (!ControlKindOf(GuestString(base, ReadU32(base, quad + fe::kQuadName), 64), &c.kind)) return;
  if (!QuadBox(base, quad, &c.box)) return;
  c.system = system;
  c.quad = quad;
  c.frame = ReadU32(base, pm::kFrameCount);
  std::lock_guard lock(g_control_mutex);
  std::erase_if(g_controls, [&c](const Control& o) { return o.quad == c.quad || c.frame - o.frame > 2; });
  if (g_controls.size() < 128) g_controls.push_back(c);
}

// The prompt of `system` under (x, y), drawn in the last two frames.
std::optional<Prompt> PromptAt(uint32_t system, float x, float y) {
  const uint32_t frame = FrameNow();
  std::lock_guard lock(g_prompt_mutex);
  for (const auto& p : g_prompts)
    if (p.system == system && frame - p.frame <= 2 && p.area.Contains(x, y)) return p;
  return std::nullopt;
}

// A left click: the item under the pointer (its value side on option rows),
// a footer prompt, or, on a page without items ("Press Start", a results
// screen), go on.
// Acts on item `index` with `code` (A, or a step left / right): at once when
// it is highlighted already, else once the highlight has moved there.
void ClickItem(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t page, const std::vector<Item>& items,
               int index, uint32_t code) {
  const bool was_selected = Selected(base, page) == index;
  if (!was_selected) SelectItem(ctx, base, page, items, index);
  if (was_selected && !PageLocked(base, page)) {
    Press(ctx, base, system, code);
    return;
  }
  // The highlight is (still) moving there: act once it has settled.
  g_hover.click_code = code;
  g_hover.click_item = index;
  g_hover.click_frames = 0;
  g_hover.target = index;
  g_hover.tries = 0;
}

void Click(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t page, float x, float y) {
  EndDrag("new click");  // [menus-brightness]
  RowControls loose;
  const std::vector<Item> items = ItemRects(ctx, base, page, system, &loose);
#if !defined(COD3_MP)
  // [claude-9b] A click on a slider row's number box: typing starts there
  // (game/menu_slider.h) and the row is highlighted.
  if (const int box = cod3::menuslider::ClickAt(page, x, y); box >= 0) {
    if (REXCVAR_GET(cod3_menu_trace)) REXLOG_INFO("cod3: menu trace: number box of item {} clicked", box);
    if (Selected(base, page) != box) SelectItem(ctx, base, page, items, box);
    g_hover.target = Selected(base, page) == box ? -1 : box;
    g_hover.tries = 0;
    return;
  }
#endif
  // [menus-brightness] An arrow steps its row's value its way; a gauge goes
  // to the place clicked (and follows the pointer while the button is held).
  // The page's own (Options > SCREEN) likewise, without a row.
  if (HasValues(base, page)) {
    for (const auto& row : items) {
      if (const uint32_t code = ArrowAt(row.ctl, x, y)) {
        if (REXCVAR_GET(cod3_menu_trace)) REXLOG_INFO("cod3: menu trace: arrow {} of item {} clicked", code, row.index);
        ClickItem(ctx, base, system, page, items, row.index, code);
        return;
      }
      if (GaugeAt(row.ctl, x, y)) {
        if (Selected(base, page) != row.index) SelectItem(ctx, base, page, items, row.index);
        StartDrag(page, row.index, x);
        g_hover.target = row.index;
        g_hover.tries = 0;
        return;
      }
    }
    if (const uint32_t code = ArrowAt(loose, x, y)) {
      Press(ctx, base, system, code);
      return;
    }
    if (GaugeAt(loose, x, y)) {
      StartDrag(page, -1, x);
      return;
    }
  }
  const Item* it = ItemAt(items, x, y);
  if (!it) {
    if (const auto prompt = PromptAt(system, x, y)) {
      const uint32_t code = prompt->code ? prompt->code : IsTitlePage(base, page) ? pm::kCodeStart : pm::kCodeA;
      if (REXCVAR_GET(cod3_menu_trace)) REXLOG_INFO("cod3: menu trace: footer prompt clicked: button {}", code);
      Press(ctx, base, system, code);
      return;
    }
    if (items.empty()) Press(ctx, base, system, IsTitlePage(base, page) ? pm::kCodeStart : pm::kCodeA);
    return;  // beside the items: nothing, like a PC menu
  }
  // An option row (the page takes left / right): a click on its value steps
  // it toward the half clicked. [menus-brightness] With both arrows drawn the
  // value is what lies between them; else (no arrows found) the old rule:
  // right of a left-aligned label, split half way to the column's edge.
  uint32_t code = pm::kCodeA;
  const RowControls& rc = it->ctl;
  const bool values = HasValues(base, page);
  if (values && rc.left && rc.right) {
    if (x >= rc.l.x0 - kControlMargin && x <= rc.r.x1 + kControlMargin)
      code = x < (rc.l.cx() + rc.r.cx()) * 0.5f ? pm::kCodeLeft : pm::kCodeRight;
  } else if (values && !rc.left && !rc.right && !rc.gauge && it->left_aligned && x > it->ink.x1 + 8.0f) {
    const float mid = (it->ink.x1 + it->hit.x1) * 0.5f;
    code = x < mid ? pm::kCodeLeft : pm::kCodeRight;
  }
  ClickItem(ctx, base, system, page, items, it->index, code);
}

// Logs each new page once with where its items are drawn (letters) and can
// be pointed at (hit), in menu space (640x480), for the notes and for bug
// reports. With cod3_menu_trace also whenever that changes (pages slide in,
// a message box gets new buttons) and the footer prompts.
void DumpPanel(uint8_t* base, uint32_t panel, const char* what);  // [menus-brightness] below

void LogPage(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t page) {
  static uint32_t last_page = 0, last_vtable = 0;
  static std::string last_rects;
  const uint32_t vt = ReadU32(base, page);
  const bool new_page = page != last_page || vt != last_vtable;
  const bool trace = REXCVAR_GET(cod3_menu_trace);
  // [menus-brightness] The arrows and gauges are known once the page has been
  // drawn: a new page is logged again (once) when they show up.
  static int controls_wait = 0;  // frames left to look for them
  if (new_page) controls_wait = 120;
  if (!new_page && !trace && controls_wait <= 0) return;
  if (!new_page && controls_wait > 0) --controls_wait;
  RowControls loose;
  const auto items = ItemRects(ctx, base, page, system, &loose);
  std::string rects;
  for (const auto& it : items)
    rects += fmt::format(" [{}: {:.0f},{:.0f}-{:.0f},{:.0f} hit {:.0f},{:.0f}-{:.0f},{:.0f}{}]", it.index,
                         it.ink.x0, it.ink.y0, it.ink.x1, it.ink.y1, it.hit.x0, it.hit.y0, it.hit.x1,
                         it.hit.y1, DescribeControls(it.ctl));  // [menus-brightness] arrows, gauge
  if (const std::string own = DescribeControls(loose); !own.empty()) rects += " [page:" + own + "]";
  const bool has_controls = rects.find(" L ") != std::string::npos || rects.find(" G ") != std::string::npos;
  const bool controls_now = !new_page && has_controls && controls_wait > 0;
  if (has_controls) controls_wait = 0;
  if (!new_page && !controls_now && (!trace || rects == last_rects)) return;
  last_page = page;
  last_vtable = vt;
  last_rects = rects;
  REXLOG_INFO("cod3: menu page {:08X} (vtable {:08X}, system {:08X}): {} items, selected {}{}", page,
              vt, ReadU32(base, system), ItemCount(base, page), Selected(base, page), rects);
  if (!trace) return;
#if !defined(COD3_MP)
  // [menus-brightness] The pause menu's Options and Options > SCREEN keep
  // their panel at +56: dump it once (layout of the added row, help bar).
  if (vt == cod3::addr::fe::adv::kVtable || vt == cod3::addr::brightness::kPageVtable)
    DumpPanel(base, ReadU32(base, page + cod3::addr::fe::adv::kPanel), "page");
#endif
  std::string prompts;
  {
    const uint32_t frame = FrameNow();
    std::lock_guard lock(g_prompt_mutex);
    for (const auto& p : g_prompts)
      if (p.system == system && frame - p.frame <= 2)
        prompts += fmt::format(" [button {}: {:.0f},{:.0f}-{:.0f},{:.0f}]", p.code, p.area.x0, p.area.y0,
                               p.area.x1, p.area.y1);
  }
  if (!prompts.empty()) REXLOG_INFO("cod3: menu trace: footer prompts{}", prompts);
}

#if defined(COD3_MP)
// [mp-menus] Whether the mouse and keyboard drive this menu system: it is on
// screen, and a pause menu or message box belongs to the keyboard's player
// (binds::KeyboardClient: in split screen the local client on controller 0,
// which the keyboard and mouse share; the others keep their controllers).
// The front end's message boxes are everyone's.
bool TakesPcInput(PPCContext& ctx, uint8_t* base, uint32_t system) {
  if (!(VCall(ctx, base, system, mpui::kSysUp) & 0xFF)) return false;
  const uint32_t vt = ReadU32(base, system);
  uint32_t client = 0;
  if (vt == mpui::kInGameSystemVtable) {
    client = ReadU32(base, system + mpui::kInGameClient);
  } else if (vt == pm::kDialogSystemVtable) {
    const uint32_t front_end = ReadU32(base, mpui::kFEManager + mpui::kFrontEndSystem);
    if (Valid(front_end) && (VCall(ctx, base, front_end, mpui::kSysUp) & 0xFF)) return true;
    client = ReadU32(base, system + mpui::kDialogClient);
  } else {
    return true;  // the front end, the after-action report
  }
  return client == cod3::binds::KeyboardClient(base);
}

// ProcessInput may run more than once a frame for a system (see mpui).
bool FirstInputThisFrame(uint32_t system, uint32_t frame) {
  struct Seen {
    uint32_t system = 0, frame = 0;
  };
  static Seen seen[16];  // game thread only
  for (auto& s : seen) {
    if (s.system != system) continue;
    if (s.frame == frame) return false;
    s.frame = frame;
    return true;
  }
  for (auto& s : seen) {
    if (s.system) continue;
    s = Seen{system, frame};
    return true;
  }
  // More systems than slots (never: 10 in all): forget the oldest.
  std::move(seen + 1, seen + 16, seen);
  seen[15] = Seen{system, frame};
  return true;
}
#endif

void OnMenuInput(PPCContext& ctx, uint8_t* base, uint32_t system) {
  if (!REXCVAR_GET(cod3_pc_menus) || !Valid(system)) return;
#if defined(COD3_MP)
  if (!FirstInputThisFrame(system, ReadU32(base, pm::kFrameCount)) || !TakesPcInput(ctx, base, system)) return;
#endif
  const uint32_t page = CurrentPage(ctx, base, system);
  if (!page) return;
  g_base.store(base, std::memory_order_relaxed);
  const uint32_t frame = ReadU32(base, pm::kFrameCount);
  g_menu_frame.store(frame, std::memory_order_relaxed);
  g_menu_time.store(Now(), std::memory_order_relaxed);
  const bool dialog = ReadU32(base, system) == pm::kDialogSystemVtable;
  if (dialog) {
    g_dialog_frame.store(frame, std::memory_order_relaxed);
    g_dialog_seen.store(true, std::memory_order_relaxed);
  } else if (g_dialog_seen.load(std::memory_order_relaxed) &&
             frame - g_dialog_frame.load(std::memory_order_relaxed) <= 1) {
    return;  // a message box over this menu takes the input
  }

  LogPage(ctx, base, system, page);

  Hover& h = g_hover;
  if (h.page != page) {  // a new page: nothing carried over
    h = Hover{};
    h.page = page;
    h.last_selected = Selected(base, page);
    g_holding_code = 0;  // [menus-brightness] a new page forgets a held direction too
  }

  bool moved = false, over = false, left_down = false;
  float x = 0.0f, y = 0.0f;
  std::optional<Request> request;
  {
    static uint32_t moves_seen = 0;
    std::lock_guard lock(g_input.mutex);
    moved = g_input.moves != moves_seen;
    moves_seen = g_input.moves;
    over = g_input.over;
    x = g_input.x, y = g_input.y;
    left_down = g_input.left_down;  // [menus-brightness]
    // One event per frame, like the pad; old ones (asked as a menu
    // closed) are dropped, but never a release.
    while (!g_input.requests.empty() && !request) {
      const Request& front = g_input.requests.front();
      if (front.ask == Ask::kRelease || frame - front.frame <= kMaxRequests + 2) request = front;
      g_input.requests.pop_front();
    }
    // An arrow tapped within one frame: one move, as a press and release
    // together (the page would otherwise repeat it until the next frame).
    if (request && request->ask == Ask::kHold && !g_input.requests.empty() &&
        g_input.requests.front().ask == Ask::kRelease && g_input.requests.front().code == request->code) {
      request->ask = Ask::kButton;
      g_input.requests.pop_front();
    }
    // A held arrow whose key-up never came (focus lost): let it go.
    if (!request && g_held.code && Now() - g_held.last_seen > kHeldTimeout) {
      request = Request{Ask::kRelease, g_held.code};
      g_held.code = 0;
    }
  }

  // The pad (the game's own input, read just before this) moved the
  // highlight: it has it now, like the keys.
  const bool trace = REXCVAR_GET(cod3_menu_trace);
  if (Selected(base, page) != h.last_selected) {
    if (trace) REXLOG_INFO("cod3: menu trace: {} -> {} (pad or the game)", h.last_selected, Selected(base, page));
    KeysTakeOver(h, x, y);
    EndDrag("the pad moved");  // [menus-brightness]
  }
  const int start_selected = Selected(base, page);
  const int start_target = h.target;

  if (request) {
    if (request->ask == Ask::kClick) {
      h.keys = false;
      h.target = -1;
      h.click_item = -1;
    } else if (request->ask != Ask::kRelease) {
      KeysTakeOver(h, x, y);
      // [mp-menus] What the pointer rests on now (a new page forgot it):
      // nudging it a pixel there is no move onto another item.
      if (over && h.pointed < 0) {
        const auto items = ItemRects(ctx, base, page, system);
        const Item* it = ItemAt(items, x, y);
        h.pointed = it ? it->index : -1;
      }
      EndDrag("keys");  // [menus-brightness]
    }
  } else if (g_drag.active) {
    // [menus-brightness] The pointer drags a gauge: the highlight stays.
  } else if (moved && !over) {
    h.pointed = -1;
  } else if (moved) {
    // The mouse moved: unless the keys have the highlight and the pointer is
    // still where it was when they took it, highlight what it points at.
    const float dx = x - h.key_x, dy = y - h.key_y;
    const auto items = ItemRects(ctx, base, page, system);
    const Item* it = ItemAt(items, x, y);
    const int pointed = it ? it->index : -1;
    const bool away = dx * dx + dy * dy >= kTakeBackDistance * kTakeBackDistance;
    if (!h.keys || away || pointed != h.pointed) {
      h.keys = false;
      if (pointed >= 0 && pointed != Selected(base, page) && pointed != h.target) {
        h.target = pointed;
        h.tries = 0;
      }
    }
    h.pointed = pointed;
  }

  // Walk the highlight to the pointer's item: one try per frame, bounded.
  if (!request && !h.keys && h.target >= 0) {
    const int count = ItemCount(base, page);
    if (Selected(base, page) == h.target || ++h.tries > count + kClickWaitFrames) {
      if (Selected(base, page) != h.target) h.click_item = -1;
      h.target = -1;
    } else if (!PageLocked(base, page)) {
      SelectItem(ctx, base, page, ItemRects(ctx, base, page, system), h.target);
    }
  }
  // A click's action, once the highlight is on its item and has settled.
  if (!request && h.click_item >= 0) {
    if (Selected(base, page) == h.click_item && !PageLocked(base, page)) {
      const uint32_t code = h.click_code;
      h.click_item = -1;
      Press(ctx, base, system, code);
    } else if (++h.click_frames > kClickWaitFrames) {
      h.click_item = -1;
    }
  }

  if (request) {
    switch (request->ask) {
      case Ask::kButton:
        Press(ctx, base, system, request->code);
        break;
      case Ask::kHold:
        Press(ctx, base, system, request->code, /*hold=*/true);
        break;
      case Ask::kRelease:
        Release(ctx, base, system, request->code);
        break;
      case Ask::kAccept:
        Press(ctx, base, system, IsTitlePage(base, page) ? pm::kCodeStart : pm::kCodeA);
        break;
      case Ask::kClick:
        Click(ctx, base, system, page, request->x, request->y);
        break;
    }
  }
  // [menus-brightness] A gauge being dragged (or going to where it was
  // clicked): at most one step a frame.
  if (!request || request->ask == Ask::kClick) UpdateDrag(ctx, base, system, page, x, left_down);
  if (trace && (Selected(base, page) != start_selected || h.target != start_target || request || moved)) {
    const char* what = !request ? "pointer"
                       : request->ask == Ask::kClick ? "click"
                       : request->ask == Ask::kAccept ? "enter"
                       : request->ask == Ask::kHold ? "key held"
                       : request->ask == Ask::kRelease ? "key let go" : "key";
    REXLOG_INFO("cod3: menu trace: {} -> {} ({}{}; pointer {:.0f},{:.0f}{}{}, target {}, keys {})",
                start_selected, Selected(base, page), what,
                request && request->code ? fmt::format(" {}", request->code) : std::string(),
                x, y, over ? "" : " outside", moved ? " moved" : "", h.target, h.keys);
  }
  // The page may have changed (Press can open another one); the next frame
  // starts over then.
  h.last_selected = Selected(base, page);
}

// ---------------------------------------------------------------------------
// Panel elements: an item's label key, a panel's elements by name, and copies
// of elements made the way the game makes them (both programs; the pages that
// get rows added use them: the campaign's main menu below, cod3mp's in
// game/mp/menus_mp.cpp).
// ---------------------------------------------------------------------------
void SetEntryLabelKey(PPCContext& ctx, uint8_t* base, uint32_t entry, const char* key) {
  Call c(ctx, base);
  const uint32_t s = c.PushString(key);
  c.Invoke(VirtualSlot(base, entry, pm::kEntrySetLabelKey), {entry, s});
}

uint32_t FindText(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  const uint32_t s = c.PushString(name);
  const uint32_t e = static_cast<uint32_t>(c.Invoke(cod3::addr::fe::kPanelFindElement, {panel, s}));
  return Valid(e) && IsText(base, e) ? e : 0;
}

// A picture of the panel by name, or 0 (the lookup's stand-in for a missing
// name is not that name).
uint32_t FindQuad(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  const uint32_t s = c.PushString(name);
  const uint32_t quad = static_cast<uint32_t>(c.Invoke(cod3::addr::fe::kPanelFindQuad, {panel, s}));
  if (!Valid(quad)) return 0;
  return GuestString(base, ReadU32(base, quad + cod3::addr::fe::kQuadName), 64) == name ? quad : 0;
}

// A copy of a panel element, made the way the game copies elements
// (0x824E1588): operator new, the class's constructor, Assign(source).
uint32_t CopyElement(PPCContext& ctx, uint8_t* base, uint32_t src, bool quad) {
  namespace fe = cod3::addr::fe;
  if (!Valid(src)) return 0;
  Call c(ctx, base);
  const uint32_t mem = static_cast<uint32_t>(c.Invoke(fe::kOperatorNew, {quad ? fe::kQuadSize : fe::kTextSize}));
  if (!Valid(mem)) return 0;
  c.Invoke(quad ? fe::kQuadCtor : fe::kTextCtor, {mem});
  c.Invoke(VirtualSlot(base, mem, fe::kElemAssign), {mem, src});
  return mem;
}

// Copies follow their source `dy` lower: a text its transform, a picture
// its vertices; both its opacity and whether it is shown.
void FollowElement(uint8_t* base, uint32_t src, uint32_t dst, bool quad, float dy) {
  namespace fe = cod3::addr::fe;
  using cod3::guest::WriteF32;
  if (!Valid(src) || !Valid(dst)) return;
  if (quad) {
    const uint32_t a = ReadU32(base, src + fe::kQuadParts), b = ReadU32(base, dst + fe::kQuadParts);
    const uint32_t n = ReadU32(base, src + fe::kQuadPartCount);
    if (!Valid(a) || !Valid(b) || n != ReadU32(base, dst + fe::kQuadPartCount) || n > 8) return;
    for (uint32_t part = 0; part < n; ++part) {
      const uint32_t qa = ReadU32(base, a + 4 * part), qb = ReadU32(base, b + 4 * part);
      if (!Valid(qa) || !Valid(qb)) continue;
      for (uint32_t v = 0; v < 4; ++v) {
        const uint32_t at = fe::kQuadPartVerts + v * fe::kQuadVertStride;
        WriteF32(base, qb + at, ReadF32(base, qa + at));
        WriteF32(base, qb + at + 4, ReadF32(base, qa + at + 4) + dy);
      }
    }
  } else {
    for (uint32_t at = fe::kTextTransform; at < fe::kTextTransformEnd; at += 4)
      WriteF32(base, dst + at, ReadF32(base, src + at));
    WriteF32(base, dst + fe::kTextY, ReadF32(base, src + fe::kTextY) + dy);
  }
  WriteF32(base, dst + fe::kElemOpacity, ReadF32(base, src + fe::kElemOpacity));
  const uint8_t shown = ReadU8(base, src + pm::kTextVisibility) & 0x4;
  base[dst + pm::kTextVisibility] = uint8_t((ReadU8(base, dst + pm::kTextVisibility) & ~0x4) | shown);
}

std::mutex g_pause_mutex;
std::function<void()> g_quit_handler;  // the app: save and end (g_pause_mutex)

void RunQuitHandler() {
  std::function<void()> quit;
  {
    std::lock_guard lock(g_pause_mutex);
    quit = g_quit_handler;
  }
  if (quit) quit();
}

#if !defined(COD3_MP)
// ---------------------------------------------------------------------------
// No storage device: the pause menu's item and the message boxes' buttons.
// ---------------------------------------------------------------------------
// Quit (QUIT TO MENU now) moves up into the storage item's row (the labels
// are swapped), and the storage item becomes QUIT TO DESKTOP on the bottom
// row: the game's own "quit without saving?" message box, whose Yes then
// saves the settings and ends the program like the window's close button.
// The page's up / down follow the rows on screen.
std::vector<uint32_t> g_pause_pages;  // pause menus with the two quit rows (g_pause_mutex)
std::atomic<bool> g_quit_to_desktop{false};  // the quit box was opened by QUIT TO DESKTOP

bool HasQuitRows(uint32_t page) {
  std::lock_guard lock(g_pause_mutex);
  return std::find(g_pause_pages.begin(), g_pause_pages.end(), page) != g_pause_pages.end();
}

void SetUpPauseQuitRows(PPCContext& ctx, uint8_t* base, uint32_t page, bool init) {
  if (!IsPage(base, page)) return;
  const uint32_t list = ReadU32(base, page + pm::kPageItems);
  if (!Valid(list) || ReadS16(base, page + pm::kPageCount) <= st::kPauseQuitItem) return;
  const uint32_t storage = ReadU32(base, list + 4 * st::kPauseStorageItem);
  const uint32_t quit = ReadU32(base, list + 4 * st::kPauseQuitItem);
  if (!Valid(storage) || !Valid(quit)) return;
  if (init) {
    const uint32_t a = ReadU32(base, storage + pm::kEntryLabel);
    const uint32_t b = ReadU32(base, quit + pm::kEntryLabel);
    if (!Valid(a) || !Valid(b)) return;
    WriteU32(base, storage + pm::kEntryLabel, b);
    WriteU32(base, quit + pm::kEntryLabel, a);
    SetEntryLabelKey(ctx, base, quit, cod3::videomenu::QuitToMenuKey());
    SetEntryLabelKey(ctx, base, storage, cod3::videomenu::QuitToDesktopKey());
    VCall(ctx, base, quit, pm::kEntrySetVisible, 1);
    VCall(ctx, base, quit, pm::kEntrySetDisabled, 0);
    std::lock_guard lock(g_pause_mutex);
    if (std::find(g_pause_pages.begin(), g_pause_pages.end(), page) == g_pause_pages.end())
      g_pause_pages.push_back(page);
  }
  if (!HasQuitRows(page)) return;
  // The game enables or disables the storage item every frame.
  if (ReadU8(base, storage + pm::kEntryDisabled)) VCall(ctx, base, storage, pm::kEntrySetDisabled, 0);
  VCall(ctx, base, storage, pm::kEntrySetVisible, 1);
}

// The rows top to bottom: QUIT TO DESKTOP (item 5) sits under QUIT (item 6).
constexpr int kPauseRowOrder[] = {0, 1, 2, 3, 4, st::kPauseQuitItem, st::kPauseStorageItem};

bool EntryUsable(uint8_t* base, uint32_t entry) {
  if (!Valid(entry) || ReadU8(base, entry + pm::kEntryDisabled)) return false;
  const uint32_t label = ReadU32(base, entry + pm::kEntryLabel);
  return Valid(label) && (ReadU8(base, label + pm::kTextVisibility) & 0x4);
}

// Up / down in the rows' order on screen, wrapping at both ends like the
// game's own pause menu, skipping disabled rows. False: not ours to move.
bool PauseMove(PPCContext& ctx, uint8_t* base, uint32_t page, bool down) {
  if (!HasQuitRows(page)) return false;
  if (PageLocked(base, page)) return true;  // the game ignores moves while the highlight animates
  const uint32_t list = ReadU32(base, page + pm::kPageItems);
  if (!Valid(list)) return false;
  constexpr int n = int(sizeof(kPauseRowOrder) / sizeof(kPauseRowOrder[0]));
  const int sel = Selected(base, page);
  int pos = 0;
  for (int i = 0; i < n; ++i)
    if (kPauseRowOrder[i] == sel) pos = i;
  for (int step = 1; step < n; ++step) {
    const int i = kPauseRowOrder[(pos + (down ? step : n - step)) % n];
    if (!EntryUsable(base, ReadU32(base, list + 4 * uint32_t(i)))) continue;
    VCall(ctx, base, page, pm::kPageSetSelection, uint64_t(i), 1);
    VCall(ctx, base, page, pm::kPageSelectionChanged);
    return true;
  }
  return true;
}

// ---------------------------------------------------------------------------
// QUIT TO DESKTOP asks first, in the game's own Yes / No message box: the
// pause menu's "quit without saving?" box, or from the main menu the same
// kind of box with "Quit the game and go back to the desktop?". Yes runs the
// pause box's Yes callback (kPauseQuitYes, hooked below), which then saves
// the settings and ends the program like the window's close button.
// g_quit_to_desktop says the box on screen is one of these; any other box
// clears it when it opens.
// ---------------------------------------------------------------------------
bool g_opening_desktop_box = false;  // game thread: our box is being opened

void QuitToDesktopFromPause(PPCContext& ctx, uint8_t* base, uint32_t page) {
  g_quit_to_desktop.store(true, std::memory_order_relaxed);
  g_opening_desktop_box = true;
  Call c(ctx, base);
  c.Invoke(st::kPauseQuit, {page});
  g_opening_desktop_box = false;
}

// The main menu's box, made with the steps of the pause menu's quit box
// (0x824DC7F8): message, Yes, No (highlighted), Open.
bool QuitToDesktopFromMainMenu(PPCContext& ctx, uint8_t* base) {
  namespace box = cod3::addr::msgbox;
  namespace mm = cod3::addr::main_menu;
  const uint32_t player = ReadU32(base, box::kPlayer);
  if (player >= 4) return false;
  const uint32_t system = ReadU32(base, box::kSystems + 4 * player);
  if (!Valid(system) || ReadU32(base, system) != box::kSystemVtable) return false;
  const uint32_t pages = ReadU32(base, system + box::kSysPages);
  const uint32_t buttons = ReadU32(base, system + box::kSysButtons);
  const uint32_t text = cod3::videomenu::Localize(base, cod3::videomenu::QuitToDesktopQuestionKey());
  if (!Valid(pages) || !Valid(buttons) || !text) return false;
  Call c(ctx, base);
  c.Invoke(box::kClose, {system});
  c.Invoke(box::kSetMessage, {system, text});
  const uint32_t mode = ReadU32(base, system + box::kSysMode);
  const bool page_two = mode < 4 && ReadU32(base, system + box::kSysHasPageTwo + 4 * mode) != 0;
  const uint32_t page = ReadU32(base, pages + (page_two ? 4 : 0));
  if (!Valid(page) || ReadU32(base, page) != box::kPageVtable) {
    c.Invoke(box::kClose, {system});
    return false;
  }
  g_quit_to_desktop.store(true, std::memory_order_relaxed);
  g_opening_desktop_box = true;
  c.Invoke(box::kAddButton, {page, mm::kYesKey, st::kPauseQuitYes});
  c.Invoke(box::kAddButton, {page, mm::kNoKey, mm::kNoCallback});
  WriteU32(base, page + box::kPageBack, 0);  // Back closes the box
  WriteU16(base, page + pm::kPageSelected, 1);  // No
  c.Invoke(box::kUpdateSelection, {buttons, 1});
  c.Invoke(box::kOpen, {page, 1, 0});
  g_opening_desktop_box = false;
  return true;
}

// ---------------------------------------------------------------------------
// Main menu: XBOX LIVE MARKETPLACE becomes QUIT TO TITLE SCREEN, and a sixth
// row under it is QUIT TO DESKTOP.
//  * The page's five-slot item array is replaced by a six-slot one (FEMenu
//    walks `count` slots and has no other capacity; the item handler ignores
//    items above 4, so ours are handled before it).
//  * Row six is a copy of row five's label, band and line one row pitch
//    lower; the copies follow row five every frame (the panel animates it).
//  * What is drawn under the rows (SIGNED IN and the footer) moves down by
//    the same pitch while it is drawn, so nothing overlaps.
//  * QUIT TO TITLE SCREEN does what B does on this page (the game's own way
//    back to "Press Start"); QUIT TO DESKTOP asks first (above).
// Other language layouts get the same rows (the labels are ours, English).
// ---------------------------------------------------------------------------
namespace mm = cod3::addr::main_menu;
constexpr int kQuitToTitleItem = mm::kMarketplaceItem;  // 4
constexpr int kQuitToDesktopItem = mm::kItemCount;      // 5

struct MainMenuRows {
  uint32_t panel = 0;
  uint32_t label5 = 0, label6 = 0;  // mm_text_option_05 and its copy
  uint32_t band5 = 0, band6 = 0;    // mm_bkg_btn_back_05 and its copy
  uint32_t line4 = 0, line5 = 0;    // mm_bkg_btn_line_04 and its copy
  uint32_t profile = 0;             // text_profile ("SIGNED IN: ...")
  float pitch = 0.0f;               // row five's distance below row four
};
std::mutex g_main_mutex;
std::unordered_map<uint32_t, MainMenuRows> g_main_menus;  // by page

bool GetMainMenu(uint32_t page, MainMenuRows* out) {
  std::lock_guard lock(g_main_mutex);
  auto it = g_main_menus.find(page);
  if (it == g_main_menus.end()) return false;
  *out = it->second;
  return true;
}

void SetUpMainMenu(PPCContext& ctx, uint8_t* base, uint32_t page) {
  namespace fe = cod3::addr::fe;
  if (!Valid(page) || ReadU32(base, page) != mm::kVtable) return;
  if (ReadS16(base, page + pm::kPageCount) != mm::kItemCount) return;
  const uint32_t old_items = ReadU32(base, page + pm::kPageItems);
  const uint32_t panel = ReadU32(base, page + mm::kPanel);
  if (!Valid(old_items) || !Valid(panel)) return;
  MainMenuRows rows;
  rows.panel = panel;
  rows.label5 = FindText(ctx, base, panel, "mm_text_option_05");
  const uint32_t label4 = FindText(ctx, base, panel, "mm_text_option_04");
  rows.band5 = FindQuad(ctx, base, panel, "mm_bkg_btn_back_05");
  rows.line4 = FindQuad(ctx, base, panel, "mm_bkg_btn_line_04");
  rows.profile = FindText(ctx, base, panel, "text_profile");
  const uint32_t entry5 = ReadU32(base, old_items + 4 * kQuitToTitleItem);
  if (!rows.label5 || !label4 || !Valid(entry5) || ReadU32(base, entry5 + pm::kEntryLabel) != rows.label5) {
    REXLOG_WARN("cod3: main menu layout not as expected; its quit items are not added");
    return;
  }
  rows.pitch = ReadF32(base, rows.label5 + fe::kTextY) - ReadF32(base, label4 + fe::kTextY);
  if (!(rows.pitch > 5.0f && rows.pitch < 60.0f)) {
    REXLOG_WARN("cod3: main menu rows {} apart; its quit items are not added", rows.pitch);
    return;
  }
  rows.label6 = CopyElement(ctx, base, rows.label5, false);
  rows.band6 = CopyElement(ctx, base, rows.band5, true);
  rows.line5 = CopyElement(ctx, base, rows.line4, true);
  if (!rows.label6) return;
  FollowElement(base, rows.label5, rows.label6, false, rows.pitch);

  // Six item slots in place of five.
  uint32_t items = 0;
  {
    Call c(ctx, base);
    items = static_cast<uint32_t>(c.Invoke(fe::kOperatorNew, {4u * (mm::kItemCount + 1)}));
  }
  if (!Valid(items)) return;
  for (int i = 0; i <= mm::kItemCount; ++i)
    WriteU32(base, items + 4 * uint32_t(i), i < mm::kItemCount ? ReadU32(base, old_items + 4 * uint32_t(i)) : 0);
  WriteU32(base, page + pm::kPageItems, items);
  {
    Call c(ctx, base);
    c.Invoke(fe::kOperatorDelete, {old_items});
  }
  VCall(ctx, base, page, fe::kPageAddItem, kQuitToDesktopItem, rows.label6);
  const uint32_t entry6 = ReadU32(base, items + 4 * kQuitToDesktopItem);
  if (!Valid(entry6)) {
    REXLOG_WARN("cod3: main menu refused its QUIT TO DESKTOP item");
    return;  // the count stays 5: the page never reads the sixth slot
  }
  SetEntryLabelKey(ctx, base, entry5, cod3::videomenu::QuitToTitleKey());
  SetEntryLabelKey(ctx, base, entry6, cod3::videomenu::QuitToDesktopKey());
  WriteU16(base, page + pm::kPageCount, uint16_t(mm::kItemCount + 1));
  REXLOG_INFO("cod3: main menu: QUIT TO TITLE SCREEN and QUIT TO DESKTOP in place of XBOX LIVE "
              "MARKETPLACE (rows {:.0f} apart)",
              rows.pitch);
  std::lock_guard lock(g_main_mutex);
  g_main_menus[page] = rows;
}

// SIGNED IN and the footer sit under the rows: while the page draws they are
// `dy` lower (the footer's lines too, which its layout placed).
void ShiftUnderRows(uint8_t* base, uint32_t page, const MainMenuRows& rows, float dy) {
  using cod3::guest::WriteF32;
  if (rows.profile) WriteF32(base, rows.profile + pm::kTextY, ReadF32(base, rows.profile + pm::kTextY) + dy);
  const uint32_t footer = ReadU32(base, page + mm::kFooter);
  if (!Valid(footer) || ReadU32(base, footer) != pm::kMultiLineTextVtable) return;
  WriteF32(base, footer + pm::kTextY, ReadF32(base, footer + pm::kTextY) + dy);
  const int32_t n = int32_t(ReadU32(base, footer + pm::kTextLineCount));
  const uint32_t lines = ReadU32(base, footer + pm::kTextLines);
  if (n <= 0 || n > 16 || !Valid(lines)) return;
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t y = lines + pm::kLineSize * uint32_t(i) + pm::kLineY;
    WriteF32(base, y, ReadF32(base, y) + dy);
  }
}

#endif  // !COD3_MP: the campaign's pause menu, quit boxes and main menu

// ---------------------------------------------------------------------------
// Panel dump (cod3_menu_trace): every picture and text of a page's panel,
// once, with where it is (for laying out added rows and for bug reports).
// ---------------------------------------------------------------------------
void DumpPanel(uint8_t* base, uint32_t panel, const char* what) {
  static std::vector<uint32_t> done;
  if (!REXCVAR_GET(cod3_menu_trace) || !Valid(panel)) return;
  if (std::find(done.begin(), done.end(), panel) != done.end() || done.size() > 64) return;
  done.push_back(panel);
  namespace fe = cod3::addr::fe;
  const uint32_t quads = ReadU32(base, panel + 0), nq = ReadU32(base, panel + 8);
  const uint32_t texts = ReadU32(base, panel + 12), nt = ReadU32(base, panel + 20);
  REXLOG_INFO("cod3: menu trace: {} panel {:08X}: {} pictures, {} texts", what, panel, nq, nt);
  for (uint32_t i = 0; Valid(quads) && i < nq && i < 128; ++i) {
    const uint32_t q = ReadU32(base, quads + 4 * i);
    if (!Valid(q)) continue;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    const uint32_t parts = ReadU32(base, q + fe::kQuadParts), np = ReadU32(base, q + fe::kQuadPartCount);
    for (uint32_t p = 0; Valid(parts) && p < np && p < 8; ++p) {
      const uint32_t part = ReadU32(base, parts + 4 * p);
      for (uint32_t v = 0; Valid(part) && v < 4; ++v) {
        const uint32_t at = part + fe::kQuadPartVerts + v * fe::kQuadVertStride;
        x0 = std::min(x0, ReadF32(base, at)), x1 = std::max(x1, ReadF32(base, at));
        y0 = std::min(y0, ReadF32(base, at + 4)), y1 = std::max(y1, ReadF32(base, at + 4));
      }
    }
    REXLOG_INFO("cod3: menu trace:   picture {} {}: {:.0f},{:.0f}-{:.0f},{:.0f}{}",
                GuestString(base, ReadU32(base, q + fe::kQuadName), 64), i, x0, y0, x1, y1,
                ReadU8(base, q + pm::kTextVisibility) & 0x4 ? "" : " (hidden)");
  }
  for (uint32_t i = 0; Valid(texts) && i < nt && i < 128; ++i) {
    const uint32_t t = ReadU32(base, texts + 4 * i);
    if (!Valid(t) || !IsText(base, t)) continue;
    REXLOG_INFO("cod3: menu trace:   text {} \"{}\": {:.0f},{:.0f} align {} font {}{}",
                GuestString(base, ReadU32(base, t + pm::kTextName), 64),
                GuestString(base, ReadU32(base, t + pm::kTextString), 64), ReadF32(base, t + pm::kTextX),
                ReadF32(base, t + pm::kTextY), ReadU16(base, t + pm::kTextAlign),
                int32_t(ReadU32(base, t + pm::kTextFont)),
                ReadU8(base, t + pm::kTextVisibility) & 0x4 ? "" : " (hidden)");
  }
}

// ---------------------------------------------------------------------------
// [mp-menus] cod3_test_menu_input (tests only): the script's events go to the
// listener above as the window's would, pointer positions straight into the
// menus' space. Ticked on the UI thread (UpdateCursor).
// ---------------------------------------------------------------------------
struct TestEvent {
  double at = 0;
  std::string what, arg;
  bool done = false;
};
struct TestScript {
  std::vector<TestEvent> events;
  Clock::time_point start;
  rex::ui::VirtualKey held = rex::ui::VirtualKey::kNone;  // let go on the next tick
  bool left_held = false;
};
TestScript g_test;  // UI thread

void LoadTestScript() {
  const std::string script = REXCVAR_GET(cod3_test_menu_input);
  g_test = TestScript{};
  g_test.start = Clock::now();
  size_t at = 0;
  while (at < script.size()) {
    size_t end = script.find(';', at);
    if (end == std::string::npos) end = script.size();
    const std::string entry = script.substr(at, end - at);
    at = end + 1;
    const size_t colon = entry.find(':');
    if (colon == std::string::npos) continue;
    TestEvent e;
    e.at = std::atof(entry.substr(0, colon).c_str());
    std::string what = entry.substr(colon + 1);
    const size_t eq = what.find('=');
    if (eq != std::string::npos) e.arg = what.substr(eq + 1), what.resize(eq);
    for (char& c : what) c = char(std::tolower(static_cast<unsigned char>(c)));
    e.what = what;
    if (g_test.events.size() < 256) g_test.events.push_back(e);
  }
  if (!g_test.events.empty())
    REXLOG_INFO("cod3: menu test input: {} events (cod3_test_menu_input)", g_test.events.size());
}

void TestKey(rex::ui::Window* window, rex::ui::VirtualKey key, bool down) {
  rex::ui::KeyEvent e(window, key, 1, !down, false, false, false, false);
  if (down) {
    g_listener.OnKeyDown(e);
    cod3::prompts::NoteKeyboardUse();
  } else {
    g_listener.OnKeyUp(e);
  }
}

void TickTestScript(rex::ui::Window* window) {
  using rex::ui::VirtualKey;
  if (g_test.events.empty()) return;
  if (g_test.held != VirtualKey::kNone) TestKey(window, g_test.held, false), g_test.held = VirtualKey::kNone;
  if (g_test.left_held) {
    std::lock_guard lock(g_input.mutex);
    g_input.left_down = false;
    g_test.left_held = false;
  }
  const double now = std::chrono::duration<double>(Clock::now() - g_test.start).count();
  for (TestEvent& e : g_test.events) {
    if (e.done || e.at > now) continue;
    e.done = true;
    float x = 0.0f, y = 0.0f;
    const bool point = std::sscanf(e.arg.c_str(), "%f,%f", &x, &y) == 2;
    REXLOG_INFO("cod3: menu test input at {:.1f} s: {}{}{}", now, e.what, e.arg.empty() ? "" : " ", e.arg);
    if (e.what == "key") {
      std::string k = e.arg;
      for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
      VirtualKey key = k == "up" ? VirtualKey::kUp : k == "down" ? VirtualKey::kDown
                       : k == "left" ? VirtualKey::kLeft : k == "right" ? VirtualKey::kRight
                       : k == "enter" ? VirtualKey::kReturn : k == "escape" ? VirtualKey::kEscape
                       : k == "backspace" ? VirtualKey::kBack : VirtualKey::kNone;
      // [claude-9b] Number boxes: key=0 .. key=9, key=period, key=minus.
      if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0])))
        key = static_cast<VirtualKey>(uint16_t(VirtualKey::k0) + uint16_t(k[0] - '0'));
      else if (k == "period") key = VirtualKey::kOemPeriod;
      else if (k == "minus") key = VirtualKey::kOemMinus;
      if (key == VirtualKey::kNone) continue;
      TestKey(window, key, true);
      g_test.held = key;
    } else if ((e.what == "move" || e.what == "click") && point) {
      std::lock_guard lock(g_input.mutex);
      g_input.over = true;
      g_input.x = x, g_input.y = y;
      ++g_input.moves;
      if (e.what == "click" && cod3::pcmenu::MenuActive()) {
        Request r;
        r.ask = Ask::kClick;
        r.x = x, r.y = y;
        g_input.left_down = true;
        g_test.left_held = true;
        PushLocked(r);
      }
      cod3::prompts::NoteKeyboardUse();
    } else if (e.what == "rclick") {
      if (cod3::pcmenu::MenuActive()) Push({Ask::kButton, pm::kCodeB});
      cod3::prompts::NoteKeyboardUse();
    } else if (e.what == "wheel") {
      const int n = std::atoi(e.arg.c_str());
      for (int i = 0; i < std::abs(n) && i < 16 && cod3::pcmenu::MenuActive(); ++i)
        Push({Ask::kButton, n > 0 ? pm::kCodeUp : pm::kCodeDown});
      cod3::prompts::NoteKeyboardUse();
    }
  }
}

}  // namespace

namespace cod3::pcmenu {

void Attach(rex::ui::Window* window, size_t z_order, std::function<bool()> overlay_open) {
  if (!window) return;
  RefreshPictureAspect();
  g_listener.window = window;
  g_listener.overlay_open = std::move(overlay_open);
  window->AddInputListener(&g_listener, z_order);
  LoadTestScript();  // [mp-menus] tests only
}

void Detach(rex::ui::Window* window) {
  if (window && g_listener.window == window) window->RemoveInputListener(&g_listener);
  g_listener.window = nullptr;
}

bool MenuActive() {
  if (!REXCVAR_GET(cod3_pc_menus)) return false;
  const int64_t when = g_menu_time.load(std::memory_order_relaxed);
  return when != 0 && FrameNow() - g_menu_frame.load(std::memory_order_relaxed) <= 1 &&
         Now() - when < kLoadingLimit;
}

void SetQuitToDesktopHandler(std::function<void()> quit) {
  std::lock_guard lock(g_pause_mutex);
  g_quit_handler = std::move(quit);
}

void NoteLineDrawn(uint8_t* base, uint32_t line, double y_scale) {
  if (!REXCVAR_GET(cod3_pc_menus) || !t_drawing_system) return;
  NoteLine(base, line, float(y_scale));
}

bool FromPcInput(uint32_t code) {  // [menus-brightness]
  return code && (g_pressing_code == code || g_holding_code == code);
}

void UpdateCursor(rex::ui::Window* window) {
  if (!window || !REXCVAR_GET(cod3_pc_menus)) return;
  TickTestScript(window);  // [mp-menus] tests only (cod3_test_menu_input)
  RefreshPictureAspect();
  using V = rex::ui::Window::CursorVisibility;
  const bool overlay = g_listener.overlay_open && g_listener.overlay_open();
  const V want = (MenuActive() || overlay) ? V::kVisible : V::kAutoHidden;
  if (window->GetCursorVisibility() != want) window->SetCursorVisibility(want);
}

void OnProcessInput(PPCContext& ctx, uint8_t* base, uint32_t system) { OnMenuInput(ctx, base, system); }

uint32_t BeginSystemDraw(uint32_t system) {
  const uint32_t outer = t_drawing_system;
  t_drawing_system = system;
  return outer;
}

void EndSystemDraw(uint32_t outer) { t_drawing_system = outer; }

void NotePictureDrawn(uint8_t* base, uint32_t quad) {
  if (t_drawing_system && REXCVAR_GET(cod3_pc_menus)) NoteControl(base, quad);
}

void QuitToDesktopNow() { RunQuitHandler(); }

uint32_t FindPanelText(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  return Valid(panel) ? FindText(ctx, base, panel, name) : 0;
}

uint32_t FindPanelPicture(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  return Valid(panel) ? FindQuad(ctx, base, panel, name) : 0;
}

uint32_t CopyPanelElement(PPCContext& ctx, uint8_t* base, uint32_t source, bool picture) {
  return CopyElement(ctx, base, source, picture);
}

void FollowPanelElement(uint8_t* base, uint32_t source, uint32_t copy, bool picture, float dy) {
  FollowElement(base, source, copy, picture, dy);
}

void SetItemLabelKey(PPCContext& ctx, uint8_t* base, uint32_t entry, const char* key) {
  if (Valid(entry)) SetEntryLabelKey(ctx, base, entry, key);
}

void DumpMenuPanel(uint8_t* base, uint32_t panel, const char* what) { DumpPanel(base, panel, what); }

}  // namespace cod3::pcmenu

// FEMenuSystem::ProcessInput (vfunc_12; cod3mp: FEMenuSystem__ProcessInput,
// slot 17, the same code): the game reads the pad first, then the mouse and
// keyboard are applied to the page now on screen.
REX_EXTERN(__imp__FEMenuSystem__vfunc_12);
REX_HOOK_RAW(FEMenuSystem__vfunc_12) {
  const uint32_t system = ctx.r3.u32;
  __imp__FEMenuSystem__vfunc_12(ctx, base);
  OnMenuInput(ctx, base, system);
}

// FEMenuSystem::HandleButton (this, code, player): every menu button event,
// the pad's (from ProcessInput) and ours. Only watched, for cod3_menu_trace.
REX_EXTERN(__imp__FEMenuSystem__vfunc_17);
REX_HOOK_RAW(FEMenuSystem__vfunc_17) {
  if (REXCVAR_GET(cod3_menu_trace))
    REXLOG_INFO("cod3: menu trace: button {} (player {}, system {:08X})", ctx.r4.u32, int32_t(ctx.r5.u32),
                ReadU32(base, ctx.r3.u32));
  __imp__FEMenuSystem__vfunc_17(ctx, base);
}

#if !defined(COD3_MP)
// The campaign's own pages (multiplayer's: game/mp/menus_mp.cpp).

// PauseMenu::Init (this, panel): builds the seven items.
REX_EXTERN(__imp__sub_824E6640);
REX_HOOK_RAW(sub_824E6640) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824E6640(ctx, base);
  SetUpPauseQuitRows(ctx, base, page, /*init=*/true);
}

// The pause menu's item states (every refresh): enables items 4 and 5.
REX_EXTERN(__imp__sub_824CD768);
REX_HOOK_RAW(sub_824CD768) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824CD768(ctx, base);
  SetUpPauseQuitRows(ctx, base, page, /*init=*/false);
}

// The pause menu's up / down.
REX_EXTERN(__imp__sub_824C4390);
REX_HOOK_RAW(sub_824C4390) {
  if (!PauseMove(ctx, base, ctx.r3.u32, /*down=*/false)) __imp__sub_824C4390(ctx, base);
}

REX_EXTERN(__imp__sub_824C4338);
REX_HOOK_RAW(sub_824C4338) {
  if (!PauseMove(ctx, base, ctx.r3.u32, /*down=*/true)) __imp__sub_824C4338(ctx, base);
}

// The pause menu's item handler (this, item): QUIT TO DESKTOP opens the
// game's own quit box; Options opens its page with the game's options shown
// (not the video rows a previous visit may have left open).
REX_EXTERN(__imp__PauseMenu__vfunc_19);
REX_HOOK_RAW(PauseMenu__vfunc_19) {
  const uint32_t page = ctx.r3.u32;
  const int item = static_cast<int32_t>(ctx.r4.u32);
  if (item == 1) cod3::videomenu::ResetPauseOptions();
  if (HasQuitRows(page) && item == st::kPauseStorageItem) {
    QuitToDesktopFromPause(ctx, base, page);
    return;
  }
  __imp__PauseMenu__vfunc_19(ctx, base);
}

// The quit box's Yes: back to the main menu, or, from QUIT TO DESKTOP, save
// the settings and end the program (Cod3App, on the UI thread).
REX_EXTERN(__imp__sub_824DC798);
REX_HOOK_RAW(sub_824DC798) {
  if (g_quit_to_desktop.exchange(false, std::memory_order_relaxed)) {
    std::function<void()> quit;
    {
      std::lock_guard lock(g_pause_mutex);
      quit = g_quit_handler;
    }
    if (quit) {
      REXLOG_INFO("cod3: quit to desktop (confirmed in the game's message box)");
      quit();
      ctx.r3.u64 = 1;
      return;
    }
  }
  __imp__sub_824DC798(ctx, base);
}

// MainMenu::Init (this, panel): the five items, then ours (see above).
REX_EXTERN(__imp__sub_824E56B8);
REX_HOOK_RAW(sub_824E56B8) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824E56B8(ctx, base);
  SetUpMainMenu(ctx, base, page);
}

REX_EXTERN(__imp__MainMenu__dtor);
REX_HOOK_RAW(MainMenu__dtor) {
  {
    std::lock_guard lock(g_main_mutex);
    g_main_menus.erase(ctx.r3.u32);
  }
  __imp__MainMenu__dtor(ctx, base);
}

// MainMenu::Update: the game greys item 4 (the marketplace) without an Xbox
// Live sign-in; QUIT TO TITLE SCREEN stays usable.
REX_EXTERN(__imp__sub_824DBEE8);
REX_HOOK_RAW(sub_824DBEE8) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824DBEE8(ctx, base);
  MainMenuRows rows;
  if (!GetMainMenu(page, &rows)) return;
  const uint32_t list = ReadU32(base, page + pm::kPageItems);
  const uint32_t entry = Valid(list) ? ReadU32(base, list + 4 * kQuitToTitleItem) : 0;
  if (Valid(entry) && ReadU8(base, entry + pm::kEntryDisabled)) VCall(ctx, base, entry, pm::kEntrySetDisabled, 0);
}

// MainMenu's item handler (this, item): QUIT TO TITLE SCREEN and QUIT TO
// DESKTOP; the others as before. The same early outs as the game's.
REX_EXTERN(__imp__MainMenu__vfunc_18);
REX_HOOK_RAW(MainMenu__vfunc_18) {
  const uint32_t page = ctx.r3.u32;
  const int item = static_cast<int32_t>(ctx.r4.u32);
  MainMenuRows rows;
  if (GetMainMenu(page, &rows) && (item == kQuitToTitleItem || item == kQuitToDesktopItem)) {
    if (!ReadU32(base, page + mm::kPanel) || ReadU8(base, page + mm::kBusy)) return;
    WriteU32(base, page + mm::kLastItem, uint32_t(item));  // highlighted when the page opens again
    if (item == kQuitToTitleItem) {
      REXLOG_INFO("cod3: main menu: quit to the title screen");
      VCall(ctx, base, page, pm::kPageBack);  // what B does here
    } else if (!QuitToDesktopFromMainMenu(ctx, base)) {
      REXLOG_WARN("cod3: main menu: no message box; quitting to the desktop without asking");
      std::function<void()> quit;
      {
        std::lock_guard lock(g_pause_mutex);
        quit = g_quit_handler;
      }
      if (quit) quit();
    }
    return;
  }
  __imp__MainMenu__vfunc_18(ctx, base);
}

// MainMenu's draw (panel, items, footer): row six follows row five, and
// what is under the rows moves down a row while it is drawn.
REX_EXTERN(__imp__MainMenu__vfunc_14);
REX_HOOK_RAW(MainMenu__vfunc_14) {
  const uint32_t page = ctx.r3.u32;
  MainMenuRows rows;
  if (!GetMainMenu(page, &rows)) {
    __imp__MainMenu__vfunc_14(ctx, base);
    return;
  }
  DumpPanel(base, rows.panel, "main menu");
  FollowElement(base, rows.label5, rows.label6, false, rows.pitch);
  FollowElement(base, rows.band5, rows.band6, true, rows.pitch);
  FollowElement(base, rows.line4, rows.line5, true, rows.pitch);
  ShiftUnderRows(base, page, rows, rows.pitch);
  __imp__MainMenu__vfunc_14(ctx, base);
  ShiftUnderRows(base, page, rows, -rows.pitch);
}

// Any panel's draw (pictures, then texts): the main menu's row six band and
// line go right after its panel, under the items.
REX_EXTERN(__imp__sub_824BFA70);
REX_HOOK_RAW(sub_824BFA70) {
  const uint32_t panel = ctx.r3.u32;
  __imp__sub_824BFA70(ctx, base);
  std::vector<uint32_t> extra;
  {
    std::lock_guard lock(g_main_mutex);
    if (g_main_menus.empty()) return;
    for (const auto& [page, rows] : g_main_menus)
      if (rows.panel == panel) extra = {rows.line5, rows.band6};
  }
  for (uint32_t e : extra)
    if (Valid(e) && (ReadU8(base, e + pm::kTextVisibility) & 0x4)) VCall(ctx, base, e, cod3::addr::fe::kElemDraw);
}

#endif  // !COD3_MP

// The menu systems' draws: footer lines drawn inside belong to that system.
// (cod3mp: slot 19 of each; its after-action report system's draw is hooked
// in game/mp/menus_mp.cpp.)
REX_EXTERN(__imp__FrontEndMenuSystem__vfunc_14);
REX_HOOK_RAW(FrontEndMenuSystem__vfunc_14) {
  const uint32_t outer = t_drawing_system;
  t_drawing_system = ctx.r3.u32;
  __imp__FrontEndMenuSystem__vfunc_14(ctx, base);
  t_drawing_system = outer;
}

REX_EXTERN(__imp__InGameMenuSystem__vfunc_14);
REX_HOOK_RAW(InGameMenuSystem__vfunc_14) {
  const uint32_t outer = t_drawing_system;
  t_drawing_system = ctx.r3.u32;
  __imp__InGameMenuSystem__vfunc_14(ctx, base);
  t_drawing_system = outer;
}

REX_EXTERN(__imp__DialogMenuSystem__vfunc_14);
REX_HOOK_RAW(DialogMenuSystem__vfunc_14) {
  const uint32_t outer = t_drawing_system;
  t_drawing_system = ctx.r3.u32;
  __imp__DialogMenuSystem__vfunc_14(ctx, base);
  t_drawing_system = outer;
}

// [menus-brightness] The picture draw (PanelQuad::Draw, this): arrows and
// gauges drawn by a menu are noted for the mouse.
REX_EXTERN(__imp__PanelQuad__vfunc_1);
REX_HOOK_RAW(PanelQuad__vfunc_1) {
  const uint32_t quad = ctx.r3.u32;
  __imp__PanelQuad__vfunc_1(ctx, base);
  if (t_drawing_system && REXCVAR_GET(cod3_pc_menus)) NoteControl(base, quad);
}

#if !defined(COD3_MP)
// [menus-brightness] Options > SCREEN's OnLeft / OnRight (this): the page
// only flashes its arrows here and moves the brightness in its draw from the
// pad's D-pad and stick, which the mouse and keys don't reach. A press from
// them (an arrow clicked, an arrow key, also held) moves it by the D-pad's
// step here; the pad's own presses are left to the page.
REX_EXTERN(__imp__sub_824BC2C0);
REX_HOOK_RAW(sub_824BC2C0) {
  if (ReadU32(base, ctx.r3.u32) == cod3::addr::brightness::kPageVtable && cod3::pcmenu::FromPcInput(pm::kCodeLeft))
    cod3::videomenu::StepBrightness(base, false, "Options > SCREEN");
  __imp__sub_824BC2C0(ctx, base);
}

REX_EXTERN(__imp__sub_824BC248);
REX_HOOK_RAW(sub_824BC248) {
  if (ReadU32(base, ctx.r3.u32) == cod3::addr::brightness::kPageVtable && cod3::pcmenu::FromPcInput(pm::kCodeRight))
    cod3::videomenu::StepBrightness(base, true, "Options > SCREEN");
  __imp__sub_824BC248(ctx, base);
}

#endif  // !COD3_MP

// Dialog AddButton(page, key, callback): no "Select Storage Device" button.
REX_EXTERN(__imp__sub_824C4428);
REX_HOOK_RAW(sub_824C4428) {
  const uint32_t key = ctx.r4.u32;
  if (key == st::kSelectDeviceKey || key == st::kFailedSelectKey) {
    REXLOG_INFO("cod3: message box without its \"{}\" button (no storage devices here)",
                cod3::guest::ReadString(base, key, 64));
    return;
  }
  __imp__sub_824C4428(ctx, base);
}

#if !defined(COD3_MP)
// (cod3mp's Open takes the system: game/mp/menus_mp.cpp.)
// Dialog Open(page, 1, 0): a box that preselected its storage button (gone
// now) highlights its last button instead (the drawn highlight too). A box
// that isn't a QUIT TO DESKTOP box clears that mark.
REX_EXTERN(__imp__sub_824CDB28);
REX_HOOK_RAW(sub_824CDB28) {
  const uint32_t page = ctx.r3.u32;
  if (!g_opening_desktop_box) g_quit_to_desktop.store(false, std::memory_order_relaxed);
  if (Valid(page) && ReadU32(base, page) == cod3::addr::msgbox::kPageVtable) {
    const int32_t buttons = static_cast<int32_t>(ReadU32(base, page + st::kDialogButtonCount));
    if (buttons > 0 && buttons <= 8 && Selected(base, page) >= buttons) {
      WriteU16(base, page + pm::kPageSelected, uint16_t(buttons - 1));
      const uint32_t list = DialogButtons(base, ReadU32(base, page + cod3::addr::fe::kPageSystem));
      if (list) {
        Call c(ctx, base);
        c.Invoke(cod3::addr::msgbox::kUpdateSelection, {list, uint64_t(buttons - 1)});
      }
    }
  }
  __imp__sub_824CDB28(ctx, base);
}
#endif  // !COD3_MP
