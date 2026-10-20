# Call of Duty 3 + World at War

Play **Call of Duty 3** (Xbox 360) natively on Linux and Windows

- **Call of Duty 3** is rebuilt from your disc as a native program (a static recompilation on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)), with a WaW-style console, an FOV slider, mouse aim, up to 4K and high frame rates.
- **Both games' files are ripped** into open formats (textures, models, materials, scripts) for the native engine that blends them. World at War itself is never run.

No game files are included. You need your own *Call of Duty 3* Xbox 360 disc image: *USA, Europe*, or the German or the French/Spanish/Italian disc (the same game program; multiplayer is built from the USA, Europe disc only). **World at War is optional:** Call of Duty 3 builds and plays without it. Set it up on the setup tool's World at War tab (or leave it out with `--no-waw`) only if you want its files ripped for the new engine.

## Quick start

1. **Download** the latest release from the [Releases](../../releases) page and unzip it. (Or use **Code → Download ZIP**.)
2. **Put your games in the `input` folder** (optional, but then they're found by themselves): your Call of Duty 3 `.iso`, and if you don't have World at War on Steam, its Xbox 360 `.iso` and any map pack downloads.
3. **Start the setup tool:**
   - **Windows:** double-click `setup.bat`. Put the folder at a short path outside OneDrive first (for example `C:\Games\COD3`); [docs/BUILDING_ON_WINDOWS.md](docs/BUILDING_ON_WINDOWS.md) has what gets installed where and the fixes for common errors.
   - **Linux:** right-click `setup.sh` → *Run as a Program*, or in a terminal: `sh setup.sh`.
   - `setup.sh` also works on Windows in Git Bash, MSYS2 or Cygwin. `setup.bat` is Windows only; it also runs under Wine, where the rips work but the game can't be compiled yet (on Linux, `sh setup.sh` builds the Windows version natively).
4. Check what it found, tick the **versions** you want (Linux and/or Windows; either system can build both), then press **Build**.

   Optional: **Upscale the extracted textures 2x with waifu2x** makes sharper copies of the ripped textures with your graphics card (any card with a Vulkan driver). Pick a style (Photo suits CoD3's textures; Best quality is several times slower) and how much compression noise to remove. Only the color pictures are upscaled (the textures the game's materials use as diffuse maps: walls, ground, uniforms, weapons); normal, specular and detail maps stay as the game has them, because upscaling and denoising them changes the lighting, and so do the HUD, fonts, effects and menu pictures, which no material describes. The copies are then packed for the playable game (`data/cod3/hd_textures/`): twice the size, in the same compressed formats as the game's own textures, so they take about 4 times the texture memory. The game uses them by default; turn them off in **F2 > Video > HD textures (waifu2x)**. Up to about 4 GB more disk space.

Anything missing is installed for you: Python, Rust, compilers and libraries. You may be asked for your password, or see a Windows permission prompt. The first build of the playable Call of Duty 3 takes 30–120 minutes; you can cancel at any time and press **Build** again to continue where it stopped.

When it's done, start the game from the **Play** page, or open the `output` folder and run `CoD3-x86_64.AppImage` (Linux) or `bin/cod3.exe` (Windows; `bin-windows/cod3.exe` when it was built on Linux).

**One tab per game.** The setup tool has a tab for each game across the top, each with its own pages and main button. **World at War:** your Steam folder or disc and the map packs, then **Rip content**. **Call of Duty 2** and **United Offensive:** **Make a new copy** copies only the game's data archives from your Steam or CB install for the Rust engine ([docs/NEW_COPY.md](docs/NEW_COPY.md)), and **Start LAN server** runs the Rust LAN server your PC game can join ([docs/PC_SERVER.md](docs/PC_SERVER.md)); both programs are built from `engine/` with cargo the first time. No game's program is ever run, copied or changed. Each tab has a **Port status** page.

**Both versions from one system.** On Linux, the Windows version is cross-compiled with llvm-mingw (downloaded for you). On Windows, the Linux version is built inside WSL 2 in a private Ubuntu that the tool sets up and removes again; WSL has to be turned on once (in an administrator PowerShell: `wsl --install --no-distribution`, then restart). macOS isn't supported.

**Wine and Proton** aren't a goal (on Linux, play the native version), but the Windows folder is complete: `cod3.exe` comes with every DLL that it and its GPU plugin (`librexgpu-xenos.dll`, or `rexgpu-xenos.dll` from Visual Studio) need, except Windows' own and the Vulkan loader that comes with the graphics driver (Wine and Proton have their own). Copy the whole output folder, not just the `.exe`. If the renderer can't start, `userdata/logs/cod3.log` names the files it tried and the Windows error.

**Linux distros.** Debian, Ubuntu, Mint and Pop!_OS (apt), Fedora (dnf), Arch, Manjaro and EndeavourOS (pacman), openSUSE (zypper) and Void (xbps) get their packages installed automatically. On image-based systems (SteamOS, Bazzite and other Fedora Atomic, openSUSE Aeon, NixOS), run the tool inside a regular distro container: `distrobox create --name cod3-build --image ubuntu:24.04`, `distrobox enter cod3-build`, then `sh setup.sh`.

### The project folder

Everything that can stays in the folder you unzipped, unless you choose other places. **Nothing is downloaded or installed until you approve it:** before a build (or "Install missing") the tool lists each download and install with its size, where it comes from and the folder it goes into, and waits for **Download and install** (on the command line: a `y`, or `--yes`). **Where things go** in the setup tool lists every folder below with an Open button.

| Folder | What's in it |
|---|---|
| `setup.sh` / `setup.bat` | Start here. |
| `input/` | Your discs and downloads. Looked at first; only ever read. |
| `output/` | The built game, the ripped files, saves, the game's settings and logs (see below). |
| `work/` | Temporary build files, deleted at the end. On a path with spaces the build reaches it through a short link (in your cache folder), so the build tools never see the spaces; the files stay here. |
| `work/tools/` | Downloaded tools (CMake, Ninja, 7-Zip, LLVM, llvm-mingw, appimagetool, waifu2x, as needed), Rust in `work/tools/rust` when your PC has none, and on Windows Python from `setup.bat`. Kept between builds; delete it to free the space (they are downloaded again, with your approval). |
| `config/` | The setup tool's settings, and in `fonts/` the game's own F2 font (Barlow, unpacked from `recomp/` for the tool's titles; only the tool uses it). |
| `COD_3_WAW_Setup-tool/`, `engine/`, `docs/`, `tools/` | The source code and its notes. |

Only two things can't live in this folder, because their installers decide where they go: **Linux build packages** (compilers and libraries from your distribution, installed into the system by your package manager, which asks for your password) and on Windows **Visual Studio's Build Tools and the Windows SDK** (Microsoft's installer). A Rust or Python you already had is used where it is.

### World at War sources

| Source | When it's used |
|---|---|
| **Steam folder** | Preferred. When a Steam folder is set, it is the **only** World at War source used. |
| **Xbox 360 disc image** (`.iso`) | When there's no Steam folder, and you pick it. |
| **PC disc**: a disc image, the disc drive, or a folder installed from the disc | When there's no Steam folder, and you pick it. |

Your discs and game folders are only read: never changed, never deleted. The `mods/` folder in your World at War install is ignored.

**Map packs (DLC).** The Xbox 360 and PC discs don't have the map packs; the Steam version has all of them. With a disc, add the official PC patches from ModDB:

| Download | Adds |
|---|---|
| [Patch 1.4 + Map Pack 1](https://www.moddb.com/games/call-of-duty-world-at-war/downloads/cod-waw-12-to-14-patch-map-pack-1) (`codwaw-v1.2_1.4.zip`, 580 MB) | Knee Deep, Nightfire, Station, Verrückt |
| [Patch 1.5 + Map Pack 2](https://www.moddb.com/games/call-of-duty-world-at-war/downloads/cod-waw-14-to-15-patch-map-pack-2) (`codwaw-v1.4_1.5.zip`, 972 MB) | Banzai, Corrosion, Sub Pens, Shi No Numa |
| [Patch 1.6 + Map Pack 3](https://www.moddb.com/games/call-of-duty-world-at-war/downloads/cod-waw-15-to-16-patch-map-pack-3) (`codwaw-v1.5_1.6.zip`, 1.2 GB) | Battery, Breach, Revolution, Der Riese |

Put them in `input/` (or add them in the **Map packs** row). Their installers are unpacked, never run, and only the maps your disc lacks are added.

**Mods and custom maps** (UGX zombies maps, total conversions) are switched off for now. They will come back as a separate project.

### Without a window

```
python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py doctor --fix      # install what's missing
python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py build             # everything from input/ (and WaW on Steam)
python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py build --iso "<CoD3 .iso>" --waw-xbox360 "<WaW .iso>" --no-recomp
python3 COD_3_WAW_Setup-tool/cod3_waw_setup.py map-pack --list   # where to get the map packs
```

`build` takes what it needs from `input/` and finds World at War on Steam by itself; `--iso`, `--waw-steam`, `--waw-xbox360`, `--waw-pc-disc` and `--waw-map-pack` choose by hand. `--no-recomp` only rips. On Linux, `--windows-exe` also makes the Windows ripper (`tools/rip.exe`).

## What you need

| | |
|---|---|
| **Games** | *Call of Duty 3* for Xbox 360 (USA, Europe; Germany; or France, Spain, Italy), as a disc image. Optionally *World at War*: Steam, Xbox 360 disc image, or PC disc. |
| **System** | 64-bit Windows 10/11, or 64-bit Linux (Ubuntu/Mint 22+, Debian 13+, Fedora 40+, Arch, openSUSE Tumbleweed). |
| **Graphics** | A Vulkan GPU driver (on Windows, Direct3D 12 also works). NVIDIA's proprietary driver, AMD and Intel (Mesa) all include Vulkan. |
| **Disk** | About 7 GB temporary, plus about 7 GB for the game files and 4 GB for the ripped files. |
| **Memory** | 8 GB works; about 2 GB per build thread is used. |
| **Internet** | For the first build (tools and the ReXGlue SDK are downloaded). |

## What ends up in the output folder

| | |
|---|---|
| `CoD3-x86_64.AppImage` (Linux) and/or `bin/cod3.exe` + DLLs (Windows; `bin-windows/` when made on Linux) | The playable recompiled Call of Duty 3. The Linux version is `bin/cod3` instead if you tick "Linux version as a plain bin/ folder" under More options (the AppImage is the default); either way `Call of Duty 3.sh` starts it, and a build removes what an earlier build made the other way. Both versions use the same `assets/` and `userdata/`. |
| `data/cod3/` | Ripped Call of Duty 3: `textures/` (DDS + PNG, and `hd_keys.tsv`, which game texture each one is), `meshes/` (glTF with materials), `materials/` (JSON). With the waifu2x option, `textures_upscaled/` (the PNGs at the chosen scale, 1x, 2x or 4x, same names; for the Rust remake and for exports; a `textures_2x/` folder from an earlier build is renamed to it) and `hd_textures/` (the same textures for the playable game: DDS in the game's own compressed formats with all mip levels, and `index.tsv`). The game replaces its textures with these while F2 > Video > HD textures is on (the default); everything else it reads from `assets/`. Your own pictures (for the swastikas and other insignia, or any texture) go in `user_textures_png/` and are packed into `user_textures/`: see [docs/TEXTURE_REPLACEMENT.md](docs/TEXTURE_REPLACEMENT.md). |
| `data/waw/` | Ripped World at War, one folder per fastfile; `map_packs/` holds the files taken out of the patches. |
| `assets/` | The game files from your Call of Duty 3 disc, which the recompiled game reads. |
| `userdata/` | Saves, console config (`cod3_config.cfg`), shader cache, and the game's log (`logs/cod3.log`). |
| `logs/` | The setup tool's build logs (the last 10). |
| `tools/rip` | The ripper, for adding maps later (and `rip.exe` if you made the Windows one); `newcopy` and `iw-server` once the Call of Duty 2 / United Offensive tabs used them. |
| `copies/` | New copies of Call of Duty 2 (`cod2/`) and United Offensive (`coduo/`): their data archives only, for the Rust engine (the default place; the tab can pick another). |
| `Launcher.sh` / `Launcher.bat` + `launcher/` | The Play page, usable without the setup folder. |
| `cod3_waw.json` | What was built, and from which sources. |

## In game (Call of Duty 3)

| Key | |
|---|---|
| `~` | World at War-style console: cvars/commands with completion, history, `condump`, `find`, `cvarlist`, `fov`, `cheats`, `quit`. `help cg_fov` says what a dvar does and `help fog` searches them; every dvar is listed in [docs/DVARS.md](docs/DVARS.md). `com_maxfps 144` caps the frame rate (0 = no limit; there is no other cap, not even the 60 Hz vblank) and `cg_drawFPS 1` shows it, both applied at once and saved. World at War's dvar names work too: CoD3's own under another name (`cg_gun_x`, `bg_viewKickScale`, `compassSize`, `hud_fade_compass` ...) and the port's settings (`sensitivity`, `m_yaw`, `r_mode 1920x1080`, `r_fullscreen`, `r_vsync`, `snd_volume` ...); `dvarlist` marks them W. |
| `Shift` + `~` | Full-height console. |
| `F2` | Settings, each with a short description. **Video:** graphics API (Windows: Vulkan by default, as on Linux, or Direct3D 12), display mode, start behind other windows (the window opens behind the one you're using, without the keyboard, until you click it; `--start_in_background=true` on the command line does it for one start), fullscreen refresh rate, render resolution (matches your screen by default; the game draws it divided by a whole number, so the picture lands pixel for pixel, never scaled up and back down; a change applies at once, without restarting), frame rate limit (`com_maxfps`, up to 1000 or none), FPS counter, V-Sync (no tearing), FXAA, native 2x MSAA, FSR/CAS, anisotropic filtering, HD textures (the setup tool's waifu2x pack; on by default when it is there), shadows (Original, High, Off), lens-flare fix. **Game:** FOV, FOV scale, aim assist, master volume, skip cutscenes automatically, skip prompt. **Crosshair:** editor; it takes the game's own colors (red over an enemy) unless you tick "Constant color". **Controls:** mouse and keyboard, button prompts (keys in brackets while you use the keyboard and mouse; Xbox, PlayStation or GameCube style for pads). **Interface:** colors (Call of Duty 3, Field olive, Briefing paper, High contrast; all taken from the game's own screens), text size, window opacity. **Performance:** log detail and "Write a report", what each frame costs (under the FPS counter), render target emulation, shader threads. **Console:** what the console has saved. Every group has its own "Reset ... to defaults" button (video, game, crosshair, mouse, key bindings, performance, console settings), and each one only resets its own group. |
| `F10` | Log viewer. |
| `Space` / `Enter` / `Esc` / click | Skips a cutscene while "SPACE Skip" shows: the logos, a level's briefing once the level has loaded, the level's opening intro. |
| Mouse / keyboard | Raw mouse aim with World at War sensitivity (default 5, m_yaw 0.022). WaW-style binds: LMB fire, RMB aim, Space jump, C crouch, R/F reload/use, 1/2 switch, G frag, 4 smoke, V melee, Shift sprint / hold breath, Tab objectives, Esc pause. Change them in F2 > Controls; the mouse's side buttons are `MB4` (back) and `MB5` (forward). Controllers work at the same time. |
| Cheats | `cheats 1` (the same as `sv_cheats 1`) in the console allows CoD3's cheats (`god`, `notarget`, `noclip`, `ufo`, `give`, `setviewpos`, `jumptonode`, and cheat-protected dvars such as `timescale`). Once one is used, a **CHEATS USED** mark stays in the top-left corner until `sv_cheats` is back to 0 (`cheats 0`), those dvars are back to their defaults, god/notarget/noclip/ufo are off, and after `give` or a teleport the level has been restarted. |
| Cheat guard | While cheats are off, cheat commands and cheat-protected dvars are refused with a plain message wherever they come from (console, autoexec, config file, F2). In a multiplayer match (when it exists) only the host can turn cheats on, and everyone sees it in a separate column next to the kill feed. |
| Mouse and arrow keys in the menus | The game's own menus work like a PC menu: hovering selects, click accepts, right click or Esc goes back, the wheel scrolls, clicking the left or right half of a value steps it; Up/Down/Left/Right, Enter and Backspace work everywhere. The "select storage device" item is gone. `cod3_pc_menus 0` turns this off; F2 stays as the fallback. |
| Options > Gameplay > **Field of View** / **FOV scale** | Native menu rows (`cg_fov` 65 to 120 in steps of 5, `cg_fovScale`), in the main menu's Options and the pause menu's. |
| Options > **Video** | Resolution (applies at once), frame rate limit, V-Sync, anti-aliasing, shadows and HD textures as native menu rows; the pause menu's Options have them under VIDEO OPTIONS. Changes show in F2 and the console, and the other way round. |

`seta cg_fov 95` in the console sets and saves the FOV (to `userdata/cod3_config.cfg`).

Quit in the pause menu (and the end of a mission) brings you back to the main menu: the game restarts itself there, as the Xbox 360 version does.

**Testing and bug reports:** `cod3_log_detail verbose` (or F2 > Performance) logs slow frames with what the emulated GPU did in them, level load times and the frame cost every 10 seconds; `cod3_report` in the console writes `userdata/logs/report-<date>-<time>.txt` to attach next to `userdata/logs/cod3.log`. `screenshot` in the console (or F2 > Performance > Logging > Take a screenshot) saves the game's own picture, never the desktop or another window, to `userdata/logs/screenshots/`; with `cod3_log_detail debug` the game also takes one every `cod3_debug_screenshots` seconds (60, 0 = none) while the picture changes, when it gets stuck and with each report. `cod3_hd_compare` (or F2 > Performance > Logging > Compare HD textures) saves the same view with the HD textures and, a moment later, without them.

## Current status

- **Works and compiles:**
  - the recompiled `default.xex` and all 15 level DLLs;
  - console, FOV, crosshair, mouse aim, settings window.
- **Ripper:** the whole Call of Duty 3 disc (370 archives) rips in well under a minute on a desktop CPU, and every format rule is checked on each archive. World at War fastfiles, PC and Xbox 360, give their scripts; the full asset loader comes next.
- **AMD CAS and FSR 1** (F2 > Output scaling) are in every build. FSR 1 upscales only when the game draws fewer pixels than the window shows (a lower render resolution or resolution scale); at the window's size it sharpens like CAS. AMD's FSR 2 / 3 aren't offered: the game gives them no depth or motion information, so they can't work as meant.
- **Multiplayer** isn't playable yet: the main menu's Multiplayer item says so. The disc's multiplayer program translates completely with the recompiler, and the plan is splitscreen and system link with Windows and Linux players together, first on a LAN or by IP address. Xbox Live modes can't come back: their servers are gone. Details in [docs/MULTIPLAYER.md](docs/MULTIPLAYER.md).

The full requirements list, with the status of every item, is in [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md). A map of the game's own code (subsystems, classes, named functions) is in [docs/ENGINE.md](docs/ENGINE.md). Where each piece of knowledge comes from is in [docs/SOURCES.md](docs/SOURCES.md). What the port protects against (and how to report a problem) is in [docs/SECURITY.md](docs/SECURITY.md). The file formats are in [docs/formats/cod3.md](docs/formats/cod3.md) and [docs/formats/waw.md](docs/formats/waw.md).

## Safety

- **Everything runs natively.** Nothing goes through Wine, Proton or an emulator. No game, patch, mod or map installer is ever run: they are only unpacked. (AMD's FidelityFX SDK build runs a Windows shader compiler, so it is left out of Linux builds; CAS and FSR 1 don't need it.)
- **Your files are only read.** Discs, game folders and downloads are opened read-only and are on a protected list. Folders are only ever deleted through one guarded function, which refuses anything outside the tool's own work and output folders and anything that overlaps your inputs. Single files are only deleted inside those two folders.
- **Nothing is downloaded without your approval.** Each download and install is listed first, with where it goes.
- **Downloads are pinned and checked.** Each tool the setup downloads itself (CMake, Ninja, 7-Zip, innoextract, LLVM, appimagetool, waifu2x) is a fixed release, checked against its SHA-256 before use, also when it was downloaded before. The Rust installer is checked against the checksum the Rust project publishes next to it. The ReXGlue SDK and its submodules are fetched at pinned commits. The Ubuntu image for the Linux build on Windows is checked against the checksum list Canonical publishes next to it. Everything else comes from your system's package manager (apt, dnf, pacman, zypper, xbps or winget), which checks its own signatures.
- **Root rights only for packages.** On Linux, missing packages are installed with your package manager through `pkexec` or `sudo`, from a script that only lists package names, written to a new folder only you can open. Nothing else asks for root.
- **Archives can't write outside their folder.** Zip and tar entries (and tar links) that would land outside the unpack folder are refused; 7-Zip, innoextract and unar strip such paths themselves.
- **No personal paths in the repository.** Every path is relative to the project folder or comes from your system's settings; `tools/check_repo.py` fails CI if a personal path or any game data gets in.

## For developers

`python3 tools/make_release.py` makes `RELEASE/`: the files a new user needs (everything git publishes, plus START-HERE.txt), checked to hold no game data and no personal paths.

| Folder | Contents |
|---|---|
| `COD_3_WAW_Setup-tool/` | The setup tool (Python 3.9+ with Tkinter): `c3w/` modules, `recomp/` (the ReXGlue project for CoD3), `patches/` (SDK patches), `tests/`. |
| `engine/` | The Rust workspace: the archive and format readers and the `rip` tool. See [engine/README.md](engine/README.md), which also has the code style rules. |
| `docs/` | Requirements, sources, file formats, the multiplayer plan. |
| `tools/` | Research scripts (XEX, PowerPC cross-references, switch tables) and `check_repo.py`. |

**Checks**, the same ones CI runs on every push:

```
python3 tools/check_repo.py                                   # no game data in the repository
cd engine && cargo fmt --check && cargo clippy --all-targets -- -D warnings && cargo test
python3 -m unittest discover -s COD_3_WAW_Setup-tool/tests    # the setup tool
```

**Running the heavy work on your own computer.** `tools/run_local.sh` (Linux) and `tools/run_local.bat` / `tools/run_local.ps1` (Windows) use all your CPU cores and your GPU. They write everything into `dev/`: logs into `dev/logs/`, rips into `dev/ripped/`:

```
sh tools/run_local.sh                    # build + test the Rust tools, check and rip output/assets
sh tools/run_local.sh rip --waw "<WaW Steam folder>"
sh tools/run_local.sh recomp-test        # build the recomp, play Saint-Lo for 10 minutes, take screenshots
sh tools/run_local.sh waw360 --waw-xbox360 "<WaW Xbox 360 .iso>"   # copy out and try the 360 fastfiles
sh tools/run_local.sh map-pack           # unpack and try every map pack patch in input/
```

Steps can be combined in one command. **Want to help?** Running these on your own copies and sharing the `dev/logs/` folder is the most useful contribution (it holds no game data, but it does show your folder paths): it tells us how the formats and the build behave on real hardware.

**Building the recomp by hand:**

```
# 1. SDK at the pinned commit, with our patches
git clone https://github.com/rexglue/rexglue-sdk && cd rexglue-sdk
git checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518 && git submodule update --init --recursive
git apply ../COD_3_WAW_Setup-tool/patches/*.patch
cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20   # win-amd64 on Windows
cmake --build out/build/linux-amd64 --config Release --target install

# 2. The game (copy default.xex + sp/<level>/<level>.dll into recomp/assets first)
cmake -S recomp -B recomp/out/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_PREFIX_PATH=<sdk>/out/install/linux-amd64
cmake --build recomp/out/build/release --target cod3_codegen   # PowerPC -> C++
cmake -S recomp -B recomp/out/build/release                   # picks up the generated sources
cmake --build recomp/out/build/release
```

Clang 19 or newer is needed. Clang 18 can't build the SDK against GCC's C++ library (no `std::expected`). Leave `-DREXGLUE_ENABLE_FIDELITYFX` off on Linux: its build runs AMD's Windows shader compiler through Wine.

**Publishing a release:** push a tag such as `v0.2.0`. The release workflow builds the ripper for Linux and Windows, and attaches a ready-to-run zip to the GitHub release.

## Credits and licences

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and the [Xenia](https://github.com/xenia-project/xenia) project it derives from.
- [CoD3Recomp](https://github.com/FrankHUN88453/CoD3Recomp), for cross-checks and the script-thread approach.
- [OpenAssetTools](https://github.com/Laupetin/OpenAssetTools), for World at War formats.
- [Barlow](https://github.com/jpt/barlow) (SIL Open Font License), the settings window's font.
- [waifu2x](https://github.com/nagadomi/waifu2x) and [waifu2x-ncnn-vulkan](https://github.com/nihui/waifu2x-ncnn-vulkan) (MIT), the optional texture upscaling.
- Everything else used is listed in [docs/SOURCES.md](docs/SOURCES.md).

This project's code is under the MIT license ([LICENSE](LICENSE)). The SDK patches in `COD_3_WAW_Setup-tool/patches/` are under the SDK's BSD 3-Clause license.

Call of Duty is a trademark of Activision. This project contains no Activision code or assets: everything game-specific is made on your machine from your own discs.
