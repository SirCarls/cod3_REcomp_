// cod3 - ultrawide (Hor+) support.
//
// Call of Duty 3 already widens its view for 16:9 the way later Call of Duty
// games do ("Hor+"): cg_fov is the horizontal field of view of a 4:3 screen,
// CG_CalcFov keeps the vertical angle that gives, and the renderer builds the
// projection from that vertical angle and an aspect ratio (16:9 when the
// console reports a widescreen TV). So a wider picture only needs a wider
// aspect ratio in that one place: the view up and down stays exactly the
// game's, and the sides show more of the world.
//
// The HUD, the menus and the movies are drawn in the game's 640x480 virtual
// screen, which covers the whole picture, so on a wider picture they would
// stretch. Here everything 2D the game draws outside the 3D view is moved
// into the picture's centered 16:9 part instead. During play, 2D that
// reaches the screen's left or right edge (fades, flashes, damage and scope
// overlays, cutscene bars) keeps reaching the picture's edges. Movies,
// briefings, loading screens and the menus get black bars at the sides.
//
// The shape follows the window live: resizing the window or changing the
// setting changes the 3D view, the HUD and the picture's shape at once, and
// a moment later the game's own render targets are remade for the new shape,
// pixel for pixel (a live resolution change, game/render_resolution.h).
//
// Addresses and the reasoning: game/addresses.h (namespace ultrawide).

#pragma once

#include <cstdint>
#include <string>

namespace cod3 {

inline constexpr float kSixteenByNine = 16.0f / 9.0f;
// The widest picture: three 16:9 screens side by side. Wider windows get
// black bars at the sides.
inline constexpr float kMaxAspect = 48.0f / 9.0f;

// The picture's shape (width / height) for a window or screen of this size
// and the cod3_aspect_ratio setting: the window's own shape for "auto" (never
// narrower than the game's 16:9, which then gets black bars), else the chosen
// shape.
float AspectForWindow(uint32_t width, uint32_t height);

// A picture shape of the player's own for cod3_aspect_ratio: "W:H" (24:10,
// 2.39:1) or a number (2.39); kept between 16:9 and kMaxAspect. False when
// the text isn't one.
bool ParseAspect(const std::string& text, float* aspect);

// Called every UI frame with the window's size in pixels (and once at start):
// applies the shape for that size to the 3D view, the HUD and the presenter.
void UpdateDisplayAspect(uint32_t window_width, uint32_t window_height);

// The shape the game draws with now; 16/9 is the game's own and every hook
// passes through.
float DisplayAspect();

// At start, before the window opens: drop a guest display size the port set
// in an earlier session and the settings window saved (it would also set the
// window's size).
void ForgetSavedDisplayShape();

}  // namespace cod3
