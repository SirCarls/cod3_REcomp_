# Call of Duty 2: Big Red One on PC

Big Red One's own engine, recompiled from the original Xbox program to C, with its graphics library (Treyarch's
NGL) rewritten on Vulkan and its sound library (NSL) on a PC audio library. Nothing here emulates the Xbox's
graphics or audio chips.

Status: the menus and the campaign's levels play, drawn with Vulkan in a window, with the game's own sound and music,
on a gamepad or keyboard and mouse. It builds for Linux, and for Windows (cross-compiled on Linux; it runs under Wine
too).

## How it fits together

- **The engine** is recompiled ahead of time with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (MIT) from
  the user's own `default.xbe`. The generated C stays on the user's machine and is never published.
- **Graphics** come from `../ngl_vk/`, shared with Call of Duty 3: NGL's functions on Vulkan. xboxrecomp's Direct3D
  and NV2A libraries are not linked. How the renderer works, and how to change it: `../docs/NATIVE_RENDERER.md`.
- **The window, sound out and gamepads** go through SDL3: the same static SDL3 Call of Duty 3's SDK (rexglue) builds,
  so both games use one SDL. xboxrecomp's audio-chip emulation is not linked.

## Layout

| Folder | What it holds |
|---|---|
| `config/` | per-title settings for the recompiler: the stand-ins (`stand_in_functions.tsv`), the functions the disassembly is seeded with (`seed_functions.json`), test runs |
| `patches/` | our changes to xboxrecomp, applied at build time (`git apply`) |
| `src/` | the game project: entry point (`main.c`), NGL on the PC (`ngl/`), NSL and movies (`audio/`), the pad (`input/`), script threads (`script/`), crash reports (`debug/`) |
| `generated/` | the recompiler's output, for a build by hand (made on your machine, not in the repository) |
| `out/` | a build by hand (made on your machine) |

The stand-ins (`config/stand_in_functions.tsv`) are the NGL, Direct3D and DirectSound functions the port replaces.
`tools/bro_decomp/ngl_stubs.py` turns the table into C at build time. To replace another of the game's functions,
add its address and name to the table and write the C function in `src/`.

## What you need

Your own copy of the game: the Xbox disc image (full or trimmed) or a folder copied from the disc. The port's tables
name addresses in one program, the `default.xbe` with MD5 `3b50372b121dc4e51b2b3dbda7cba19d`; the build checks it
first. Everything game-specific is made on your machine from your disc; the repository holds tools and patches only.

To build: CMake 3.20 or newer, a C compiler, git, Python 3 with venv, SDL3 (see below), OpenSSL, the Vulkan headers
and loader, and glslc.

## Building with the setup tool

The setup tool's **Big Red One** tab (`sh setup.sh` in the project folder) does it the same way as Call of Duty 3:
choose your disc on its Set up page, press Build (it lists what it would download or install and waits for your
yes), then Play. From the command line:

```
python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py bro check "<the .iso, or a folder copied from the disc>"
python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py bro build "<the .iso, or a folder copied from the disc>"
python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py bro play [--level gela] [--fullscreen] [--mute]
```

The build:

1. checks the disc and copies its files into `output/bro/game`;
2. fetches xboxrecomp at the commit the port was made with into `work/tools/`, and Capstone (its disassembler) into
   a Python environment of its own beside it;
3. copies xboxrecomp into the work folder and applies `patches/`;
4. recompiles `default.xbe` with xboxrecomp's steps: `xbe_parser`, `disasm` (seeded with `config/seed_functions.json`),
   `func_id`, `abi_analysis`, then `recomp --all --split 250` with the stand-ins' addresses as `--manual-functions`
   (about an hour, mostly the disassembly, on one core);
5. builds this folder with CMake against the generated C;
6. installs `output/bro/bin/bro`, `output/bro/play.sh` and `output/bro/saves/`, and deletes the work folder's build
   files (xboxrecomp and its Python stay for the next build).

A build that stops goes on where it stopped: the copied files and each finished recompiler step are kept.

## Building by hand

1. Check out xboxrecomp at commit `f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b` (v0.16.0) and apply `patches/*.patch`
   with `git apply`. `pip install capstone`.
