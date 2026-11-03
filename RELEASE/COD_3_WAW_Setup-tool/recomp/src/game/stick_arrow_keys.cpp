// cod3 - [claude-9b-think] 2026-10-27, applied by claude-9b 12:35 (owner 12:28: prompts): the battle actions' stick picture with the
// keyboard and mouse (owner 10:27: "glyphs or hints with right stick or left stick either words or icon are still
// showing when using keyboard and mouse"). Campaign only: multiplayer has no battle actions and no such widget
// (game/mp/campaign_stubs.cpp; cod3mp's strings have no "~rstick"). A new file, so neither game/button_prompts.cpp
// (on hold) nor game/qte_actions.cpp (claude-setup's) changes; add it to the campaign's sources only.
//
// IGOStickArrowWidget (vtable 0x820715F4; the one at [[g_FEManager 0x82A8DF2C] + 20] + 76, made by the in-game front
// end's setup 0x824EAA80) shows a stick battle action's arrows, the page elements BA_arrow_full_a / _full_b /
// _half_b / _up_a / _up_b (its Init 0x824E0038), and in a FEMultiLineText of its own (+36, made by its ctor
// 0x824E01E8 in font 2) the token "~rstick" (0x8206F398): the right stick's picture from the button font. Show
// 0x824C0EB0 (from the Start of StickSwirl, StickToggleHoriz, StickToggleVert and Rowboat) sets it with slot 132;
// Hide 0x824C0E08 clears it. game/qte_actions.cpp holds Show back while its spinner shows (cod3_qte_style spinner
// and the mouse driving, or cod3_qte_spinner_with_pad), so with the keyboard and mouse the widget still shows with
// cod3_qte_style original, when an action starts with the pad driving, and for Show type 6 (not held back). Then
// the token is laid out as the right stick's click key (game/button_prompts.cpp names the RS picture by
// keybind_rstick_press or the port's action on it): a key cap for a button the action doesn't use.
//
// Here: the widget's Update (vfunc_2 0x824C8FA8, each frame) sets that text to a MOUSE key cap while the first
// player's prompts show keys (cod3::prompts::FirstPlayerLook) and back to the game's "~rstick" when they show a
// controller's buttons, only when that changes. The mouse is what moves the stick in these actions
// (game/qte_actions.h), and the hint line above already says so ("Circle the mouse clockwise."). The arrows stay:
// they show the way to move for either device.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/button_prompts.h"
#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_stick_arrow_keys, true, "cod3/Battle actions",
                    "With the keyboard and mouse, the game's stick arrow (cod3_qte_style original) shows a MOUSE key "
                    "under its arrows instead of the right stick's picture")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;

constexpr uint32_t kWidgetActive = 4;     // u8: shown (vfunc 20 sets it)
constexpr uint32_t kWidgetText = 36;      // FEMultiLineText*: the stick picture
constexpr uint32_t kWidgetType = 48;      // s32: Show's type, 0 = none
constexpr uint32_t kSetTextKey = 132;     // the text's slot Show and Hide call (const char* key or text)
constexpr uint32_t kStickToken = 0x8206F398;  // "~rstick", the game's own string
// game/button_prompts.cpp's key token ("~" 0x02 NAME 0x02, its KeyToken): the token pass lays it out as the name, and
// with cod3_key_caps the overlay draws it as a key cap.
constexpr char kMouseToken[] = "~\x02" "MOUSE" "\x02";

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

// [claude-9b] The mouse token in guest memory of its own, made once: the text's slot may keep the pointer it is
// given (as for the game's own "~rstick"), so a string on a call's stack could be gone by the time it is laid out.
uint32_t MouseTokenText(uint8_t* base) {
  static uint32_t text = 0;
  if (!text) {
    text = rex::system::kernel_state()->memory()->SystemHeapAlloc(16);
    if (text) std::memcpy(base + text, kMouseToken, sizeof(kMouseToken));
  }
  return text;
}

// What the shown widget's text is: 0 the game's "~rstick" (as Show set it), 1 the mouse key. A new Show (another
// type) sets "~rstick" again: start over. Game thread.
struct Shown {
  uint32_t widget = 0;
  int32_t type = 0;
  int keys = 0;
};
Shown g_shown;

}  // namespace

// IGOStickArrowWidget::Update(this, f1 seconds): the arrows' pulse; the text's device swap after it.
REX_EXTERN(__imp__IGOStickArrowWidget__vfunc_2);
REX_HOOK_RAW(IGOStickArrowWidget__vfunc_2) {
  const uint32_t widget = ctx.r3.u32;
  __imp__IGOStickArrowWidget__vfunc_2(ctx, base);
  if (!Valid(widget)) return;
  const uint32_t text = g::ReadU32(base, widget + kWidgetText);
  const int32_t type = g::ReadS32(base, widget + kWidgetType);
  if (!Valid(text) || type == 0 || g::ReadU8(base, widget + kWidgetActive) == 0) {
    if (g_shown.widget == widget) g_shown = Shown{};  // hidden: Hide cleared the text
    return;
  }
  if (g_shown.widget != widget || g_shown.type != type) g_shown = Shown{widget, type, 0};  // Show set "~rstick"
  const int want = REXCVAR_GET(cod3_stick_arrow_keys) &&
                           cod3::prompts::FirstPlayerLook() == cod3::prompts::Look::kKeys
                       ? 1
                       : 0;
  if (want == g_shown.keys) return;
  const uint32_t key = want ? MouseTokenText(base) : kStickToken;
  if (!key) return;
  g::Call c(ctx, base);
  c.Invoke(g::VirtualSlot(base, text, kSetTextKey), {text, key});
  g_shown.keys = want;
  REXLOG_INFO("cod3: battle action stick arrow (type {}) shows {}", type, want ? "the mouse key" : "the right stick");
}
