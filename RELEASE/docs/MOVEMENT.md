# Movement: sprint in the campaign, the CoD4 movement style, and server_sided_protected

**Status (2026-10-16): built and tested in the campaign (Saint-Lô, section 9);
the CoD4 style moves in CoD4 PC's 125 fps steps (4.2).**
Sprint (hold and toggle, keyboard and controller) and the CoD4 sprint and
speeds were measured in game; bounces are untested (section 10).
2.10.13: bounces happen only for a jump falling onto a steep slope (a walk or
strafe up an uneven incline used to lift the player; 4.3).
The multiplayer side (cod3mp, built from the same sources) is designed here, with its
addresses, but CoD3's multiplayer is peer to peer, so its transport
(section 6) is to be redone; none of this is compiled into cod3mp yet.

What the owner asked for:

- Sprint in the campaign, as a setting of its own (on/off), with **hold** or
  **toggle** as the player's choice. A toggle ends when you stop moving
  forward, aim, fire or press sprint again. No repeated fake presses.
- A **Call of Duty 4 movement style**, off by default (the game moves exactly
  as on the console), made of three parts, each on/off: *Sprint like CoD4*,
  *Speeds and feel*, *PC jump tricks*.
- Every setting has a row in **F2** and in **the game's own Options menu**,
  with the same values in both.
- A new, general dvar category, **`server_sided_protected`** (not sv_cheats):
  the host of a match sets it, every player's machine uses the server's value
  during the match and gets its own value back afterwards, and nothing on a
  player's machine can change it during the match. Its first member is
  **`alternate_movement`** 0/1/2.

Contents:

