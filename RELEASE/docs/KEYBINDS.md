# Keyboard and mouse: keys that call the game's actions

Both programs (the campaign `cod3` and multiplayer `cod3mp`), 2026-10-19. The design is
`dev/agents/keybinds/PLAN.md`; this page is what is built.

## What it does

Call of Duty 3 was made for a controller: one button often does two things (the X button
reloads when tapped and uses when held; B crouches when tapped and goes prone when held) and
there is no button for some actions at all (leaning, the second weapon). Until now the
keyboard pretended to be that controller: each key pressed a controller button (the
`keybind_*` rows), so it had the same limits.

Now each keyboard and mouse key calls one of the game's own actions directly, the way the
controller's button calls it, while you play:

- R reloads and F uses (they used to press X together).
- C crouches, CTRL goes prone, SPACE jumps or stands up.
- Q / E lean, the mouse wheel switches weapons, 1 / 2 pick the first or second weapon.
- Every action can have a key of its own (up to two), and any key can be given to any action.

A controller keeps working at the same time, unchanged: its layout (A-D), its buttons, the
game's own TOGGLE ADS. A key and a controller button can hold the same action together
(the game keeps two holders per button), so switching between them mid-action works.

In the game's menus (the main menu, the pause menu, a scoreboard) the keys go on pressing the
controller buttons of their `keybind_*` rows, as before: SPACE is A, ESCAPE is START, the
arrows and WASD move the selection. Keys that no action has (WASD, TAB, ESCAPE by default)
press their rows' buttons in play too: WASD moves through the left stick rows, TAB is the
controller's BACK (objectives, or the scores in multiplayer), ESCAPE its START (pause).

Inside an interaction (a mounted gun, a vehicle, a battle action) the game expects controller
buttons, so there a key presses the controller button the game binds to its action (fire
presses RT in layout A, reload or use press X ...). The interaction then decides, exactly as
for a controller. A key held when an interaction starts or ends goes on as a held controller
button would: fire held onto a turret keeps firing; a key held off a mounted gun does nothing new
until it is pressed again.

## Default keys (World at War PC style)

