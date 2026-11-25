// menukit - native in-game menus that every port can share. [waw-decomp/menus1] 2026-11-23
//
// The core knows nothing about any game, renderer or platform: plain C++17, no includes from the SDK or a game.
// A port links it and gives it one Adapter (menukit_adapter notes in this header): the game's rows (RowProvider),
// its text in the chosen language, its sounds and its conditions. Menus are data: assets/menus/<game>/*.toml, read
// by ParseMenu, one page per [[page]], rows named "provider:name" (never by number), so adding an option is one line.
//
// Flow, per local player:
//   input (pad, keys, mouse) -> Navigator::Press / Hover / Click -> the adapter's providers change the game's values;
//   every frame: LayOut() -> a list of DrawItems in a 640x480 box -> the port's drawer draws them (ImGui for the
//   modern look, the game's own text and quads for the original look). Words are asked for every frame, so a
//   language change shows on the next frame.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace menukit {

// ---------------------------------------------------------------------------------------------------------------
// Rows

enum class Kind : uint8_t {
  kChoice,  // left / right step through values
  kSlider,  // a number in a range, stepped or set by a click
  kAction,  // accept does something (opens a page, resumes, quits)
  kNote,    // text only, never focused
};

struct Number {
  double min = 0.0, max = 1.0, step = 1.0;
};

// What the drawer shows for one row, asked every frame.
struct RowView {
  Kind kind = Kind::kChoice;
  std::string label;  // already in the chosen language
  std::string value;  // "ON", "90", "1280 X 720"
  std::string note;   // the help line for the focused row
  bool locked = false;  // shown, but can't change now (the note says why)
  Number number;        // kSlider only
  double number_value = 0.0;
};

// Who asks: the local player (0-3) and the adapter's own pointer (for example the game thread's state).
struct Context {
  int player = 0;
  void* game = nullptr;
};

// One per prefix ("port", "video", "dvar", "game", ...). Wraps the code a port already has for its options; nothing
// is re-implemented in the menus. Step / SetNumber / Activate / Reset run on the thread the port's input runs on.
class RowProvider {
 public:
  virtual ~RowProvider() = default;
  virtual std::string_view Prefix() const = 0;
  // false when this game or program has no such row (the data check uses it too).
  virtual bool View(std::string_view name, const Context& context, RowView* out) const = 0;
  virtual bool Step(std::string_view name, const Context& context, int direction);
  virtual bool SetNumber(std::string_view name, const Context& context, double value);
  virtual bool Activate(std::string_view name, const Context& context);
  virtual bool Reset(std::string_view name, const Context& context);
};

enum class Sound : uint8_t { kMove, kStep, kAccept, kBack, kRefuse };

// One per game and program. Everything game-specific lives behind it.
class Adapter {
 public:
  virtual ~Adapter() = default;
  // "cod3", "waw", ... (the folder under assets/menus/).
  virtual std::string_view Game() const = 0;
  // Whether this program answers to an `only` tag of a row or page ("cod3mp", "sp", "mp", "split_screen" ...).
  virtual bool HasTag(std::string_view tag) const = 0;
  // A text key in the chosen language; the adapter falls back to English, then to the key itself.
  virtual std::string Text(std::string_view key) const = 0;
  virtual RowProvider* Provider(std::string_view prefix) = 0;
  // A row's `when` ("in_level", "host", ...); true when the adapter doesn't know it.
  virtual bool Condition(std::string_view when, const Context& context) const;
  virtual void Play(Sound sound, const Context& context);
};

// ---------------------------------------------------------------------------------------------------------------
// The menus as data

struct Row {
  std::string id;        // "port:skip_cutscenes"
  std::string note_key;  // replaces the provider's note ("" = the provider's)
  std::string when;      // a condition for the adapter ("" = always shown)
};

struct Page {
  std::string id, label_key, title_key;
  std::vector<Row> rows;  // only the rows this program has
};

struct Menu {
  std::string id, title_key;
  std::string takes_over;  // the game's own page this one replaces (the adapter's name for it)
  std::string back;        // "game": back on the top level hands back to the game's own page
  std::vector<Page> pages;
};

