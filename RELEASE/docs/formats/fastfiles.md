# Telling Call of Duty game files apart

Our own readers open the fastfiles (`.ff`) of every Call of Duty game we have looked at, and the `.cod` archives of Call of Duty 3 and of its predecessor, Call of Duty 2: Big Red One. Each file's header says which game and platform it comes from, so nothing is read as the wrong game.

- **Rust** (the engine and the ripper): `engine/crates/fastfile` (which game, and the zone), `engine/crates/kapf` (`.cod` archives, both games), `engine/crates/ff_t4` (World at War's zones). `rip identify <file | folder> [--open]` prints one line per file and a count per game.
- **Python** (the setup tool): `COD_3_WAW_Setup-tool/c3w/fastfiles.py`, the same table. The setup tool rips only World at War's fastfiles, names any others it finds and leaves them out, and refuses another game's Xbox 360 disc or custom map.

Status words: **checked** means read from the owner's own files (how many is in the last column); **from a source** means taken from a page in `docs/SOURCES.md` and not yet seen in a file.

## Fastfile headers

From Call of Duty 4 on, every fastfile starts with an 8-byte magic and a u32 version. The magic says how the file is packed; the version says which game and platform it is for.

| Magic | Meaning |
|---|---|
| `IWffu100` | unsigned |
| `IWff0100` | signed (RSA): the zone is read through hash chunks |
| `TAff0100` | Treyarch's, from Black Ops II on |
| `S1ff0100` | Sledgehammer's (Advanced Warfare) |

Consoles store the version big-endian and PCs little-endian. The byte order therefore tells a console file from a PC file even where the numbers are equal: version 1 is Call of Duty 4 on a console (`00 00 00 01`) and Modern Warfare 3 on PC (`01 00 00 00`). A version only counts under its game's magic: Black Ops II's 0x93 under `IWffu100` is nothing we know.

| Game | PC | Xbox 360 | PS3 | Wii / Wii U |
|---|---|---|---|---|
| Call of Duty 2 | no fastfiles | 0x68, no magic (see below); checked, 111 files | | |
| Call of Duty 4 | 5; checked, 4 files | 1; checked, 60 files | 1, from a source | 0x1A2, from a source |
| World at War | 387 (0x183); checked, 5 files here (and every file the ripper has read) | 387; checked, 67 files | 387, from a source | 0x19B, from a source |
| Modern Warfare 2 | 276 (0x114); checked, 3 files | 269 (0x10D); checked, 84 files | 0x10D, from a source | |
| Black Ops | 473 (0x1D9); checked, 2 files | 473; checked, 48 files | 473, from a source | 0x1DD, from a source |
| Modern Warfare 3 | 1; checked, 4 files | 0x70, from a source | 0x70, from a source | 0x6B, from a source |
| Black Ops II | 147 (0x93), `TAff0100`; checked, 2 files | 0x92, from a source | 0x92, from a source | 0x94 (Wii U), from a source |
| Ghosts | 565 (0x235); checked, 4 files | 0x22E, from a source | 0x22E, from a source | |
| Advanced Warfare | 0x72E, `S1ff0100`, from a source | 0x72E, from a source | 0x72E, from a source | |
| Black Ops III | 0x1FB (beta), from a source | 0x253, from a source | 0x25E, from a source | |

The COD Engine Research wiki gives Ghosts on PC as 0x135; the owner's PC copy has 0x235 in every file (`iw6x` copy), so we use that.

**Xbox 360 or PS3.** The two consoles share their numbers. A file is taken for the Xbox 360's when its header shows the Xbox 360 packing: a signed file with its authentication header right after the 12 bytes, or an unsigned one with its zlib stream there (or where Modern Warfare 2 puts it). Otherwise the header alone can't tell, and the readers say "Xbox 360 or PS3" (CoD-FF-Tools describes PS3 files as raw deflate blocks behind 2-byte sizes; not seen by us). Opening a Modern Warfare 2 file can tell more than its first 64 bytes, so `rip identify --open` sometimes names the Xbox 360 where the header alone couldn't.

**Call of Duty 2 (Xbox 360)** has fastfiles without a magic: a big-endian u32 0x68, the zone's size, three block sizes, then one zlib stream from byte 20. On all 111 fastfiles of the owner's disc the stream inflates to exactly the size the header gives. The zone itself starts with the asset list (the `XFile` header is the one in front).

## How each game packs its zone

After the header, the zone (an `XFile` header, then the streamed asset list and assets) is packed one of three ways.

1. **One zlib stream**, starting at a fixed place for each game and platform (table below).
2. **Signed:** the `IWffs100` authentication header where the zlib stream would start, then 8 KB chunks counted from there: the authentication chunk (a 32-byte hash, a 256-byte RSA signature, the zone name, one SHA-256 per group), then groups of one hash chunk (a SHA-256 for each of the next 256 chunks) and up to 256 data chunks. The data chunks joined are one zlib stream. The same in every signed file we have: World at War and Call of Duty 4 on the Xbox 360, Modern Warfare 2 on both, Modern Warfare 3 on PC. (World at War's was found first, `docs/formats/waw.md`.)
3. **Encrypted:** `PHEEBs71` and the zone name (Black Ops II on PC at byte 12, Black Ops on the Xbox 360 at byte 16). These are not read.

The `XFile` header has the stream's size, an external size and one size per memory block. How many blocks it has differs by game and platform. The size field plus the header's length is the zone's whole length, which checks this table on every file (`Zone::size_matches` in Rust).

| Game, platform | Where the data starts | `XFile` header | Checked on |
|---|---|---|---|
| Call of Duty 2, Xbox 360 | byte 20 | in front, in the file header: 3 blocks | 111 files (68 inflated) |
| Call of Duty 4, PC | byte 12 | 9 blocks, 44 bytes | 4 files |
| Call of Duty 4, Xbox 360 | byte 12 | 7 blocks, 36 bytes | 60 files (21 inflated, signed and unsigned) |
| World at War, PC and Xbox 360 | byte 12 | 7 blocks, 36 bytes | 5 PC files; 35 of the 67 Xbox 360 files inflated, signed and unsigned |
| Modern Warfare 2, PC | byte 21 (after a 1-byte online update flag and a file time) | 8 blocks, 40 bytes | 3 files (one signed) |
| Modern Warfare 2, Xbox 360 | byte 37 + 12 per entry: after the flag, the file time, a region, an entry count, the entries and two sizes | 6 blocks, 32 bytes | all 84 files (44 inflated, signed and unsigned) |
| Black Ops, PC | byte 12 | 7 blocks, 36 bytes | 2 files |
| Modern Warfare 3, PC | byte 21 | 9 blocks, 44 bytes | 4 files (one signed) |
| Ghosts, PC | byte 44 | 64-bit fields: 7 blocks, 72 bytes | 4 small files |

Anything not in this table (the PS3, the Wii, Modern Warfare 3 and later on consoles) is identified but not opened yet: the reader says so instead of guessing.

## `.cod` archives: Call of Duty 3 and Big Red One

Call of Duty 3 doesn't use fastfiles: its `.cod` archives are `KAPF` files (`docs/formats/cod3.md`). Call of Duty 2: Big Red One (Xbox, 2005), the Treyarch game whose engine Call of Duty 3 grew from, uses the same container in an older version. The version field is a float:

| Game | Version | Header | Chunk table entries | Chunks |
|---|---|---|---|---|
| Call of Duty 3 (Xbox 360) | 2.06 (`0x4003D70A`) | 0x30 bytes, with the builder name (`CODAUTO30` / `32`) | 0x20 bytes | 64 KB aligned, NCH/LZO1X packed or stored |
| Big Red One (Xbox) | 2.02 (`0x400147AE`) | 0x1C bytes, no builder name | 0x1C bytes: offset, data size, slot size, a flag, 12 reserved bytes | 32 KB aligned, all stored as they are |

Checked on all 389 archives of the owner's Big Red One disc: every one parses, and every chunk lies inside its file. Its level layout is Call of Duty 3's: `sp/<level>/<level>.cod`, `chars.cod`, `weapons.cod`, vehicles and streamed zones, `sp/global.cod`, `sp/anim.cod`, `mp/global.cod` (and `codmp_xboxf.xbe`, where Call of Duty 3 has `codmp_xenonf.xex`). The sections have Call of Duty 3's names ("", `ANIM`, `AUDIO`, `STRING`). The chunks hold `SBII` banks where Call of Duty 3 has APKF (not decoded yet), DDS textures, `XBXM` data, `WAVE` sound and `TRTS` string tables. `rip cod3-chunks` writes a Big Red One archive's chunks out whole.

## Tools for checking more files

These are in `dev/agents/fastfiles/` (private: they read the owner's disc images):

- `iso_headers.py`: the header of every fastfile on Xbox 360 disc images, with inflate checks for the small ones;
- `iso_pick.py`: copies a few small files off a disc image as text, for the samples in `dev/decomp-input/fastfile_samples/`;
- `cargo_test.py`: builds and tests the readers on the owner's PC in REA's `rea-pcdecomp` sandbox, and runs `rip identify` there.
