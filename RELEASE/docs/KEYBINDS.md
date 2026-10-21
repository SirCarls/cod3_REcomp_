# Keyboard and mouse: keys that call the game's actions

Both programs (the campaign `cod3` and multiplayer `cod3mp`), 2026-10-19; vehicles 2026-10-20. The design is
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
arrows and WASD move the selection. Keys that no on-foot action has (WASD, TAB, ESCAPE by
default) press their rows' buttons in play too: WASD moves through the left stick rows, TAB is
the controller's BACK (objectives, or the scores in multiplayer), ESCAPE its START (pause).

Vehicles have keys of their own (the Vehicle group below): the tank's machine gun (MOUSE2) and
turret alignment (R), a vehicle's gas (W), brake / reverse (S) and handbrake (SPACE), and the
view switch (V). They are rebindable like every other action and count only while you drive:
the vehicle reads them itself (the game's "button aliases" and its view switch), and the key is
answered right there, where the game reads it, without pressing any controller button. Each
counts only while a vehicle reads its button: the tank reads its machine gun and turret
buttons, so in the tank MOUSE2 and R are those and W / S still drive it through the movement
rows; the jeep reads gas, brake and handbrake, so W / S / SPACE are those and A / D steer. On
foot the same keys keep their own actions (MOUSE2 aims, R reloads, V melees). In the tank MOUSE1
still fires the cannon (`+attack`). The log says when a vehicle starts reading its keys: "cod3:
binds: the tank reads its controls (AlignTurret, FireCoax): its keys count now (Tank: machine
gun MOUSE2, Tank: align turret R, Vehicle: change view V); on foot the same keys keep their own
actions". Prompts that show a vehicle's button (the tank's LB) show its Vehicle action's key. The tank of the Polish missions (Black Baron) is no
interaction; from setup tool 2.10.18 to 2.12.17 its machine gun didn't fire with the keyboard,
2.12.18 made the keys press the controller's buttons in vehicles for a day, and 2.13.0 replaced
that with these actions. Multiplayer too: the keyboard player's vehicle.

Inside an interaction (a mounted gun, a battle action, and the jeep, which is one too) the game
expects controller buttons, so there a key presses the controller button the game binds to its
action (fire presses RT in layout A, reload or use press X ...). The interaction then decides,
exactly as for a controller. A key held when an interaction starts or ends goes on as a held
controller button would: fire held onto a turret keeps firing; a key held off a mounted gun
does nothing new until it is pressed again. A button the vehicle you drive reads as one of its
own controls is never pressed for an on-foot action: in the jeep RT is its gas in layout A, so
MOUSE1 no longer drives it (W does, or whatever key Vehicle: gas has).

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
| Vehicle | Tank: machine gun (while held) | `tank_gun` (the alias FireCoax) | MOUSE2 |
| | Tank: align turret (while held) | `tank_turret` (AlignTurret) | R |
| | Vehicle: gas (while held; not the tank) | `veh_gas` (Gas) | W |
| | Vehicle: brake / reverse (while held) | `veh_reverse` (Reverse) | S |
| | Vehicle: handbrake (while held) | `veh_handbrake` (HandBrake) | SPACE |
| | Vehicle: change view | `veh_view` (the right stick click's press) | V |

The Vehicle keys count only while you drive and never take a key from an on-foot action: a key
can be an on-foot action's and a vehicle action's at once (MOUSE2 aims on foot and fires the
tank's machine gun), but not two on-foot or two vehicle actions'. The Vehicle "commands" are
the actions' ids (the game has no command for them; it reads them itself).

Movement (WASD), the pause menu (ESCAPE, fixed) and the controller's BACK (TAB) stay the
keyboard's controller rows (in the jeep W and S are its gas and brake while it reads them; a
player who changed the movement rows gets those keys for Vehicle: gas and brake once, below;
change them later in F2 together). ` / ~ (the console), F2 (settings) and the overlays' keys
(F3, F4, F7, F10) can't be bound. A key given to both an on-foot and a vehicle action is the
vehicle action's while you drive: `tank_gun=MOUSE1` would leave the tank's cannon without a key.

Changes for players coming from the old keys: C was the controller's B (tap crouch, hold
prone); it is now the crouch toggle and CTRL the prone toggle (give "Change stance" a key for
the old behavior). 1 and 2 used to press Y (next weapon); they now pick weapon 1 and 2.

## Changing them

- **F2 > Controls > Key bindings > Actions**: one row per action; type the keys (up to two,
  separated by a comma: `F, MOUSE4`; empty = none). What you type shows in capitals and applies
  on Enter, when you click anywhere else (Save too) or when F2 closes, so typing a key and
  clicking Save is enough. A key another action of the same kind had (on foot, or Vehicle)
  moves and the row says so. Above the rows: the switch "Keys call the game's actions" and
  "Aim toggle (keyboard and mouse)". Below them, while "Keys call the game's actions" is on,
  are only the movement rows (forward, back, strafe left / right, which steer a vehicle too); the
  controller button rows show only when it is off, that is when the keyboard plays as a
  controller (owner 2026-10-20). The rows below (`keybind_*`) work the same way; they show their
  names in capitals too (SPACE, SHIFT+UP) and are saved in the
  SDK's own spelling (`Space`, `Shift+Up`), which is the one it reads
  (`game/keybinds/sdk_spelling.h`). World at War's names work there too (ENTER, UPARROW,
  MOUSE1, KP_0 are saved as `Return`, `Up`, `LMB`, `Numpad0`), and a row saved earlier in a
  spelling the SDK can't read (`space`) is written in its spelling when F2 saves or closes.
- **The ~ console** (campaign): `bind K +reload` or `bind K reload`, `bind MOUSE4 tank_gun`,
  `unbind K` (on foot and in vehicles), `bind K` (what K does, on foot and "while driving").
  Controller buttons (L3, UPARROW ...) stay the game's own `bind`.
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
game itself treats every press there as a menu key). Vehicles (the jeeps and tanks of the maps
that have them) read the Vehicle keys as in the campaign, for the keyboard player's vehicle only
(split-screen players on other controllers keep theirs). Multiplayer reads the campaign's
`cod3.toml`, so keys changed in the campaign carry over; keys changed in multiplayer's F2 are
saved to `cod3mp.toml`. G always throws the frag and 4 the special grenade, whichever shoulder
the multiplayer layout gives them. A controller row the player changed keeps its keys in play
too, once (`cod3mp_binds_version`; multiplayer's layouts aren't the campaign's, so no row's
keys move to an action there). Not yet in multiplayer: the console's `bind` for these actions
(there it is still the game's own, for controller buttons). To check: multiplayer's D-pad rows
are the SDK's Shift+arrows; SHIFT is sprint / hold breath in play now, so those rows work in
the menus only (give the D-pad rows keys of their own if a match needs them).

## Controller vibration

Both programs (2026-10-20). The keyboard and mouse belong to player 1 together with the first
controller (the SDK puts them on the same guest user), so with the game's Vibration option on,
the controller lying next to a keyboard and mouse player would buzz. It doesn't now: while the
keyboard and mouse are in use (a key or mouse press since the last controller button, trigger
or stick push, the same switch the button prompts follow) the first controller gets no
vibration. What the game asks for is kept and sent as soon as one of the controller's buttons,
triggers or sticks is used again, so picking the controller up brings its vibration straight
back. The game's own Vibration option and its values never change.

Split screen: only the first controller (player 1's, the one the keyboard and mouse share) is
ever held back; players 2-4 on their own controllers always feel the game's vibration. With
`mnk_mode` off (F2's Keyboard & mouse box) nothing is held back. `cod3_rumble_with_keys true`
(the console, or `cod3.toml` / `cod3mp.toml`) lets the first controller vibrate anyway. The log
says each change, "cod3: controller 1 vibration held back (the keyboard and mouse are in use)"
and "cod3: controller 1 vibration back (the controller is in use; ...)", and the first request
after it ("cod3: controller 1 vibration asked for (left 13369, right 0): held back"). Code:
`recomp/src/game/rumble.{h,cpp}` (the game's XamInputSetState wrapper: campaign `sub_82344FA8`,
multiplayer `sub_82314370`).

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

3. (2026-10-20, multiplayer too: `cod3mp_binds_version` 3) The Vehicle actions and the
   player's changed rows: `keybind_lstick_up`'s keys become Vehicle: gas's and
   `keybind_lstick_down`'s Vehicle: brake / reverse's (ESDF: E and D); the keys of every other
   row that keeps its keys in play (2.'s second kind; in multiplayer every changed row) leave
   the Vehicle actions (ESDF's S steers left in the jeep too). Otherwise a saved
   `cod3_binds_kbm_p1` without them gets their default keys, whatever the on-foot actions have.

## How it works (developers)

Files: `recomp/src/game/keybinds/` (`model.*` the actions and the stored form, plain code with
a unit test in `dev/agents/keybinds/tests/test_binds.cpp`; `keys.*` key names; `binds.*` the
game side), `[keybinds]` hunks in `cod3_app.cpp`,
`cod3mp_app.cpp`, `game/button_prompts.cpp` (the prompts and the pad hook, both programs), `ui/hud_overlay.cpp`,
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
  reads (the `GetPadState` hook in `button_prompts.cpp`, both programs; since 2026-10-20
  multiplayer's `pad_state_mp.cpp` isn't built), for the pad the game reads.
- Vehicles: `ButtonAlias_Value` (campaign 0x82467710, cod3mp 0x824829C0 through
  `campaign_map.tsv`; r3 the alias context `*kAliasContexts + context * 328` (campaign
  0x82A2AB44, cod3mp 0x82BCC800), 0 Vehicle, 1 VehicleTank; r4 the pad; r5 the alias: 0 Gas,
  1 Reverse, 2 HandBrake, 3 AlignTurret, 4 FireCoax; cod3mp reads a 5 too, which no action
  answers) is hooked. r4 is the controller (the local client's, or the input's active pad at
  input + 24; cod3mp's machine-gun read 0x82591450 passes input + 24); cod3mp counts only reads
  with the keyboard player's controller. Each (context, alias) read counts for 15 pad polls in
  play and 250 ms, whichever is longer; while it counts, the key of the Vehicle action for that
  alias is that action's (the listener looks a key up in the vehicle table first; Vehicle:
  change view while the vehicle reads any alias), and the hook makes the game's answer 255
  while the action is held (every threshold the vehicles use: Gas > 150, Reverse != 0,
  HandBrake, AlignTurret and FireCoax > 50). The controller's own answer stays when no key holds
  it. The tank's think (0x8256A2A8) reads FireCoax every frame and fires its machine gun above
  50. Inside an interaction, a pad-mode press of a button the reading vehicle's alias table maps
  to an alias with a Vehicle action is dropped, and prompts show that action's key for it.
- The view switch: the camera select (campaign 0x82447D48, cod3mp 0x82462C68) reads the right
  stick click's pressed edge (`bl 0x820E86A8`, entry 12; cod3mp `0x820FA6E8`); a mid-asm hook right
  after it (`Cod3VehicleViewPressed`, campaign 0x82448084, cod3mp 0x82462F48 through
  `codmp_campaign.toml`) answers 1 once per Vehicle: change view press (taken within 2 pad
  polls). In cod3mp only for the keyboard player's local client (r29 + 480).
- Testing: `--cod3_test_vehicle_hold=tank_gun` (command line only; elsewhere it is refused with a
  log line) holds the named Vehicle actions' aliases in play as if their keys were down (devrun
  runs can't press keys).
- Releases always go; a menu opening, an overlay, focus loss or the switch going off lets
  every held action go; when play goes on, keys still held press their held actions again
  (fire, aim, sprint, lean, the binoculars ...; a use, a grenade or a toggle waits for a new press). The frag's throw-back (it holds `+activatereload` with key 211,
  which `-grenadeattack` never lets go) is let go with the frag key.
- The prompts show the action's key (both programs; in multiplayer only to the player on the
  keyboard and mouse): a controller button's glyph becomes the key
  of the action its binding is ("~r1" fire: `[MOUSE1]`), and the game's action words
  (`[HOLD_USE]`, `[RELOAD]`, `[STANCE_DOWN]` ...) read "Press and hold [F]".

To add an action: a row in `model.cpp` `kDefs` (in `Action`'s order, with its group), the
enum value in `model.h`, its command; nothing else (the game side finds its handler). A vehicle
action: group Vehicle, delivery `kAlias` with the alias id, its id as the command.

## Not done yet

- Steering stays the stick rows (A / D, and WASD for the tank, through the left stick rows).
- The vehicle cameras' own look buttons (LB / RB look states and the right stick in the toggled
  view, 0x8243EAA0) and the left stick click that also fires the tank's machine gun (0x8255B900)
  read the controller directly; the mouse looks through the right stick rows as before.
- Interactions (a mounted gun, a battle action, the jeep) still press controller buttons for
  on-foot actions (above), except the buttons the jeep reads as its own controls.
- The tank's zoom, if it has one on the aim button: MOUSE2 is the machine gun in the tank by
  default (World at War's tanks), so the aim key doesn't reach the tank (owner check).
- Movement and look as actions (they are the stick rows, which vehicles and stick hints need).
- Press-to-capture and the game-menu KEY BINDINGS page (PLAN 7), controller rebinding (PLAN 6).
- The aim rule for hold and toggle across devices (PLAN 5, the `IN_SpeedDown` hook): toggled
  aim with the mouse, then holding the controller's LT, stops aiming while held.
- Multiplayer's console `bind` for actions.

## What to check on the PC

Build with the setup tool (2.13.0), then in a campaign level and in a multiplayer match:

1. R reloads without using, F uses (pick up a weapon, open a door) without reloading.
2. C toggles crouch, CTRL prone, SPACE jumps; Q / E lean; the wheel and 1 / 2 switch weapons.
3. G and 4 throw their grenades; a grenade thrown back with G doesn't leave "use" held.
4. MOUSE2 aims while held; with "Aim toggle" on, each press aims or stops.
5. In the pause menu and the main menu the keys still move and select (SPACE, ENTER, the
   arrows); after closing the menu the keys act again.
6. On a mounted gun the mouse fires as before (controller buttons). In the tank (Black Baron)
   MOUSE1 fires the cannon, MOUSE2 the machine gun, R aligns the turret and V switches the view;
   on foot again MOUSE2 aims and R reloads. The log has "cod3: binds: the tank reads its
   controls (AlignTurret, FireCoax): its keys count now (...)"; W / S still drive the tank. In a
   jeep W / S / SPACE are gas, brake and handbrake, and MOUSE1 no longer drives it.
   Rebind one in F2 (Vehicle: Tank: machine gun = MOUSE4) and it fires with that key only.
7. A controller at the same time: everything still works on it.
8. With `cod3_debug_binds = true` the log has `cod3: binds: fire down (+attack, holder AUX1,
   direct)` lines, `cod3: key binds: 39 of 39 actions call the game's own commands` and, in the
   tank, "cod3: binds: the tank's FireCoax read: held by its key (the controller's 0)" when its
   key is pressed.
