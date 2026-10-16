# Call of Duty 3 (Xbox 360) data formats

These are working notes for the CoD3 ripper (`engine/crates/kapf`, `engine/tools/rip`). Each fact has a status:

- **Verified:** decoded from the retail files and cross-checked.
- **Observed:** seen in the data but not yet explained.
- **From the reference:** taken from CoD3Recomp's `docs/animation.md` (see SOURCES.md).

## Files on the disc

Each mission folder `sp/<level>/` holds:

- `<level>.cod`: the level's own archive (the main banks, plus ANIM, AUDIO, FLI and STRING sections);
- `<level>.dll`: the level scripts, compiled C++ (see the recomp);
- `<level>.wbk`: the level's sound bank;
- `chars.cod`, `weapons.cod`, `vehicles.cod`, `fx.cod`, `start.cod`, `mid.cod`, `ei_char.cod`, `int_veh.cod`;
- `znNN.cod`: streamed zones.

Shared files: `sp/global.cod`, `sp/anim.cod`, and the `bcen_*.wbk` sound banks.

## `.cod` container: KAPF (verified)

All header integers are little-endian.

| Offset | Field |
|---|---|
| 0x00 | `"KAPF"` |
| 0x04 | u32 version, `0x4003D70A` in every retail file |
| 0x08 | u32 header size, `0x30` |
| 0x0C | u32 section count |
| 0x10 | u32 end of the chunk tables |
| 0x14, 0x18 | u32 unknown |
| 0x1C | char[20] builder: `"CODAUTO30"` (global.cod) or `"CODAUTO32"` |
| 0x30 | section records |

**Section records** are 0x18 bytes each:

| Offset | Field |
|---|---|
| +0 | char[12] name. `""` is the main banks; also `ANIM`, `AUDIO`, `FLI`, `STRING` |
| +12 | u16 chunk count |
| +14 | u16 unknown, probably an item count (for example, 248 for the Saint-Lô main section) |
| +16 | u32 offset of the chunk table |
| +20 | u32 unknown; the values grow from record to record |

**Chunk table entries** are 0x20 bytes each:

| Offset | Field |
|---|---|
| +0 | u32 file offset, 64 KB aligned |
| +4 | u32 unpacked size |
| +8 | u32 slot size, usually `0x300000` |
| +12 | u32 flags, `0x82` in every file seen |
| +16 | u32 stored size. 0 means the chunk is stored as is; otherwise it is NCH-packed |
| +20 | u32 × 3, zero |

### NCH blocks (verified)

A packed chunk is a run of blocks. Each one starts at the next 512 KB (0x80000) boundary inside the chunk. The header is 0x20 bytes, big-endian:

```
"NCH\0"  u32 packed size  u32 checksum  u32 unpacked size
u32 output offset  u32 checksum  u32 block size (= 0x20 + packed)  u32 1
```

The payload is a **plain LZO1X stream**.

- How this was established: decompressing it gives exactly the stated unpacked size, and the stream's end marker lands exactly at the stated block size.
- The two checksums are not identified yet.
- `rip cod3-split <file.cod> <out>` unpacks every chunk and splits it into its resources. Tested on all 30 archives of Saint-Lô plus the shared ones.

## Unpacked main chunks: APKF (verified)

All values are big-endian. "Self-relative" pointers in the directory are added to the address of the field that holds them.

**Header:**

| Offset | Field |
|---|---|
| 0x00 | `"APKF"` |
| 0x04 | `04010000`, `00000004`, `FFFFFFFF` (constant) |
| 0x10 | u32 data area count: 2 ("image" and "physical"); 1 in small chunks |
| 0x14 | u32 directory end |
| 0x1C | type records, 0x1C bytes each, until the first item table |

**Type records:**

| Offset | Field |
|---|---|
| +0 | type: a byte-reversed four-character code (`TEX`, `MESH`, `MAT`, `SKEL`, `MLTI`, `FX`, `FONT`), or the hash of a file extension (`.cgb`, `.csv`, `.bgib`, …) |
| +4 | version: TEX 3, MESH 2, MAT 2, SKEL 2, MLTI 100, FX 1, FONT 1, others 0 |
| +8 | part count: 1 = object data, 2 = object + GPU data |
| +12 | self-relative pointer to the item table |
| +16 | item count |
| +20 | object data alignment |
| +24 | GPU data: `0xFF000000` = none; otherwise `0x01` in the top byte and the alignment below (TEX `0x1000`, MESH 4) |

