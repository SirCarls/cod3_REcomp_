// cod3 - number boxes for the slider rows of the game's menus (owner
// 2026-10-20 21:09: "the arrows should be next to the sliders and the sliders
// need to reflect the text value (this needs to be a textbox too)"; asked, he
// chose "Every slider in the game menus"; 21:15: "please also make sure the
// menu scales").
//
// A slider row shows a left arrow, a gauge filled to the value, a right arrow
// and a box with the value as a number. The pad, the arrow keys and the
// arrows step the value as before; the mouse drags the gauge; and the number
// can be typed: with the row highlighted, digits (a period and a minus where
// the setting takes them) start typing in its box, Enter applies the number
// (kept within the setting's range, on its steps), Esc or moving off the row
// drops it, Backspace takes back a digit. A click on the box starts typing
// there.
//
// This file keeps what is typed and turns it into a value; it knows nothing
// of the game. The page code that draws the rows (game/menu_fov.cpp for the
// campaign's Pause > Options) says each frame which slider row is highlighted
// (Offer), shows BoxText in the box, notes where the box is (NoteBox) and
// applies what TakeTyped hands it. The mouse and keyboard layer
// (game/pc_menu.cpp) passes keys to OnKey and clicks to ClickAt first.

#pragma once

#include <cstdint>
#include <string>

namespace cod3::menuslider {

// What a slider row's number is: the setting's range, the steps a typed value
// lands on, and how the box shows it.
struct Number {
  double min = 0.0, max = 1.0;  // the setting's own range
  double step = 1.0;            // a typed value goes to the nearest min + k * step
  double shown_per_unit = 1.0;  // the box shows value * this (volume 0..50 shows 0..100%)
  int decimals = 0;             // shown with this many decimals
  const char* suffix = "";      // after the number ("%")
};

// The value as the box shows it: "12", "100%", "1.25".
std::string Format(double value, const Number& n);

// Where `value` lies in the range, 0..1 (what the gauge shows).
double Fraction(double value, const Number& n);

// --- game thread -----------------------------------------------------------

// Every frame a page with slider rows is up: the highlighted row, if it is a
// slider row (else item -1). Typing on another row (or page) stops.
void Offer(uint32_t page, int item, const Number& n);

// The text for a row's box: the value, or while it is typed in, the digits so
// far and a blinking caret.
std::string BoxText(uint32_t page, int item, double value, const Number& n);

// True while the row's number is being typed.
bool Typing(uint32_t page, int item);

// A number typed into the row and confirmed with Enter: true once, with the
// value within the range and on a step.
bool TakeTyped(uint32_t page, int item, double* value);

// Where the row's box is drawn this frame (menu space, 640x480).
void NoteBox(uint32_t page, int item, float x0, float y0, float x1, float y1);

// pc_menu.cpp, a left click on `page` at (x, y) in menu space: on a number box,
// typing starts there and its row's item comes back (the caller highlights
// the row); anywhere else -1 (and typing elsewhere stops).
int ClickAt(uint32_t page, float x, float y);

// --- UI thread ---------------------------------------------------------------

// pc_menu.cpp's key handler, while a game menu takes the keyboard: true when
// the key went to a number box (the caller marks it handled and stops). Keys
// are rex::ui::VirtualKey values.
bool OnKey(uint16_t virtual_key, bool shift);

}  // namespace cod3::menuslider
