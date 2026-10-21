# Dvars

Dvars are the game's console settings: `cg_fov`, `timescale`, `r_fog` and a few
hundred more. This page lists every one Call of Duty 3 has (the campaign and
multiplayer), says what each does, and explains what the PC port adds around
them: help in the console, World at War and Call of Duty 4 names, and the
cheat protection.

Call of Duty 3's dvars are Quake III cvars: unlike World at War's, they carry
no description, no type and no range. The lines here are this project's own,
written from what the game does with each dvar (its code, its level scripts,
and runs on a PC). No text is taken from the game or from any other game.

## In the game

Open the console with `~` (Shift + `~` for the full console), or F2 > Console.

| type | what you get |
|---|---|
| `cg_fov` | its value, default and domain (World at War style), then what it does |
| `cg_fov 90` | sets it (cheat-protected dvars only with `sv_cheats 1`) |
| `help cg_fov` | what it does, its group, where the game has it, its value, default and flags |
| `help fog` | every dvar whose name or help has "fog" (several words narrow it: `help compass range`) |
| `find fov` | the dvars, World at War names and commands whose name has "fov" |
| `cvarlist` / `dvarlist` / `dvardump` | every dvar with its flags and value (`cvarlist cg_` filters) |
| `cmdlist` | the game's commands (the game's own `cmdlist` prints nothing in this build) |
| `dvarexport` | every dvar (name, flags, value, default) and command as a table in `userdata/dvars.txt` |
| `condump <file>` | the console's text into `userdata/<file>.txt` |

The completion box shows the help line too: under the value and domain when
one dvar matches, and under the list for the highlighted one.

The flag letters (`cvarlist`, `help`, the completion box):

| letter | meaning |
|---|---|
| S | server info: sent to everyone in a match |
| U | user info: sent to the server (your name) |
| R | read only: the game sets it |
| I | init: set only at start |
| A | archived: saved in the console config (`userdata/cod3_config.cfg`, `cod3mp_config.cfg`) |
| L | latched: takes effect at the next level or restart |
| C | cheat protected: only with `sv_cheats 1` |
| T | temporary: never saved |

Values reset by the game: a dvar marked "Set by the game" below is the game's
own state (the current map, a button being held, the mission's quote). Setting
it does nothing useful and the game sets it back.

## Cheat protection

- `sv_cheats` (or `cheats`) is the one switch. Every mission starts with
  cheats off; `sv_cheats 1` puts a CHEATS USED mark on screen; `sv_cheats 0`
  puts every cheat-protected dvar back to its default.
- A cheat-protected dvar (C) changes only while cheats are on, from every
  route: the console, `seta`, configs, `exec`, `cod3_autoexec`. One set in a
  config is dropped with a message.
- In a multiplayer match only the host can set `sv_cheats`, and everyone is
  told (a line in its own column, like the kill feed).
- **Multiplayer only, added by the port:** these dvars aren't cheat protected
  in the game but would let one player see or react to the others better, so
  cod3mp registers them cheat protected (the game's own flag, so all of the
  above applies to them):

  | dvars | why |
  |---|---|
  | `cg_tracerchance`, `cg_tracerlength`, `cg_tracerSpeed`, `cg_tracerwidth` | more or longer tracers show where every shot comes from |
  | `cg_viewKickDeflectTime`, `cg_viewKickReturnTime` | how fast your view settles after a hit |
  | `cg_hudCompassMaxRange`, `cg_hudCompassMinRange`, `cg_hudCompassMinRadius` | how far the compass reaches |
  | `cg_shellshockblur`, `cg_redFlashTime` | the blur and red flash when hit |
  | `fx_cull`, `fx_cullbias`, `fx_cullscale` | effects culled sooner: thinner smoke far away |
  | `r_znear` | a large near clip distance shows through walls up close |
  | `r_LightScale` | brighter light everywhere |
  | `r_lodbias`, `r_lodscale` | model detail at a distance |
  | `mp_debug`, `mp_debugrender`, `mp_debugrenderspawnpoints`, `RadarFakeFriendlies`, `showvehmap`, `show_playerdist`, `cg_mpDebugAnimEntity` | the game modes' debug drawing (spawn points ...) |

  The campaign keeps them as the game made them (nobody to gain on). The
  list is `kMultiplayerCheatDvars` in `recomp/src/game/cheat_guard.cpp`.
- Texture replacement is never used in multiplayer (docs/TEXTURE_REPLACEMENT.md).

## World at War and Call of Duty 4 names

Many World at War dvars exist in Call of Duty 3 under the same name (`cg_fov`,
`cg_drawFPS`, `timescale` ...). For the rest the console takes World at War's
name too (W in `cvarlist`):

- the same setting under Call of Duty 3's name, converted where the scales
  differ: `cg_gun_x` is `cg_gunX`, `bg_viewKickScale` is `cg_viewKickScale`
  ÷ 10, `hud_fade_compass` is `g_compassSolidTime`, `compassMaxRange` is
  `cg_hudCompassMaxRange`;
- the port's settings: `sensitivity`, `m_yaw`, `r_mode 1920x1080`,
  `r_fullscreen`, `r_vsync`, `r_aaSamples`, `r_texFilterAnisoMax`,
  `sm_enable`, `snd_volume`, `r_scaleViewport` (the resolution scale as a
  fraction);
- Call of Duty 4's names from its public dvar lists: `r_lodScaleRigid` and
  `r_lodScaleSkinned` (Call of Duty 3 has one `r_lodscale`), `g_gametype`
  (this game's mode names: dm, tdm, ctf, hq, scf, dom, war, sd; war is Call of
  Duty 3's own mode, not Call of Duty 4's team deathmatch), `scr_teambalance`,
  and `scr_<mode>_timelimit` / `_scorelimit` / `_roundlimit` for each mode
  (Call of Duty 3 has one limit for every mode: `mp_timelimit`,
  `mp_scorelimit`, `mp_roundlimit`);
- every other World at War dvar: the console says it is one Call of Duty 3
  has nothing for, instead of "unknown command".

A name goes through the game under Call of Duty 3's name, so the cheat
protection above applies to it the same way. The table is in
`recomp/src/ui/waw_dvars.cpp`.

## Counter-Strike 2's weapon position

The console also takes Counter-Strike 2's weapon position commands, so a CS2
config line can be pasted as it is (campaign and multiplayer):

| Command | Range | What it does |
|---|---|---|
| `viewmodel_offset_x` | -2.5 to 2.5 | weapon to the right (negative: to the left) |
| `viewmodel_offset_y` | -2 to 2 | weapon away from you (negative: closer) |
| `viewmodel_offset_z` | -2 to 2 | weapon up (negative: lower) |
| `viewmodel_presetpos` | 0 to 3 | CS2's positions: 1 Desktop (1, 1, -1), 2 Couch (0, 0, -1.5), 3 Classic (2.5, 0, -1.5); 0 your own offsets |

They are the port's settings `cod3_viewmodel_offset_x`, `_y`, `_z` and
`cod3_viewmodel_presetpos` (F2 > Game > Weapon: a preset list and Right,
Forward and Up sliders; saved with the other settings, and multiplayer uses
the campaign's), in game units, all 0 by default (the game's own place). Typing an
offset while a preset is chosen starts from that preset and makes the
position your own (0), as moving F2's sliders does. They aren't cheat
protected: the game's own `cg_gunX`, `cg_gunY` and `cg_gunZ` are, and are left
alone; the port adds the offsets where the game places the weapon. They fade
out as the weapon comes up to the sights, so aiming down the sights stays as
it was: only FOV scale changes the aimed view. `viewmodel_fov` has no counterpart: Call of Duty 3 draws the weapon
with the view's field of view (`cg_fov`), and the console says so.

## Aim: sensitivity per weapon and aim assist

<!-- [claude-9b] 2026-10-20 -->

**Sensitivity per weapon class** (F2 > Controls > Sensitivity per weapon;
campaign and multiplayer). The port settings are `cod3_weapon_sensitivity`
(player 1) and `cod3_weapon_sensitivity_p2` to `_p4` (multiplayer's
split-screen players 2 to 4). Each is a list of `group=hip/aiming`
percentages from 0 to 200, for example `smg=80/60 sniper=100/40`. A group
that isn't listed stays at 100/100, the game's own feel; `group=N` sets both
numbers. The groups follow the game's weapon classes: `rifle`, `shotgun`,
`sniper` (scoped rifles), `smg`, `lmg`, `mg`, `pistol`, `launcher`,
`mounted`, `other`. The percentage blends from hip to aiming as the weapon
comes up to the sights. It applies to the mouse and to the stick (after the
game's own turn limits, so it holds at full stick). The FOV, the zoom and
aiming itself don't change, and aim assist is never touched.

**Aim assist (Sticky Aim)** is Call of Duty 3's only aim assist. Each
player's own switch is Options > Gameplay > Sticky Aim (also F2 > Game >
Aim). Near an enemy it pulls the view a little toward them each time it
runs, which is what slows a sweep past an enemy. The game ran it once a
frame, so it got stronger as the frame rate went up (2.4 times at 144 fps)
and weaker below 60. The port runs it 60 times a second at any frame rate,
the console's rate, counted from the game's own input times so the host and
every player's prediction agree. Two `server_sided_protected` dvars
(docs/MOVEMENT.md, section 5) are the match's rule:

| dvar | values (default) | what it does |
|---|---|---|
| `sticky_aim` | 0 / 1 (1) | 0: Sticky Aim off for everyone. 1: each player's own Options choice (never turned on for someone who chose off). |
| `aim_slowdown` | 0 to 100 (100) | How hard Sticky Aim pulls, in percent of the game's own. It can't go above 100. |

In a match the host's values apply to every player, and nothing on a
guest's machine can change them (the console says why); a player's own
values come back when they leave. In the campaign you are the host. The
host sets them on F2 > Game > Server, a tab only the host sees (with the
movement rule); F2 > Game > Aim shows what applies now.
cod3mp doesn't build this category yet (it waits for the multiplayer
transport), so multiplayer keeps the game's own rule, each player's choice
at the full pull, at the same fixed 60 a second. `cod3_debug_sticky_aim`
logs once a second how many inputs the game made and how many pulls ran.

