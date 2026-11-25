// cod3 - [waw-decomp/menus2] 2026-11-23 Native in-game menus for Call of Duty 3, both programs, on menukit
// (src/menukit/menukit.h, the core every port shares): the CoD3 adapter, the takeover of Pause > Options and the
// drawer of its two looks.
//
// cod3_native_menus (off by default) hands the game's Pause > Options page (the campaign's AdvancedOptionsMenu,
// cod3mp's InGameOptionsMenu) to menukit while it is up. The game still opens and closes the page (pause, Start,
// the end of a match); while it is up its draw is skipped and ours is drawn (cod3_menu_style: modern, left-aligned
// with a category list, or original, the game's one page at a time), and its buttons come to us first. Back on the
// top level goes on to the game's own Back, so leaving the page is the game's. The rows are
// assets/menus/cod3/pause_options.toml (built into the program, cod3_menus_data.inc.in); each row is one of the
// port's existing row functions (game/menu_port_rows.h, menu_video.h, menu_controller.h, menu_aim.h,
// movement/menu_movement.h, fov.h), the same ones the game's own Options pages and F2 change. Split screen: one page
// per local player, each with its own rows' values, drawn in that player's view.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace cod3::nativemenus {

// Game thread: pc_menu.cpp's FEMenuSystem::HandleButton hook (this, code, player) asks first. True when our page is
// up on that menu system and took the button, so the game's page under it never sees it. Back on our top level and
// Start are left to the game (false).
bool TakeButton(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t code, int32_t player);

// Game thread: pc_menu.cpp's menu input, once a frame for the system the keyboard and mouse use. (x, y) is the
// pointer in the menus' 640x480 space over the whole picture, `over` false when it is outside the picture, `moved`
// when it moved this frame, `click` when the left button was clicked at (x, y). True when our page is up on that
// system: the pointer is ours then, and the game's pointer code leaves its page alone.
bool TakePointer(PPCContext& ctx, uint8_t* base, uint32_t system, float x, float y, bool over, bool moved,
                 bool click);

// Game thread: the in-game menu system's draw hook (pc_menu.cpp) asks first. True when our page is up on that system
// and was laid out for this frame (the overlay draws it, DrawOverlay), so the hook skips the game's own draw.
bool DrawSystem(PPCContext& ctx, uint8_t* base, uint32_t system);

// UI thread, inside the overlay's ImGui frame (ui/hud_overlay.cpp, HudOverlay::OnDraw): draws every page laid out
// in the last quarter second, each in its player's view, in the chosen look.
void DrawOverlay();

}  // namespace cod3::nativemenus