| Group | Action | Game command | Keys |
|---|---|---|---|
| Combat | Fire | `+attack` | MOUSE1 |
| | Aim down the sights (hold, or toggle) | `+speed` / `toggle cl_run` | MOUSE2 |
| | Use / interact | `+activate` | F |
| | Reload | `+reload` | R |
| | Use or reload (the controller's X) | `+activatereload` | - |
| | Melee | `+melee` | V, MOUSE3 |
| | Use or melee | `+activatemelee` | - |
| | Frag grenade | `+grenadeattack` | G |
| | Smoke grenade (multiplayer: the class's special grenade) | `+smokegrenade` | 4 |
| | Binoculars | `+binoculars` | B |
| | Sprint / hold breath | `+sprintbreath` | SHIFT |
| | Hold breath | `+holdbreath` | - |
| | Sprint | `+sprint` | - |
| | Lean left / right | `+leanleft` / `+leanright` | Q / E |
| | Lean left / right, switch weapon when not aiming | `+leanleftswitchnext` / `+leanrightswitchnext` | - |
| Stance | Jump / stand up | `+gostand` | SPACE |
| | Raise stance / jump | `+moveup` | - |
| | Use or raise stance | `+activatemoveup` | - |
| | Change stance (the controller's B: tap crouch, hold prone) | `+stance` | - |
| | Crouch (pressed again: stand) | `togglecrouch` | C |
| | Crouch while held | `+movedown` | - |
| | Prone (pressed again: stand) | `toggleprone` | CTRL |
| | Prone while held | `+prone` | - |
| | Lower / raise stance | `lowerstance` / `raisestance` | - |
| Weapons | Next / previous weapon | `weapnext` / `weapprev` | MWHEELDOWN / MWHEELUP |
| | Weapon's other mode | `weapalt` | - |
| | Weapon 1 / 2 | `weaponslot 1` / `weaponslot 2` | 1 / 2 |
| General | Objectives / scores (while held) | `+scores` | - (TAB is the controller's BACK) |

Movement (WASD), the pause menu (ESCAPE, fixed) and the controller's BACK (TAB) stay the
keyboard's controller rows. ` / ~ (the console), F2 (settings) and the overlays' keys
(F3, F4, F7, F10) can't be bound.

Changes for players coming from the old keys: C was the controller's B (tap crouch, hold
prone); it is now the crouch toggle and CTRL the prone toggle (give "Change stance" a key for
the old behavior). 1 and 2 used to press Y (next weapon); they now pick weapon 1 and 2.

## Changing them

- **F2 > Controls > Key bindings > Actions**: one row per action; type the keys (up to two,
  separated by a comma: `F, MOUSE4`; empty = none) and press Enter. A key another action had
  moves and the row says so. Above the rows: the switch "Keys call the game's actions" and
  "Aim toggle (keyboard and mouse)". The controller rows (`keybind_*`) are below them.
- **The ~ console** (campaign): `bind K +reload` or `bind K reload`, `unbind K`, `bind K`
  (what K does). Controller buttons (L3, UPARROW ...) stay the game's own `bind`.
- **cod3.toml** (`[cod3/Binds]`): `cod3_binds_kbm_p1 = "fire=MOUSE1;aim=MOUSE2;use=F;..."`
  (empty = the defaults; an action missing from the text keeps its default unless another
  action has the key), `cod3_binds_actions` (true; false = every key presses its row's
  controller button, as before this change), `cod3_aim_toggle_kbm_p1` (false).
  Only action ids and key names are read; anything else is dropped with a log line.

Key names are World at War's: A-Z, 0-9, SHIFT, CTRL, ALT, SPACE, TAB, ENTER, UPARROW ...,
F1-F12, KP_0 ..., COMMA, MOUSE1-MOUSE5, MWHEELUP, MWHEELDOWN (the SDK's LMB, RMB, MB4,
Control, Up ... are read too). No key combinations: World at War has none, and Shift+arrows
were how the binoculars used to come up by accident.

## Multiplayer

The same actions and keys, for player 1 (the keyboard and mouse player), while player 1 is in a
match: in the front end, a lobby or a loading screen the keys press the controller rows (the
game itself treats every press there as a menu key). Multiplayer reads the campaign's
`cod3.toml`, so keys changed in the campaign carry over; keys changed in multiplayer's F2 are
saved to `cod3mp.toml`. G always throws the frag and 4 the special grenade, whichever shoulder
the multiplayer layout gives them. A controller row the player changed keeps its keys in play
too, once (`cod3mp_binds_version`; multiplayer's layouts aren't the campaign's, so no row's
keys move to an action there). Not yet in multiplayer: the console's `bind` for these actions
(there it is still the game's own, for controller buttons). To check: multiplayer's D-pad rows
are the SDK's Shift+arrows; SHIFT is sprint / hold breath in play now, so those rows work in
the menus only (give the D-pad rows keys of their own if a match needs them).

## One-time moves of old keys (campaign: `cod3_binds_version`)

1. (2026-10-16) The left stick click and the D-pad: their old keys (Shift, Shift+arrows) went;
   a player's own keys there moved to the binoculars and hold breath.
2. (2026-10-19) A controller row the player changed (in `cod3.toml` and not its default):
   - a row whose button is one action in layout A (`keybind_x` "use or reload", `keybind_a`
     jump, `keybind_b` change stance, `keybind_y` next weapon, the triggers, the shoulders,
     the right stick click) gives that action its keys, taken from the action that had them
     by default;
   - any other row (movement, BACK, START, the D-pad) keeps its keys in play too: an action
     that had one of them by default lets it go (ESDF players keep E, S, D and F for walking).

   The rows themselves stay as they are, for the menus. Each move is one log line
   (`cod3: key binds: ...`).

## How it works (developers)

Files: `recomp/src/game/keybinds/` (`model.*` the actions and the stored form, plain code with
a unit test in `dev/agents/keybinds/tests/test_binds.cpp`; `keys.*` key names; `binds.*` the
game side; `pad_state_mp.cpp` multiplayer's pad hook), `[keybinds]` hunks in `cod3_app.cpp`,
`cod3mp_app.cpp`, `game/button_prompts.cpp` (campaign prompts), `ui/hud_overlay.cpp`,
`ui/game_console.cpp`; addresses in `game/addresses.h` `addr::keybinds` (multiplayer's from
`recomp/mp/config/campaign_map.tsv` through `tools/mp_aliases.py`).

- A window listener (UI thread, z 197) takes a bound key while the game takes the player's
  presses (`binds::InPlay()`, set at every pad poll); otherwise the key goes on to the SDK's
  keyboard driver (the `keybind_*` rows). Events go to the game thread through a fixed ring.
- The pad poll hook (campaign `sub_82522138`, multiplayer `0x82543340`, once before every
  frame) drains the ring and, after the game's own poll, calls the actions:
  - the handlers are found by name in the game's command list (`cmd_functions`,
    `cmd_function_t {next, name, type, func}`), so both programs use the same table:
    button commands (type 2) get a key id of their own (AUX1 239 / AUX2 240) and
    `Sys_Milliseconds`, plain commands (type 1) nothing, as the pad path calls them;
    cgame commands (`weapnext`, `weaponslot 1`, `+scores`) go through `Cbuf_AddText`;
  - a command the game doesn't have is logged once and its keys stay the rows' (looked up
    again every 600 polls);
  - multiplayer runs its pad poll once per local client and keeps each client's kbuttons apart
    (696 bytes at `0x82B3FFD0 + cl * 696`, copied into the game's set at `0x82B53E20` around
    the client's poll and frame): the keys' calls do the same for local client 0, with
    `clientActiveIndex` 0, and leave the game's set as it was.
- The press rule is the game's own: the campaign's (`in_joystick`, the poll-off byte, the
  front end's block at `*(g_FEManager+32)`), multiplayer's (`in_joystick`, `g_FEManager+50`,
  the controller's skip byte, the client's front-end object's vfunc at +92, the client in a
  match: local client +184 == 2, else its presses are menu keys).
- Inside an interaction a press becomes the controller button whose binding in the game's bind
  table is the action's command or one of its equivalents, OR-ed into the pad state the game
  reads (`GetPadState` hook: `button_prompts.cpp` in the campaign, `pad_state_mp.cpp` in
  multiplayer), for the pad the game reads.
- Releases always go; a menu opening, an overlay, focus loss or the switch going off lets
  every held action go; when play goes on, keys still held press their held actions again
  (fire, aim, sprint, lean, the binoculars ...; a use, a grenade or a toggle waits for a new press). The frag's throw-back (it holds `+activatereload` with key 211,
  which `-grenadeattack` never lets go) is let go with the frag key.
- The campaign's prompts show the action's key: a controller button's glyph becomes the key
  of the action its binding is ("~r1" fire: `[MOUSE1]`), and the game's action words
  (`[HOLD_USE]`, `[RELOAD]`, `[STANCE_DOWN]` ...) read "Press and hold [F]".

To add an action: a row in `model.cpp` `kDefs` (in `Action`'s order, with its group), the
enum value in `model.h`, its command; nothing else (the game side finds its handler).

## Not done yet

- Vehicle actions of their own (gas, reverse, handbrake as keys: the `ButtonAlias_Value` hook,
  PLAN 4.3); in a vehicle the keys press controller buttons through their actions.
- Movement and look as actions (they are the stick rows, which vehicles and stick hints need).
- Press-to-capture and the game-menu KEY BINDINGS page (PLAN 7), controller rebinding (PLAN 6).
- The aim rule for hold and toggle across devices (PLAN 5, the `IN_SpeedDown` hook): toggled
  aim with the mouse, then holding the controller's LT, stops aiming while held.
- Multiplayer's console `bind` for actions.

## What to check on the PC

Build with the setup tool (2.10.17), then in a campaign level and in a multiplayer match:

1. R reloads without using, F uses (pick up a weapon, open a door) without reloading.
2. C toggles crouch, CTRL prone, SPACE jumps; Q / E lean; the wheel and 1 / 2 switch weapons.
3. G and 4 throw their grenades; a grenade thrown back with G doesn't leave "use" held.
4. MOUSE2 aims while held; with "Aim toggle" on, each press aims or stops.
5. In the pause menu and the main menu the keys still move and select (SPACE, ENTER, the
   arrows); after closing the menu the keys act again.
6. On a mounted gun and in a jeep the mouse fires / drives as before (controller buttons).
7. A controller at the same time: everything still works on it.
8. With `cod3_debug_binds = true` the log has `cod3: binds: fire down (+attack, holder AUX1,
   direct)` lines and `cod3: key binds: 33 of 33 actions call the game's own commands`.
