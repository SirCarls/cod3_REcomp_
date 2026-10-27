// cod3 - the port's keyboard and mouse actions and their keys (plain logic;
// the game side is game/keybinds/binds.cpp).
//
// The key binds plan (dev/agents/keybinds/PLAN.md): each keyboard and mouse key
// calls one of the game's own actions, never a controller button the keyboard
// pretends to press. An action is the game's own command, called the way the
// controller's button calls it (binds.h has how):
//   Combat   fire (+attack, MOUSE1), aim (+speed, MOUSE2; hold or toggle),
//            use (+activate, F), reload (+reload, R), melee (+melee, V and
//            MOUSE3), frag (+grenadeattack, G), smoke (+smokegrenade, 4),
//            binoculars (+binoculars, B), sprint / hold breath
//            (+sprintbreath, SHIFT), lean left / right (Q / E); and with no
//            key by default: the controller's own mixes (use or reload =
//            the pad's X, use or melee), hold breath, sprint, the lean keys
//            that switch weapons when not aiming
//   Stance   jump / stand up (+gostand, SPACE), crouch (togglecrouch, C),
//            prone (toggleprone, CTRL); no key by default: change stance (the
//            pad's B: tap crouch, hold prone), crouch / prone while held,
//            raise / lower stance, +moveup, use or raise stance
//   Weapons  next / previous weapon (MWHEELDOWN / MWHEELUP), weapon 1 / 2
//            (weaponslot, 1 / 2), the weapon's other mode
//   General  objectives / scores (+scores; no key by default: TAB stays the
//            controller's BACK, which shows them in both programs)
//   Vehicle  the tank's machine gun (MOUSE2) and turret alignment (R), the
//            gas (W), brake / reverse (S) and handbrake (SPACE) of the jeep and
//            the other vehicles that aren't tanks, the vehicle's
//            view (V). The vehicles read these themselves (their "button
//            aliases" and the view switch), so they are answered where the game
//            reads them, never by pressing a controller button. A vehicle key
//            counts only while a vehicle reads its alias (while you drive); on
//            foot the same key keeps its own action (MOUSE2 aims, W walks).
// Up to two keys per action; a key belongs to one on-foot action and to at
// most one vehicle action. Movement (WASD), the pause menu (ESCAPE) and the
// controller's BACK stay the keyboard's controller rows (keybind_*) on foot,
// and so does everything in the game's menus.
//
// Stored in cod3.toml as cod3_binds_kbm_p1: "" = these defaults, otherwise
// every action: "fire=MOUSE1;aim=MOUSE2;use=F;...;sprint=" (an empty list =
// unbound; an action the text doesn't list keeps its default unless another
// action has the key). Only action ids and key names are ever read; any
// other text is dropped, never run.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/keybinds/keys.h"

