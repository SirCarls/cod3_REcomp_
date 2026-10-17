// cod3 - picture shape: ultrawide (Hor+) and narrower shapes.
//
// Call of Duty 3 already widens its view for 16:9 the way later Call of Duty
// games do ("Hor+"): cg_fov is the horizontal field of view of a 4:3 screen,
// CG_CalcFov keeps the vertical angle that gives, and the renderer builds the
// projection from that vertical angle and an aspect ratio (16:9 when the
// console reports a widescreen TV). So another picture shape only needs
// another aspect ratio in that one place: the view up and down stays exactly
// the game's, and the sides show more (wider) or less (narrower, down to the
// console's own 4:3 view) of the world.
//
// The HUD, the menus and the movies are drawn in the game's 640x480 virtual
// screen, which covers the whole picture, so on a wider picture they would
// stretch. Here everything 2D the game draws outside the 3D view is moved
// into the picture's centered 16:9 part instead. During play, 2D that
// reaches the screen's left or right edge (fades, flashes, damage and scope
// overlays, cutscene bars) keeps reaching the picture's edges. Movies,
// briefings, loading screens and the menus get black bars at the sides. On
// a narrower picture (4:3, 16:10) the 640x480 screen simply fills it, as on
// a 4:3 TV.
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
// The narrowest picture: the console's own 4:3 view (narrower shapes would
// cut the vertical view the game keeps, and the game would no longer see a
// widescreen display).
inline constexpr float kMinAspect = 4.0f / 3.0f;
// The widest picture: three 16:9 screens side by side. Wider windows get
// black bars at the sides.
inline constexpr float kMaxAspect = 48.0f / 9.0f;

// cod3_aspect_ratio's value for "Match the render resolution": the shape of
// cod3_render_resolution's size (a typed WxH; the named sizes are 16:9;
// "auto" draws the window or screen, so its shape).
inline constexpr const char* kMatchResolution = "resolution";

// Shapes within this of 16:9 count as 16:9.
inline constexpr float kSameShape = 0.002f;
inline bool IsSixteenByNine(float aspect) {
  return aspect > kSixteenByNine - kSameShape && aspect < kSixteenByNine + kSameShape;
}

// The picture's shape (width / height) for a window or screen of this size
// and the cod3_aspect_ratio setting: the window's own shape for "auto" (never
// narrower than the game's 16:9, which then gets black bars), the render
// resolution's for kMatchResolution, else the chosen shape.
float AspectForWindow(uint32_t width, uint32_t height);

// A picture shape as written: "W:H" (24:10, 2.39:1) or a number (2.39),
// whatever its value. False when the text isn't one.
bool ParseAspectText(const std::string& text, float* aspect);
// The same, kept between kMinAspect and kMaxAspect: what the game uses for a
// shape of the player's own in cod3_aspect_ratio.
bool ParseAspect(const std::string& text, float* aspect);

// The guest display size the port gives the presenter for a shape (it
// letterboxes to it): `ax` x `ay` is 16 x 9 for the game's own 16:9, else
// about 1000 x shape by 1000 (rounded up below 16:9, so 4:3 still counts as
// widescreen), fewer lines above 4.095:1 so the width stays within the SDK's
// 4095.
void ShapeRatio(float aspect, uint32_t* ax, uint32_t* ay);

// Where the presenter puts a picture of this shape in a window or screen of
// view_width x view_height pixels, with its rounding (the SDK's
// ui/presenter.cpp): the shape fills the window's width or height; the other
// side is cropped at most to the safe area (only with
// present_allow_overscan_cutoff), then gets black bars (present_letterbox)
// or is stretched to the window. x and y are negative when it is cropped.
struct PictureRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};
PictureRect PictureInView(uint32_t view_width, uint32_t view_height, float aspect);

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