// Reads one menu file. Rows and pages tagged `only` for another program are left out quietly; an unknown row, a
// missing field or a syntax error is added to `problems` (each once) and left out, so a typo never breaks a menu.
// false when nothing usable is left.
bool ParseMenu(std::string_view text, std::string_view source, Adapter& adapter, Menu* out,
               std::vector<std::string>* problems);

// Splits "prefix:name" and finds the provider; false when there is no ':' or no such provider.
bool FindRow(Adapter& adapter, std::string_view id, RowProvider** provider, std::string_view* name);

// ---------------------------------------------------------------------------------------------------------------
// Navigation, one per local player

enum class Input : uint8_t { kUp, kDown, kLeft, kRight, kAccept, kBack, kPagePrev, kPageNext, kDefault };

enum class Outcome : uint8_t {
  kIgnored,  // not ours: the game may use it
  kHandled,  // ours, done
  kLeave,    // back on the top level: hand the page back to the game (its own Back, "save changes?")
};

enum class Look : uint8_t {
  kModern,    // left-aligned: a category list, then the page's rows (the owner's 11-16 14:12 answer)
  kOriginal,  // the game's own look: one page at a time, page left / right on the bumpers
};

class Navigator {
 public:
  void Open(const Menu* menu, Look look, Adapter& adapter, const Context& context);
  void Close();
  bool IsOpen() const { return menu_ != nullptr; }
  const Menu* menu() const { return menu_; }
  Look look() const { return look_; }

  Outcome Press(Input input, Adapter& adapter, const Context& context);
  // The pointer over a row (-1: over nothing), or over a category when `category` is set.
  void Hover(int index, bool category, Adapter& adapter, const Context& context);
  // A click: on a category opens it; on a row focuses it and steps toward the side clicked (`side` -1 / +1), or for
  // a slider sets the value at `fraction` (0..1 across its gauge, < 0 when the click was on the label).
  Outcome Click(int index, bool category, int side, float fraction, Adapter& adapter, const Context& context);

  int page() const { return page_; }
  int row() const { return row_; }
  bool in_list() const { return in_list_; }
  int scroll() const { return scroll_; }  // the first visible row shown, counted among the visible rows
  // Rows of the current page that show now (their `when` holds), as indexes into Page::rows.
  std::vector<int> VisibleRows(Adapter& adapter, const Context& context) const;
  // Keeps the focused row (at `focus_at` of `count` visible rows) inside a window of `rows_shown` (LayOut calls it).
  void KeepInView(int focus_at, int rows_shown, int count);

 private:
  bool Focusable(int index, Adapter& adapter, const Context& context) const;
  void MoveRow(int direction, Adapter& adapter, const Context& context);
  void MovePage(int direction);
  void FirstRow(Adapter& adapter, const Context& context);

  const Menu* menu_ = nullptr;
  Look look_ = Look::kModern;
  int page_ = 0, row_ = 0, scroll_ = 0;
  bool in_list_ = true;
};

// ---------------------------------------------------------------------------------------------------------------
// Layout: what to draw, shared by every look and every port's drawer

struct Box {
  float x0 = 0, y0 = 0, x1 = 640, y1 = 480;  // the game's 640x480 menu space unless the port says otherwise
};

enum class What : uint8_t {
  kTitle,      // the menu's or page's title
  kCategory,   // a page name in the list (modern) or the page tabs line (original)
  kLabel,      // a row's label
  kValue,      // a row's value text
  kGauge,      // a slider's gauge: `fill` 0..1
  kArrows,     // the step arrows around a value (original look)
  kNote,       // the focused row's help line
  kRule,       // a thin line
  kHighlight,  // the focus band behind the focused row or category
  kScroll,     // more rows above (`fill` 0) or below (`fill` 1)
};

struct DrawItem {
  What what = What::kLabel;
  Box box;
  std::string text;
  bool focused = false;
  bool locked = false;
  int index = -1;          // the row (index into Page::rows) or the page it belongs to; -1 none
  bool category = false;   // index is a page
  float fill = 0.0f;       // kGauge
};

// Lays out the open menu for one player. Text is asked for here (every frame).
std::vector<DrawItem> LayOut(Navigator& navigator, Adapter& adapter, const Context& context, const Box& area);

// The item under (x, y), for the pointer; nullptr when none. Rows and categories only.
const DrawItem* HitTest(const std::vector<DrawItem>& items, float x, float y);

}  // namespace menukit
