# Call of Duty: World at War data formats

Working notes for the World at War ripper (`engine/crates/ff_t4`, `rip waw-ff`, `rip waw-zone`). The containers are read by `engine/crates/fastfile`, which first tells which game a fastfile is from: Call of Duty 4, Modern Warfare 2 and Black Ops use the same magic, so a fastfile from one of them is refused as that game's ([fastfiles.md](fastfiles.md) has every game's headers). Each fact has a status:

- **Verified:** decoded from the retail files and cross-checked.
- **Found by us in the game's code:** read statically from CoDWaW.exe 1.7 (never run), with the addresses given.
- **From OpenAssetTools:** the T4 asset type list in OpenAssetTools (see SOURCES.md).

## Where the fastfiles are

| Version | Location |
|---|---|
| PC (Steam or disc) | `zone/<language>/*.ff` (for example `zone/english/`); `main/*.iwd` holds loose files (zip). |
| Xbox 360 disc | The disc's root folder: 67 fastfiles, such as `common.ff`, `ui.ff`, `code_post_gfx.ff`, the campaign levels (`ber1.ff`, `pel1.ff`, …), `nazi_zombie_prototype.ff`, and `mp_*.ff` with their `*_load.ff`. Also on the disc: `highmip/` (3,212 files, probably streamed high-resolution textures), `Stream/`, `SFX/`, `voiceovers/` and `battlechatter/` (audio). |

## Fastfile containers (verified)

All three use version 387.

| Kind | Header | Data |
|---|---|---|
| PC | `"IWffu100"`, u32 version, little-endian | one zlib stream from byte 12 |
| Xbox 360, unsigned (campaign, zombies; 38 files) | `"IWffu100"`, u32 version, big-endian | one zlib stream from byte 12 |
| Xbox 360, signed (multiplayer; 29 files) | `"IWff0100"`, u32 version (big-endian), then `"IWffs100"` and u32 0 | see below |

**Signed files.** After the 12-byte container header, the file is cut into 0x2000-byte (8 KB) chunks:

1. The first chunk is the `"IWffs100"` authentication header:
   - a 32-byte hash;
   - a 256-byte signature;
   - the zone name (32 bytes, for example `ui_mp`);
   - u32 0;
   - one SHA-256 per group: the hash of that group's hash chunk;
   - padding to the end of the chunk.
2. Then groups, each made of:
   - one hash chunk: the SHA-256 of each of the next 256 data chunks (padded when the group is shorter);
   - up to 256 data chunks (2 MB).

Joining the data chunks gives an ordinary zlib stream, with a valid Adler-32. The last data chunk is padded after the stream ends.

How this was checked, on `ui_mp.ff` (5 groups) and `mp_seelow.ff` (35 groups):

- Each file has as many master hashes as groups.
- The SHA-256 of the first data chunk is the first entry of the first hash chunk.
- The SHA-256 of the first hash chunk is the first master hash.
- The stream inflates with a matching Adler-32, to exactly the zone's declared size plus the 36-byte `XFile` header.

Our first reading put the chunks 12 bytes off: it started the data at 0x4000 instead of 0x400C. That misread 12 bytes at every 2 MB boundary. Inflating as raw DEFLATE (with the Adler-32 ignored) mostly resynchronized, but `mp_seelow.ff` failed with "invalid stored block lengths", which is how the mistake was found.

## Zone (verified for PC and Xbox 360)

The zone is little-endian on PC and big-endian on Xbox 360. The layout is otherwise the same:

```text
XFile        u32 size, u32 externalSize, u32 blockSize[7]
XAssetList   u32 scriptStringCount, ptr scriptStrings, u32 assetCount, ptr assets
             then the script strings, then assetCount × { u32 type, ptr header }
```