**Item table entries** are `4 + 4 × part count` bytes: a self-relative pointer to a name record, then one size per part.

**Name pool.** It starts right after the last item table. A name record is a u32 hash (h = h × 33 + c, lower case) followed by the zero-terminated name.

**Data area descriptors** start at directory end + 0x14, 0x18 bytes each:

| Offset | Field |
|---|---|
| +0 | self-relative pointer to the area's name record: `"image"` (object data) or `"physical"` (GPU data) |
| +8 | alignment: image 0x10 (or the largest item alignment, e.g. 0x800), physical 0x10000 |
| +12 | unknown: 0 or 0x10000 |
| +16 | size of the area's data |
| +20 | unknown |

**Data layout.** Nothing points to the item data; it follows directory order:

1. **Image** (object data). It starts right after the descriptors, aligned to the image alignment. Each item is aligned as its type record says. Unused bytes are filled with `0xCD` inside objects (MSVC debug fill, for runtime-only fields) and with `'C'` (0x43) between them.
2. **Physical** (GPU data). It starts at the next 64 KB boundary after the image. Each item is aligned by its type's flags.
3. **Pointer table.** One u32 per object field that holds a pointer, as a dword index from the image start. It ends with `0xFFFFFFFF`. The field's own value is either:
   - `0xFC0000nn`: a name, the record at name pool + 4 × nn;
   - otherwise the target, as a dword index from the image start.
4. **Import table**, 12 bytes per entry, ending with `0xFFFFFFFF`:
   - u32 field, as a dword index from the image start;
   - u32 type (four-character code);
   - u32 name: byte offset of a name record in the name pool.

   The game fills the field with the named resource of that type, which may come from another chunk or archive. In the file the field holds a placeholder (`0xCDCDCDCD`, or a count).

**Checks.** `rip cod3-check` tests every rule above. On the 30 archives of Saint-Lô plus the shared ones (132 chunks), everything holds:

- all 132 chunks end exactly where the import table ends;
- 1720/1720 texture D3D headers are where the layout puts them;
- 1720/1720 texture sizes match the fetch constant;
- 5044/5044 materials and meshes start with a reference to their own name;
- 103,835/103,835 pointers land inside an item;
- 1523/1523 meshes decode.

Over the owner's whole disc (370 archives, 2026-10-06), every rule also holds, with one exception: 8 of 343 pointers in `crssrds/fx.cod` land outside any item. `rip cod3-check` now reports where they land, so the next run will show what they are.

A second, known exception is `frontend.cod`'s `DTEX` section. It holds one 1024×512 DXT1 texture, and about 45 MB follow its tables, probably more frames of the animated menu background.

**Correction (2026-10-06).** Earlier notes placed the object data 16 bytes too late (`directory end + 0x54`). As a result, they read the next texture's width at +0x50, and they could not explain the chunk with 0x800 alignment.

The game's side, for reference:

- **Type registry:** sub_82128500(type, version, load, unload, arg). It is a linked list at 0x8290E810. TEX and MESH are registered in sub_8245F3D8: TEX v3 → sub_8245C018, MESH v2 → sub_8245BF68.
- **Directory walker:** sub_82128CF8. It steps through type records with a stride of `(area count + 5) × 4`, and through items with a stride of `(part count + 1) × 4`.

### Textures (TEX v3, verified)

The object data is 0x60 bytes:

| Offset | Contents |
|---|---|
| +0x00 | u32 width, u32 height |
| +0x10 | resource header: name reference, a value, `0x4000`, zeros |
| +0x2C | Xbox 360 D3D texture header: Common `0x00200003`, ReferenceCount, Fence, ReadFence, Identifier, BaseFlush, MipFlush |
| +0x48 | the Xenos texture fetch constant, 6 words (ends the object) |

