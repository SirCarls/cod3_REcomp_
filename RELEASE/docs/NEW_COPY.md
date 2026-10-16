# Making a new copy of Call of Duty, United Offensive or Call of Duty 2

The Rust engine plays Call of Duty (2003), Call of Duty: United Offensive and Call of Duty 2 from your own copy of each game. `newcopy` makes that copy: it finds your install (Steam, or any folder you point at) and gathers the game's data into a folder of its own that the engine reads.

## What goes in, and what never does

The new copy holds **only the game's data archives** and two small text files:

| Game | Short name | Archives copied | Text files copied |
|---|---|---|---|
| Call of Duty | `cod` | `main/pak*.pk3`, `main/localized_<language>_pak*.pk3` | `version.inf`, `localization.txt` |
| United Offensive | `coduo` | Call of Duty's archives above (United Offensive needs them), plus `uo/pakuo*.pk3`, `uo/localized_<language>_pakuo*.pk3`, `uo/uomappack*.pk3` | the same, plus `uo/version.inf` |
| Call of Duty 2 | `cod2` | `main/iw_*.iwd`, `main/localized_<language>_iw*.iwd` | `version.inf`, `localization.txt` |

Only names on this list are copied. Everything else is left alone, including:

- **every program file**: `.exe`, `.dll`, PunkBuster (`pb/`), Miles sound (`mss32.dll`, `Miles/`), mods such as CoD2x. They are never opened, read, hashed, copied or run. `newcopy` only notices their *names* in the folder listing, to recognize which game a folder holds;
- your profiles, configs, saved games, mods and custom maps (`zzz_mod.pk3`, `mp_mymap.pk3`, ...);
- CD keys: no registry key holding a key is ever read, and no key file is touched.

**Why Steam's DRM doesn't matter.** Steam's copy protection lives only in the game's executables. The Rust engine replaces the executable, so the new copy has none, and the protection never comes into it. Nothing is cracked, patched or bypassed: the data archives are plain ZIP files that were never protected.

## Where your games are looked for

`newcopy find` looks in Steam, plus any folder you name with `--from`.

**Any folder** (`--from <dir>`): a CB copy, a disc install, a copy on another drive. Folder and file names are matched without regard to case (Call of Duty spells its folder `Main`, Call of Duty 2 `main`), so a copy made on Windows works on Linux. A folder holding both Call of Duty and United Offensive (the usual layout) is reported as both.

**Steam** is found here:

| System | Places checked |
|---|---|
| Windows | the registry values Steam writes for its own folder (`HKCU\Software\Valve\Steam` `SteamPath`, `HKLM\SOFTWARE\WOW6432Node\Valve\Steam` `InstallPath`, and `HKLM\SOFTWARE\Valve\Steam` on 32-bit Windows), read with Windows' own `reg.exe`; then `%ProgramFiles(x86)%\Steam` and `%ProgramFiles%\Steam` |
| Linux | `$HOME/.steam/steam`, `$HOME/.steam/root`, `$HOME/.steam/debian-installation`, `$XDG_DATA_HOME/Steam` (normally `$HOME/.local/share/Steam`), Flatpak (`$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam` and `.../data/Steam`), Snap (`$HOME/snap/steam/common/.local/share/Steam`) |
| macOS | `$HOME/Library/Application Support/Steam` |

`--steam <dir>` (or the environment variable `GAME_SOURCES_STEAM`, several folders separated like `PATH`) replaces this search. It may name the Steam folder or any Steam library folder.

In each Steam folder, `newcopy` reads Steam's list of library folders (`steamapps/libraryfolders.vdf`, old and new formats) and, in each library, the app manifest of each game (`steamapps/appmanifest_<app id>.acf`), which names its folder under `steamapps/common/`. If a manifest is missing, the usual folder name is tried.

| Game | Steam app | Folder in `steamapps/common/` |
|---|---|---|
| Call of Duty | 2620 | `Call of Duty` |
| United Offensive | 2640 | `Call of Duty` (the same folder: United Offensive installs on top of Call of Duty) |
| Call of Duty 2 | 2630 | `Call of Duty 2` |

**Proton (Linux).** These are Windows games; on Linux, Steam installs them through Proton into the same `steamapps/common/<folder>`, so they are found the same way. `newcopy find` also shows the game's Proton prefix (`steamapps/compatdata/<app id>/pfx`) for information. It never looks inside it, and nothing here needs Wine or Proton.

## Steam emulators, Proton-GE and Proton-CachyOS

**No Steam emulator is used or needed.** Tools such as SmartSteamEmu replace Steam's own library
(`steam_api.dll`) so that a game's original executable starts without Steam; that is a way around
the executable's protection. The Rust engine never starts the original executable, so there is
nothing to emulate: `newcopy` doesn't use, install or recommend one, and the copy it makes has no
executable in it at all.

