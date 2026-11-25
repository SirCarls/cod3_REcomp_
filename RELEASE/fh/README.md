# Call of Duty: Finest Hour (original Xbox) on PC

[acct2-bro] 2026-11-08. The port of Finest Hour (Spark Unlimited, 2004; the engine is RenderWare) to PC, the way Big
Red One's port (`../bro`) works: xboxrecomp turns the game's own `default.xbe` into C, and that C runs on xboxrecomp's
kernel replacement. The game's files come from the owner's own disc; nothing of the game is in this repository.

## Phase 1 (now): boot to a log, nothing drawn

Nothing of the Xbox's GPU or sound chip is emulated. Every function of the game's Direct3D (the XDK's D3D8 library,
which the XDK put in the XBE section `D3D`, 292 functions) and DirectSound (section `DSOUND`, 456) is a **stand-in**
that logs its call and returns. The stand-ins come from one table, `config/stand_in_functions.tsv`: per function its
address, its name where one is known, and how many argument bytes its own `ret N` removes and which registers it takes
arguments in (both read from its instructions by `tools/bro_decomp/ngl_stubs.py`). The build turns the table into C
(`tools/fh_decomp/fh_stubs.py`).

Where it is (2026-11-08, boot 13): the game starts, opens RenderWare and its Direct3D device, reads `COD.PAK`, shows
its loading banner, loads its first sound banks, plays its start-up movies (`atvi`, `spark`, `kuju`, `Intro`; the
frames are decoded by the game's own Bink code into the render target, and `FH_FRAME_DUMP` saves one) and reaches its
front end, which draws a few primitives a frame through the Direct3D stand-ins and plays its music buffer.

The next phases: the native renderer replaces the Direct3D stand-ins (RenderWare's Xbox driver calls them, so they
become calls to Vulkan), then sound, then input, then the settings and the F2 menu every game in the collection has.

## Phase 2 (started 2026-11-09, [waw-decomp]): first light

The owner chose (2026-11-09 14:42) the Europe v3 disc as the base and the renderer cut **at Direct3D's level**: the
game's RenderWare stays the game's own, and about 25 Direct3D functions (plus the game's Xbox vertex shader programs
and pixel shader definitions) get native answers on Vulkan.

- **The window:** `D3DDevice_Swap` (0x0022D440) shows the render target at the end of each frame
  (`src/fh_present.c`): through `../ngl_vk` (the Vulkan presenter Big Red One and Call of Duty 3 share) in an SDL3
  window, or offscreen without a display. What the game draws by the CPU shows at once: its Bink movies and XFONT
  text.
- **The device's state:** SetStreamSource, SetVertexShader, SetTexture, SetPalette, SetRenderState,
  SetTextureStageState, SetTransform and SetViewport keep what the game gives them (`src/fh_d3d_state.c`), so each
  DrawIndexedVertices can be drawn with the state current then. `FH_DRAW_LOG=<frame>` writes everything one frame does
  through them to the log (the draw census): the evidence the native draws are written from.
- **The draws** (`src/fh_draw.c`, 15:30): each DrawIndexedVertices goes to Vulkan through
  `../ngl_vk/include/ngl_vk_low.h` (the layer for renderers that bring their own shaders), with fixed-function shaders
  of our own (`shaders/fixed.vert`, `fixed.frag`, made into SPIR-V when the port is built): FVFs with position,
  diffuse color and texture coordinates, texture stage 0, the alpha test, blending, the depth test; swizzled P8 (with
  its palette), A8R8G8B8 and X8R8G8B8 textures and their linear forms. What isn't written yet is left out and counted
  in the log. Draws whose vertex format is 0 (the title screen's background) use the last FVF the game used with the
  same stride; `FH_FVF0_AS=<hex>` forces one. **2026-11-09 15:25: the title screen ("Press START") is drawn in full.**
- Next: whatever the front end shows that is still missing, then input.

## How the table was made

From the XBE alone, read as data (the tools take every path as an argument):

1. `tools/fh_decomp/xdk_names.py` names 2,839 of the game's XDK library functions from Big Red One's campaign, which
   was built with the same XDK (1.0.5849): the same library code at other addresses, paired by its bytes with the
   addresses masked. Direct3D pairs poorly (Big Red One links the link-time-compiled D3D8LTCG): 25 of 292 are named.
2. `tools/fh_decomp/fh_stand_in_spec.py` lists the D3D and DSOUND sections' functions with those names (the others
   are called `D3D_<address>` or `DSOUND_<address>`).
3. `tools/bro_decomp/ngl_stubs.py` reads each one's `ret N` and register arguments with objdump.

After that the table is kept by hand, as Big Red One's is: a row gets a real name, and a native answer in its
`stand_in_return` column, once a boot log shows what the game needs from it (the evidence is in `src/fh_d3d.c`).

One row is not Direct3D or DirectSound: `RW__RwErrorSet` (0x001CFDE0), where every RenderWare error is recorded.
Its stand-in records the error as the game's own code does and prints each error with its caller
(`fh_rw_error_set` in `src/fh_log.c`), so a boot log shows where RenderWare fails.

14 rows had no usable `ret N` (their `problems` column says why: a tail jump not followed, or data in the section
read as code). Six of them are tail jumps whose target's `ret N` was then read by hand (0x00223D00, called by RenderWare's
driver just before the loading banner's texture list broke, had been leaving 4 bytes on the stack); the others
still pop only the return address, and a boot log that stops there points at them.

## Build

```
# 1. the recompiler, with the stand-ins' addresses as --manual-functions (a JSON list of the table's first column)
python -m tools.recomp default.xbe --game-name "Finest Hour" --all --split 250 --gen-dir GEN -o OUT \
    --manual-functions manual_functions.json -v          # run from the xboxrecomp checkout
# 2. the port
cmake -S fh -B fh/out -DXBOXRECOMP_DIR=<xboxrecomp checkout> -DFH_GEN_DIR=GEN
cmake --build fh/out -j
```

The xboxrecomp checkout is the one Big Red One's port uses, with its lazy-decode patch (`bro/patches/`), and
`patches/xboxrecomp-case-insensitive-paths.patch` (`git apply`): on Linux the game's paths are found whatever the
case of the extracted disc's folders and files, as on the console.

## Run

`fh GAME_DIR [SAVE_DIR]`: GAME_DIR holds the game's files from the disc. `FH_LOG=0/1/2` sets how much the stand-ins
print (`src/fh_log.h`). `FH_FRAME_DUMP=FILE.ppm` writes what the game has drawn into its render target by the CPU
(a Bink movie frame, or XFONT text) once, after `FH_FRAME_DUMP_AT` locks of it (default 300; `src/fh_d3d.c`).
`FH_SKIP_FILMS=1` stops every movie as soon as it starts, for tests (the game's own movie stop flag, held with
xboxrecomp's `RECOMP_POKE`; `src/main.c` says how it was found). The game's own way, a button on the pad, works
since the pad is native (below).

Levels load (2026-11-13, `dev/agents/acct2-fh/TEST-RUNS.md` boot10-15b). Two settings matter, and the port sets both:
- The code is lifted with xboxrecomp's `--back-edge-yield` (into the work folder's `gen3`, by
  `dev/agents/acct2-fh/tools/fh_gen3.sh`). The game's level loader thread waits for work in a loop with no kernel
  call in it (`0x00068DC0`-`0x000691DB`); with `RECOMP_GUEST_LOCK=1` (one guest thread at a time, as on the console's
  one CPU) that thread kept the CPU for ever unless loop back edges can hand it over.
- `RECOMP_ASYNC_IO=1`, set by `src/main.c` unless `FH_ASYNC_IO=0`: a read of a file opened for asynchronous I/O
  answers "pending", as on the Xbox. RenderWare's file system runs a read's completion callback at once when the
  read finishes at once, and the game's stream code (`0x00156790`) reads while holding the lock that callback
  takes, so a read that finished at once deadlocked the level load on its own thread.

The pad (`src/fh_pad.c`, 2026-11-13): XAPI's seven gamepad functions are stand-ins (the stand-in table's "input"
rows), so the game sees one Xbox pad in the first port, driven by a real controller (SDL3), the keyboard while the
window has the focus (W A S D the left stick, the arrows the d-pad, I J K L the right stick, Return start, Escape back,
Space A, Left Ctrl B, R X, F Y, Q white, E black, Z and X the triggers), and `FH_PAD_SCRIPT` for tests:
`FH_PAD_SCRIPT=15000:start,20000:a,22000:down:400` (ms from the game's first pad read : buttons joined with + : hold
ms, default 150; buttons up down left right start back lthumb rthumb a b x y black white lt rt, and the sticks
move_forward move_back move_left move_right look_up look_down look_left look_right). `FH_PAD=0` unplugs it.

