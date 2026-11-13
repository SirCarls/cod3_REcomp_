# Mod tools

What you can change in the recompiled Call of Duty 3 (and in Big Red One), and the tools that do it. Every tool here
works on files made from **your own copy** of the game; the project ships no game data, and nothing you make from the
game's files should be shared.

| You want to | Use | Where it's explained |
|---|---|---|
| Replace a texture with your own picture (any texture: insignia, flags, signs, weapons) | the **user texture pack** (setup tool, Textures page) | [TEXTURE_REPLACEMENT.md](TEXTURE_REPLACEMENT.md) |
| Sharper textures for the whole game | the **HD texture pack** (waifu2x upscale, setup tool) | below; `COD_3_WAW_Setup-tool/c3w/upscale.py` |
| Look at the game's textures, models and materials in other programs | the ripper: `rip cod3-all` (DDS + PNG, glTF, JSON) | below; [`engine/README.md`](../engine/README.md) |
| Change a texture inside a `.cod` archive itself | `tools/cod_convert/xtex.py import` | below |
| Read or rebuild `.cod` archives, change or add items | `tools/cod_convert/` (`kapf.py`, `apkf.py`) | below; [formats/cod3.md](formats/cod3.md) |
| Add the Xbox 360 map packs | the setup tool's add-ons (Set up, "Your disc") | `COD_3_WAW_Setup-tool/c3w/addons.py` |
| Switch the German release's censored textures on or off | the setup tool (Set up) | `COD_3_WAW_Setup-tool/c3w/censored.py` |
| Change settings, cheats and hidden switches | dvars | [DVARS.md](DVARS.md) |
| Change keys and controller buttons | the keybinds | [KEYBINDS.md](KEYBINDS.md) |
| Change how the native renderer draws | `ngl_vk` and each game's NGL code | [NATIVE_RENDERER.md](NATIVE_RENDERER.md) |