## Hidden switches

Some dvars aren't made by the game at start:

- **Level script switches.** The missions' scripts (compiled into each level's
  DLL) make some at the start of every mission (`debug`, `fallback`, `chain`,
  `start`, `skipto`, `debug_battlechatter`, `debug_mortars` ... with "0",
  "off" or "start") and only read others (`animDebug`, `debug_audio`,
  `ff_off`, `notank`, `melee_clinch` ...: 0 unless set by hand). They are the
  developers' test switches. Set one before the mission loads
  (`set notank 1`, then load the mission) and the scripts find it set. What
  each one does in this build is listed under "Level script switches" below;
  most of the debug drawing they control was taken out of the retail game.
- **Made later.** `introScreen`, `letterbox_enabled`, `cull_instances`,
  `instance_lods`, `max_drones`, `ui_campaign`, the death and victory screens'
  quotes: the game registers them while a mission loads, so `cvarlist` at the
  title screen doesn't show them.
- **Debug and developer dvars** (`r_showtris`, `r_speeds`, `fx_count`,
  `com_statmon`, `cg_drawPosition` ...): the game still has them; most are
  cheat protected, and some do nothing in the retail build. Each row below
  says what it does.

## How this list was made

- Every dvar the campaign program registers or reads (its calls that register
  dvars, the cgame and game dvar tables, the renderer's, and the level DLLs'
  script calls), with default and flags: 418 names.
- Console dumps on a PC through the REA sandbox (`dev/tools/rea/`): the
  campaign at the title screen and in Forest, multiplayer at the title screen
  and in Verdun (`dvardump`, `dvarexport`). 471 names in all: 420 in the
  campaign, 384 in multiplayer.
- World at War's and Call of Duty 4's names only for the mapping above.

To change a line: edit `docs/dvars.tsv` (name, group, programs, default,
flags in each program, the line), then `python3 tools/gen_dvar_help.py`. It
writes the console's table (`recomp/src/ui/dvar_help.inc`) and the tables
below; `--check` says whether both are current.

## Reference

Defaults are the game's own (the campaign's where both programs have the
dvar). "Flags" shows the campaign's / multiplayer's when they differ. "In":
SP the campaign, MP multiplayer.

<!-- dvar tables: written by tools/gen_dvar_help.py from docs/dvars.tsv; edit the .tsv, not these -->