The window (`src/fh_present.c`): `FH_VK=0` draws nothing; `FH_WINDOW=0` draws offscreen; `FH_FULLSCREEN=1`;
`FH_VSYNC=0`; `FH_START_IN_BACKGROUND=1` opens it behind the window in use, taking no focus until clicked (test runs);
`FH_SIZE=<w>x<h>`; `FH_SHOW_FPS=1` puts the frame rate in the title and in the log every 5 seconds;
`FH_VK_DUMP=<frame>[,...]` with `FH_VK_DUMP_PATH=<file.ppm>` saves what Vulkan drew. The draw census:
`FH_DRAW_LOG=<frame>[,...]` (from frame 2; frames count Swaps); a census frame prints at most 800 lines of draws
and states, `FH_DRAW_LOG_LINES=<n>` another number (a frame in a level needs about 25,000).

The vertex shaders (`src/fh_vsh.c`, 2026-11-13): the game's CreateVertexShader and its two inline constant writers
are native. A shader made without a program (the level's world) is drawn by register meaning; one with a program
(the clones) has the program run on the CPU for each vertex (each program is decoded once, when it is made).
`FH_VSH_RUN=0` leaves program draws out; `FH_VSH_W=0` drops w after the program's divide, as build10 did (program draws
then have no clipping behind the eye and no perspective in their texture coordinates; for comparison runs only).
A draw's vertices go through the program up to 64 at a time (`fh_vsh_run_many`, the same results to the bit, about
three times faster); `FH_VSH_MANY=0` runs them one at a time. Since 2026-11-13 16:25 the programs run on the GPU instead
(`shaders/program.vert`: one shader that reads a program's instructions from the pixel constants and runs them per
vertex as `fh_vsh_run` does), except a program that writes a constant register or reads an input with no Vulkan vertex
format, which stays on the CPU. `FH_VSH_GPU=0` runs every program on the CPU (comparison runs). The four constant writers the game uses
(`0x00221450`, `0x00221500`, `0x002214B0`, `0x002215B0`; also `0x00221690`) are native and keep the game's own copy
of the registers where the real functions do.
The skinned meshes (2026-11-13 17:20): RenderWare's skinned pipeline loads its program into the NV2A's program memory
(LoadVertexShaderProgram, `0x002217F0`) and draws with SelectVertexShader (`0x00221860`), which names a declaration
and the program's start without SetVertexShader. Both are native now (`src/fh_vsh.c` keeps the 136-instruction program
memory, `src/fh_d3d_state.c` the selection), and such a draw runs the memory's program from the selected start with
the selected declaration (`fh_vsh_selected`). Their declarations read two streams (stream 1: the bone indices and
weights, 3, 6 or 9 bytes a vertex); the draw puts each vertex's two streams side by side in a buffer of its own and
runs the program on that, on the CPU while the declaration has a packed normal. `FH_VSH_SELECTED_LOG=1` prints each
(declaration, start) pair once. Until then these draws ran the last SetVertexShader's declaration through the fixed
pipeline, without their bones.
The draws without indices (2026-11-13 16:10): DrawVertices (`0x00221F60`: the particles' and RenderWare's quad lists)
and DrawVerticesUP (`0x00221DF0`: RenderWare's 2D and the game's screen quads, from a pointer) are drawn like
DrawIndexedVertices. Quad lists, quad strips and polygons become triangles, and screen-space vertices (FVF XYZRHW) are
mapped from the viewport's pixels into clip space. GetViewportOffsetAndScale (`0x002223F0`) and the multisample scale
(`0x0022CBC0`) answer in the port's clip space (`src/fh_d3d_state.h` says why); `FH_SCREEN_ANSWERS=0` leaves both
unanswered as their stand-ins did (comparison runs only).
Culling and the depth test (2026-11-14, `src/fh_draw.c` pipeline_state, read from the game's own handlers): CULLMODE
(state 147, `0x00222730`) drops the triangles that run the way it names on the screen (0x900 clockwise, 0x901
counterclockwise; FRONTFACE only picks which face the NV2A calls them), ZENABLE (143, `0x002235B0`) turns the depth
test on and off (with ZFUNC's compare, ALWAYS included), and ZBIAS (149, `0x00222970`) becomes the polygon offset in
states 77, 78 and 81, which the draws take as Vulkan's depth bias. Before build36 nothing was culled (a double-sided
surface's two faces fought for the same pixels: the owner's flickering tent) and the depth test followed ZFUNC alone.
`FH_CULL=0` culls nothing and `FH_CULL=-1` the other way round, `FH_ZENABLE=0` follows ZFUNC alone and
`FH_DEPTH_BIAS=0` drops the bias (comparison runs); `FH_CULL_PROBE=<frame>` draws that frame as the game asks, the next
the other way round and the one after without culling, for `FH_VK_DUMP` of all three.
Every 5 seconds, beside the frame rate, `[fh] draw time a frame` says where the draw time went, a frame on average:
all draws, the vertex programs (and ns a vertex), the declaration rewrites, the plain copies, textures, pipelines and
Vulkan draws, the frame end (submit and wait), and the most of ngl_vk_low's 32 MB frame room one frame used.

The frame rate (`src/fh_frame_pace.c`, 2026-11-13): each Swap is held to the presentation the game asked the Xbox
for (60 Hz, every vertical blank: 60 frames a second), because the game's speed follows its frame rate.
`FH_FRAME_PACING=0` lets frames come as fast as they can (tests only: the game then runs faster than on the console).
Run with `RECOMP_GUEST_LOCK=1` (xboxrecomp: one guest thread runs guest code at a time, as on the console's one CPU);
the game's loading and title threads share RenderWare's lists with no lock (`dev/agents/acct2-fh/SOURCES-REVIEW.md`).

Above 60 frames a second (`src/fh_frame_clock.c`, 2026-11-22): with `game.fixed_step_above_60` (`FH_FIXED_STEP`,
on by default from build68) the game's update runs in whole 60ths above 66.7 frames a second, as its step was never
smaller on the Xbox; slower frames are as before, and the pad is held between updates (`src/fh_pad.c`). A frame held
at a step of 0 is drawn by the timer itself (the in-play loop skips its drawing on such a frame: boot92 drew at 59.9).

The game's clears (`src/fh_d3d_state.c`, 2026-11-22): D3DDevice_Clear is answered natively; a clear of the depth in
the middle of a frame (as before the first-person arms and gun) is done in the frame with `video.depth_clear`
(`FH_DEPTH_CLEAR`, on by default). The frame-rate log's `[clears]` line counts them. Why:
`dev/agents/acct2-fh/FRAMERATE-AUDIT.md` F3, F10 and F11.

Where the main thread's frames go (`src/fh_profile.c`, 2026-11-13): `FH_PROFILE=<file>` samples the thread that
calls Swap 1000 times a second of wall-clock time (`FH_PROFILE_HZ=<n>` another rate) and writes each sample's
addresses, innermost first, to the file; `dev/agents/acct2-fh/tools/fh_profile.py <file> build/fh` prints the time
by kind (game code, draw code, vertex programs, frame pacing, waiting for the guest CPU, Vulkan), by function and by
game function (`sub_<guest address>`). Waits count, as they are wall-clock time. For measuring runs only (Linux x86-64).

For finding what a screen waits for (`src/fh_watch.c`, 2026-11-13): `FH_WATCH=<address>[:<bytes>][,...]` (hexadecimal
guest addresses, 1, 2 or 4 bytes each, 4 if not given) prints those words at the first Swap and then whenever one
changes, with the frame number (`[watch] frame 812: 0x003B67E0 (4) = 0x00000000, was 0xFFFFFFFF`). It only reads.

The console (`src/fh_console.c`, 2026-11-14; `dev/agents/acct2-fh/CONSOLE.md`): the F2 window's CONSOLE page, which
`~` (the key left of 1) opens directly, as in World at War. Finest Hour has no console of its own, so this is the
port's: the log's last 1,000 lines (stderr goes on to where it went, a copy is kept; `interface.console_log = false`
or `FH_CONSOLE_LOG=0` leaves stderr alone), and a line for commands. fh.toml's settings are its variables: a name alone
shows the value and where it comes from (set while playing, its `FH_*` variable, fh.toml, or the default), a name and a
value sets it, as the F2 rows do (`display.max_fps 144`). Commands: `help [name]` (a setting's help is fh.toml's own
comment), `list`/`cvarlist [text]`, `cmdlist`, `get`, `set`, `toggle`, `save`, `exec <file>`, `echo`, `clear`, `condump <file>`; `;`
separates commands, Tab completes a name, Up and Down go through what was typed. `FH_CONSOLE_EXEC="cmd; cmd"` runs
commands at the first frame; `FH_F2_TEST`'s `console <line>` step runs one at a set time (test runs).

The game's own debug menu is still in its program, shut, with nothing left that opens it (`src/fh_game_vars.c` has how
it works). Its values are the console's variables too, by names of the port's, read and written in the game's memory
at once and never saved: `debug.*` (its Renderer Switching, Game Settings, Cheats and Debug Switches pages: the
renderers it can turn off, the light scales, the game speed, unlimited ammo, four values left for testing), `perf.*`
(its Performance Statistics page) and `graph.*` (its 49 performance graphs; a level's load turns them off).
`timescale [speed]` is the game speed (1 normal), and `debugmenu [on|off]` opens the menu itself, worked with the pad.
`cheat` lists the seven cheats of the game's Extras screen (Big Head, Ricochet, Head Shot, Ricochet All, Shrink Shot,
Dead Eye, Big 'N' Small) and `cheat <name|1-7> on|off|toggle` sets one in the game's memory at once; the game keeps them
in the profile and saves them with it, as it does from Extras (turning one on unlocks it there too, so Extras can turn
it off). `unlockcheats` unlocks all seven, as the game's own pad code on its menus does; `unlocklevels` opens every
level at the level select (the profile's furthest level set to the last); `unlockall` does both and opens the three
campaigns' films. `debugmenu back` is the menu's own Back (up a page; at the first page it shuts the menu).
`god [on|off]` is the game's own "the player takes no damage" switch (what its level scripts use), kept on through
levels while it is on. `ammo [on|off]` (or `noreload`) is the debug menu's Unlimited Ammo, kept on the same way: the
magazine never runs down, so there is nothing to reload. `pos` shows where the player is, the weapon in hand, the
rounds in its magazine and the health left (1.000 full, of the player's hit points; read only). `hurt [<damage>]`
(100 alone) is a hit on the player made as the game makes one, through the player's own "take a hit": `god` stops
it, the difficulty scales it, and enough of it kills the player (it also says what in the game stopped a hit, when
something did). `difficulty [0|1|2]` shows the game's difficulty and the scale it gives the damage the player takes;
with a number it sets it where the level select does (what each level starts with) and for the level being played.
`hits [on|off]` prints a line for each change of the player's health as it comes: the seconds since the start, the
health before and after, the points of the hit, the game's time and frame, and what is left of the time after an
attacker's hit in which no other attacker's hit counts (read only).
`weapons` lists the weapons whose data gives their bullets a damage, by number and name, with that damage before the
difficulty's scale (read only).
`hitcalls [on|off]` prints a line for each hit the game hands the player, whether it takes any health or not: the game's
time and frame, which of the game's calls handed it, the hit's kind, its damage, the shooter and its class, and the
health before and after; for a projectile's hit, a second line with how old the projectile was, how long it may fly, its
speed, damage record and class. Every 5 s of the game's time a line counts the rounds the game made since, by kind, with
how far from the player they started (the mean and the nearest), and another counts the launches since by the code that
fired them (past the projectile factory and its one caller). It watches the game's own "take a hit", projectile
step, the projectile's five launch functions and its set-up, and runs them unchanged (read only).
`teleport <x> <y> <z>` moves the player there by the game's own "set the position" (the one its level scripts'
teleports call); y is up, and `~` counts from where the player is (`teleport ~ ~200 ~` is 200 up). `noclip
[on|off|<speed>]` lets the player go through anything: the left stick moves along the way the player faces, A goes
up and B down, the same distance a second at any frame rate (400 units a second at first, walking is about 260); it
turns itself off when the player is made anew (a level's start, a death).
`give [<weapon>|<name>|next]` puts a weapon into the player's hand the way the level scripts' SetPlayerWeapon does;
only the weapons the level loaded can be given (`give` alone lists them with the game's own names, `give next` takes
the next one, `give nagant_sniper` one by name). A level also loads the guns of its tanks and mounted weapons (L_1_2's
29, "T34 Cannon"); in the hand those don't fire.
`map [<level>]` goes to another level the way the level scripts' QuitAndLoadNextLevel does, by number (1 to 38) or
by the game's name for it (`L_2_3`, or `2_3`); `map` alone lists them and says which is played.

Level changes (`src/fh_relaunch.c`, 2026-11-15): Finest Hour starts itself again to change levels, as the console
does: at the end of a level it saves the profile, puts the next level in its launch data and reboots into its own
`default.xbe`, and the new start goes straight into that level. The port does the same: it starts again (the same
process on Linux; a new one on Windows, not tried yet) and gives the game its launch data back, so the window closes
and opens. `FH_RELAUNCH=0` ends the game there instead, as before. The test scripts `FH_PAD_SCRIPT` and
`FH_F2_TEST` don't run again after a restart; `FH_PAD_SCRIPT_NEXT` and `FH_F2_TEST_NEXT` take their place for the
next start when set. The log says `[fh] relaunch N` at each.

## Files

| | |
|---|---|
| `CMakeLists.txt` | the build |
| `config/stand_in_functions.tsv` | the stand-ins' table |
| `src/main.c` | loads default.xbe, brings up the kernel replacement, calls the game's entry point; `FH_SKIP_FILMS` |
| `src/recomp_manual.c` | the hooks the recompiled code calls: the stand-in lookup and three trouble reports |
| `src/fh_relaunch.c`, `fh_relaunch.h` | a level change: the game's reboot into itself, the port started again with its launch data |
| `src/fh_log.c`, `fh_log.h` | the stand-ins' logging and the guest memory they hand back |
| `src/fh_d3d.c`, `fh_d3d.h` | native answers for the Direct3D stand-ins the game can't go on without (the display modes, the device, its render target, depth buffer and back buffer, their GetDesc and LockRect, D3D8's Lock2 and word copier) |
| `src/fh_d3d_state.c`, `fh_d3d_state.h` | the device's state as the game sets it, and the draw census |
| `src/fh_present.c`, `fh_present.h` | Swap: the window and the picture, through `../ngl_vk` |
| `src/fh_pad.c`, `fh_pad.h` | XAPI's gamepad functions: a controller, the keyboard and `FH_PAD_SCRIPT` as one Xbox pad |
| `src/fh_frame_pace.c`, `fh_frame_pace.h` | each frame held to the game's presentation (60 a second), the game's other threads running meanwhile |
| `src/fh_watch.c`, `fh_watch.h` | `FH_WATCH`: guest memory words printed as they change, frame by frame |
| `src/fh_console.c`, `fh_console.h` | the console: the log's last lines and the commands (F2's CONSOLE page, `~`) |
| `src/fh_game_vars.c`, `fh_game_vars.h` | the game's debug menu's values and graphs by name, for the console, the menu opened, and the game's cheats |
| `src/f2/` | the F2 settings window (Call of Duty 3's, shared), its pages and the CONSOLE page |
| `src/fh_draw.c`, `fh_draw.h`, `shaders/` | the game's draws on Vulkan (`../ngl_vk/src/ngl_vk_low.c` is built in with them) |
| `patches/` | changes to the xboxrecomp checkout this port needs |
| `../bro/src/runtime/no_gpu_emulation.c`, `../bro/src/x86_privileged.h` | shared with Big Red One's port |
| `../ngl_vk/` | the Vulkan presenter and renderer, shared with Big Red One and Call of Duty 3 |