A pointer of `0xFFFFFFFF` means the data follows inline in the stream (see [Reading every asset of a PC zone](#reading-every-asset-of-a-pc-zone-verified)).

**Asset type numbers differ on Xbox 360.** The Xbox 360 list has one extra type at number 7, so every type from 8 on is one higher than on PC (`techniqueset` is 7 on PC and 8 on Xbox 360; `rawfile` is 32 and 33).

- How this was checked: in `code_post_gfx.ff`, every per-type asset count on Xbox 360 matches the PC count of the type one number lower (65 technique sets, 7 images, 19 sounds, 2 light defs, 9 fonts, 2 menu lists, 36 string tables).
- Type 7 hasn't appeared in any zone looked at yet; it is probably the pixel shaders.

**Raw files** (GSC/CSC scripts and configs) are found by their inline layout: `{ name = -1, len, buffer = -1 }`, then the name, then the data. This works on both platforms:

- Xbox 360 `code_post_gfx.ff`: 96 raw files;
- PC: 79.

## Reading every asset of a PC zone (verified)

`ff_t4::Zone::read_assets` and `rip waw-zone` read a PC zone the way the game's own loader does: `DB_LoadXFileInternal` (0x0047B300 in CoDWaW.exe 1.7) and the generated `Load_*` functions it calls. Found by us in the game's code:

- **Memory.** Seven blocks, sized by the zone header: TEMP 0, RUNTIME 1, LARGE_RUNTIME 2, PHYSICAL_RUNTIME 3, VIRTUAL 4, LARGE 5, PHYSICAL 6.
  - The stream is read into the current block (`Load_Stream` 0x00490310). RUNTIME is zeroed memory and never in the stream.
  - `DB_PushStreamPos` and `DB_PopStreamPos` (0x00490210, 0x00490280) switch blocks. Popping back into TEMP puts TEMP back where it was, so TEMP is reused for each asset.
- **Pointers.**
  - 0 is null.
  - -1 (`0xFFFFFFFF`): the data follows in the stream.
  - -2: the same, and first a 4-byte slot that will hold it (in VIRTUAL, 4-aligned; `DB_InsertPointer` 0x004902E0).
  - Any other value is a reference to data loaded before: `((block << 29) | offset) + 1`. For an asset it refers to a slot holding the asset (`Load_XModelPtr` 0x00480840 is the pattern).
- **Assets.** An asset's structure is read into TEMP and its contents into the blocks its loader pushes. Then the structure is copied to the asset's pool (`DB_AddXAsset` 0x0048E300). Two exceptions:
  - string tables are read where they are (`Load_StringTablePtr` 0x004896E0);
  - a sound named by an alias is a 4-byte slot, then the name (`Load_snd_alias_list_tPtr` 0x0047BAD0).
- **Script strings.** A pointer array, then each string inline (`Load_XAssetListCustom` 0x0047B1F0). An entry can be null.

**Checks.** A zone *reads exactly* when all of these hold:

- the stream ends exactly at the zone's end;
- RUNTIME, VIRTUAL, LARGE and PHYSICAL each end at the size the header gives, and TEMP never goes past its size;
- every reference lands on something loaded before: a structure, a string, a slot, or an element of an array.

**Result.** All 129 fastfiles of the PC game (1.7, `zone/english`) read exactly: 230,789 assets of 28 types, maps included (`gfxworld`, `clipmap`, `clipmap_pvs`, `comworld`, `map_ents`). This was first checked with a Python prototype. The Rust reader was then built and run on the owner's PC on 14 of those fastfiles: 11,699 assets, the same per type.

**The layout file.** The reader holds the loader's rules above. A *layout* says what the loader does structure by structure: which members are pointers and what counts them, which member of a union is read, alignment and blocks. Its format is described in `engine/crates/ff_t4/src/layout.rs`. A few irregular shapes are loaders written out in the reader, and the layout names the fields they read (animation index tables, animation delta parts, weapon accuracy graphs).

- **World at War PC's layout is not published.** It has 121 structures, and their names, member names, sizes and offsets were taken from the T4SP plugin's type header (AGPL-3.0). Nothing from that header is published (SOURCES.md, section 5). It is kept with the project's private notes.
- **How rip gets it:** `rip --layout <file>`, or the `RIP_WAW_LAYOUT` environment variable.
- **Without it,** `rip waw-ff` finds raw files by their inline layout (as described in the previous section) and `rip identify --open` stops at the asset table.

**Xbox 360** zones are not read asset by asset yet. Their structures may differ in places: the Xbox 360 build's asset type list has the extra type 7, and its `dvar_s` is Call of Duty 4's rather than PC 1.7's. So they need a layout of their own, read big-endian.

## Map packs from the PC patches

The discs have no map packs. The official PC patches add them as ordinary PC fastfiles (plus `.iwd` files), which the ripper reads like any other. Patch 1.6 adds Map Pack 3: `mp_drum` (Battery), `mp_bgate` (Breach), `mp_vodka` (Revolution) and `nazi_zombie_factory` (Der Riese). Sources: SOURCES.md, section 3.

**Not checked yet:** how the patch installer (`CoDWaW-1.5-1.6-PatchSetup.exe`, inside `codwaw-v1.5_1.6.zip`) stores its files. `cod3_waw_setup.py map-pack <download>` reports what it's made of: the Windows program's appended data, and the installer and archive markers it contains.

## Not decoded yet

- The Xbox 360 asset loader (its layout).
- A layout made only from the game's own loader with our own names, which could be published.
- Turning the assets read into open formats (models, materials, images, sounds): PC zones are read completely, nothing is converted yet but raw files.
- Xbox 360 images, which are tiled like CoD3's (see `engine/crates/xenos`), and `highmip/`.
- `.iwd` contents on PC (zip archives; `engine/crates/inflate` reads them).
