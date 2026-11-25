# Call of Duty: Finest Hour (original Xbox) file formats

What the project's tools read from the player's own Finest Hour disc. Everything here was worked out from the disc's
files and from the port's own renderer (`RELEASE/fh/src/fh_draw.c`); no outside code was used.

## COD.PAK (2026-11-24)

The disc's game data is one file, `COD.PAK`, starting with the text `Spark Pack (C)2004 Spark Unlimited`.

| Offset | Size | What |
|---|---|---|
| 0x78 | u32 | version (1) |
| 0x7C | u32 | number of entries (1,372 on the Europe disc) |
| 0x80 | 16 bytes per entry | u32 size, u32 offset, u64 hash of the entry's name |

Each entry's data starts on a 0x800 boundary. No names are stored, only their hashes, so readers find
what they want by content. Some entries are plain `DDS ` files; most are the game's own containers, whose first words
vary, holding RenderWare streams among other data.

## Textures: RenderWare Xbox texture dictionaries

Textures are RenderWare texture dictionaries in their Xbox form, inside the pack's entries. All numbers are little-endian.
Each RenderWare chunk starts with a 12-byte header: type, size of the body, library version (0x1803FFFF here).

- A dictionary is chunk 0x16. Its first child is a struct (type 1) of 4 bytes: u16 texture count, u16 device.
  `rip fh-hd-textures` finds dictionaries by this header (a 0x16 chunk whose first child is a 4-byte struct with the
  same version), since the containers around them aren't read yet. COD.PAK has 2,969 of them, with 15,695 textures.
- Each texture is chunk 0x15, a texture native. Its first child is a struct, whose body is:

| Offset | Size | What |
|---|---|---|
| 0 | u32 | platform: 5 (Xbox) for every texture on the disc |
| 4 | u32 | filter and addressing |
| 8 | 32 | name, zero-padded |
| 40 | 32 | mask name |
| 72 | u32 | raster format (below) |
| 76 | u32 | has alpha |
| 80 | u16, u16 | width, height |
| 84 | u8 | depth: 8 for palette textures, 32 for the rest |
| 85 | u8 | levels |
| 86 | u8 | raster type (4, a texture) |
| 87 | u8 | compression (0 everywhere: no DXT) |
| 88 | u32 | size of the pixel data, the level chain rounded up to 4 bytes |
| 92 | | the palette: 256 colors for PAL8, 32 for PAL4, none otherwise; 4 bytes each (B, G, R, A) |
| | | then the pixel data: every level, swizzled, one after another |

Raster formats are RenderWare's bits: 0x2000 PAL8, 0x4000 PAL4, 0x8000 has mip levels, 0x0500 C8888 (8-bit alpha),
0x0600 C888 (no alpha). PAL4 textures keep one byte per index, as PAL8 ones do; only the palette is shorter. On the
Europe disc, 8,493 textures are PAL4, 6,955 are PAL8 and 247 have 32-bit colors:

| Raster | Count |
|---|---|
| 0x4600 PAL4 C888 | 7,136 |
| 0x2600 PAL8 C888 | 4,278 |
| 0x2500 PAL8 C8888 | 2,424 |
| 0xC600 PAL4 C888, mips | 789 |
| 0x4500 PAL4 C8888 | 568 |
| 0x0600 C888 | 168 |
| 0xA600 PAL8 C888, mips | 133 |
| 0xA500 PAL8 C8888, mips | 120 |
| 0x0500 C8888 | 78 |
| 0x8600 C888, mips | 1 |

The swizzle is the Xbox's: the bits of x and y interleaved, x first, for as long as each side still has bits (the
port's `swizzled_offset`).

## HD pack keys

The port's renderer decodes a texture's level 0 into 32-bit colors (`texture_pixels`), one little-endian
word per texel (B, G, R, A), row by row:
- P8 gets its colors from the palette as stored, and indices past the palette's length read as 0.
- X8R8G8B8 gets alpha 0xFF.
- A8R8G8B8 is kept as stored.

A texture's HD key is FNV-1a 64 (offset basis 0xCBF29CE484222325, prime 0x100000001B3) over those bytes. Its file
in the pack is `<width>x<height>_<key in 16 lowercase hex digits>.png`.

`rip fh-hd-textures <COD.PAK | disc folder> <out_dir>` writes every texture's level 0 under that name in
`<out_dir>/textures`, with `hd_keys.tsv` in the columns the setup tool's upscaler reads (key, width, height, raster
format, name, file, kind, source). The 15,695 textures are 4,567 distinct pictures. A picture whose alphas are all 0
(a palette that keeps no alpha) is written opaque, so the upscaler sees its colors.

These keys assume the game hands Direct3D the dictionary's palette and pixel bytes unchanged. That will be checked
against the keys the port logs once its texture replacement is in.
