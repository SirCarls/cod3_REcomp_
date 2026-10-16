# Call of Duty: World at War data formats

Working notes for the World at War ripper (`engine/crates/ff_t4`, `rip waw-ff`). Each fact has a status:

- **Verified:** decoded from the retail files and cross-checked.
- **From OpenAssetTools:** the T4 structures in OpenAssetTools (see SOURCES.md).

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

A pointer of `0xFFFFFFFF` means the data follows inline in the stream (from OpenAssetTools).

**Asset type numbers differ on Xbox 360.** The Xbox 360 list has one extra type at number 7, so every type from 8 on is one higher than on PC (`techniqueset` is 7 on PC and 8 on Xbox 360; `rawfile` is 32 and 33).

- How this was checked: in `code_post_gfx.ff`, every per-type asset count on Xbox 360 matches the PC count of the type one number lower (65 technique sets, 7 images, 19 sounds, 2 light defs, 9 fonts, 2 menu lists, 36 string tables).
- Type 7 hasn't appeared in any zone looked at yet; it is probably the pixel shaders.

**Raw files** (GSC/CSC scripts and configs) are found by their inline layout: `{ name = -1, len, buffer = -1 }`, then the name, then the data. This works on both platforms:

- Xbox 360 `code_post_gfx.ff`: 96 raw files;
- PC: 79.

## Map packs from the PC patches

The discs have no map packs. The official PC patches add them as ordinary PC fastfiles (plus `.iwd` files), which the ripper reads like any other. Patch 1.6 adds Map Pack 3: `mp_drum` (Battery), `mp_bgate` (Breach), `mp_vodka` (Revolution) and `nazi_zombie_factory` (Der Riese). Sources: SOURCES.md, section 3.

**Not checked yet:** how the patch installer (`CoDWaW-1.5-1.6-PatchSetup.exe`, inside `codwaw-v1.5_1.6.zip`) stores its files. `cod3_waw_setup.py map-pack <download>` reports what it's made of: the Windows program's appended data, and the installer and archive markers it contains.

## Not decoded yet

- The full asset loader (models, materials, images, sounds, weapons) for either platform.
- Xbox 360 images, which are tiled like CoD3's (see `engine/crates/xenos`), and `highmip/`.
- `.iwd` contents on PC (zip archives; `engine/crates/inflate` reads them).
