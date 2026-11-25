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

## Xbox 360 images (verified)

`ff_t4::Zone::xbox_images` (`engine/crates/ff_t4/src/xbox_images.rs`) and `rip waw-images` read the textures of Xbox 360 zones without a zone layout, by their own records. Found by us in the disc's zones on 2026-11-24 (`code_post_gfx`, `ui`, `ber1`, `mp_seelow`). The reference tools don't read Xbox 360 assets: OpenAssetTools writes an Xbox 360 zone out as one raw dump, and the image-stream header it skips is only in Modern Warfare 2's Xbox 360 files (SOURCES.md).

**The record.** 40 bytes, big-endian:

| Offset | Field |
|---|---|
| 0 | u32 map type: 3 = 2D, 5 = cube (4 = 3D, not seen yet) |
| 4 | pointer to the D3D texture: always -2 (it follows, with a slot) |
| 8 | u8 semantic (2 colour, 5 normal, 8 specular, ...), 3 bytes padding |
| 12 | u32 card memory: the bytes of pixel data, mips included, a multiple of 4 KB |
| 16 | u16 width, height, depth |
| 22 | u8 category; u8 "pixels at the end" (see below) |
| 24 | pointer to the pixels: -1 |
| 28 | u32, not needed (unknown) |
| 32 | u16 stream slot; u8 streamed (1: `highmip/` has this texture); u8 0 |
| 36 | pointer to the name: -1 (the name follows) or a reference to a string read before (3 images in `ber1`) |

Then, in this order:

1. the name, when its pointer is -1;
2. the pixels, unless they are at the end;
3. a 16-byte load record: u8 levels, u8 flags, u16 width, height, depth (the same as the record's), u32 D3D format (0x1A200152 DXT1, 0x1A200154 DXT5, 0x1A200171 DXN, 0x18280186 A8R8G8B8), u32 -1;
4. the 52-byte D3D texture header, **stored little-endian**: 7 words (common = 3, reference count 1, ...), then the six-word texture fetch constant that `engine/crates/xenos` reads for Call of Duty 3.

**Pixels at the end.** Most textures (every one in a level's zone) keep their pixels in block 2, LARGE_RUNTIME, which the stream holds last: the last `blockSize[2]` bytes are every such image's pixels, one after another in record order. The check that every record was found is that their sizes add up to the block exactly: in `ber1` 810 records with an inline name add up to 119,730,176 bytes, and the 3 with a name read before bring it to the block's 119,853,056. When the sizes don't add up, the reader gives no image pixels from that block. Block 3 is empty in every zone looked at; the reader refuses a zone that uses it.

**The pixels** are tiled and byte-swapped like Call of Duty 3's, and untile with the `xenos` crate unchanged. Checked by eye on the colour and normal maps of a `ber1` uniform and the `ui` logo.

**`highmip/`.** 3,212 files named `<image name>.hi`, with no header. Each is the top level of a streamed texture at twice the width and height, tiled the same way, in the same format: exactly that level's tiled size (a 512 x 512 DXT1 texture in the zone has a 524,288-byte file: 1024 x 1024 DXT1). `rip waw-images` writes that level as `<name>.png` / `.dds` and the zone's own top level as `<name>.zone.png`. In `ber1`, 415 textures have one; in `mp_seelow`, 318.

**HD pack keys (2026-11-24).** `rip waw-hd-textures <file.ff | folder> <out_dir>` lists every texture the HD pack can replace as `rip captured-textures` does (`<out_dir>/textures/` PNGs and `hd_keys.tsv`), so World at War's HD pack needs no playing first. The key is `xenos::hd_key` of the top level, the bytes the game's texture cache hashes. A streamed texture gets two keys, the zone's level and the `highmip/` level, because either can be in memory when the game first draws with it. Kinds are guessed as for captured textures (DXN normal, the rest diffuse; `hd::keep_original` left alone). In `ber1`: 1,137 keys, 811 diffuse and 322 normal pictures, 4 left alone; `cod3-hd-pack` packs the list (checked at 1x). Not yet checked against the running game: no World at War texture capture exists on the build PC.

**Results.** Every record found and every block adding up in the four zones: `code_post_gfx` 81 images, `ui` 111, `ber1` 813, `mp_seelow` 490. Not converted yet: cube maps (79 in `ber1`, 21 in `mp_seelow`: reflection probes and skies) and two formats `xenos` doesn't handle (Xenos formats 2 and 10, one-channel and two-channel 8-bit: 13 in `code_post_gfx`, 14 in `ui`, 3 in each level).

## Map packs from the PC patches

The discs have no map packs. The official PC patches add them as ordinary PC fastfiles (plus `.iwd` files), which the ripper reads like any other. Patch 1.6 adds Map Pack 3: `mp_drum` (Battery), `mp_bgate` (Breach), `mp_vodka` (Revolution) and `nazi_zombie_factory` (Der Riese). Sources: SOURCES.md, section 3.

**Not checked yet:** how the patch installer (`CoDWaW-1.5-1.6-PatchSetup.exe`, inside `codwaw-v1.5_1.6.zip`) stores its files. `cod3_waw_setup.py map-pack <download>` reports what it's made of: the Windows program's appended data, and the installer and archive markers it contains.

## Not decoded yet

- The Xbox 360 asset loader (its layout).
- A layout made only from the game's own loader with our own names, which could be published.
- Turning the assets read into open formats (models, materials, images, sounds): PC zones are read completely, nothing is converted yet but raw files. Xbox 360 images are converted (section above).
- Xbox 360 cube maps, and the two 8-bit formats above.
- `.iwd` contents on PC (zip archives; `engine/crates/inflate` reads them).