2. In xboxrecomp's folder, with `default.xbe` copied to `game_files/`:

   ```
   python -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json
   python -m tools.disasm game_files/default.xbe -v --seed-functions <repo>/bro/config/seed_functions.json
   python -m tools.func_id game_files/default.xbe -v
   python -m tools.abi_analysis game_files/default.xbe -v
   awk -F'\t' 'NR>1 {printf "%s\"%s\"", (n++ ? "," : "["), $1} END {print "]"}' <repo>/bro/config/stand_in_functions.tsv > manual_functions.json
   python -m tools.recomp game_files/default.xbe --game-name "Big Red One" --all --split 250 \
       --gen-dir <repo>/bro/generated --manual-functions manual_functions.json
   ```
3. Configure and build:

   ```
   cmake -S bro -B bro/out -DXBOXRECOMP_DIR=<xboxrecomp checkout> -DCMAKE_BUILD_TYPE=Release \
       -DBRO_SDL3_INCLUDE_DIR=<folder holding SDL3/SDL.h> -DBRO_SDL3_LIBRARY=<libSDL3.a>
   cmake --build bro/out -j
   ```

   SDL3: the static library Call of Duty 3's SDK builds (`rexglue-sdk/thirdparty/sdl3`; its headers are in that
   folder's `include/`), or leave both settings out to use an SDL3 installed on the system. Without any SDL3 the game
   still builds, but draws offscreen, with no sound out and no pads.
4. `BRO_VK=1 bro/out/bro <folder with the game's files> <a folder for saves>`.

### For Windows