**Proton runs the original games, not the Rust engine.** The Rust engine is a native Windows and
Linux program. Proton (Valve's, [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom)
or [Proton-CachyOS](https://github.com/CachyOS/proton-cachyos)) matters only for running the
*original* Windows games on Linux, for example your CB copy, to test cross-play against a Rust
server on the same machine or network ([PC_MULTIPLAYER.md](PC_MULTIPLAYER.md)).

- `newcopy find` lists the Proton builds it sees, for information: custom builds in Steam's
  `compatibilitytools.d/` (GE-Proton's place: `~/.steam/steam/compatibilitytools.d/`; Flatpak
  Steam: `~/.var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d/`), system packages
  in `/usr/share/steam/compatibilitytools.d/`, and Valve's in `steamapps/common/Proton*`. It never
  starts one.
- To run a CB copy with one: add its `CoD2MP_s.exe` or `CoDUOMP.exe` to Steam as a non-Steam game
  and pick the build under Properties > Compatibility ("Force the use of a specific Steam Play
  compatibility tool"), or, outside Steam, use [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher)
  (`umu-run`), which both projects support for that.
- Inside Proton the game uses the network like any Windows program: a Rust server on UDP port
  28960 (the games' default) is reached by address (`connect 127.0.0.1:28960` from the game's
  console on the same machine) or through the game's LAN list.

## Commands

```
newcopy find  [--from <dir>]... [--steam <dir>]
newcopy make  <cod|coduo|cod2> --out <dir> [--from <dir>] [--steam <dir>]
              [--copy] [--language <lang>] [--allow-missing]
newcopy check <dir>
newcopy --help
```

- **`find`** lists every install found: the game, its version (from `version.inf`), its language(s), where it is, whether Steam installed it, and which archives are missing.
- **`make`** makes the copy in `<out>/<game>/`. Without `--from` it uses the Steam install (if Steam has the game in more than one place, it asks you to pick with `--from`).
  - `--language english` picks the language when the install has several.
  - `--copy` copies every file. Without it, archives are **hard-linked** when the copy is on the same drive as the game: instant, and no extra disk space. (A hard link is the same file under a second name. Deleting the game keeps the copy's files; but if Steam later updates an archive in place, the copy changes too, and `newcopy check` will tell you. Use `--copy` for a fully independent copy.)
  - `--allow-missing` makes the copy even when required archives are missing (normally it stops and lists them).
- **`check`** reads every file of a copy (`<out>` or `<out>/<game>`) and compares it with the manifest. Run `make` again to repair whatever it reports.

Exit codes: 0 when everything worked, 1 when the command failed (game not found, missing or damaged archives, a copy that doesn't match), 2 when the command line is wrong.

Example:

```
newcopy find
newcopy make cod2 --out "<your folder>/cod-copies"
newcopy make coduo --from "<your CB copy>" --out "<your folder>/cod-copies"
newcopy check "<your folder>/cod-copies"
```

## What the copy looks like

```
<out>/
  cod2/
    newcopy.toml           the manifest
    version.inf
    localization.txt
    main/iw_00.iwd ... iw_15.iwd
    main/localized_english_iw00.iwd ... iw11.iwd
  coduo/
    newcopy.toml
    version.inf  localization.txt  uo/version.inf
    main/pak0.pk3 ... pakb.pk3, main/localized_english_pak0.pk3 ... pak5.pk3
    uo/pakuo00.pk3 ... pakuo07.pk3, uo/localized_english_pakuo00.pk3 ... 02, uo/uomappack00.pk3
```

Folder and file names in the copy are always lowercase, so the engine finds them the same way on every system.

The archives expected are those of each game's last official patch, which the engine targets: Call of Duty 1.5, United Offensive 1.51 and Call of Duty 2 1.3. An older install shows the patch's archives as missing. For languages other than English, the localized archives are checked for gaps (the full set for each language hasn't been listed yet).

### The manifest, `newcopy.toml`

A plain TOML file: where the copy came from (`source_kind` = `steam` or `folder`, `source_path`, `steam_app_id`), the game, its version and language, the tool's version, the date (UTC), and one `[[file]]` entry per file with its `path`, `size`, `sha256` and `how` (`hard link` or `copy`). It holds your own folder's path, so it belongs on your machine; don't publish it.

## Safety rules

- **Nothing is ever written into the game's folder.** `make` refuses an output folder inside the game's folder (or the game's folder inside the output), also when a symlink leads there.
- **Every archive is checked before anything is written**: it must be a whole ZIP file (its end record found, every directory entry inside the file). A file cut short by a failed download stops the run with the file's name; repair the install (in Steam: Properties > Installed Files > Verify integrity of game files) and run again.
- **Safe to interrupt.** Each file is written under a temporary name (`*.newcopy-partial`) and renamed into place when complete; the next run clears leftovers.
- **Incremental.** Running `make` again only redoes files whose size or SHA-256 changed. Switching language removes the old language's archives, and only files the old manifest listed are ever removed.
- **Untrusted input is handled as such.** Archive names with folder separators or `..` are refused, sizes and entry counts are bounded before anything is allocated, and a damaged Steam file is reported, never a crash.
- The only program `newcopy` ever starts is Windows' own `reg.exe`, to read Steam's folder.

## For developers

The work is done by the crate `engine/crates/game_sources` (std only, no `unsafe`); `engine/tools/newcopy` is its command line.

| Module | What it does |
|---|---|
| `games` | The game table: Steam app ids and folders, data folders, archive name rules and the expected sets |
| `steam` | Steam folders per system, `libraryfolders.vdf`, app manifests, Proton prefixes, `reg query` output |
| `vdf` | Valve's KeyValues text format (quoted strings with escapes, blocks, `//` comments) |
| `install` | Recognizing the games in a folder: archives, missing archives, version, languages |
| `zip` | Checking an archive is a whole ZIP file without unpacking it |
| `sha256` | SHA-256 (FIPS 180-4), streamed in 1 MiB blocks |
| `copy`, `manifest` | Making and checking the copy; `newcopy.toml` |
| `testkit` | Temporary folders, tiny ZIP files and fake installs for tests |

`cargo test -p game_sources -p newcopy` builds fake Steam folders and installs under the system temp folder: old and new `libraryfolders.vdf`, Windows paths with escaped backslashes, mixed-case folders, Call of Duty and United Offensive in one folder, a cut-short archive, an archive with a comment, a stand-in executable next to the archives (never copied), an output inside the game folder (refused), and incremental runs.