Game logic (missions, weapons' behavior, AI) can't be modded yet: it is the game's own recompiled code. Changing it
means writing C++ hooks in `COD_3_WAW_Setup-tool/recomp/src/game/` ([ENGINE.md](ENGINE.md) maps the game's code), which
is development, not modding. Mod support in the add-ons list is planned (`addons.json` already has a `"mod"` kind for it).

## Textures: the easy way (no archive editing)

The game looks for replacement textures in two packs before it uses its own, so you never touch the game's archives:

1. **HD texture pack** (`<game folder>/data/cod3/hd_textures/`): the setup tool rips every texture, upscales the color
   pictures with waifu2x at the scale you choose (1x, 2x, 4x) and packs them back in the game's own formats with all
   their mip levels. Only color maps are upscaled: normal, specular and detail maps, lightmaps and lookup tables stay
   as they are, since enlarging them changes the lighting.
2. **User texture pack** (`<game folder>/data/cod3/user_textures/`): your own PNGs, each saved under the ripped
   picture's name in `data/cod3/user_textures_png/`. It is read after the HD pack, so your picture wins.
   [TEXTURE_REPLACEMENT.md](TEXTURE_REPLACEMENT.md) has the rules (sizes, alpha, names), how to check it, and what
   happens in multiplayer (only the world insignia on the game's own list are replaced there, so everyone sees the
   same map).

Both packs are DDS files plus an `index.tsv`, found by each texture's key (an XXH64 of the game's texture, as the
ripper's `hd_keys.tsv` lists it); the game reads them in `recomp/src/game/hd_textures.cpp`. The game settings
`cod3_hd_textures` and `cod3_user_textures` (F2 > Video) turn each pack on or off.

## The ripper: `rip`

`engine/tools/rip`, written in Rust (`cargo build --release` in `engine/`; the setup tool builds it for you, or uses
a prebuilt one from a release download). It reads only; it never changes the game's files.

```
rip cod3-all <assets folder> <out>        textures (DDS + PNG), meshes (glTF), materials (JSON)
rip cod3-tex <file.cod> <out>             one archive's textures
rip cod3-mesh <file.cod> <out>            one archive's meshes and materials
rip cod3-split <file.cod> <out>           one archive's resources as raw object/GPU files
rip cod3-check <file.cod | folder>        check every layout rule the ripper knows
rip cod3-locate <file.cod | folder> <name | key>...
                                          where textures are, in every archive
rip cod3-hd-pack <textures> <upscaled> <out>
                                          the HD pack from upscaled PNGs of any scale
rip cod3-user-pack <textures> <png dir> <out>
                                          the user pack from your PNGs (checked with the game's rules)
rip cod3-check-pack <pack dir>            check a texture pack
rip identify <file | folder> [--open]     which game each .ff / .cod file comes from
rip version
```

`cod3-all`'s output: `textures/<name>.dds` and `.png` (shared by all archives, as the game finds textures by name),
`textures/hd_keys.tsv`, `meshes/<archive path>/<mesh>.gltf` + `.bin`, `materials/<archive path>.json`. Open the glTF
files in Blender or any glTF viewer. The ripper also reads Big Red One's older `.cod` archives (`kapf::Archive::game`
tells them apart) and World at War's fastfiles (`rip waw-ff`).

## Editing `.cod` archives: `tools/cod_convert`

Plain Python 3, no other packages. Every reader and writer here was checked against all 451 retail archives: parsing
then rebuilding gives the same bytes. Keep a copy of any archive you change; put the changed one where the game reads
its assets (the setup tool's `assets/` folder); the game reads stored (unpacked) chunks as well as packed ones.

| File | What it does |
|---|---|
| `kapf.py` | the `.cod` container (KAPF 2.06): sections, chunk tables, directories, source paths; NCH/LZO1X chunks unpacked on read; the writer stores every chunk as is |
| `apkf.py` | the APKF chunks inside (the game's resource banks): items, their parts, pointer and import tables. Items can change size and new items and names can be added; every pointer follows |
| `xtex.py` | textures (TEX items): list them, export one level as PNG or all as DDS, import pictures over textures |
| `lzo1x.py` | LZO1X decompression (bounds-checked) |
| `apkfcheck.py`, `dircheck.py`, `roundtrip.py`, `survey.py` | checks of the above against your archives (read-only, they print counts) |

Textures in an archive:

```
python3 tools/cod_convert/xtex.py list IN.cod
python3 tools/cod_convert/xtex.py export IN.cod NAME OUT.png [--level N]
python3 tools/cod_convert/xtex.py dds IN.cod OUTDIR [NAME ...]
python3 tools/cod_convert/xtex.py import IN.cod OUT.cod NAME PICTURE.png [NAME PICTURE.png ...] \
    [--no-mips] [--alpha auto|cutout|plain]
```

`import` writes each picture over texture `NAME` in that texture's format (DXT1, DXT3 or DXT5; others are refused), at
any power-of-two size from 4 to 4096 a side; a new size updates the texture's header, its GPU data and the archive's
directory, and the mip levels are made again. For most uses the user texture pack above is simpler and leaves the
archives alone.

The formats themselves (KAPF, APKF, TEX, meshes, materials, the HD keys) are in [formats/cod3.md](formats/cod3.md),
each fact marked verified or observed.

## Add-ons and other switches in the setup tool

- **Map packs** (Xbox 360 marketplace packages): only packs whose SHA-1 is known are installed; their files go to the
  game's content folder (`userdata/.../415607E1/00000002/<package>/`), where both programs find them as the console
  did. Removing deletes only what was installed (`userdata/addons.json`).
- **Censored textures**: the German release's 84 censored campaign archives, switched on or off by renaming (copied
  once from that disc, checked by MD5).
- **Texture packs**: the Textures page (HD pack scale, user pack, the built-in designs that cover the Nazi flags and
  insignia).

## Big Red One

Big Red One has no texture packs yet. To look at its textures, run it with `BRO_TEX_DUMP_DIR=<folder>`: each decoded
texture is saved there as a `.ppm` with its name and format in the log (`bro/README.md` lists every setting). Its
archives are read by `engine/crates/kapf` too.

## Writing a new tool

- Paths come in as arguments; nothing is written outside the output folder you name; game files are only read.
- Treat names and offsets from game files as untrusted: check every size and keep writes inside the output folder.
- Document the format you decode in `docs/formats/`, with each fact marked verified or observed, and name your
  sources in `docs/SOURCES.md`.
- Test with generated data, never retail game data, and never commit game data.
- The Rust side's rules are in [`engine/README.md`](../engine/README.md) ("Writing code people can debug").
