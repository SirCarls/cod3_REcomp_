// f2 - Counter-Strike 2's crosshair, every option, one for each split-screen player, for every game's F2 window.
// [acct2-ocr] 2026-11-14 (owner 15:22: "Please copy EVERY option from CS2's crosshair settings and start transfering it
// to all the f2 menus"; 15:24: "each individual splitscreen player should be able to have their own custom
// crosshair").
//
// What CS2 has (CS2 1.41.9.6, read by acct2-decomp on 11-07: its settings menu, its share-code layout and its
// crosshair shader, dev/agents/acct2-decomp/NOTES.md 15:56 and cs2-crosshair/SHADER.md): 10 styles, length,
// thickness, gap, dot, T, color, outline (none, full, half) with its own color, the dynamic spread limit, the split
// (distance, inner and outer alpha, ratio), follow recoil, the iron sight dot, friendly warning, the observer's and
// the observed bot's crosshair, the grenade crosshair, the sniper options and share codes. All of them are here.
//
// How it is drawn: Place() lays the crosshair out as CS2's C++ side does, as a list of shapes (rectangles, rings,
// four arcs), and Render() shades every pixel with CS2's own pixel shader, worked on the CPU (fill over outline, the
// strongest of each layer, CS2's mix), then hands the pixels to Dear ImGui as runs of one color. So see-through
// colors and outlines come out as in CS2, which drawing the outline and then the fill with ImGui does not.
// Place()'s sizes for the styles CS2 added in 2025-2026 (the circles, quads and square) are this file's reading of
// CS2's menu; CS2's own placement code (CCsgoCrosshairTileRenderer in libclient.so) is still to be read.
//
// [acct2-ocr] 2026-11-14 17:40 Special crosshairs (owner 17:06 / 17:19): turrets, vehicles, scopes, weapons the game
// gives a crosshair of their own and Call of Duty 3's aiming mode keep the game's own unless the player chooses Mine,
// Both or None for that kind (Special, PlanFor); the game's hooks ask PlanFor what to do.
//
// Plain Dear ImGui and rex::cvar only (the BRO and FH ports' shims give rex::cvar), so Call of Duty 3, the three
// rexglue ports, Big Red One and Finest Hour can all build it. Settings, the rows and the drawing are this file's;
// each game gives the views (where its hook found each player's crosshair) and four text settings to keep the four
// players' crosshairs in.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawList;

namespace f2::xhair {

constexpr int kPlayers = 4;

// cl_crosshairstyle, as CS2's menu names them.
enum Style : int {
  kDynamicCross = 0,
  kDynamicCircle = 1,
  kDynamicCrossLegacy = 2,  // CS:GO's Classic Dynamic, with the split
  kStaticCircle = 3,
  kStaticCross = 4,
  kStaticCrossShotFeedback = 5,
  kDotOnly = 6,
  kDynamicQuad = 7,  // CS2's default
  kStaticSquare = 8,
  kStaticQuad = 9,
  kStyleCount = 10,
};
const char* StyleName(int style);
bool StyleIsDynamic(int style);

// cl_crosshair_drawoutline
enum Outline : int { kOutlineNone = 0, kOutlineFull = 1, kOutlineHalf = 2 };

// The port's own, on top of CS2's: where the crosshair's color comes from. The Call of Duty games color their
// crosshair by what is under it (white, red over an enemy, green over a friendly, Call of Duty 3 also black over a
// dead body); CS2 has one color.
enum ColorFrom : int { kColorGame = 0, kColorOwnPerCase = 1, kColorConstant = 2 };
enum Case : int { kCaseNormal = 0, kCaseEnemy = 1, kCaseFriendly = 2, kCaseDead = 3, kCaseCount = 4 };

// CS2's grenade crosshair: one switch and one delay for each kind.
enum Grenade : int { kFlash = 0, kExplosive = 1, kFire = 2, kSmoke = 3, kDecoy = 4, kGrenadeCount = 5 };

struct Rgba {
  uint8_t r = 255, g = 255, b = 255, a = 255;
  bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

// [acct2-ocr] 2026-11-14 The crosshairs the game draws for something other than an ordinary weapon from the hip (owner
// 17:06: "you will also need to find vehicle crosshairs, and unique ones like the WAW flamethrower"; 17:19: "default
// should be the game's own"). Each player chooses, for each kind their game has, what is shown there.
enum Special : int {
  kSpecialOwnCross = 0,  // a weapon the game gives a crosshair of its own (World at War's flamethrower, launchers...)
  kSpecialTurret = 1,    // turrets and mounted guns
  kSpecialVehicle = 2,   // vehicles, the HUD's vehicle reticle too
  kSpecialScope = 3,     // a scope's overlay (and a scoped turret's)
  kSpecialCountdown = 4,  // [acct2-ocr] 11-15 a cooked grenade's countdown (Call of Duty 3's ring of six marks).
                          // Was kSpecialAimMode, which no game offered (its row was greyed, so it was never set):
                          // Call of Duty 3's "aiming mode" turned out to be its vehicles (kSpecialVehicle)
  kSpecialCount = 5,
};
constexpr int kHipFire = -1;  // an ordinary weapon's hip-fire crosshair (the custom crosshair's own place)
enum SpecialMode : int { kGameOwn = 0, kMine = 1, kBoth = 2, kNone = 3, kSpecialModeCount = 4 };
const char* SpecialName(int kind);

// What the game's hook does for a crosshair of one kind: let the game draw its own, and/or draw this player's.
struct Plan {
  bool game = true;
  bool mine = false;
};

// One player's crosshair. Sizes are pixels on a screen `screen_height` lines tall, as CS2's (cl_crosshair_screen_height);
// they scale with the view's height.
struct Settings {
  bool enabled = false;  // this player's custom crosshair replaces the game's own