The GPU data is the tiled surface:

- base level at 0;
- mip chain at fetch `dword5 & 0xFFFFF000`.

Formats seen: DXT1 (18), DXT2/3 (19), DXT4/5 (20), with 8-in-16 endian swap, and 8_8_8_8 (6) with 8-in-32 swap and the swizzle Z Y X W, Direct3D's A8R8G8B8: after the swap the bytes are B, G, R, A in memory (the ripper's DDS and PNG files take them so; a texture with another swizzle, none seen, is not listed for the HD pack).

**Small textures (fixed 2026-10-11):** with `packed_mips` set, the levels 16 texels or smaller share one 32×32 tile, and when the texture's *smaller* side is already 16 or less (a 256×16 trim, a 128×16 health bar, a 16×32 door) the base level itself lies in that tile, 16 texels in: 16 rows down for a texture wider than tall, 16 columns to the right otherwise (Xenia's `GetPackedMipOffset` for level 0); the tile's first rows hold the smaller levels and padding. Reading row 0 instead gives the mips and garbage (`weaponnameback`, the strip behind the weapon's name, came out as magenta noise, and the game drew that noise behind the name once the HD pack replaced it). Such a texture's fetch constant has mip address 0, so the game treats it as having one level.

`rip cod3-tex` untiles the base level (XGAddress2DTiledOffset, from that offset). It writes a DDS file and a PNG (decoded from BC1/2/3/5) per texture.

- **Results:** 1285 distinct textures from the Saint-Lô and shared archives. The Thompson, Kar98k, Dixon and sandbags were checked by rendering them textured.
- **Not handled yet:** cube maps (dimension 3, 5 seen), mip levels, and the DTEX frames.

#### HD texture keys and pack (ripper output layout 2; any upscale since layout 3)

The recompiled game can load upscaled textures in place of the originals. The ripper and the game name a texture by a key computed from its pixels, so the names in the archives don't matter:

- **Key:** XXH64 (seed 0) of the base level as `rip` untiles it: blocks in rows, packed tightly (`width_blocks × height_blocks × bytes_per_block` bytes), after the fetch constant's endian swap. Tiled textures use XGAddress2DTiledOffset with `pitch_blocks = max(pitch / block_width, width_blocks)`.
- **Which textures:** 2D, format DXT1 (18), DXT2/3 (19), DXT4/5 (20), DXN (49) or 8_8_8_8 (6), with the usual swizzle (see above). A base level in the packed mip tile (a side of 16 or less) is hashed from its place in the tile; the game's texture cache does the same (patch 0016), so the keys of such textures changed with output layout 6.
- **`textures/hd_keys.tsv`** (written by `cod3-all` and `cod3-tex`, output layout 6): one line per texture instance in every archive, also the copies skipped because another archive already wrote that name. Columns are `key` (16 lowercase hex digits), `width`, `height`, `xenos_format`, `name`, `file` and `kind`, separated by tabs. `file` is the PNG in `textures/` (without extension) that holds exactly this key's picture: `<name>` for the copy saved under the name, `<name>__<key>` for another picture under the same name, or `-` for a texture the HD pack leaves as the game has it. `kind` is what the texture holds, from the material parameters that use it (by name hash, see below, through texture sets): `diffuse` (a color picture: `diffusemap_texture`, `diffuse__w_alpha__texture`, `garand_col_texture`, overlay maps, and parameters with no data word in their name), `normal` (`normalmap_texture`, `normal_swatch_texture`, `k98_n_texture` …), `specular`, `detail` (`detailmap_texture`), `other` (height maps, masks, lightmaps, noise, environment maps), `mixed` (one picture used as several kinds) or `unknown` (no material names it: fonts, HUD, effects, menu pictures, trees). The upscaler and the pack take `diffuse` only unless told otherwise (`rip cod3-hd-pack --kinds`): a normal, specular or detail map upscaled and denoised like a picture changes the lighting, and an unknown texture may be anything. One name often has several pictures: every level has its own lightmaps (`lm0000` …), terrain lightmaps (`pg_colorlm_*`) and fog ramp (`fogramp`), and names longer than the archive's name field are cut short, so a color map and its normal, specular and mask maps can share a name (`brick$us_dirty_broken_wall_`). Left as the game has them (`-`): lightmaps, fog ramps and other lookup tables (8 texels or fewer on a side), whose values are lighting data rather than pictures (upscaling changes the lighting, and a lightmap atlas would bleed between its parts). Lines are sorted by name, then key, after a `#` header line. (Until 2026-10-11 there was no `file` column and the pack gave every key of a name the one saved picture: in Saint-Lô the ground was lit with another level's lightmaps. The setup tool repairs such an index, `c3w/upscale.py` `repair_old_pack`.)
- **`rip cod3-hd-pack <textures> <upscaled> <out> [--kinds diffuse,normal,...|all]`:** for each `file` of hd_keys.tsv of a packed kind (`diffuse` by default) with an `<upscaled>/<file>.png` (the setup tool's folder is `textures_upscaled`; it upscales only those files) that is a scaled-up copy of the original, it builds a full mip chain (2×2 box filter, down to 1×1) and encodes every level in the original format. The size of the PNG is not fixed, because another upscaler or model may pick another one: any scale of 1 or more works, whole or not (1x, 2x, 3x, 4x, 2.5x ...), as long as the aspect ratio is the original's to within one texel of rounding on each side (`|png_w × h − png_h × w| ≤ w + h`); a PNG of another shape, or smaller than the original, is skipped and counted as "wrong size". The DDS keeps the PNG's size except that a longest side over 8192 is scaled down to 8192 (so a huge upscale can't take gigabytes of memory), and for the BC formats each side is resampled to the nearest multiple of 4 (a tie rounds up; Direct3D 12 can't create a compressed texture of another size). Resampling is separable, with a tent filter that is widened when scaling down. The summary line `scales:` counts the packed textures per scale (the PNG's longest side over the original's) and how many were resampled. (The PNG reader takes sides up to 16384; a larger PNG is skipped as unsupported.) The formats: BC1 (with transparent texels where alpha < 128), BC2, BC3, BC5 (red and green), or B, G, R, A bytes for 8_8_8_8 (the usual 32-bit DDS masks). It writes `<out>/<file>.dds` (legacy header with a mip count, largest level first) and `<out>/index.tsv`: `key`, `width`, `height`, `xenos_format` (the original size and format) and the DDS `file`, one line per key. A DDS serves only the key whose picture it was made from (a key found under several names is listed once), so no texture gets another texture's picture. A DDS that is newer than its PNG and has the expected header and length is kept; DDS files in `<out>` the index no longer lists are removed.

The ripper doesn't rewrite output files whose content is unchanged, so the upscaler, which skips PNGs older than their upscaled copy, has nothing to redo after a re-rip.

**Name hashes.** Every name record in an archive is a u32 hash followed by the text, and the text is cut to 27 characters while the hash is of the full name: `hash = hash × 33 + byte` over the lower-cased name, from 0. Imports (a material's texture parameter, a texture set's member) are matched by that hash, which is how the game tells `brick$us_dirty_broken_wall_c`, `_h`, `_n` and `_s` apart when all four show as `brick$us_dirty_broken_wall_`. `kapf::apkf::name_hash` computes it; the ripper matches materials to textures by it when it works out each texture's `kind`.

### Texture sets (TEX items without GPU data, verified)

Character archives list interchangeable heads and bodies this way. For example, `c_uspriv_ng_head_text` lists `c_uspriv_head1_c` … `head3mp_c`. A material names the set, and the game picks one of its textures.

| Offset | Contents |
|---|---|
| +0x00 | zeros |
| +0x10 | name reference |
| +0x18 | u32 kind: 4 (a real texture has `0x4000`) |
| +0x24 | u32 count |
| +0x28 | pointer to the list: one field per texture, each an import or a local pointer |

### Materials (MAT v2, verified)

| Offset | Contents |
|---|---|
| +0x00 | name reference: `<part>.<model>` |
| +0x04 | name reference: class. `fx` for almost all; `cdgunsight` and `cdsimple` have no parameters |
| +0x14 | the shader effect (FX): an import (152 of 157) or a local pointer |
| +0x24 | u32 parameter count (at most 13 seen) |
| +0x28 | pointer to the parameter table |

**Parameter entries** are 0x24 bytes:

| Offset | Contents |
|---|---|
| +0x00 | name reference: `diffuse_texture`, `normal_map_texture`, `specular_power`, … |
| +0x10 | u32 type: 2 int, 3 float, 4 float4, 11 texture |
| +0x14 | pointer to the value |
| +0x18 | u32 value size: 4, 4, 16, 28 |

A texture value's first field links to the TEX. It is either a pointer to a TEX in the same chunk or an import by name; `"none"` means no texture. All 509 texture parameters seen resolve one of these two ways (443 imports, 66 local).

The parameter names belong to each shader (`diffuse_map_texture`, `garand_col_texture`, `w_sten_n_tga_texture`, …). The glTF export therefore picks base color and normal maps by name (see `cod3_assets::material::texture_for`).

### Meshes (MESH v2, verified)

| Offset | Contents |
|---|---|
| +0x00 | name reference: `<mesh>.<model>` |
| +0x08 | u32 surface count |
| +0x0C | pointer to the surface table: 8 bytes per surface (u32 0, pointer to the surface) |
| +0x14 | u32 count of lower LODs |
| +0x18 | pointer to the LOD table: 8 bytes per level (pointer to the MESH, f32 switch distance) |
| +0x20 | bounding sphere: center, radius |

**Surfaces.** A surface is one draw with one material. Static surfaces are 0x130 bytes; skinned ones are 0x1A0.

| Offset | Contents |
|---|---|
| +0x00 | bounding sphere, then box min and max |
| +0x34 | pointer to the material (a MAT in the same chunk) |
| +0x38 | pointer to the bone palette, and its size (skinned meshes) |
| +0x40 | pointer to the D3D vertex buffer header (Common `0x00200001`). Its fetch constant at +0x18 gives dword0 = offset in the mesh's GPU data, dword1 bits 2–25 = size in dwords |
| +0x48 | u32 vertex count |
| +0x50 | pointer to the D3D index buffer header (Common `0x20200002`), with offset and size at +0x18 |
| +0x58 | u32 index count (16-bit, big-endian, triangle list) |
| +0x60 | pointer to the vertex declaration: u32 stride, pointer to the D3DVERTEXELEMENT9 array (12 bytes each, ending with stream `0x00FF`) |

In all 236 surfaces checked, vertex count × stride equals the vertex buffer size, and index count × 2 equals the index buffer size.

Declaration types seen:

| Type | Value | Used for |
|---|---|---|
| FLOAT3 | `0x002A23B9` | position |
| DEC3N | `0x002A2187` | normal, tangent, binormal |
| FLOAT2 | `0x002C23A5` | UV 0–2 |
| D3DCOLOR | `0x00182886` | color |
| UBYTE4 | `0x001A2286` | blend indices (skinned meshes) |
| UBYTE4N | `0x001A2086` | blend weights (skinned meshes) |

`rip cod3-mesh` writes glTF 2.0: positions, normals and UV0, converted to Y-up, with one material per surface. Each material links its base color and normal textures as PNG. The full parameter list goes into the material's `extras`.

**Not handled yet:** skinning (SKEL and the bone palette), and level world geometry (`.bgib` / `.btrb` / `.cgb`).

### Models (MLTI v100, the `.xemesh` items, verified)

| Offset | Contents |
|---|---|
| +0x00 | u32 group count: 2 (MAT, MESH) or 3 (MAT, MESH, SKEL) |
| +0x04 | pointer to the group table: 12 bytes per group (type code, count, pointer to members) |

**Members** are 8 bytes each: a name reference and a pointer to the item.

`rip cod3-mesh` lists every model's parts in `materials/<archive>.json`.

## Other sections (observed)

| Section | Contents |
|---|---|
| `STRING` | `"STRT"` header, a hash table and the level's text |
| `FLI` | `"FLIN"` header, purpose unknown |
| `AUDIO` | the path of the matching `.wbk` (`c:\c…`) |
| `ANIM` | one large stored chunk, no magic |