1. [Settings](#1-settings)
2. [What CoD3's movement is](#2-what-cod3s-movement-is)
3. [Sprint in the campaign](#3-sprint-in-the-campaign)
4. [The Call of Duty 4 style](#4-the-call-of-duty-4-style)
5. [server_sided_protected](#5-server_sided_protected)
6. [alternate_movement in a match (multiplayer)](#6-alternate_movement-in-a-match-multiplayer)
7. [Where the code is](#7-where-the-code-is)
8. [How to test it](#8-how-to-test-it)
9. [Test results](#9-test-results)
10. [Known limits](#10-known-limits)
11. [Sources](#11-sources)

---

## 1. Settings

### 1.1 The player's own settings

Saved in `cod3.toml` (category `cod3/Movement`), shown in **F2 > Game >
Movement** and in game in **Pause > Options > MOVEMENT OPTIONS**, and set in
the ~ console by name (`cod3_sprint_toggle 1`).

| Setting | Values (default) | Row (F2 / Options) | What it does |
|---|---|---|---|
| `cod3_sprint` | on / off (**on**) | Sprint / SPRINT | Sprint in the campaign. Off: the sprint keys do nothing, as on the console (Shift still holds your breath while aiming). On changes nothing until sprint is pressed. Multiplayer always has sprint. |
| `cod3_sprint_toggle` | off = hold / on = toggle (**hold**) | Sprint button / SPRINT BUTTON | Hold: sprint while the button is held. Toggle: press once to sprint (section 3.3). |
| `cod3_cod4_movement` | off / on (**off**) | Movement style / MOVEMENT STYLE: COD3 / COD4 | Your movement style. Off (COD3) is the console game, number for number. |
| `cod3_cod4_sprint` | on / off (on) | CoD4 sprint / COD4 SPRINT | Part 1, only with the CoD4 style. |
| `cod3_cod4_speeds` | on / off (on) | CoD4 speeds / COD4 SPEEDS | Part 2, only with the CoD4 style: CoD4's strafe and back speeds and CoD4 PC's movement at 125 fps (its air strafing and jump height) at any frame rate (4.2). |
| `cod3_cod4_jumps` | on / off (on) | CoD4 jump tricks / COD4 JUMP TRICKS | Part 3, only with the CoD4 style. |
| `cod3_debug_movement` | off / on (off) | (none) | Logs the player's movement four times a second, for tests. |

The parts default to on, so choosing the CoD4 style gives all of CoD4 at once;
each can then be switched off on its own. While the style is Call of Duty 3,
the parts' rows say "Only with the Call of Duty 4 movement style" and don't
change.

Every change applies at once, in the middle of a level (the movement code
reads the settings for every step), and is saved to `cod3.toml` right away,
whichever of F2, the game's Options page or the console made it.

### 1.2 The match's settings (server_sided_protected, section 5)

Shown in **F2 > Game > Server**, a tab only the host sees (in the campaign,
or while hosting a match; owner 2026-10-20: "the server sided dvars should be
their own tab, and only accessible if they are host"), and in the console.

| Setting | Values (default) | What it does |
|---|---|---|
| `alternate_movement` | 0 / 1 / 2 (**2** in the campaign, **0** in multiplayer) | 0: everyone moves as in CoD3 (the console). 1: everyone moves CoD4 style, with the parts below. 2: each player's own Movement style and parts. |
| `alternate_movement_sprint` | 0 / 1 (1) | Under 1: CoD4 sprint for everyone. |
| `alternate_movement_speeds` | 0 / 1 (1) | Under 1: CoD4 speeds for everyone. |
| `alternate_movement_jumps` | 0 / 1 (1) | Under 1: PC jump tricks for everyone. |

**Decision: the three parts are server_sided_protected too.** Under mode 1 the
host's `alternate_movement_*` apply to everyone; under mode 2 each player's
own `cod3_cod4_*` apply; under mode 0 none apply. A server that wants CoD4
movement without the bounces sets `alternate_movement 1` and
`alternate_movement_jumps 0`.

**In the campaign you are the host**: there is no match, so these behave as
normal settings, and the default 2 means your own Movement style applies.
Setting 0 or 1 in the campaign works as described; most players only need
the Movement style row.

### 1.3 The rule in one place

```
mode = alternate_movement        (the server's value in a match, your own otherwise)
mode 0: Call of Duty 3
mode 1: Call of Duty 4 with the parts alternate_movement_*
mode 2: your cod3_cod4_movement: Call of Duty 4 with the parts cod3_cod4_*, or Call of Duty 3
sprint: cod3_sprint in the campaign, always in multiplayer
```

`EffectiveRules()` in `src/game/movement/rules.cpp` is this rule, used by
both programs.

---

## 2. What CoD3's movement is

- **Only the server moves players.** In the campaign, `ClientThink_real`
  0x8256C260 calls `Pmove` 0x82496978 once per usercmd (`Pmove` has no other
  caller), which runs `PmoveSingle` 0x824960D0. The client game's prediction
  (`CG_PredictPlayerState` 0x8244AAE0) replays usercmds through
  `PM_UpdateViewAngles` 0x8248DF18 only: **clients predict their view angles,
  not their position.** The multiplayer program is the same (`Pmove`
  0x824B4270, called only from its game module; its `CG_PredictPlayerState`
  0x82465000 calls only `PM_UpdateViewAngles` 0x824AC818). So
  `docs/MULTIPLAYER.md` 4.4's "clients only predict their own movement" is
  about view angles only.
- **It is Call of Duty 2's movement**, as Call of Duty 4's is: Quake III's
  `PmoveSingle` structure with CoD2's stances, sprint, landing slowdown and
  "jump step".
- **Almost every number is a constant in code, not a dvar**: friction 5.5,
  stop speed 100, acceleration 9 / 12 / 19 (stand / crouch / prone), air
  acceleration 1.0, jump height 39 (velocity `sqrt(78 x g_gravity)`), step
  18 (prone 10), the landing slowdown x0.65 or x0.5 for 1.8 / 1.2 s, ladder
  push 128, jump spread +64. They sit in constant pools that hundreds of
  functions share, so **the port never patches them**; it hooks the function
  that uses them.
- **The real dvars**: `g_speed` 190 (the level scripts use it, so the port
  never changes it), `g_gravity` 800, `pmove_fixed` / `pmove_msec`, the
  `bg_viewheight_*`, `bg_fallDamageMin/MaxHeight` and `bg_*_yawcap` dvars.
- **The speed scales are per-player fields** of the playerState, set at spawn
  by `ClientSpawn` 0x8256E370: ADS 0.4, run 1.0, sprint 1.6 (forward only),
  prone 0.15, crouch 0.65, strafe 1.0, back 1.0, stamina 1.0. The CoD4 speeds
  are applied by changing these for the length of one `Pmove` call and
  putting the game's values back after it (section 4.2).

The functions, the playerState and usercmd layouts and every multiplayer
equivalent are in `src/game/movement/program_sp.h` / `program_mp.h` and in
`dev/agents/movement/PLAN.md` section 2.

---

## 3. Sprint in the campaign

### 3.1 What was there

The campaign's movement code has CoD3 multiplayer's sprint complete:
`PM_UpdateSprint` 0x824655B0 sets the sprinting flag (pm_flags 0x10000) from
the usercmd's sprint button (bit 2), `PM_UpdateStamina` 0x82477F28 drains and
recovers stamina, `PM_WalkMove` scales the forward speed by the sprint scale,
a jump while sprinting costs stamina, and the client game draws the gun in
the weapon's sprint pose while the flag is set. Only the input was cut: the
`+sprint`, `-sprint`, `+sprintbreath` and `-sprintbreath` commands are empty
functions (`blr`) in the campaign. In the multiplayer program they press
kbutton 15, which `CL_CmdButtons` 0x82509BB8 turns into bit 2.

The campaign's stamina numbers were never tuned: 1.5 s of sprint, and the
"out of breath" sound event (163) every frame below half stamina.

### 3.2 What the port turns on

- `+sprint` / `-sprint` press and release **the game's own sprint button**
  (kbutton 15, `IN_KeyDown` 0x8250BB50 / `IN_KeyUp` 0x8250A110), exactly as
  the multiplayer program does. Each key is its own holder, so a pad and a
  keyboard never release each other.
- `+sprintbreath` / `-sprintbreath` (World at War's Shift): hold breath while
  aiming, sprint otherwise.
- **The stamina** is the game's own structure with tuned numbers
  (`StepStamina` in `rules.cpp`):

| | Campaign (retail, never used) | CoD3 movement with sprint on | CoD4 sprint (part 1) |
|---|---|---|---|
| Sprint time | 1.5 s | **9.2 s** (drain 0.108525/s) | **4 s** (`player_sprintTime` 4) |
| Recovery | 0.333/s after 1 s | **6 s** to full (0.165424/s) after 1 s | **4 s** to full (estimate), no pause (`player_sprintRechargePause` 0) |
| Recovery while sprint is held | no | no (as the game) | yes |
| Needed to start | 25% | 25% | 25% = 1 s of 4 (`player_sprintMinTime` 1) |
| Out of breath sound | every frame below 50% | once, at empty | once, at empty |
| Forward speed | x1.6 | x1.6 (the campaign's own) | **x1.5** (`player_sprintSpeedScale` 1.5) |
| Sideways while sprinting | x1 | x1 | **x0.667** (`player_sprintStrafeSpeedScale`) |
| Needs forward | any | any forward | **105 of 127** (`player_sprintForwardMinimum`) |
| Jump while sprinting | -0.667 stamina | -0.667 stamina (as the game) | free |
| Sprint again after running out | | let go of sprint (the stamina comes back only then) | let go of sprint and press it again (it comes back while held, but holding doesn't restart) |

The CoD3 column uses CoD3 multiplayer's own tuned numbers (read in
`codmp_xenonf.xex`, `PM_UpdateStamina` 0x82495980).

- **Aiming or firing stops a sprint, and so does not moving forward**, in
  both styles (a wrapper of `PM_UpdateSprint` decides before the game's own
  test, so a sprint that can't start never cancels a toggled aim). With hold,
  sprint resumes when you stop aiming while still holding it, as in CoD4.

### 3.3 Hold and toggle

- **Hold**: the button is down while held.
- **Toggle**: one press starts sprinting, only while moving forward (a press
  while standing still is ignored, so a toggle never waits armed). It ends
  when any of these happens:
  - you stop moving forward or start backing up;
  - you aim (the usercmd's aim bit) or fire;
  - you press sprint again;
  - you crouch or go prone;
  - the server stops sprinting for a quarter second although the button is
    held (out of breath, a mounted gun, a vehicle);
  - you die, use noclip or ufo, mount a gun or enter a vehicle;
  - the game stops making commands for two seconds (a menu, the pause
    menu, a level change; long enough that a slow frame doesn't end it).
- The latch is **one `IN_KeyDown` and one `IN_KeyUp`** under its own key id
  (AUX3, key 241, which no pad or keyboard sends). Nothing is ever pressed
  twice or repeated. The end is checked once per usercmd, right after the
  game finished it (`CL_FinishMove` 0x82509A90), and the sprint bit is cleared
  in that same usercmd, so it ends at once.
- A real key released while the latch holds does nothing.

### 3.4 Which buttons sprint

- **Keyboard**: **Shift**, the *Sprint / hold breath* action (World at War's
  Shift): it sprints, and holds your breath instead while you aim down the
  sights or look through the binoculars. The binoculars have their own key
  (B), so Shift never brings them up. Change the key in F2 > Controls > Key
  bindings, with the console's `bind <key> +sprintbreath` (or `+sprint` for a
  key that only sprints), or in `cod3.toml` (`cod3_binds_kbm_p1`). These are
  the keybinds work's actions (`src/game/keybinds/`); they call the same
  `+sprintbreath` / `+sprint` as below.
- **Controller**: **L3 while pushing the left stick forward** (more than half)
  sprints; L3 otherwise keeps its layout action (binoculars in layouts A, B
  and D; while looking through the binoculars L3 is the game's steadying).
  This is CoD4's and CoD3 multiplayer's L3. Layout C binds L3 to `+stance`, so
  it has no controller sprint yet.
- **Any key**: `bind <key> +sprint` in the console.

### 3.5 The gun while sprinting

The client game moves the gun by the weapon's own **sprint offset**
(`sprintMoveF/R/U`, weaponDef +1260) instead of its walking offset while the
sprinting flag is set, scaled by your speed (full at the walk speed). That
is the whole sprint pose in the campaign: the weapon files have no sprint
animations and no other code reads the sprint fields with the sprint flag.
The campaign's weapons carry these values (the Thompson: -14, -10, -14: the
gun pulled back, left and down). A weapon whose offset is all zero gets a
lowered carry of the port's own (-1, 1, -1.5) the first time it sprints, so
the gun never just freezes in place (`kSprintPoseFallback`, `rules.h`).

---

## 4. The Call of Duty 4 style

CoD3 and CoD4 movement both come from Call of Duty 2's, and the public CoD4
1.0 defaults match CoD3 on most numbers: `g_speed` 190, `friction` 5.5,
`stopspeed` 100, `jump_height` 39, `jump_stepSize` 18, `jump_ladderPushVel`
128, `jump_spreadAdd` 64 and the landing slowdown (`jump_slowdownEnable` 1).
So the three parts change little code. Every value below names its source
(section 11); no CoD4 code was used.

### 4.1 Part 1: Sprint like CoD4

| What | CoD4 | In the port |
|---|---|---|
| Sprint time | `player_sprintTime` 4 | stamina drains in 4 s |
| Minimum to start | `player_sprintMinTime` 1 | 25% (the game's own start test) |
| Recharge pause | `player_sprintRechargePause` 0 | none |
| Recharge rate | not a public dvar | **estimate: 4 s from empty to full** (to be measured in CoD4 with a stopwatch) |
| Speed | `player_sprintSpeedScale` 1.5 | the sprint scale x1.5/1.6 (relative to the game's, so a script's change is kept) |
| Sideways | `player_sprintStrafeSpeedScale` 0.667 | the stick's sideways part x0.667 while sprinting |
| Forward only | `player_sprintForwardMinimum` 105 | no sprint below 105 of 127 forward |
| Aim or fire stops it | CoD4 behavior | as in both styles (3.2) |
| Gun lowered | CoD4's sprint animations | the weapon's own sprint offset (3.5) |
| Sprint again after running out | let go and press again | the same: holding sprint doesn't restart it |
| Jump while sprinting | keeps sprinting | no stamina cost |

### 4.2 Part 2: Speeds and feel

| What | CoD3 | CoD4 | In the port |
|---|---|---|---|
| Walk speed | `g_speed` 190 | `g_speed` 190 | the same |
| Strafe | 1.0 | `player_strafeSpeedScale` 0.8 | **x0.8** |
| Backward | 1.0 | `player_backSpeedScale` 0.7 | **x0.7** |
| Friction / stop speed | 5.5 / 100 | `friction` 5.5 / `stopspeed` 100 | the same |
| Acceleration | 9 / 12 / 19 | not a dvar | the same (same lineage) |
| ADS / crouch / prone speed | 0.4 / 0.65 / 0.15 | not dvars | the same (not public) |
| Jump height | 39 | `jump_height` 39 | the same |
| Step height | 18 | `jump_stepSize` 18 | the same |
| Landing slowdown | x0.65 / x0.5 | `jump_slowdownEnable` 1 | the same mechanism |
| Fall damage | 256 / 480 | `bg_fallDamageMin/MaxHeight` 128 / 300 | **not changed**: the campaign's balance; the dvars are cheat-protected in CoD3 |

How: a wrapper of `Pmove` multiplies the playerState's strafe and back
scales (and, with part 1, the sprint scale) for the length of the call and
puts the game's own values back after it. The playerState, savegames,
snapshots and anything a level script set stay the game's; no dvar changes.

#### CoD4 PC's air strafing: the 125 fps movement steps

Owner, 2026-10-16: "the cod4 values need the exact same air strafe system";
asked how CoD4's frame-rate dependence should work: **fixed at 125 fps**.

CoD3's air move is already the same Quake III-family code as CoD4's (both
come from Call of Duty 2's): no friction in the air, the wish direction from
the view and the stick, `PM_Accelerate` 0x82478358 at 1.0 (with CoD2's
floor of 100 on the wish speed it accelerates with), then the step slide move
with gravity (`PM_AirMove` 0x82490B70). What the console game doesn't have is
what CoD4 PC does around that code, and it is what makes CoD4's air
strafing (public descriptions, section 11):

- the movement runs once per client frame, in whole milliseconds;
- after every step the velocity is rounded to whole units (Quake III's
  `SnapVector`).

The rounding's error is about the same every frame, so it adds up, and what
it adds depends on the step's length. At 125 fps (8 ms) a jump reaches
about 41.3 units instead of 39 and the air keeps adding a little forward
speed; at 250 and 333 fps it does other things (higher jumps, more turning,
or diagonal drift), which is why CoD4 players pick their frame rate.

With CoD4 speeds on, the port moves the player in **CoD4 PC's 125 fps steps
whatever the real frame rate**, so it is the same at 60, 144 or 240 fps and
the same for everyone:

- Each usercmd's time and what was left over from the ones before make
  whole 8 ms steps, run by the game's own `pmove_fixed` loop in `Pmove`
  0x82496978 (`pmove_fixed` 1 and `pmove_msec` 8 for the call only); what is
  left (under 8 ms) waits for the next usercmd. At most 1000 ms at once, as
  the game's own limit.
- After every step (`PmoveSingle` 0x824960D0) the velocity is rounded to
  whole units, halves to even as on a PC's FPU.
- The playerState's commandTime is the usercmd's time again after the call,
  so the game's own time keeping (ClientThink, the server, the client's
  view-angle prediction) is as before; a usercmd too short for a step still
  updates the view angles (`PM_UpdateViewAngles` 0x8248DF18).
- Walking goes through the same steps, as it does in CoD4 PC.

Measured in Saint-Lô (section 9): every CoD4 jump reaches 41.269 units
(CoD3: 38.9-39.0), the same number a model of CoD4's steps gives, and CoD4
PC at 125 fps clears a 41-unit wall but not a 42-unit one (the public
measurements); a straight jump gains a little forward speed (190 to 191.2,
CoD3: none) and a strafe jump turning right gained 22 units/s in one jump.
What can't be compared with CoD4 itself (no CoD4 code is used, the owner's
rule): the rest of its air move, assumed the same as CoD3's from the common
CoD2 code. One difference to know: CoD3's air move scales the wish speed by
the ADS and run scales only, so CoD4's strafe 0.8 and back 0.7 apply on the
ground (CoD4's own air scaling isn't public).

### 4.3 Part 3: PC jump tricks

- **Strafe jumping**: both styles gain speed by strafing with the mouse
  (Quake III's air acceleration, 1.0 at the full wish speed); the CoD4
  style's comes with CoD4 PC's 125 fps steps (part 2, 4.2), so it behaves as
  CoD4's at 125 fps. The port's raw mouse makes it practical.
- **Bounces**: by public description, CoD4's `PM_StepSlideMove` *projects*
  the velocity onto a surface it lands on during the step move, where CoD2's
  (and CoD3's) *clips* it; a non-walkable slope (its normal's z from 0.3 up
  to the walkable 0.7) then turns a fall into a slide up and away: a bounce.
  The port does that at CoD3's two clip calls in the step move (0x8248FB6C and
  0x8248FCD4): the velocity is projected onto the slope keeping its speed.
  Walls (steeper than 0.3) and walkable ground keep the game's clip.
  - **Only a jump falling onto the slope** (`BounceApplies` in
    `rules.cpp`): inside the air move (`PM_AirMove`), with the jump flag set,
    falling, and below the jump's top (its start + 39: the game's own jump
    step, which the public description ties the projection to). The step
    move also runs inside the walk move, and its step down there often
    meets a steep bit of an uneven floor (rocks, roots, seams of the
    terrain): projected, a walk at 190 into a 0.5 facet leaves at about 165
    u/s upward, about 17 units (a foot and a half). That was the owner's
    report on 2026-10-16 ("randomly raise a couple feet in the air every
    couple seconds ... pressing strafing keys, while on a slight incline",
    Forest); fixed in setup tool 2.10.13. The second clip call is the walk
    move's only (disassembly: it runs only while on the ground), so it never
    bounces now.
  - With `cod3_debug_movement`, a clip on such a slope left to the game
    logs `cod3: no bounce (<why>)` (the walk move's step down, no jump,
    rising, above the jump's top), at most four a second.
- **CoD4 PC's frame-rate tricks**: the CoD4 style is CoD4 at 125 fps at any
  frame rate (4.2). The 250 and 333 fps behaviors are not offered: the
  port never ties the game's physics to the player's frame rate (owner's
  choice: fixed 125 only).

---

## 5. server_sided_protected

### 5.1 The rule

A dvar in this category is the match's, not the player's. The host (the
server's admin) sets it. While a player is in a match, the server's value
applies on that player's machine and **nothing on that machine can change
it**, neither for the server nor for that player's own game. The player's own
value is kept aside and comes back when they leave or quit. Outside a match
(the campaign, the menus) it is a normal setting. It is not sv_cheats: a
protected dvar is not a cheat, doesn't show the CHEATS USED mark and isn't
announced in the cheats column.

The list is **one table**, `Entries()` in `src/game/protected_dvars.cpp`.
Each entry has the name, its integer domain, the default of the player's own
value in this program, the value a match uses when its server didn't send one
(alternate_movement 0), and a one-sentence help. Adding a dvar later is one
entry there plus its port setting (`REXCVAR_DEFINE_INT32` with the validator,
in the same file); the policy, the console, F2 and the transport need
nothing else.

Aim assist's match rule is in the list too (claude-9b, 2026-10-20):
`sticky_aim` (0 off for everyone, 1 each player's own choice) and
`aim_slowdown` (0 to 100% of the game's pull, never more). docs/DVARS.md
(Aim) explains them. Every entry is on F2 > Game > Server, shown only to the
host, with its own "Reset match rules to defaults"; "Reset movement
settings to defaults" on the Movement tab no longer touches them.

### 5.2 Two values

- **Own value**: the player's setting (a port setting in `cod3.toml`).
  Changed only when no match runs, or by the host for the host's own match.
- **Server value**: memory only, from the server's settings string while
  this machine is a guest in a match; dropped when the match ends, the
  player disconnects or quits. The own value was never touched, so it is back
  by construction, and `cod3.toml` never holds a server's value.
- All port code reads the **effective value** (`protected_dvars::Get`): the
  server's during a match, the own value otherwise.

### 5.3 Every path that could change it during a match

| Path | What stops it | What the player is told |
|---|---|---|
| The ~ console: `alternate_movement 0`, `set` / `seta` / `sets` / `setu` / `reset` / `toggle` / `togglep` / `setfromcvar` | `HandleConsoleLine` (in `ui/game_console.cpp`, before every other handler) | `alternate_movement is set by the server in this match (1 (Call of Duty 4 movement for everyone)); your own value, 2, comes back when you leave.` |
| World at War names and aliases | the same: a World at War alias whose target is protected is checked under the target's name (none exists today) | the same |
| `cod3_autoexec` | the same console path | the same |
| Config files, `exec`, the console config, guest scripts, the game's menus | they reach the game's `Cvar_Set2`; the hook refuses a protected name **always** (in a match or not: it is the port's setting, never a game dvar) | `alternate_movement is a port setting: set it in the ~ console ..., in F2 > Game > Movement or in the game's Options; a config or a script can't.` (in a match: the line above) |
| F2 rows | the rows ask `IsLocked`: shown with the server's value, disabled | the reason under the row |
| The game's Options (MOVEMENT OPTIONS) | the rows the server sets don't step | the line under the rows: "Set by the server in this match." |
| Any port code (`rex::cvar::SetFlagByName`) | the port settings' validator refuses while locked | a log line |
| The command line, `cod3.toml` | read only at start, before any match: they set the own value, checked against the domain (an out-of-range value goes back to the default with a log line) | a log line |
| Another player's command | refused always (`cheats::Check` with `Source::kNetwork`, and `RequestSet`) | `alternate_movement: not allowed from another player.` |
| The server reading a client's value | never: values travel server -> client only | |

Each refusal is logged (`cod3: protected: refused ...`), at most once a second
per name and path (a script or a key could repeat it every frame).

### 5.4 The server never takes these from a client

The values flow from the server to its clients only (section 6.2). The only
thing a client sends is its mode-2 movement preference (6.3), which is
checked strictly, applied to that player alone and never becomes a dvar. The
server simulates every player's movement with the server's rules (section 2:
clients don't predict positions), so a tampered client changes nothing but
its own input, view and menus: it gains nothing.

---

## 6. alternate_movement in a match (multiplayer)

Designed here for cod3mp (the multiplayer program, built from the same sources), with the
shared code ready; not wired, and nothing of it is compiled into cod3mp yet.

> **To be redone for the peer-to-peer model.** CoD3's multiplayer is peer to
> peer: between consoles each one moves its *own* players and sends their
> state to the others; there is no server that simulates everyone. So the
> transport below (systeminfo, userinfo) becomes the peer equivalent: the
> host's (a player host or, later, a dedicated server) `alternate_movement`
> and parts must reach every peer, each peer applies them to its own players
> at the same game time, and peers can check others' movement against the
> match's rules. That transport is the multiplayer work's. What stays: the
> modes, the rule that nothing on a player's machine can change the match's
> values (section 5), and the announcement. Matches are PC only (the owner,
> 2026-10-16), so there is no Xbox 360 player rule.

### 6.1 Modes

| Value | Effect |
|---|---|
| 0 | Everyone moves as in CoD3 (the console). The multiplayer default. |
| 1 | Everyone moves CoD4 style, with the host's `alternate_movement_*`. |
| 2 | Each player's own Movement style and parts. |


### 6.2 Transport and timing (the first design; see the note above)

- The host adds the protected values to its systeminfo string
  (`protected_dvars::ServerInfoPairs()`: `\alternate_movement\1\alternate_movement_sprint\1...`).
  Late joiners get them with the gamestate.
- CoD3's `CL_SystemInfoChanged` 0x8250E518 reads only `sv_serverid`,
  `sv_cheats` and the pak keys; unlike Quake III 1.32 it does not copy other
  keys into dvars, so an older client ignores the extra keys.
- A PC client's hook of `CL_SystemInfoChanged` passes the string to
  `protected_dvars::ApplyServerInfo()`, which takes only the listed keys,
  each checked against its domain (anything else is dropped with a log line).
  Leaving the match calls `protected_dvars::LeaveMatch()`.
- **A change takes effect immediately**: the host's change applies at the
  start of the next server frame (server time T) and the systeminfo
  configstring goes out in that frame; a client executes server commands in
  order with the snapshots, so its value changes at the first snapshot with
  serverTime >= T, the same command time as the server. Positions come from
  the server only, so there is nothing to correct; a future position
  prediction would see at most one small correction while the value
  arrives.

### 6.3 Mode 2: the player's choice

- The client sends its choice as the userinfo key **`cod3mv`**: `"3"` (CoD3)
  or `"4"` followed by four 0/1 digits: sprint, speeds, an unused digit
  (sent as 0, ignored when read) and jumps (for example `"41001"`).
  `EncodePreference` / `DecodePreference` in `rules.h`.
- The server's `ClientUserinfoChanged` hook passes it to
  `movement::PlayerPreferences::Update()`: exactly those forms, anything else
  is refused (the player keeps CoD3 or their last valid choice), at most one
  change per 5 seconds per player, applied to that player alone at the next
  frame. A client without the key (an older port) is Call of Duty 3.
- The server's `Pmove` wrapper takes each player's rules from that table
  (`RulesFor(client)` in the glue), so every player is simulated with the
  server's view of their choice.

### 6.4 The announcement

Changes are told as **a normal server message** (the game's own `print`
server command, shown in the HUD's message lines and the console; not the
cheats column): `protected_dvars::SetServerMessageSender()` is cod3mp's hook.
Examples: "Movement: Call of Duty 4 movement for everyone (set by the
host).", "Movement: each player's own movement style (set by the host)."
The text is plain, never run as a command.

### 6.5 What cod3mp adds

- `src/game/movement/program_current.h` naming `kMultiplayer`
  (`program_mp.h`; its zeros are fields still to find there: the traces'
  pass entity, view angles, binoculars and hold-breath handlers, the player
  box and clip mask in `pmove_t`, the WeaponDef sprint offset), and a client
  index per moved player for the per-player tables (the campaign's
  `ClientIndex()` is always 0; playerState +128 is not a client number).
- `hooks_mp.cpp`: the same hooks at the multiplayer addresses (2.4 of
  `dev/agents/movement/PLAN.md` has them all), with `RulesFor(client)` from
  `PlayerPreferences`, and on its client the sprint button hooks
  (`IN_SprintDown` 0x825273A8 already presses kbutton 15; only the toggle
  needs the `CL_FinishMove` hook).
- The systeminfo build (host), the `CL_SystemInfoChanged` hook (client), the
  `ClientUserinfoChanged` hook (host) and the `print` sender, or their
  peer-to-peer equivalents.
- `mp::IsMatchRunning()` / `mp::IsHost()` answering from the session
  (`game/multiplayer.h`), which the protected dvars already ask.

---

## 7. Where the code is

| File | What |
|---|---|
| `src/game/protected_dvars.{h,cpp}` | the server_sided_protected category: the list, own and server values, the console forms, the guest-write refusal, the multiplayer glue, the test match |
| `src/game/movement/rules.{h,cpp}` | the rules (no game addresses): effective rules, stamina, sprint conditions, the preference codec and table, info strings, the bounce projection |
| `src/game/movement/sprint_input.{h,cpp}` | the sprint button: hold and toggle |
| `src/game/movement/settings.{h,cpp}` | the player's settings, the rows (shared by F2 and the menu), F2's Movement section |
| `src/game/movement/menu_movement.{h,cpp}` | the rows' texts and steps for the game's Options menu |
| `src/game/movement/program.h`, `program_sp.h`, `program_mp.h`, `program_current.h` | the per-program address tables |
| `src/game/movement/hooks_sp.cpp` | the campaign's hooks (with CoD4 PC's 125 fps steps: `RunCod4Steps` and the `PmoveSingle` rounding) |
| `src/game/menu_fov.cpp` | the in-game Options page: VIDEO OPTIONS and MOVEMENT OPTIONS as two "sub pages" of the same copied rows |
| small hunks | `CMakeLists.txt`, `ui/game_console.cpp`, `ui/hud_overlay.cpp`, `game/cheat_watch.cpp`, `game/cheat_guard.cpp`, `src/cod3_app.cpp` |

---

## 8. How to test it

- **Console**: `cod3_sprint_toggle 1`, `cod3_cod4_movement 1`,
  `alternate_movement` (shows the value, the default, the domain and, in a
  match, who sets it), `+forward`, `+sprint`, `+speed` (aim), `+moveup`
  (jump).
- **`--cod3_debug_movement=true`**: four lines a second,
  `cod3: move t=<serverTime> pos=(x y z) hspeed=<u/s> vz stamina flags type fwd right up buttons latch rules`,
  plus a line for every sprint button press and release (with the key: -1
  typed, 220 the pad's L3, 239/240 the keyboard actions, 241 the toggle), every
  bounce, and each weapon's sprint offset the first time it sprints.
- **A pretend match** (test only): `--cod3_test_fake_match="0:alternate_movement=1;0:alternate_movement_jumps=0;120:end"`
  makes this machine a guest in a pretend match whose server sends those
  values (seconds after the level's first movement step), through the same
  parser a real server's string goes through. Read only at start, and never
  used while a real match runs.
- **The logic on its own**: `dev/agents/movement/tests/test_rules.cpp` (build line in
  its header) checks the rules, the stamina times, the sprint conditions, the
  preference codec and table, the info strings, the bounce projection and the
  toggle latch.

---

## 9. Test results

Cloud runs in Saint-Lô, 2026-10-16 (SwiftShader, rendering off for the
movement parts, so the game ran at about 25 frames a second; every number is
from the game's own player state, four samples a second of game time;
`dev/agents/movement/NOTES.md` has the details).

| What | Expected | Measured |
|---|---|---|
| Walk | 190 u/s | 190-197 |
| Sprint, Call of Duty 3 | x1.6 = 304 | 304-310 |
| Sprint time, CoD3 | 9.2 s (0.1085/s) | 0.1085/s; ran out after about 9.2 s |
| Recovery, CoD3 | 1 s pause, then 6 s (0.165/s); none while held | the same |
| Aim / fire while sprinting | stops it; held sprint resumes after | the same (aiming: 76-78 u/s) |
| Toggle | on with one press while moving forward; off on stop, aim, fire, press again, out of breath | each, one press and one release, nothing repeated |
| Keyboard (Shift, the real window path) | sprint; hold breath while aiming | the same (key 239); toggle with Shift taps |
| Controller L3 (scripted pad) | sprint with the stick forward, binoculars otherwise | the same (key 220) |
| Sprint off | the sprint keys do nothing | nothing |
| Sprint, CoD4 | x1.5 = 285 | 285-291 |
| Sprint time / recovery, CoD4 | 4 s / 4 s, no pause, also while held | 0.25/s / 0.25/s, no pause, while held |
| Held after running out, CoD4 | no new sprint until let go | none for 8 s held (full stamina after 4); pressed again: sprint |
| Strafe / back, CoD4 | x0.8 = 152 / x0.7 = 133 | 152-158 (in a circle) / 133 |
| Strafe / back, CoD3 | 190 / 190 | 190 / 190 |
| Jump while sprinting, CoD4 | no stamina cost, keeps sprinting | the same |
| The gun while sprinting | the weapon's sprint offset at speed | the Thompson goes down out of view at 200-300 u/s and comes back when blocked (speed 0) |
| A pretend match (server: mode 1) | the server's rules; every console form, `exec`, and the menus refused | rules "Call of Duty 4 (sprint, speeds, jump tricks)" while the own style was CoD3; all refused and logged; own value back afterwards; `cod3.toml` never held the server's value |
| Pause > Options > MOVEMENT OPTIONS | the rows, live and saved, by keys and by mouse | COD3 / COD4 fit; each change (keys, or clicks on the arrows) applies at once and is saved to `cod3.toml`; a CoD4 part's row is locked while the style is COD3 and says why |
| Jump height, CoD3 (runs j1, j2) | 39 | 38.87-39.00 (6 jumps) |
| Jump height, CoD4 125 fps steps | 41.27 (model; CoD4 PC at 125 fps clears 41, not 42) | 41.269 in all 6 jumps (the cloud's 25 fps: 5 steps per usercmd) |
| Straight jump at walk speed | CoD3: no gain; CoD4 at 125 fps: a little forward gain | CoD3 190.0 to 190.0; CoD4 189.9 to 191.2 |
| Strafe jump (right strafe, turning right at 140 deg/s), CoD4 | gains speed in the air | 191.4 at takeoff, 213.8 in the air |
| Back, CoD4 with the 125 fps steps | x0.7 = 133 | 133.5-133.7 |

---

## 10. Known limits

- CoD4's sprint **recharge rate** is not a public dvar: 4 s from empty to full
  is an estimate.
- CoD4's **ADS, crouch and prone speeds** are not public dvars: CoD3's are
  kept.
- **Fall damage** stays CoD3's (CoD4's 128 / 300 would change the campaign's
  balance).
- **Bounces** (PC jump tricks): a real bounce (a jump falling onto a steep
  slope) hasn't been seen yet. The 2.10.13 fix (only such a jump bounces)
  was checked on the owner's PC in Forest on 2026-10-18 (REA sandbox, run
  `20261018-015315-forest`, `dev/tools/rea/tests/forest_slopes.txt`): in
  two minutes of walking and strafing up its slopes with the CoD4 style the
  walk move's step down met a steep bit 39 times (slopes 0.36-0.69), every
  one left to the game's clip (`no bounce (the walk move's step down)`), the
  speed upward while walking stayed at most 80 units/s (walking uphill; a
  jump leaves at 237), no lift; nine jumps reached 41.27 units (two landed
  higher, 55-57).
- **The front end's Options** (main menu) has no MOVEMENT page yet; the
  rows are in game (Pause > Options > MOVEMENT OPTIONS) and in F2.
- **Layout C** binds L3 to `+stance`, so it has no controller sprint yet;
  the keyboard's Shift and `bind <key> +sprint` work in every layout.
- No sprint meter of the port's own and no extra camera bob while sprinting
  (CoD4's `player_sprintCameraBob`); the game's own footstep bob speeds up
  with the sprint scale, and its HUD shows the stance figure next to the
  compass while you sprint.
- Sprinting into a wall still uses stamina (the game's own rule: it looks at
  the stick, not at the speed).
- CoD4 PC's frame-rate tricks: only its 125 fps behavior, at any frame rate
  (4.2, 4.3). CoD4's own air scaling of the strafe and back speeds isn't
  public: CoD3's air move is kept (ADS and run scales only).

---

## 11. Sources

Game code (the owner's disc, read only): `default.xex` and
`codmp_xenonf.xex`; the functions were read in their disassembly (notes in `dev/agents/movement/`).

Quake III Arena's source (GPL; for understanding and names only, nothing
copied): `code/game/bg_pmove.c` (PmoveSingle, PM_StepSlideMove,
PM_Accelerate), `code/client/cl_parse.c` (CL_SystemInfoChanged),
`code/server/sv_main.c` (CS_SYSTEMINFO from CVAR_SYSTEMINFO).

Call of Duty 4, public information only (no CoD4 code was used):

- CoD4 1.0 dvar list (`player_sprintTime` 4, `player_sprintSpeedScale` 1.5,
  `player_sprintMinTime` 1, `player_sprintRechargePause` 0,
  `player_sprintStrafeSpeedScale` 0.667, `player_sprintForwardMinimum` 105,
  `player_sprintCameraBob` 0.5, `player_strafeSpeedScale` 0.8,
  `player_backSpeedScale` 0.7, `jump_height` 39, `jump_stepSize`
  18, `jump_ladderPushVel` 128, `jump_spreadAdd` 64, `stopspeed` 100,
  `friction` 5.5, `g_speed` 190):
  https://nextgenupdate.com/forums/call-duty-4-modern-warfare/65279-all-cod4-commands-dvars.html
- CoD4 command list with defaults (`jump_slowdownEnable` 1, `friction` 5.5,
  `jump_height` 39, `jump_stepSize` 18, `bg_fallDamageMinHeight` 128 /
  `MaxHeight` 300, `hud_fade_sprint` 1.7, `cg_sprintMeter*`):
  https://www.gamerconfig.eu/commands/call-of-duty-4-modern-warfare
- Bounces, the mechanism in prose (the step move projects instead of
  clipping; the 0.3 normal limit; the jumping flag not checked in CoD4). These
  pages also show decompiled code, which was **not** used:
  https://xoxor4d.github.io/research/cod4-doublebounce/ and
  https://xoxor4d.github.io/research/mw2-bounce/
- PC jumping techniques (strafe jumping, bounces on slanted surfaces,
  frame-rate dependent heights):
  http://egcodclan.blogspot.com/2015/07/a-guide-to-cod4-jumping-techniques-pc.html
- What each CoD4 frame rate does (125: forward acceleration in the air; 250:
  more turning, slightly higher; 333: highest, diagonal drift; only 1000/N
  frame rates exist): https://codpromod.fandom.com/wiki/FPS_Effects
- Measured CoD4 PC jump heights by frame rate (125 fps: a 41-unit wall
  cleared; 250: 42; 333: 46) and CoD4's strafe 152 / back 133 speeds:
  https://wiki.zeroy.com/index.php/Call_of_Duty_:_A_Study_on_FPS
- Why the frame rate changes jumps in Quake III engines (the velocity
  rounded to whole numbers every movement step, frame times in whole
  milliseconds): http://jolie.techa.free.fr/docs/ucguides/FAQFPSJumps.html
- Extreme Conditioning (CoD4's perk doubling sprint time; not used):
  https://callofduty.fandom.com/wiki/Extreme_Conditioning