Cross-compiled on Linux with llvm-mingw (the toolchain the setup tool fetches for Call of Duty 3's Windows build):

- a CMake toolchain file for `x86_64-w64-mingw32-clang`, with `CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER` so the
  build machine's `glslc` is found;
- SDL3 built by the SDK for Windows (`-DBRO_SDL3_LIBRARY=<the Windows libSDL3.a>`);
- `-DVulkan_INCLUDE_DIR=<the SDK's thirdparty/vulkan-headers/include>`,
  `-DVulkan_LIBRARY=<libvulkan-1.a>` (made with
  `x86_64-w64-mingw32-dlltool -d <the SDK's thirdparty/vulkan-loader/loader/vulkan-1.def> -D vulkan-1.dll -l libvulkan-1.a`)
  and `-DVulkan_GLSLC_EXECUTABLE=<glslc>`.

`bro.exe` needs `vulkan-1.dll` at run time, which the graphics driver installs (Wine has its own). The SDK's Windows
SDL3 has no Vulkan support, so on Windows `src/ngl/ngl_present.c` makes the window's Vulkan surface itself from its
handle; `../docs/NATIVE_RENDERER.md` explains.

## Controls

A gamepad (SDL3's, the first one plugged in) and the keyboard and mouse work at the same time. On the pad, A B X Y are
the Xbox's, the left shoulder is the Xbox's white button and the right shoulder its black one; triggers, sticks,
d-pad, start, back and the stick clicks are as on the Xbox. Every control can be changed in `bro.toml`
(`config/bro.toml` has every setting with its default).

**In play the keyboard and mouse work as in CoD3** (`[binds]`): each key does one of the game's own actions, with
CoD3's names and default keys, up to two keys an action and up to three actions a key. `input.binds_kbm` takes CoD3's
own line (`fire=MOUSE1;aim=MOUSE2;...`), so binds can be copied over from CoD3.

| Key | Action |
|---|---|
| left / right mouse button | fire / aim down the sights |
| F / R | use / reload |
| V, middle mouse button | melee |
| G | frag grenade |
| left Shift | sprint |
| Q / E | lean left / right |
| Space | jump (from crouch or prone: stand up) |
| C / Ctrl | crouch / prone (pressed again: stand up) |
| mouse wheel down / up | next / previous weapon |
| W A S D | move |
| the mouse, arrow keys | look |
| Enter / Esc | start / back |
| 1 2 3 4 | d-pad up, down, left, right |

The other actions (use or reload, use or melee, lean or switch weapon, raise stance, crouch or prone while held, lower
or raise stance, the weapon's other mode, objectives) have no key by default. The smoke grenade, binoculars and the
weapon slots have no command of their own in Big Red One, so they are reached through their controller buttons (in
play E and Q lean, so give the white and black buttons other keys in `[keys]` if you use them from the keyboard).

In the game's menus, on loading screens and in movies the keys are the controller's buttons instead (`[keys]`:
Space A, left Ctrl B, R X, F Y, E black, Q white, Z or the right mouse button the left trigger, X or the left mouse
button the right trigger, left Shift and V the stick clicks), so the menus work as on the Xbox. `input.binds = false`
keeps the keys the controller's everywhere.

**The mouse aims as on the PC** (`input.raw_mouse`, on by default): each count turns the view 0.022 degrees times
`input.mouse_sensitivity` (5 by default, as World at War and CoD3), whatever the frame rate; `input.mouse_scale_x` /
`_y` and `input.invert_y` change it by direction. While aiming down the sights it turns slower by the zoom, as in CoD3
(`input.zoom_sensitivity` on top). With `raw_mouse = false` the mouse moves the right stick instead
(`input.mouse_speed`). The mouse is held by the window while it has the focus (`BRO_MOUSE_LOOK=0`: never). What each
controller button does in the game is the game's own controller layout, chosen in its options.

## Settings

Environment variables, read when the game starts.

| Variable | Effect |
|---|---|
| `BRO_VK=1` | draw with Vulkan (the setup tool's `play.sh` sets it) |
| `BRO_DEVMAP=<map>` | start in a level: prologue, oran, Tunisia, kasserine_pass, kasserine, liberator, gela, piano_lupo, troina, omaha_beach, mons1, eilendorf, buchholz, dragons_teeth |
| `BRO_WINDOW=0` | no window, even with a display (frames are drawn offscreen) |
| `BRO_FULLSCREEN=1` | the window fills the screen |
| `BRO_VSYNC=0` | frames shown as they come instead of in step with the display |
| `BRO_VK_SIZE=<w>x<h>` | the size the game is drawn at (640 x 480 offscreen, 1280 x 960 in a window by default) |
| `BRO_MOUSE_LOOK=0` | the mouse isn't held for looking around |
| `BRO_MOUSE_SPEED=<n>` | the mouse's speed (1600 by default) |
| `BRO_PAD=0` | no gamepad |
| `BRO_SOUND_MUTE=1` | silence (the game still runs its sound) |
| `BRO_FRAME_PACING=0` | frames don't wait for the display's vblank (for test runs that should go as fast as they can) |

For development and test runs:

| Variable | Effect |
|---|---|
| `BRO_PAD_SCRIPT=<ms>:<button>[+<button>...][:<hold ms>],...` | press buttons at set times from the game's first pad read (e.g. `6000:start,9000:a`); buttons: up down left right start back lthumb rthumb a b x y black white lt rt |
| `BRO_VK_DUMP=<frame>[,<frame>...]`, `BRO_VK_DUMP_PATH=<file.ppm>` | save the drawn picture at those frames (`%lu` in the name becomes the frame number) |
| `BRO_VK_PLACEHOLDERS=1` | quads whose texture has no pixels yet drawn as faint boxes |
| `BRO_NGL_LOG=0/1/2` | log the game's NGL calls |
| `BRO_MESH_CENSUS=1` | report the first meshes, their sections and matrices, and every material type |
| `BRO_TEX_DUMP_DIR=<folder>` | save each decoded texture there as a .ppm and log its name and format |
| `BRO_DUMP_LOCK=<n>`, `BRO_DUMP_PATH=<file.ppm>` | save the lock buffer as the n-th texture lock finds it (what the lock before it wrote) |
| `BRO_SOUND_WAV=<file>` | also write the sound mix to a WAV file |
| `BRO_SOUND_TRACE=1`, `BRO_MOVIE_TRACE=1`, `BRO_SCRIPT_TRACE=1` | log sound buffer calls, the movie player, script threads |
| `BRO_SCRIPT_WATCH=<address>` | report when that guest word changes, and at which step of a script thread |
| `BRO_WHERE=<seconds>[,...]` | at those times, report where the game's thread is |
| `BRO_FAULT_REPORT=0` | no crash reports; `BRO_WRITE_GUARD=<address>[,...]` and `BRO_GUARD_MIRRORS=1` catch writes to guest memory (`src/debug/bro_debug.c`) |