### View and field of view (14)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_blood` | `0` | A | SP MP | Show blood. |
| `cg_camerashake` | `1` | C | SP MP | Camera shake from explosions and artillery. |
| `cg_fov` | `65` | T | SP MP | Field of view (degrees). The port allows 65-165 with cg_fovScale; F2 > Video. |
| `cg_fovScale` | `1` | A | SP MP | Multiplies cg_fov (the port's FOV scale; the effective value is 65-165). |
| `cg_letterbox` | `0` | T | SP MP | Set by the game: the cinematic black bars. |
| `cg_night` | `0` | R | SP MP | Set by the game: night vision tint for night missions. |
| `cg_viewKickDeflectTime` | `10` |  | SP MP | Time (ms) the view takes to kick when you are hit. |
| `cg_viewKickMax` | `90` | C | SP MP | Largest view kick when hit (World at War: bg_viewKickMax). |
| `cg_viewKickReturnTime` | `400` |  | SP MP | Time (ms) the view takes to come back after a kick. |
| `cg_viewKickScale` | `2.0` | C | SP MP | View kick per point of damage (World at War's bg_viewKickScale x 10). |
| `cg_viewsize` | `100` | A | SP MP | Size of the 3D view (percent, 30-100). |
| `cg_widescreen` | `0` | A | SP MP | 16:9 picture and HUD layout (the port's F2 aspect ratio sets it). |
| `com_maxfps` | `0` | A | SP MP | Frame rate cap (the port's frame limiter: 0 = no limit, up to 1000; F2 > Video). |
| `letterbox_enabled` | `0` |  | SP | Set by the game: the cinematic black bars are allowed. |

### HUD and messages (65)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_centertime` | `3` | C | SP MP | Seconds a message in the middle of the screen stays up. |
| `cg_deadscreen_backdrop` | `levelshots/deadscreen/defeat_american.tga` |  | SP MP | Set by the game: the picture behind the death screen. |
| `cg_deadscreen_levelname` | `levelshots/level_name_text/hud@bastogne1.tga` |  | SP MP | Set by the game: the mission name on the death screen. |
| `cg_draw2D` | `1` | A | SP MP | Draw everything 2D (HUD, crosshair, messages). 0 = clean screenshots. |
| `cg_drawFPS` | `0` | A | SP MP | Frames-per-second counter (the port adds World at War's modes: 0 off, 1 simple, 2 with ranges, 3 verbose). |
| `cg_drawpaused` | `1` |  | SP MP | Show the paused screen. |
| `cg_drawPosition` | `0` | A | SP MP | Debug: prints your position on screen. |
| `cg_drawStatus` | `1` | A | SP MP | Draw the HUD's status parts (health, ammo, compass). |
| `cg_gameBoldMessageWidth` | `30` | A | MP | Multiplayer: widest bold game message (characters). |
| `cg_gameMessageWidth` | `70` | A | MP | Multiplayer: widest game message (characters). |
| `cg_hudAlpha` | `1.0` | A | SP MP | HUD opacity (0-1). |
| `cg_hudCompassMaxRange` | `1024` | A | SP MP | Compass: farthest distance shown (units). |
| `cg_hudCompassMinRadius` | `0` | A | SP MP | Compass: the smallest ring markers are drawn on. |
| `cg_hudCompassMinRange` | `0` | A | SP MP | Compass: nearest distance shown (units). |
| `cg_hudCompassSize` | `1.0` | A | SP MP | Compass size. |
| `cg_hudCompassSpringyPointers` | `0` | A | SP MP | Compass pointers swing with a spring. |
| `cg_hudCompassVisible` | `1` | R | SP MP | Set by the game: whether the compass shows. |
| `cg_hudDamageIconHeight` | `64` | A | SP MP | Height of the arrow that shows where damage came from. |
| `cg_hudDamageIconInScope` | `0` | A | SP MP | Show the damage arrows while looking through a scope. |
| `cg_hudDamageIconOffset` | `128` | A | SP MP | Damage arrow distance from the screen's center. |
| `cg_hudDamageIconTime` | `2000` | A | SP MP | Time (ms) a damage arrow stays up. |
| `cg_hudDamageIconWidth` | `128` | A | SP MP | Width of the damage arrow. |
| `cg_hudObjectiveMaxHeight` | `70` | A | SP MP | Objective marker: highest it is drawn above the target. |
| `cg_hudObjectiveMaxRange` | `2048` | A | SP MP | Objective marker: farthest distance it shows at. |
| `cg_hudObjectiveMinAlpha` | `1` | A | SP MP | Objective marker: faintest it gets. |
| `cg_hudObjectiveMinHeight` | `-70` | A | SP MP | Objective marker: lowest it is drawn. |
| `cg_hudObjectiveNumRings` | `10` | A | SP MP | Objective marker: number of pulse rings. |
| `cg_hudObjectiveRingSize` | `64` | A | SP MP | Objective marker: pulse ring size. |
| `cg_hudObjectiveRingTime` | `10000` | A | SP MP | Objective marker: time (ms) one pulse ring takes. |
| `cg_redFlashTime` | `350` |  | SP MP | Time (ms) of the red flash when you are hit. |
| `cg_subtitles` | `1` | A | SP MP | Show subtitles. |
| `cg_victoryscreen_backdrop` | `levelshots/victoryscreen/victory_american.jpg` |  | SP MP | Set by the game: the picture behind the mission complete screen. |
| `cg_victoryscreen_levelname` | `levelshots/level_name_text/hud@bastogne1.tga` |  | SP MP | Set by the game: the mission name on the mission complete screen. |
| `cl_conXOffset` | `0` |  | SP MP | Console text left margin (the original console; the port's console is its own). |
| `con_minicon` | `0` | A | MP | Multiplayer: a few console lines always on screen. |
| `g_ammoFadeTime` | `1.0` | C | SP MP | Seconds the ammo counter takes to fade. |
| `g_ammoSolidTime` | `4.0` | C | SP MP | Seconds the ammo counter stays fully shown. |
| `g_compassFadeTime` | `1.0` | C | SP MP | Seconds the compass takes to fade. |
| `g_compassSolidTime` | `9.0` | C | SP MP | Seconds the compass stays fully shown. |
| `g_grenadeFadeTime` | `1.0` | C | SP MP | Seconds the grenade counter takes to fade. |
| `g_grenadeSolidTime` | `4.0` | C | SP MP | Seconds the grenade counter stays fully shown. |
| `g_stanceFadeTime` | `1.0` | C | SP MP | Seconds the stance icon takes to fade. |
| `g_stanceSolidTime` | `4.0` | C | SP MP | Seconds the stance icon stays fully shown. |
| `introScreen` | `1` |  | SP | Show the mission's intro text (the port skips it with cod3_skip_movies; set by the game). |
| `mp_headIconDistAbovePlayer` | `16` |  | MP | Multiplayer: name/rank icon height above a player (units). |
| `mp_headIconDistAboveVehicle` | `60` |  | MP | Multiplayer: icon height above a vehicle (units). |
| `mp_headIconHeight` | `16` |  | MP | Multiplayer: head icon size. |
| `mp_headIconMinScreenSize` | `20` |  | MP | Multiplayer: the smallest a head icon gets far away. |
| `mp_headIconReviveMaxAlphaDist` | `2000` |  | MP | Multiplayer: distance where the revive icon is faintest. |
| `mp_headIconReviveMinAlphaDist` | `1000` |  | MP | Multiplayer: distance where the revive icon starts to fade. |
| `mp_itemIconDistAboveItem` | `15` |  | MP | Multiplayer: icon height above an item (units). |
| `mp_itemIconHeight` | `16` |  | MP | Multiplayer: item icon size. |
| `mp_itemIconMaxAlphaDist` | `2000` |  | MP | Multiplayer: distance where item icons are faintest. |
| `mp_itemIconMinAlphaDist` | `1000` |  | MP | Multiplayer: distance where item icons start to fade. |
| `mp_itemIconMinScreenSize` | `16` |  | MP | Multiplayer: the smallest an item icon gets far away. |
| `mp_objectiveFarAlpha` | `0` |  | MP | Multiplayer: objective icon opacity far away. |
| `mp_objectiveFarAlphaDist` | `10` |  | MP | Multiplayer: distance for the far opacity. |
| `mp_objectiveMaxSize` | `50` |  | MP | Multiplayer: largest objective icon. |
| `mp_objectiveMinSize` | `20` |  | MP | Multiplayer: smallest objective icon. |
| `mp_objectiveNearAlpha` | `20` |  | MP | Multiplayer: objective icon opacity up close. |
| `mp_objectiveNearAlphaDist` | `0` |  | MP | Multiplayer: distance for the near opacity. |
| `mp_objectiveSize` | `0` |  | MP | Multiplayer: objective icon size (0 = automatic). |
| `scr_conspeed` | `3` |  | MP | Multiplayer: how fast the original console slides open. |
| `ui_deadquote` |  |  | SP | Set by the level scripts: the quote on the death screen. |
| `ui_victoryquote` | `@VICTORYQUOTE_HE_CONQUERS_WHO_ENDURES` |  | SP | Set by the level scripts: the quote on the mission complete screen. |

### Crosshair and hints (7)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_crosshairAlpha` | `1.0` | A | SP MP | Crosshair opacity (0-1). |
| `cg_crosshairAlphaMin` | `0.0` | A | SP MP | Crosshair opacity at the start of its fade in. |
| `cg_crosshairDynamic` | `0` | A | SP MP | The crosshair spreads with movement and firing. |
| `cg_crosshairNoGun` | `gfx/reticle/hud@center_ads.tga` | A | SP MP | The crosshair picture shown with no weapon. |
| `cg_cursorHints` | `3` | A | SP MP | How use hints (pick up, open ...) are drawn: 0 none, 1-4 different pulses and fades. |
| `cg_drawCrosshair` | `1` | A | SP | Draw the crosshair. |
| `cg_hintFadeTime` | `100` | A | SP MP | Time (ms) a use hint takes to fade. |

### Weapon view (27)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_bobAmplitudeDucked` | `0.026` |  | SP MP | View bob while crouched. |
| `cg_bobAmplitudeProne` | `0.08` |  | SP MP | View bob while prone. |
| `cg_bobAmplitudeStanding` | `0.013` |  | SP MP | View bob while standing. |
| `cg_bobMax` | `8` |  | SP MP | Largest view bob. |
| `cg_bobWeaponAmplitude` | `0.16` |  | SP MP | Weapon bob size. |
| `cg_bobWeaponLag` | `0.25` |  | SP MP | How far the weapon bob lags behind your steps. |
| `cg_bobWeaponMax` | `10` |  | SP MP | Largest weapon bob. |
| `cg_bobWeaponRollAmplitude` | `1.5` |  | SP MP | How much the weapon rolls as it bobs. |
| `cg_drawGun` | `1` | C | SP MP | Draw your weapon. 0 hides it. |
| `cg_gun_move_f` | `0` | C | SP MP | Weapon sway forward while moving. |
| `cg_gun_move_minspeed` | `0` | C | SP MP | Slowest weapon sway rate. |
| `cg_gun_move_r` | `0` | C | SP MP | Weapon sway to the right while moving. |
| `cg_gun_move_rate` | `0` | C | SP MP | Base weapon sway rate. |
| `cg_gun_move_u` | `0` | C | SP MP | Weapon sway up while moving. |
| `cg_gun_ofs_f` | `0` | C | SP MP | Weapon pushed forward while crouched or prone. |
| `cg_gun_ofs_r` | `0` | C | SP MP | Weapon pushed right while crouched or prone. |
| `cg_gun_ofs_u` | `0` | C | SP MP | Weapon pushed up while crouched or prone. |
| `cg_gun_rot_minspeed` | `0` | C | SP MP | Slowest weapon turn sway. |
| `cg_gun_rot_p` | `0` | C | SP MP | Weapon pitch sway as you turn. |
| `cg_gun_rot_r` | `0` | C | SP MP | Weapon roll sway as you turn. |
| `cg_gun_rot_rate` | `0` | C | SP MP | Base weapon turn sway rate. |
| `cg_gun_rot_y` | `0` | C | SP MP | Weapon yaw sway as you turn. |
| `cg_gunX` | `0` | C | SP MP | Weapon model offset forward (World at War: cg_gun_x). |
| `cg_gunY` | `0` | C | SP MP | Weapon model offset sideways (World at War: cg_gun_y). |
| `cg_gunZ` | `0` | C | SP MP | Weapon model offset up (World at War: cg_gun_z). |
| `conical_spread` | `1` |  | SP MP | Weapon spread is a cone (1) rather than flat. |
| `ik_ADS` | `0` | C | SP | The arms follow the weapon (inverse kinematics) while aiming down the sight. |

### Shell shock (32)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_shellshockblur` | `1` | A | SP MP | Blur the screen while shell-shocked. |
| `cg_shock_mouse` | `1` | C | SP MP | Slow the view down while shell-shocked (World at War: bg_shock_lookControl). |
| `cg_shock_mouse_fadeTime` | `2` | C | SP MP | Seconds the shell-shock view slowdown takes to fade. |
| `cg_shock_mouse_maxpitchspeed` | `90` | C | SP MP | Fastest up/down turning while shell-shocked (degrees/second). |
| `cg_shock_mouse_maxyawspeed` | `90` | C | SP MP | Fastest left/right turning while shell-shocked (degrees/second). |
| `cg_shock_mouse_sensitivityscale` | `0.5` | C | SP MP | Look sensitivity while shell-shocked (fraction). |
| `cg_shock_sound` | `1` | C | SP MP | Play the shell-shock sound. |
| `cg_shock_soundFadeInTime` | `0.25` | C | SP MP | Shell-shock sound fade in (seconds). |
| `cg_shock_soundFadeOutTime` | `2.5` | C | SP MP | Shell-shock sound fade out (seconds). |
| `cg_shock_soundLoopEndDelay` | `-3.0` | C | SP MP | When the shell-shock sound loop ends, relative to the shock's end (seconds). |
| `cg_shock_soundLoopFadeTime` | `1.5` | C | SP MP | Shell-shock sound loop fade (seconds). |
| `cg_shock_soundModEndDelay` | `2` | C | SP MP | When the muffled sound ends, relative to the shock's end (seconds). |
| `cg_shock_soundRoomType` | `underwater` | C | SP MP | The reverb used while shell-shocked (e.g. underwater). |
| `cg_shock_soundWetLevel` | `0.5` | C | SP MP | How much of the sound goes through that reverb (0-1). |
| `cg_shock_viewKickFadeTime` | `3` | C | SP MP | Seconds the shell-shock view wobble takes to fade. |
| `cg_shock_viewKickPeriod` | `1.75` | C | SP MP | Shell-shock view wobble period (seconds). |
| `cg_shock_viewKickRadius` | `0.05` | C | SP MP | Shell-shock view wobble size. |
| `cg_shock_volume_announcer` | `1.0` | C | SP MP | Announcer volume while shell-shocked (0-1). |
| `cg_shock_volume_auto` | `0.1` | C | SP MP | Ambient sound volume while shell-shocked (0-1). |
| `cg_shock_volume_body` | `0.1` | C | SP MP | Body (footsteps, impacts) volume while shell-shocked (0-1). |
| `cg_shock_volume_item` | `0.1` | C | SP MP | Item sound volume while shell-shocked (0-1). |
| `cg_shock_volume_local` | `1.0` | C | SP MP | Your own sounds' volume while shell-shocked (0-1). |
| `cg_shock_volume_menu` | `1.0` | C | SP MP | Menu sound volume while shell-shocked (0-1). |
| `cg_shock_volume_music` | `1.0` | C | SP MP | Music volume while shell-shocked (0-1). |
| `cg_shock_volume_shellshock` | `1.0` | C | SP MP | The shell-shock sound's own volume (0-1). |
| `cg_shock_volume_voice` | `0.2` | C | SP MP | Voice volume while shell-shocked (0-1). |
| `cg_shock_volume_weapon` | `0.5` | C | SP MP | Weapon volume while shell-shocked (0-1). |
| `shock_screen_blur_fadein_time` | `0.25` | C | SP MP | Seconds the shell-shock blur takes to come in. |
| `shock_screen_blur_fadeout_time` | `0.75` | C | SP MP | Seconds the shell-shock blur takes to go. |
| `shock_screen_blur_level` | `0.6` | C | SP MP | How strong the shell-shock blur is. |
| `shock_screen_blur_time` | `5.0` | C | SP MP | Seconds the shell-shock blur lasts. |
| `shock_screen_feedback` | `0.03` | C | SP MP | The shell-shock blur's trail (how much of the last frame is kept). |

### Third person and vehicle cameras (5)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_altTankCam` | `1` |  | SP MP | The tank's alternate (closer) chase camera. |
| `cg_thirdPerson` | `0` | C | SP MP | Third person view. |
| `cg_thirdPersonAngle` | `0` | C | SP MP | Third person camera angle around you (degrees). |
| `cg_thirdPersonLock` | `0` | C | SP MP | Third person camera stays behind you instead of turning freely. |
| `cg_thirdPersonRange` | `150` |  | SP MP | Third person camera distance (units). |

### Effects (14)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_tracerchance` | `0.5` |  | SP MP | Chance a bullet is drawn as a tracer (0-1). |
| `cg_tracerlength` | `150` |  | SP MP | Tracer length (units). |
| `cg_tracerSpeed` | `5200` |  | SP MP | Tracer speed (units per second). |
| `cg_tracerwidth` | `0.8` |  | SP MP | Tracer width. |
| `fx_count` | `0` | C | SP MP | Debug: print the number of effects. |
| `fx_cull` | `1` |  | SP MP | Skip effects out of view. |
| `fx_cullbias` | `0` | A | SP MP | Effects culling: distance bias. |
| `fx_cullscale` | `1` | A | SP MP | Effects culling: distance scale. |
| `fx_debug` | `0` | C | SP MP | Debug: effect information on screen. |
| `fx_debugBolt` | `0` | C | SP MP | Debug: effects attached to models. |
| `fx_draw` | `1` | C | SP MP | Draw effects (0 runs them but draws nothing). |
| `fx_enable` | `1` | C | SP MP | Effects on (0 = no effects at all). |
| `fx_freeze` | `0` | C | SP MP | Freeze all effects in place. |
| `g_vehicleTexScrollScale` | `0` | C | SP MP | How fast tank tread textures scroll. |

### Renderer (49)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_norender` | `0` |  | SP MP | Debug: the game draws nothing. |
| `cg_skybox` | `1` | C | SP MP | Draw the sky. |
| `cull_instances` | `1` | T | SP MP | Skip drawing repeated objects out of view. |
| `instance_lods` | `1` | T | SP MP | Repeated objects use lower detail models far away. |
| `r_clear` | `0` | C | SP MP | Debug: clear the screen each frame (shows what isn't drawn). |
| `r_cullBModels` | `1` |  | SP MP | Skip drawing brush models out of view. |
| `r_cullXModels` | `1` |  | SP MP | Skip drawing models out of view. |
| `r_drawentities` | `1` | C | SP MP | Draw entities (models, weapons, characters). |
| `r_dynamiclight` | `1` | A | SP MP | Dynamic lights (muzzle flashes, explosions). |
| `r_finish` | `0` | A | SP MP | Wait for the GPU each frame. |
| `r_flares` | `1` | A | SP MP | Light flares. |
| `r_fog` | `1` | C | SP MP | Fog. |
| `r_fullbright` | `0` | LC | SP MP | No lighting: everything fully lit. |
| `r_fullscreen` | `1` | AL | SP MP | Full screen (the port's window mode: F2 > Video; World at War's r_fullscreen works too). |
| `r_LightScale` | `1.0` | T | SP MP | Light brightness scale. |
| `r_lockpvs` | `0` | C | SP MP | Debug: keep the current visibility, then look around to see what is culled. |
| `r_lodbias` | `0` | A | SP MP | Model detail bias (higher = lower detail sooner). |
| `r_lodscale` | `1` | A | SP MP | Model detail distance scale. |
| `r_logFile` | `0` |  | SP MP | Debug: log the renderer's calls for this many frames. |
| `r_maxEntLights` | `8` | A | SP MP | Most lights on one model. |
| `r_minEntLightIntensity` | `0.02` | AC | SP MP | Faintest light that lights a model. |
| `r_mode` | `3` | AL | SP MP | The screen mode (the port's resolution is F2 > Video; World at War's r_mode 1920x1080 works). |
| `r_nocull` | `0` | C | SP MP | Debug: draw everything, in view or not. |
| `r_noportals` | `0` | C | SP MP | Debug: ignore portals (visibility). |
| `r_norefresh` | `0` | C | SP MP | Debug: don't draw the 3D view. |
| `r_offsetfactor` | `-1` | C | SP MP | Decal depth offset factor. |
| `r_offsetunits` | `-2` | C | SP MP | Decal depth offset units. |
| `r_portalOnly` | `0` | C | SP MP | Debug: draw only what portals see. |
| `r_profileDrawElements` | `0` | C | SP MP | Debug: time the draw calls. |
| `r_showLocationalDamage` | `0` | C | SP MP | Debug: shows the hit zones (head, chest, limbs). |
| `r_showportals` | `0` | C | SP MP | Debug: outline the portals. |
| `r_showSkeletons` | `0` | C | SP MP | Debug: draw the characters' skeletons. |
| `r_showsurfcounts` | `0` | C | SP MP | Debug: print the number of surfaces drawn. |
| `r_showtricounts` | `0` | C | SP MP | Debug: print the number of triangles drawn. |
| `r_showtris` | `0` | C | SP MP | Debug: wireframe over everything. |
| `r_singlecell` | `0` | C | SP MP | Debug: draw only the cell the camera is in. |
| `r_speeds` | `0` | C | SP MP | Debug: renderer statistics. |
| `r_swapDelay` | `0` | A | SP MP | Debug: wait (ms) after each frame is shown. |
| `r_testshadow` | `1` | T | SP MP | Internal shadow test switch (leave it alone; it isn't an off switch: use cod3_shadow_quality). |
| `r_texturebits` | `0` | AL | SP MP | Texture color depth (latched; the port's GPU emulation decides). |
| `r_textureMode` | `GL_LINEAR_MIPMAP_NEAREST` | A | SP MP | Texture filter (the port's anisotropic filtering is F2 > Video). |
| `r_uifullscreen` | `0` |  | SP MP | Set by the game: a full screen menu is up. |
| `r_vc_compile` | `0` | L | SP MP | Developer: build the vertex cache. |
| `r_vc_makelog` | `0` | L | SP MP | Developer: write the vertex cache log. |
| `r_vc_showlog` | `0` |  | SP MP | Developer: show the vertex cache log. |
| `r_verbose` | `0` |  | SP MP | Debug: more renderer messages. |
| `r_weightMipMaps` | `0` | L | SP MP | Developer: weighted texture mipmaps. |
| `r_zfar` | `8192` | C | SP MP | Far drawing distance (0 = from the level's fog). |
| `r_znear` | `6.0` | T | SP MP | Near drawing distance (things closer aren't drawn). |

### Player movement (21)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `bg_foliagesnd_fastinterval` | `500` | C | SP MP | Time (ms) between bush rustle sounds when moving fast. |
| `bg_foliagesnd_maxspeed` | `180` | C | SP MP | Speed at which moving through bushes is loudest. |
| `bg_foliagesnd_minspeed` | `40` | C | SP MP | Slowest speed that makes bushes rustle. |
| `bg_foliagesnd_resetinterval` | `500` | C | SP MP | Time (ms) after leaving bushes before the rustle sounds reset. |
| `bg_foliagesnd_slowinterval` | `1500` | C | SP MP | Time (ms) between bush rustle sounds when moving slowly. |
| `bg_ladder_yawcap` | `100` | C | SP MP | How far (degrees) you can turn to the sides on a ladder. |
| `bg_lmg_yawcap` | `80` | C | SP MP | How far (degrees) you can turn while a machine gun is set up (deployed). |
| `bg_meleeassistaspeed` | `100` | C | SP MP | Melee lunge: speed toward the target. |
| `bg_meleeassistfov` | `0.75` | C | SP MP | Melee lunge: how close to the crosshair (cosine of the angle) a target must be. |
| `bg_meleeassistrange` | `50` | C | SP MP | Melee lunge: farthest target distance (units). |
| `bg_prone_yawcap` | `85` | C | SP MP | How far (degrees) you can turn while prone before you have to shift. |
| `bg_viewheight_crouched` | `40` | C | SP MP | Eye height (units) while crouched. |
| `bg_viewheight_prone` | `11` | C | SP MP | Eye height (units) while prone. |
| `bg_viewheight_standing` | `66` | C | SP MP | Eye height (units) while standing. |
| `cm_playerCurveClip` | `1` | AC | SP MP | The player collides with curved surfaces. |
| `g_bounds_height_standing` | `70` | C | SP MP | Player height (units) for collision while standing. |
| `g_bounds_width` | `30` | C | SP MP | Player width (units) for collision. |
| `g_gravity` | `800` |  | SP MP | Gravity (units per second squared). |
| `g_speed` | `190` | T / R | SP MP | Player speed (set by the game). |
| `pmove_fixed` | `0` |  | SP MP | Fixed movement steps (pmove_msec) instead of the frame time. |
| `pmove_msec` | `8` |  | SP MP | The movement step (ms) when pmove_fixed is on. |

### Damage, health and pickups (19)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `bg_fallDamageMaxHeight` | `480` | C | SP MP | Fall height (units) that does full damage (kills). |
| `bg_fallDamageMinHeight` | `256` | C | SP MP | Fall height (units) where fall damage starts. |
| `g_knockback` | `1000` |  | SP MP | How hard damage pushes you. |
| `g_player_maxhealth` | `1` | RL | SP MP | The player's most health (set by the game from the difficulty). |
| `g_weaponAmmoPools` | `0` |  | SP MP | Weapons with the same ammo share it. |
| `g_weaponrespawn` | `5` |  | SP MP | Seconds before a dropped weapon pickup comes back. |
| `health_recharge_rate` | `30` | T | MP | Multiplayer: how fast health comes back (points a second). |
| `hud_healthOverlay_phaseEnd_pulseDuration` | `700` | C | MP | Multiplayer: the red damage overlay's last pulse (ms). |
| `hud_healthOverlay_phaseEnd_toAlpha` | `0.0` | C | MP | Multiplayer: the red damage overlay's opacity at its end. |
| `hud_healthOverlay_phaseOne_pulseDuration` | `150` | C | MP | Multiplayer: the red damage overlay's first pulse (ms). |
| `hud_healthOverlay_phaseThree_pulseDuration` | `400` | C | MP | Multiplayer: the red damage overlay's third pulse (ms). |
| `hud_healthOverlay_phaseThree_toAlphaMultiplier` | `0.6` | C | MP | Multiplayer: the red damage overlay's opacity in the third pulse. |
| `hud_healthOverlay_phaseTwo_pulseDuration` | `320` | C | MP | Multiplayer: the red damage overlay's second pulse (ms). |
| `hud_healthOverlay_phaseTwo_toAlphaMultiplier` | `0.7` | C | MP | Multiplayer: the red damage overlay's opacity in the second pulse. |
| `hud_healthOverlay_pulseStart` | `0.35` | C | MP | Multiplayer: the health fraction below which the red overlay pulses. |
| `hud_healthOverlay_regenPauseTime` | `5000` | CT | MP | Multiplayer: time (ms) after a hit before health comes back. |
| `livegrenadeangle` | `60` |  | SP MP | How far (degrees) from your view a live grenade shows its warning icon. |
| `player_deathInvulnerableTime` | `1000` | C | SP | Time (ms) after a deadly hit during which you can't be hit again (the save from a near death). |
| `player_deathInvulnerableToMelee` | `1` | C | SP | That last-moment protection also covers melee. |

### AI (campaign) (35)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `ai_corpseCount` | `20` | A | SP | Most AI bodies left lying in the level; older ones are removed past this. |
| `ai_debugMayMove` | `0` | C | SP | Debug: shows the AI's checks of whether it may move to a spot. |
| `ai_disableSpawn` | `0` | C | SP | Debug: no AI spawns while set (a mission can't progress without them). |
| `ai_NoCriticalSections` | `1` | T | SP | Internal: the AI's thread locking switch (leave it alone). |
| `ai_noDodge` | `0` | C | SP | AI never side-steps out of the line of fire. |
| `ai_playerFarAccuracy` | `0.1` |  | SP | AI accuracy against the player at long range (0-1). |
| `ai_playerFarRange` | `2000` |  | SP | Distance (units) from which AI use the long-range accuracy. |
| `ai_playerNearAccuracy` | `0.5` |  | SP | AI accuracy against the player up close (0-1). |
| `ai_playerNearRange` | `800` |  | SP | Distance (units) up to which AI use the close-range accuracy. |
| `aitosser_debugenemy` | `0` | C | SP MP | Debug: draws the enemy grenade throwers' (tossers') state. |
| `aitosser_debugenemyspawner` | `0` | C | SP MP | Debug: draws the enemy tosser spawners. |
| `aitosser_debugfriendly` | `0` | C | SP MP | Debug: draws the friendly grenade throwers' state. |
| `animscriptent` | `-1` |  | SP | Debug: the entity number whose animation script is traced (-1 = none). |
| `disable_ai` | `0` | C | SP MP | Debug: AI stop thinking. |
| `g_accuracyStanceCrouchMod` | `0.75` | C | SP | AI accuracy against a crouched player (fraction). |
| `g_accuracyStanceProneMod` | `0.5` | C | SP | AI accuracy against a prone player (fraction). |
| `g_accuracyVsAI` | `0.0` | C | SP | AI accuracy against other AI (0 = the weapon's own). |
| `g_accuracyVsHero` | `0.0` | C | SP | AI accuracy against the squad's heroes (0 = the weapon's own). |
| `g_accuracyVsPlayer` | `0.0` | C | SP | AI accuracy against the player (0 = the weapon's own). |
| `g_aiEffectiveRange` | `0.0` | C | SP | AI effective range override (0 = the weapon's own). |
| `g_aiMissRange` | `0.0` | C | SP | AI miss distance override (0 = the default). |
| `g_aiShowTargeting` | `0.0` | C | SP | Debug: shows who the AI are aiming at. |
| `g_connectpaths` | `0` |  | SP | Developer: connect the AI paths (a level-making tool). |
| `g_friendlyfireDist` | `384` |  | SP | Distance (units) within which you can't fire at a friendly. |
| `g_friendlyfireUseDist` | `256` |  | SP | Distance (units) for using (pushing) a friendly. |
| `g_friendlyfireUseTime` | `500` |  | SP | Time (ms) for using (pushing) a friendly. |
| `g_playerTrailTime` | `500` | C | SP | How often (ms) the AI note where you were, to follow you. |
| `g_spawnai` | `1` | LC | SP | Spawn AI (latched: from the next level load). |
| `gAIAllowEnemyReinforcements` | `1` |  | SP MP | Enemy reinforcements may spawn. |
| `gAIAllowFriendlyReinforcements` | `1` |  | SP MP | Friendly reinforcements may spawn. |
| `gAIMaxNumEnemyReinforcements` | `0` |  | SP MP | Most enemy reinforcements (0 = the level's own). |
| `gAIMaxNumReinforcements` | `0` |  | SP MP | Most friendly reinforcements (0 = the level's own). |
| `gAIMaxSpawnedEnemyCount` | `0` |  | SP MP | Most enemies alive at once (0 = the level's own). |
| `gAITosserInvalidateRange` | `512.000000` |  | SP MP | Distance (units) at which a grenade thrower's spot stops being used. |
| `max_drones` | `100` | T | SP | Most drones (simple background soldiers) at once. |

### Multiplayer (15)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_mpDebugAnimEntity` | `-1` |  | MP | Debug: the entity whose animations are traced in multiplayer (-1 = none). |
| `enemy_radar` | `1` |  | SP MP | Enemies show on the compass (firing without a silencer). |
| `mp_debug` | `0` |  | MP | Multiplayer debug output from the game modes. |
| `mp_debugrender` | `0` |  | MP | Multiplayer debug drawing. |
| `mp_debugrenderspawnpoints` | `0` |  | MP | Multiplayer debug: draws the spawn points. |
| `mp_friendlyfire` | `1` |  | MP | Friendly fire in team modes. |
| `mp_gametype` | `dm` |  | MP | The game mode (dm, tdm, ctf, war, bas ...; set from the match settings). |
| `mp_gametypename` |  |  | MP | Set by the game: the game mode's name. |
| `mp_lastmanstanding` | `0` |  | MP | Last man standing rounds. |
| `mp_roundlimit` | `0` |  | MP | Rounds in a match (0 = no limit). |
| `mp_scorelimit` | `0` |  | MP | Score that ends the match (0 = no limit). |
| `mp_teambalance` | `0` |  | MP | Keep the teams even. |
| `mp_timelimit` | `1` |  | MP | Minutes in a match. |
| `name` | `Unknown Soldier` | A | SP MP | Your player name. |
| `RadarFakeFriendlies` | `0` |  | SP MP | Debug: fake friendly markers on the compass. |

### Mouse, controller and keys (37)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_controllerconfig` | `0` | A | SP MP | The controller layout picked in the Options menu. |
| `cg_weaponCycleDelay` | `0` | A | SP MP | Delay (ms) between weapon switches while the button is held. |
| `cl_altFireButtonDown` | `0` | T | SP MP | Set by the game: the alternate fire button is held (read only in practice). |
| `cl_analogStickLean` | `0` | T | SP MP | Set by the game: leaning with the stick. |
| `cl_anglespeedkey` | `1.5` |  | SP MP | How much faster turning keys/stick turn while the speed key is held. |
| `cl_binocButtonDown` | `0` | T | MP | Multiplayer, set by the game: the binoculars button is held. |
| `cl_bomberKeyboardMode` | `0` | T | SP | Set by the game: the bomber mission's keyboard steering. |
| `cl_disable_ads` | `0` | T | SP MP | Set by the game: aiming down the sight is off for the moment. |
| `cl_freelook` | `1` | A | SP MP | Look with the mouse. |
| `cl_goStandJumpTime` | `0` | A | SP MP | Time (ms) the jump button has to be held to stand up from crouch or prone. |
| `cl_grenadeButtonDown` | `0` | T | SP MP | Set by the game: the grenade button is held. |
| `cl_mouseAccel` | `0` | A | SP MP | Mouse acceleration (the port's raw mouse aim is separate: F2 > Controls). |
| `cl_pitchspeed` | `140` | A | SP MP | Up/down turning speed with the pad or keys (degrees/second). |
| `cl_sensitivityX` | `0` | A | SP | The Options menu's horizontal look sensitivity. |
| `cl_sensitivityY` | `0` | A | SP | The Options menu's vertical look sensitivity. |
| `cl_stance` | `0` | T | SP | Set by the game: the stance button state. |
| `cl_stanceHoldTime` | `300` |  | SP MP | Time (ms) the stance button has to be held to go prone. |
| `cl_stanceTemp` | `0` | T | SP MP | Set by the game: a temporary stance change. |
| `cl_viewPitchCompensate` | `0` | R | SP MP | Set by the game: aim compensation up/down (vehicles, turrets). |
| `cl_viewYawCompensate` | `0` | R | SP MP | Set by the game: aim compensation left/right (vehicles, turrets). |
| `cl_yawspeed` | `140` | A | SP MP | Left/right turning speed with the pad or keys (degrees/second). |
| `g_useholdtime` | `0` | A | SP MP | Time (ms) to hold Use to use things (0 = a press). |
| `g_vehControlMode` | `1` | A | SP MP | Tank and jeep steering: 1 the camera steers, 0 the stick steers (World at War: vehControlMode). |
| `in_debugjoystick` | `0` | T | SP MP | Debug: print the controller's input. |
| `in_joyBallScale` | `0.02` | A | SP MP | Trackball scale (unused on PC). |
| `in_joystick` | `1` | AL | SP MP | Use the controller. |
| `in_mouse` | `0` | AL | SP MP | Use the mouse (the port reads the mouse itself). |
| `in_stickLegacy` | `0` | A | SP MP | The controller's legacy stick layout. |
| `in_stickSouthPaw` | `0` | A | SP MP | The controller's left-handed (southpaw) stick layout. |
| `joy_threshold` | `25` | A | SP MP | Controller stick dead zone. |
| `m_filter` | `0` | A | SP MP | Smooth the mouse over two frames. |
| `m_forward` | `0.25` | A | SP MP | Forward speed from the mouse when moving with it. |
| `m_pitch` | `0.7` | A | SP MP | Mouse up/down speed (degrees per count); negative inverts. |
| `m_side` | `0.25` | A | SP MP | Sideways speed from the mouse when moving with it. |
| `m_yaw` | `0.7` | A | SP MP | Mouse left/right speed (degrees per count). |
| `sensitivityX` | `1` | A | SP | Look sensitivity left/right (the Options menu sets it). |
| `sensitivityY` | `1` | A | SP | Look sensitivity up/down (the Options menu sets it). |

### Network and server (17)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cg_errordecay` | `100` |  | SP MP | How fast a prediction error is smoothed out (ms). |
| `cg_nopredict` | `0` | A | SP MP | Debug: no local prediction of your movement (moves only when the server answers). |
| `cl_nodelta` | `0` |  | SP MP | Debug: ask the server for full snapshots instead of changes. |
| `cl_shownet` | `0` | T | SP MP | Debug: print network messages (1-4 = more detail). |
| `cl_showSend` | `0` | T | SP MP | Debug: print the commands sent to the server. |
| `showdrop` | `0` | T | SP MP | Debug: print dropped network packets. |
| `showpackets` | `0` | T | SP MP | Debug: print network packets. |
| `sv_framerate_smoothing` | `1` |  | SP MP | Smooth the server's frame rate. |
| `sv_killserver` | `0` |  | SP MP | Set by the game: shut the server down. |
| `sv_mapname` |  | SR | SP MP | The server's map (read only). |
| `sv_pakNames` |  | R | SP MP | Set by the game: the server's data packs. |
| `sv_paks` |  | R | SP MP | Set by the game: the server's data pack checksums. |
| `sv_referencedPakNames` |  | R | SP MP | Set by the game: the data packs in use. |
| `sv_referencedPaks` |  | R | SP MP | Set by the game: the checksums of the data packs in use. |
| `sv_running` | `0` | R | SP MP | Set by the game: a server is running (read only). |
| `sv_serverid` | `0` | R | SP MP | Set by the game: the server instance (read only). |
| `sv_showCommands` | `0` |  | SP MP | Debug: print the commands players send. |

### Files and language (15)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `cl_language` | `0` | AL | SP MP | The text and voice language (latched: takes effect after a restart). |
| `cl_languagesavailable` | `0` |  | SP MP | Set by the game: which languages the disc has. |
| `cl_languagetranslate` | `1` | L | SP MP | Translate text through the language files (0 shows the raw string names). |
| `cl_languagewarnings` | `0` |  | SP MP | Debug: warn about strings missing from the language files. |
| `cl_languagewarningsaserrors` | `0` |  | SP MP | Debug: treat missing strings as errors. |
| `fs_basegame` |  | I | SP MP | The base game folder (read only). |
| `fs_basepath` |  | I | SP MP | The install folder (read only). |
| `fs_cdpath` | `d:` | I | SP MP | The disc folder (read only). |
| `fs_copyfiles` | `0` | I | SP MP | Developer: copy files from the disc folder (read only). |
| `fs_debug` | `0` |  | SP MP | Debug: print the files the game opens. |
| `fs_game` |  | I | SP MP | A mod folder (read only; the port doesn't load mods). |
| `fs_gamedirvar` |  |  | SP | Set by the game: the mod folder name (empty). |
| `fs_homepath` | `<fs_basepath>` | I | SP MP | The folder for the player's files (read only). |
| `fs_ignoreLozalized` | `0` | LC | SP MP | Debug: ignore the localized files (the name's typo is the game's). |
| `fs_restrict` |  | I | SP MP | Restrict the files to a demo set (read only). |

### System and debugging (60)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `arch` | `ps2` |  | SP MP | The platform the engine reports (the Xbox 360 build still says "ps2"); read only in practice. |
| `autostart` | `0` | I | SP | Internal start-up switch (not used on PC). |
| `bg_stickyAimRender` | `0` | C | SP MP | Debug: draws the aim assist's sticky targets. |
| `capture_dir` | `0` | I | SP | Movie capture folder number (developer tool, not used on PC). |
| `capture_movie` |  | I / - | SP MP | Movie capture name (developer tool, not used on PC). |
| `cg_configversion` | `0` | A | SP MP | The saved settings' version (the game resets old ones). |
| `checkpoint` | `0` |  | SP MP | Set by the game: the checkpoint being loaded. |
| `cl_avidemo` | `0` |  | SP MP | Developer: save this many frames a second as pictures (not used on PC; use the port's screenshot). |
| `cl_debugMove` | `0` |  | SP MP | Debug: graphs your movement input. |
| `cl_forceavidemo` | `0` |  | SP MP | Developer: record frames even when the game isn't active. |
| `cl_frameadvance` | `0` |  | SP MP | Debug: step the game one frame at a time. |
| `cl_freeze` | `0` |  | SP MP | Debug: freezes your input. |
| `cl_noprint` | `0` |  | SP MP | Print nothing to the console. |
| `cl_paused` |  |  | SP | Set by the game: the game is paused. |
| `cl_restartdeath` | `0` |  | SP MP | Set by the game: the mission is restarting after a death. |
| `cl_running` | `0` | R | SP MP | Set by the game: the client is running (read only). |
| `cl_showmouserate` | `0` |  | SP MP | Debug: print the mouse rate to the console. |
| `cl_testAnimWeight` | `0` |  | SP MP | Debug: an animation blend weight to test. |
| `cl_xmodelcheck` | `0` | AL | SP MP | Debug: check models as they load. |
| `cm_noCurves` | `0` | C | SP MP | Debug: curved surfaces don't collide. |
| `com_animCheck` | `0` |  | SP MP | Debug: check the animation trees. |
| `com_expectedhunkusage` | `-1` |  | SP MP | Set by the game: the memory the level is expected to need. |
| `com_journal` |  |  | SP | Developer: record or play back input (not used on PC). |
| `com_speeds` | `0` |  | SP MP | Debug: print how long each part of a frame took. |
| `com_statmon` | `0` |  | SP MP | Debug: the stats monitor on screen. |
| `con_debug` | `0` | A | MP | Multiplayer: the console's debug output. |
| `developer` | `0` | T | SP MP | Developer mode: more console messages (the port shows the game's answers anyway). |
| `developer_script` | `0` | T | SP MP | Developer mode for scripts: more script messages. |
| `disable_default_streaming` | `0` |  | SP MP | Developer: the default streaming of level data is off. |
| `exception` | `1` | I | SP MP | Internal: the game's exception handling switch (leave it alone). |
| `fixedtime` | `0` | C | SP MP | Debug: a fixed frame time (ms) instead of the real one. |
| `g_changelevel_time` | `4` |  | SP MP | Seconds before the next level loads. |
| `g_debugProneCheck` | `0` |  | SP MP | Debug: shows the prone space checks. |
| `g_debugProneCheckDepthCheck` | `1` |  | SP MP | Debug: the prone check's depth test. |
| `g_gameskill` | `1` | SRL | SP MP | Difficulty: 0 Greenhorn, 1 Regular, 2 Hardened, 3 Veteran (set from the menu; latched). |
| `g_reloading` | `0` | R | SP MP | Set by the game: a checkpoint or save is loading. |
| `g_restoreFullCheckpoint` | `1` | T | SP | Loading a checkpoint restores everything (not just the player). |
| `gamedate` | `Oct  9 2006` | R | SP MP | The game's build date (read only). |
| `gamename` | `main` | SR | SP MP | The game's name (read only). |
| `journal` | `0` | I | SP MP | Developer: record or play back input (read only). |
| `loading_movies` |  | I | SP | Set by the game: the movies shown while loading. |
| `logfile` | `0` |  | SP MP | Write the console to a log file (0 off, 1 buffered, 2 flushed at once; the port has its own log). |
| `mapname` | `nomap` | SR | SP MP | The current map (read only). |
| `nextdemo` |  |  | SP | Set by the game: the next demo to play. |
| `nextmap` | `map_restart` | T | SP MP | The command run when the level ends (set by the game). |
| `num_banks` | `80.000000` | I | SP MP | Set by the game: the number of sound banks. |
| `outsideMapEnts` | `0` | C | SP MP | Debug: allows entities outside the map. |
| `show_playerdist` | `0` |  | SP MP | Debug: prints distances to the player. |
| `showvehmap` | `0` |  | SP MP | Debug: shows the vehicle path map. |
| `sound_debug` | `0` |  | SP MP | Debug: sound information. |
| `sv_cheats` | `0` | R | SP MP | Cheats on (read only: use the port's sv_cheats / cheats command; in multiplayer only the host, and everyone is told). |
| `sys_cpuid` | `1` |  | SP MP | Set by the game: the CPU type. |
| `timerbars_advance` | `1` | T | MP | Multiplayer debug: the frame timing bars' advance part. |
| `timerbars_dma` | `1` | T | MP | Multiplayer debug: the frame timing bars' DMA part. |
| `timerbars_on` | `0` | T | MP | Multiplayer debug: frame timing bars on screen. |
| `timerbars_scene` | `1` | T | MP | Multiplayer debug: the frame timing bars' scene part. |
| `timerbars_user` | `1` | T | MP | Multiplayer debug: the frame timing bars' user part. |
| `timescale` | `1` | C | SP MP | Game speed (cheat; the port keeps it 1 outside sv_cheats 1). |
| `ui_campaign` | `american` |  | SP | Set by the game: the campaign of the current mission (american, british, canadian, polish). |
| `viewlog` | `0` | C | SP MP | Debug: show the console log window. |

### Level script switches (39)

| dvar | default | flags | in | what it does |
|---|---|---|---|---|
| `animDebug` |  |  | SP | Level script switch (read, never set): animation debugging in the level scripts. |
| `artist` |  |  | SP | Level script switch: "artist mode" for the level builders (the scripts print "Artist Mode Enabled!"). |
| `artist_drive` |  |  | SP | Hostage's level script switch: an artist mode for the drive. |
| `battlechatter_debug` |  |  | SP | Level script switch (read, never set): squad chatter debugging. |
| `cg_atmos` | `-1` |  | SP | Set by the level scripts at the start of every mission (-1). |
| `cg_atmosDense` | `999` |  | SP | Set by the level scripts at the start of every mission (999). |
| `cg_hudcom` | `1` |  | SP | Set by the level scripts: the compass is allowed (1). |
| `cg_victoryscreen_menu` | `off` |  | SP | Set by the level scripts: the mission complete screen's menu ("off" until the end). |
| `chain` | `0` |  | SP | Level script switch (set to 0 at the start): developer chain test. |
| `debug` | `0` |  | SP | Level script switch (set to 0 at the start): the level scripts' debug output. |
| `debug_ai_location` |  |  | SP | Mayenne's level script switch (read, never set): AI location debugging. |
| `debug_audio` |  |  | SP | Level script switch (read, never set): sound debugging in the level scripts. |
| `debug_battlechatter` | `0` |  | SP | Level script switch (set to 0 at the start): squad chatter debugging. |
| `debug_drawcone` |  |  | SP | Level script switch ("off" at the start): draws the AI's view cones. |
| `debug_mortars` | `0` |  | SP | Level script switch (set to 0 at the start): mortar debugging. |
| `debug_stundraw` |  |  | SP | Level script switch ("off" at the start): draws stun (shell shock) areas. |
| `debug_tankall` |  |  | SP | Level script switch ("off" at the start): tank debugging, all tanks. |
| `debug_tanknone` |  |  | SP | Level script switch ("off" at the start): tank debugging off for all tanks. |
| `debug_tankque` |  |  | SP | Level script switch ("off" at the start): tank debugging, their order queue. |
| `debug_tanksamples` |  |  | SP | Level script switch ("off" at the start): tank debugging, path samples. |
| `debugplayerangles` |  |  | SP | Level script switch (read, never set): prints your view angles. |
| `fallback` | `0` |  | SP | Level script switch (set to 0 at the start): the AI fall back positions' debugging. |
| `ff_off` |  |  | SP | Level script switch (read, never set): friendly fire mission failure off. |
| `g_useGear` |  |  | SP | Level script switch (read, never set): the soldiers' gear. |
| `group_killtime` |  |  | SP | Hostage's level script switch (read, never set): a squad's kill timer. |
| `hummer` |  |  | SP | Fuel Plant's and Saint-Lo's level script switch (read, never set): a jeep test. |
| `melee_clinch` |  |  | SP | Level script switch (read, never set): the melee struggle (battle action) test. |
| `mg42` |  |  | SP | Level script switch ("off" at the start): machine gun nest debugging. |
| `no_shrimps` |  |  | SP | Level script switch (read, never set): leaves out some of the level's scenery. |
| `notank` |  |  | SP | Level script switch (read, never set): no tanks. |
| `path_assist` |  |  | SP | Mace's level script switch: path help for the player. |
| `quickevents` |  |  | SP | Mayenne's level script switch: quick events (battle actions) test. |
| `remove_nonheroes` |  |  | SP | Mayenne's level script switch: removes the soldiers who aren't heroes. |
| `setplayerpitch` |  |  | SP | Level script switch: sets your view pitch at the start (0 = no). |
| `setplayeryaw` |  |  | SP | Level script switch: sets your view yaw at the start (0 = no). |
| `sim_nextgen` |  |  | SP | Mayenne's level script switch: a next-gen simulation test. |
| `skipto` | `0` |  | SP | Level script switch (set to 0 at the start): developer skip to a later part of the mission. |
| `start` | `start` |  | SP | Level script switch ("start" at the start): the start point of the mission (developer). |
| `truck_drawtags` |  |  | SP | Level script switch ("off" at the start): draws the trucks' attachment points. |

<!-- end of the dvar tables -->