  // CS2's share-code fields.
  int style = kStaticCross;
  float length = 6.0f;     // cl_crosshair_length, 0-255
  float thickness = 2.0f;  // cl_crosshair_thickness, 0-32 (at least 1 pixel on screen)
  float gap = 4.0f;        // cl_crosshair_gap, 0-128 (-10 to 128 for the legacy style)
  bool dot = false;        // cl_crosshairdot
  bool t_style = false;    // cl_crosshair_t: no top line
  Rgba color{0, 255, 0, 255};  // cl_crosshaircolor_r/_g/_b/_a
  int outline = kOutlineFull;  // cl_crosshair_drawoutline
  Rgba outline_color{0, 0, 0, 255};  // cl_crosshairoutline_r/_g/_b/_a
  int spread_limit = 0;            // cl_crosshair_dynamic_spread_limit, 0-255 (0: no limit)
  int split_distance = 7;          // cl_crosshair_dynamic_splitdist, 0-127
  float split_inner_alpha = 1.0f;  // cl_crosshair_dynamic_splitalpha_innermod, 0-1
  float split_outer_alpha = 0.5f;  // cl_crosshair_dynamic_splitalpha_outermod, 0.3-1
  float split_ratio = 0.35f;       // cl_crosshair_dynamic_maxdist_splitratio, 0-1 (also the static quad's)
  bool follow_recoil = false;      // cl_crosshair_recoil
  bool ironsight_use_color = false;  // cl_ironsight_usecrosshaircolor
  float ironsight_dot_scale = 1.0f;  // cl_ironsight_dot_scale, 0.1-2
  int screen_height = 1080;          // cl_crosshair_screen_height: the screen the sizes are for

  // CS2's settings that a share code doesn't carry.
  bool friendly_warning = true;   // a mark while a friendly is under the crosshair
  bool observer = true;           // show the crosshair of the player you watch
  bool observed_bot = false;      // ... also when that player is a bot
  bool grenade_keep_mine = true;  // keep this crosshair while a grenade is out
  std::array<bool, kGrenadeCount> grenade_on{};    // the grenade crosshair for each kind
  std::array<float, kGrenadeCount> grenade_delay{};  // seconds after taking it out, 0-5
  bool sniper_delay_unscope = false;
  bool sniper_show_inaccuracy = false;
  bool sniper_auto_rezoom = false;
  int sniper_width = 1;  // the scope's lines, 1-6