namespace cod3::binds {

enum class Action : uint8_t {
  // Combat
  kFire,
  kAim,
  kUse,
  kReload,
  kUseReload,
  kMelee,
  kUseMelee,
  kFrag,
  kSmoke,
  kBinoculars,
  kSprintBreath,
  kHoldBreath,
  kSprint,
  kLeanLeft,
  kLeanRight,
  kLeanLeftNext,
  kLeanRightNext,
  // Stance
  kJump,
  kMoveUp,
  kUseMoveUp,
  kStance,
  kCrouch,
  kCrouchHold,
  kProne,
  kProneHold,
  kLowerStance,
  kRaiseStance,
  // Weapons
  kWeapNext,
  kWeapPrev,
  kWeapAlt,
  kSlot1,
  kSlot2,
  kFlipGun,  // [claude-9b] 2026-10-27 the weapon in the left hand (a port setting, Delivery::kPort)
  // General
  kScores,
  // Vehicle
  kTankGun,
  kTankTurret,
  kVehGas,
  kVehReverse,
  kVehHandbrake,
  kVehView,
  kCount
};
inline constexpr int kActionCount = static_cast<int>(Action::kCount);
inline constexpr int kSlots = 2;

// How the game is told (binds.cpp). The game's commands are found by name
// in its own command list, so the same table serves both programs.
enum class Delivery : uint8_t {
  kButton,  // a button command pair: "+attack" on the press, "-attack" on the
            // release, each with a key id of its own and the time (two keys
            // are two holders, as two controller buttons would be)
  kPlain,   // a plain command pair without arguments: "+stance" / "-stance"
  kOnce,    // one plain command on the press: "togglecrouch"
  kText,    // console text on the press ("weapnext"; the game's cgame runs
            // it); text starting with '+' sends its '-' twin on the release
  kAim,     // +speed / -speed while held, or the game's own aim toggle
            // ("toggle cl_run") once per press (cod3_aim_toggle_kbm_p1)
  kAlias,   // a vehicle's button alias (ActionDef::alias, in whichever vehicle
            // reads it): the vehicle's own read of it (ButtonAlias_Value) gets
            // "held" while the key is down; no game command
  kView,    // the vehicle view switch: the game's own read of it (the right
            // stick click's press) gets one press per key press
  kPort,    // [claude-9b] 2026-10-27 a port setting, not the game's: the press switches the bool setting named by
            // `command` (for split-screen players 2 to 4 its "_2" .. "_4" twin, where there is one)
};

enum class Group : uint8_t { kCombat, kStance, kWeapons, kGeneral, kVehicle, kCount };
const char* GroupName(Group group);

struct ActionDef {
  const char* id;              // in cod3.toml: "reload"
  const char* label;           // F2 and messages: "Reload"
  const char* help;            // F2: one sentence
  Group group;
  Delivery delivery;
  const char* command;         // the game's command or console text: "+reload"
  const char* equivalents[4];  // other game bindings that count as this action
                               // (prompts; the controller button used inside an
                               // interaction), nullptr-terminated
  KeyCode defaults[kSlots];    // World at War style
  int8_t alias = -1;           // kAlias: the game's alias id (0 Gas, 1 Reverse,
                               // 2 HandBrake, 3 AlignTurret, 4 FireCoax), answered
                               // in whichever vehicle reads it
};

const ActionDef& Def(Action action);
// The Vehicle group: keys of their own, apart from the on-foot actions' keys.
bool IsVehicleAction(Action action);
// By id or by the game's command (case-insensitive); Action::kCount when none.
Action ActionById(std::string_view id);
Action ActionByCommand(std::string_view command);

struct KbmSet {
  KeyCode key[kActionCount][kSlots] = {};
  bool operator==(const KbmSet&) const = default;
};

KbmSet Defaults();

// cod3_binds_kbm_p1 -> the keys. "" = the defaults. Unknown ids or keys,
// reserved keys, combinations and keys already taken are dropped, each with a
// note in `notes` (may be null). At most 4 KB are read.
KbmSet Parse(std::string_view text, std::vector<std::string>* notes);
// The keys -> cod3_binds_kbm_p1: "" when they are the defaults.
std::string Serialize(const KbmSet& set);

// The action and slot that have `key` among the on-foot actions (or, with
// `vehicle`, among the vehicle actions); Action::kCount when none.
Action Find(const KbmSet& set, KeyCode key, int* slot = nullptr, bool vehicle = false);

// Puts `key` in `slot` of `action` (kNoKey clears the slot). A key another
// action of the same kind (on foot or vehicle) had is taken from it; the note
// says so ("B was Binoculars; moved here"). Returns "" when nothing else
// changed.
std::string Assign(KbmSet& set, Action action, int slot, KeyCode key);

// Both keys of an action as text ("B", "B, MOUSE4", "" when unbound) and back
// (F2's text box). AssignText sets both slots from "B" / "B, MOUSE4" / "";
// false (and nothing changed) when a name isn't a key or a key is reserved,
// with the reason in `note`.
std::string KeysText(const KbmSet& set, Action action);
bool AssignText(KbmSet& set, Action action, std::string_view text, std::string* note);

// The controller row (keybind_*) the action's controller button is in the
// campaign's layout A, or nullptr (the one-time move below, and F2's notes).
const char* RowForAction(Action action);

// --- one-time moves of the old controller-button keys (PLAN.md section 10) ------
// The keyboard used to press controller buttons through keybind_* settings.
struct OldBind {
  std::string name;   // "keybind_lstick_press"
  std::string value;  // its value now
  bool from_file;     // the player's cod3.toml set it (else nothing to move)
  std::string default_value;  // the row's default (MigrateRows: unchanged rows are left alone)
};
struct Migration {
  KbmSet set;                 // the keys after the move
  bool set_changed = false;   // `set` differs from what came in
  // keybind_* -> its new value; "" means back to its (campaign) default.
  std::vector<std::pair<std::string, std::string>> new_values;
  std::vector<std::string> log;  // one line each, for the log
};

// Version 1 (2026-10-16): the left stick click and the D-pad. keybind_lstick_press
// = "Shift" (L3: binoculars), keybind_dpad_up = "Shift+Up" (D-pad up: hold
// breath) ... The campaign doesn't use those any more (app_defaults.h). A
// player's own keys there move to the actions; the old defaults and every
// Shift+key combination go. `others`: every other keybind_* value, so a key
// still pressing another controller button (keybind_x "R,F") stays there.
Migration Migrate(const KbmSet& current, const std::vector<OldBind>& binds,
                  const std::vector<OldBind>& others);

// Version 2 (2026-10-19): every action has a key of its own now, and the
// controller rows keep working in the game's menus (and for keys no action
// has). A row the player changed (`from_file` and not its default):
//  * a row whose button is one action in layout A (keybind_x: use or reload,
//    keybind_a: jump ..., RowForAction) gives that action its keys (up to
//    two), taken from the action that had them by default;
//  * any other row (movement, BACK, START, the D-pad ...) keeps its keys:
//    an action that had one of them by default lets it go (ESDF players keep
//    E, S, D and F for walking).
// The rows themselves are left as they are (new_values stays empty).
// `to_actions` false (cod3mp, whose layouts differ from the campaign's
// layout A): every changed row keeps its keys.
Migration MigrateRows(const KbmSet& current, const std::vector<OldBind>& rows, bool to_actions = true);

// Version 3 (2026-10-20): the Vehicle actions follow the player's own
// controller rows (the same changed rows as version 2):
//  * keybind_lstick_up's keys become Vehicle: gas's, keybind_lstick_down's
//    Vehicle: brake / reverse's (an ESDF player's E and D);
//  * the keys of every other row that keeps its keys in play (version 2's
//    second kind; every changed row when `to_actions` is false) leave the
//    Vehicle actions (an ESDF player's S steers left in a jeep too).
// The rows are left as they are (new_values stays empty).
Migration MigrateVehicleRows(const KbmSet& current, const std::vector<OldBind>& rows, bool to_actions = true);

}  // namespace cod3::binds