  // The port's own.
  int color_from = kColorGame;
  std::array<Rgba, kCaseCount> case_color{{{255, 255, 255, 255}, {255, 0, 0, 255}, {64, 255, 64, 255}, {0, 0, 0, 255}}};
  float outline_thickness = 1.0f;  // the outline's width in whole pixels (CS2: 1)
  float open_scale = 1.0f;         // dynamic styles: how far they open for the game's spread (1: exactly)
  bool ads_dot = false;            // the iron sight dot while the sights are up (CS2 has it on scoped rifles)
  std::array<int, kSpecialCount> special{};  // SpecialMode for each kind: the game's own unless chosen

  // Storage only: a hash of the old settings as they were when player 1's crosshair was last written (0: none), so
  // a change made to them while the game was closed is taken in at the next start.
  uint64_t legacy_hash = 0;
};

// "key=value" pairs, one line; unknown keys are ignored and missing ones keep their defaults, so an older or newer
// build reads it. Text() then FromText() gives the same settings.
std::string Text(const Settings& s);
Settings FromText(std::string_view text);

// CS2 share codes ("CS" and 44 characters). Import keeps the port's own settings and the ones a code doesn't carry.
// False, with the reason, for anything CS2 itself would refuse.
bool ImportCode(std::string_view code, Settings* s, std::string* why);
std::string ExportCode(const Settings& s);

// The ports' crosshair settings before this file (f2_crosshair_* in World at War and Call of Duty 4, cod3_crosshair_*
// in Call of Duty 3): player 1 starts from them, so nobody's crosshair changes.
struct Legacy {
  bool enabled = false;
  int shape = 0;  // 0 cross, 1 cross with dot, 2 dot, 3 circle, 4 T
  double size = 6.0, thickness = 2.0, gap = 4.0, outline_thickness = 1.0;
  bool outline = true, dot = false;
  bool constant_color = false;
  std::string color = "0 255 0 255";  // "r g b a"
  bool own_colors = false;
  std::string colors[kCaseCount] = {"255 255 255 255", "255 0 0 255", "64 255 64 255", "0 0 0 255"};
  int dynamic = 0;  // 0 static, 1 dynamic, 2 dynamic split
  double dynamic_scale = 1.0, split_distance = 7.0, split_ratio = 0.35, split_alpha_inner = 1.0,
         split_alpha_outer = 0.5;
};
Settings FromLegacy(const Legacy& legacy);

// What a game's crosshair hook can tell; a row whose input the game doesn't give is shown greyed, with the reason.
struct Caps {
  int players = 1;            // split-screen players this game can have (1 to 4)
  bool spread = false;        // the game's own spread, for the dynamic styles
  bool shots = false;         // when the player fires (shot feedback; without it a jump in the spread counts)
  bool recoil = false;        // the view's kick, for follow recoil
  bool sights = false;        // how far the sights are up, for the iron sight dot
  bool grenades = false;      // which grenade is out
  bool scope = false;         // the sniper scope is up
  bool spectating = false;    // the player watches someone else
  bool dead_color = false;    // the game has a color for a dead body under the crosshair (Call of Duty 3)
  // [acct2-ocr] 2026-11-14 the special crosshairs: the kinds the hooks tell apart (1 << kind, a row each), the game's
  // own name for a kind (nullptr: SpecialName's), and the kinds the game has that its hooks don't tell apart yet (a
  // greyed row each: the game's own stays).
  uint32_t special = 0;
  std::array<const char*, kSpecialCount> special_label{};
  uint32_t special_later = 0;
};

// One player's crosshair this frame, from the game's hook.
struct View {
  float cx = 0, cy = 0;     // its middle, in the window's pixels (ImGui's coordinates)
  float height = 1080;      // the view's height in the window's pixels (a split-screen view's own)
  float rgba[4] = {1, 1, 1, 1};  // the game's crosshair color now, its alpha with the game's fades
  float spread = 0;         // the game's spread in its HUD units (480 = the view's height)
  float recoil_x = 0, recoil_y = 0;  // the kick, in the view's pixels
  float sights = 0;         // 0 hip, 1 the sights up
  bool over_friendly = false;
  int grenade = -1;         // kFlash..kDecoy while one is out
  float grenade_seconds = 0;  // since it was taken out
  bool scoped = false;
  bool watching = false, watching_bot = false;
  double seconds_since_shot = -1;  // < 0: not known
  // [acct2-ocr] 2026-11-15 a cooked grenade's time left, 1 when the pin is pulled to 0 at the bang (< 0: none), drawn
  // as six marks round the crosshair (kSpecialCountdown's Mine); `cross` false: the marks alone (the game drew no
  // crosshair this frame, only its countdown).
  float countdown = -1;
  bool cross = true;
};

// The port's settings from player 1's crosshair, as near as they can say it (for a game whose own menus still set the
// old names: Call of Duty 3's in-game AIM page).
Legacy ToLegacy(const Settings& s);

// The storage: the game's four text settings (rex::cvar names, or its shim's), player 1 to 4. `get_legacy` reads the
// game's old settings: player 1's crosshair while its text setting is still empty, and afterwards a change made to
// them elsewhere (an in-game menu, the console) is taken into player 1's crosshair, field by field. `set_legacy`
// (may be empty) writes player 1's crosshair back to the old names whenever it changes here, so the two agree.
// Once, at start; the UI thread.
void Bind(const std::array<const char*, kPlayers>& names, std::function<Legacy()> get_legacy,
          std::function<void(const Legacy&)> set_legacy = {});
// Once a UI frame: picks up a change made elsewhere (the console, the file, a test script).
void Sync();
const Settings& Player(int player);
// Writes `s` as `player`'s crosshair (into its text setting).
void SetPlayer(int player, const Settings& s);
// Whether `player`'s custom crosshair is on. Any thread (the game's crosshair hook asks it to skip the game's own).
bool Enabled(int player);
// [acct2-ocr] 2026-11-14 What `player`'s hook does for a crosshair of `kind` (kHipFire or a Special). Any thread. While
// the player's own crosshair is off, every kind is the game's own; kHipFire is then the game's, else theirs.
Plan PlanFor(int player, int kind);

// The game says what its hook gives (once, or when it changes).
void SetCaps(const Caps& caps);
const Caps& GetCaps();

// Draws `player`'s crosshair for this frame's view (nothing while that player's is off). The UI thread.
void DrawView(ImDrawList* dl, int player, const View& view);

// The CROSSHAIR page: the player being edited (`selected`, kept by the caller), `game_rows` (the game's own rows for
// the crosshair, such as its show-crosshair switch; may be empty, returns true when it changed something), every CS2
// row for that player, and on the right a column that never scrolls with the preview at its size in the game. True
// when the player's crosshair changed (it is then already written with SetPlayer).
bool DrawPage(int* selected, const std::function<bool()>& game_rows = {});
// The preview alone: `player`'s crosshair at its size in the game, in a box `side` pixels square.
void DrawPreview(int player, float side);

// ---- for tests and for the preview

// A shape as CS2's crosshair shader takes it (SHADER.md "Inputs"): kind 0 a rectangle (x0,y0)-(x1,y1) in whole
// pixels, inclusive; kind 1 a ring, kind 2 four arcs and kind 3 one arc around (x0, y0), outer radius x1, thickness
// y1, each arc
// `span` radians wide, centered on the diagonals. `grow` is how far the outline reaches past the shape, on the
// top / left (grow0) and on the bottom / right (grow1).
struct Shape {
  int kind = 0;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float span = 0;
  int arcs = 15;  // kind 2: which arcs, 1 top right, 2 bottom right, 4 bottom left, 8 top left (the port's own)
  float angle = 0;  // [acct2-ocr] kind 3 (the port's own): one arc `span` wide centered at `angle` radians, 0 at the
                    // top, clockwise (the grenade countdown's marks)
  float grow0 = 0, grow1 = 0;
  float fill[4] = {};
  float outline[4] = {};
};
// The shapes of `s` for `view` (pixel coordinates as given in view.cx / cy), at time `now` seconds (shot feedback).
std::vector<Shape> Place(const Settings& s, const View& view, double now, int player = 0);
// One pixel as CS2's shader shades it: premultiplied red, green, blue and alpha, 0-1. (px, py) is the pixel's
// whole-number position.
void Shade(const std::vector<Shape>& shapes, int px, int py, float out[4]);
// The pixels of `shapes` as rectangles of one color: x, y, width, height, color (straight alpha, as ImGui blends).
struct Run {
  int x, y, w, h;
  uint32_t color;  // IM_COL32 order
};
void Rasterize(const std::vector<Shape>& shapes, std::vector<Run>* runs);

}  // namespace f2::xhair
