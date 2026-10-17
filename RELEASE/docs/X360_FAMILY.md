# Xbox 360 relatives of Call of Duty 3

What the other Xbox 360 discs on the shelf tell us about Call of Duty 3: the other regional releases
of CoD3 itself, and the later games of the family (*Call of Duty 4*, *Modern Warfare 2*, *Black Ops*,
*Call of Duty Classic*). Started 2026-10-15 at the owner's request ("Please work through any other
xbox 360 isos that might be of use. Mw2 cod4?", then "look at the other cod3 versions").

The ground rules are World at War's ([WAW_DECOMP.md](WAW_DECOMP.md)): executables are only read,
never run or patched; names from other projects are facts (name + address) with their source in
[SOURCES.md](SOURCES.md); everything derived from the games stays private in `dev/x360-family/`.

## The regional releases of Call of Duty 3

Four discs, all title ID `415607E1`. The program files were copied off each disc with
`tools/waw_decomp/xiso_get.py` and turned into plain images with `xex2pe.py`; `tools/x360_family/build_diff.py`
and `rebuild_diff.py` compared them with the supported *USA, Europe* build.

| Disc | XEX version, region | `default.xex` SHA-1 | Campaign code | `codmp_xenonf.xex` SHA-1 | Multiplayer code |
|---|---|---|---|---|---|
| USA, Europe (supported) | 1, all regions | `681724c40e250e584e6585dd903e05afa00a8634` | built 2006-10-10 | `907985e2f1bdea255b52226ef39acf627c2d7ffc` | built 2006-10-09 |
| Europe (Fr, Es, It) | 2, PAL | `6f9247bb74c4006632a8556dcd1d41332139ff83` | **the same** (decrypted image identical, 15 level DLLs identical) | `758c31d57488502bdef19b5c016a97adfd81c28a` | a rebuild: see below |
| Germany | 3, PAL | `52447cb4189bc4168e71324255e9e7acf5495eb8` | **the same** (image and level DLLs identical) | `3bf53b8853650b214954358ee54b3ea9b4470053` | a rebuild (`codmp_DE_xenonf`): see below |
| Japan | 5, NTSC-J | `44cbd1e1d4e6d2e6b3ca1fdb525713cca144a888` | **a later build** (2007-01-31) | `d4ee5ba811b43bf89753d6a7f381f0650242d972` | a later build (2007-01-31) |

The XEX files differ even where the code doesn't: each disc's executables carry their own media ID,
version, region mask, ratings and signature (build dates are the link times, UTC).

### Germany and Europe (Fr, Es, It)

- **Campaign:** the decrypted `default.xex` image and all 15 level DLLs are byte for byte the supported
  build's. Everything the recomp generates from them, and every hook address, is the same.
- **Multiplayer:** the same 14,159 functions in the same order. Two menu panels' setup functions
  (USA 0x8239A160 and 0x823A1CB8) gain 22 instructions each that scale a text widget by 0.85 (longer
  German, French, Spanish and Italian strings); the Fr/Es/It build also changes 0x824AE270, the sound
  bank loader (the campaign's 0x82494E90), which in the other builds loads the language slot of the
  region index (0x82BCBE24: 0 US, 1 DE, 2 FR, 3 ES, 4 IT, the same index `mp\dlmaps.txt` is read
  with) and here always slot 2: that disc has only French multiplayer banks. Every other
  function is identical code, moved by 0, +0x58 or +0xB0 (+0xB8 in Fr/Es/It); `.data` keeps its layout
  exactly (every difference there is a moved code or `.rdata` pointer), so all globals keep their
  addresses. The network code is untouched: these copies speak the same protocol as the USA copy.
- **Data:** the speech banks are `.wde` (German) or `.wfr` / `.wes` / `.wit` with `.wul` (French,
  Spanish, Italian; `.wul` holds the sounds without speech: see *Voices from another disc* below)
  instead of `.wbk`; the movies' sound tracks and the legal screens are
  per language; a few archives have another size (the file lists give names and sizes only): Germany `mp/global.cod`, `mp/weapusge.cod`, the Falaise and
  St. Bert character and zone archives and `fuelplnt.cod` (most likely the German content changes);
  Fr/Es/It `mp/global.cod`, `sp/frontend.cod`, `sp/global.cod` (the text). The game knows all six
  speech extensions (a table at 0x820779B0) and picks the legal screen from a language index
  (0x82A2A150, read by FrontEndMenuSystem's slot 11, 0x824EF788). That index comes from the archives'
  region tag (0x82458798): `de` German, `eu` French, Spanish or Italian by the console language,
  otherwise English.

What it means:

- **Setup tool:** a German or Fr/Es/It disc compiles into exactly the same campaign as the supported
  one, but `KNOWN_XEX` (`c3w/pipeline.py`) only lists the USA file's SHA-1, so the tool refuses those
  discs today. Accepting the two SHA-1s above for the campaign is safe for the code; the game then
  picks its speech from the disc's region tag (Fr/Es/It: by the console language the runtime
  reports; not tried yet). Their voices work in the supported build already: see below.
- **cod3mp:** `KNOWN_MP_XEX` refuses the German and Fr/Es/It multiplayer programs, which is right:
  their code addresses above 0x8239A578 differ. `build_diff.py --addresses` carries every address of
  `recomp/mp/config/names_mp.toml` and `codmp.toml` to either build (2,300 of 2,300 real addresses
  found; the private maps are in `dev/x360-family/cod3_regions/`), if support for them is wanted.

### Voices from another disc

Owner 2026-10-16 01:15: "please allow for switching audio, even if it's from a different disc."
Done for the campaign: setup tool 2.7.0 (`c3w/voices.py`, Play page), game code
`recomp/src/game/voice_language.{h,cpp}` and `voice_paths.h`, tests `tests/test_voices.py` and
`tests/test_voice_paths.py`.

**How the game picks its speech** (the same code on every disc):

- Each archive's AUDIO list names its sound banks. The level loader (0x82462790) makes each bank's
  path (the archive's folder, `sound\` when the entry says so, the bank's file name: `sp\blkbrn\blkbrn.wbk`)
  and its slot from the extension: `.wen .wde .wfr .wes .wit` are languages 0-4, anything else
  (`.wbk`, `.wul`) is 5, every language. 0x82478B80 registers it: it opens the file (0x82122590)
  and keeps the handle in that slot of the bank's entry, found by the bank's name hashed, so every
  language of a bank shares one entry (16 entries of six slots).
- 0x82494E90 loads slot 5 and the slot of the game's language (0x82A2A150, set as above; the USA
  disc: always English, which also picks the text).
- A movie's sound track, `movies/<name>-en/-de/-fr/-es/-it.wma`, follows the console language
  itself: 0x824EED20 calls XGetLanguage (0x82343D78) at 0x824EEDCC and opens that language's track,
  falling back to English.

**The discs' banks** (all "NSLB" sound banks):

| Disc | Campaign | Multiplayer |
|---|---|---|
| USA, Europe; Japan | whole English banks: `sp/<level>/<level>.wbk`, the squads' battle chatter `sp/bcen_usa/bri/can/pol.wbk`, `sp/global.wbk` | `mp/**/*.wbk` |
| Germany | whole German banks at the same paths: `.wde` | `.wde` |
| Europe (Fr, Es, It) | split: `sp/<level>/sound/<level>.wul` (music, effects, weapons) and `sound/<level>.wfr / .wes / .wit` (the speech); whole battle chatter `sp/bcen_*.wfr / .wes / .wit`; `sp/sound/global.wul` | French only, whole: `mp/**/*.wfr` |

Checked on the owner's copies (`dev/decomp-input/voices/`, private): `blkbrn.wde` has the same 748
entries and 288 sound names as `blkbrn.wbk`, and `sound/blkbrn.wul` (620 entries: music, effects,
weapons) and `sound/blkbrn.wfr` (128: the speech) together have exactly `blkbrn.wbk`'s names;
`sp/global` is the same size on all three discs (no speech in it). A German bank is a drop-in for an
English one, and a split pair is an English bank cut in two, which the Fr/Es/It game loads anyway
(slot 5 plus its language's slot). Not every line exists in every language: the French battle chatter
of the US squad (`bcen_usa.wfr`) lacks 35 of the English bank's 1,089 lines (a missing line stays
silent: the game finds no sound by that name), and the German and French multiplayer banks are
smaller than the English ones. The bank format and the rest of the sound system:
[COD3_AUDIO.md](COD3_AUDIO.md). The Direct3D library linked into the program, the device's state layout and how the game
drives it: [COD3_D3D.md](COD3_D3D.md). Which parts of the program are libraries (Treyarch's, Microsoft's, third-party) and which are
game code: [COD3_LIBRARIES.md](COD3_LIBRARIES.md).

**What the port does:**

- *Add voices from another disc...* on the setup tool's Play page (or
  `python3 -m c3w.voices <disc.iso> <output folder> [--language fr]`; `--list` shows a disc's voice
  files) recognises the disc by its `default.xex` SHA-1 and copies its voice files (the banks under
  `sp/` and `mp/`, the movies' sound tracks; not the legal screens) into the game's `assets/` next to
  its own. The file names never clash and nothing already there is overwritten.
- The setting `cod3_voice_language` (`disc`, `en`, `de`, `fr`, `es`, `it`; Play page > Game settings
  and F2 > Game > Sound > Voices) makes the hook on 0x82478B80 register, in place of the game's bank,
  the chosen language's whole bank in the same slot, or else its split pair (`.wul` in slot 5, the
  speech in the game's language slot); without either, the game's own bank. A mid-asm hook right
  after the XGetLanguage call (0x824EEDD0) gives the movies the chosen language. The text stays the
  game's. A change applies from the next level load (the game restarts between missions).
- Not done: multiplayer. cod3mp registers its banks at 0x824964E8 and loads them in 0x824AE270 by
  its region index 0x82BCBE24; the same kind of hook on its register function would do it there
  (only English, German and French multiplayer banks exist). Not yet tried in game.

### Japan: a later build

Built 2007-01-31, four months after the others, with its own data (210 of the disc's files have another size,
3.5 GB of them) and a newer system update on the disc.

- **Campaign:** 10,964 functions against 10,896. Lined up by `rebuild_diff.py`: 10,132 the same code,
  626 changed, 138 gone, 206 new (World at War's `match_builds.py`, which pairs by strings and the call
  graph instead, agrees on 99.5% of the 8,967 functions both pair). Of the 626, 509 differ only in structure
  offsets and constants (fields moved: `CG_CalcFov`, `PM_GroundTrace`), 117 in their instructions (some only in their switch
  tables). New: a Unicode string library (UTF-8 / UTF-16 text for Japanese and Korean; `legal-jp` and
  `legal-kr` screens), and with it the client's reliable command strings grow from 128 to 256 bytes
  (`CL_AddReliableCommand`, `CL_ParseCommandString`); the audio bank manager's entries grow from 0x6C to
  0x84 bytes. Gone: the warnings about non-localized objective strings and a "too many recursions"
  bullet message. Of the functions the project names, 106 changed, among them `Com_Frame`,
  `CL_CreateCmd`, `SV_SpawnServer`, `G_InitGame`, `CG_DrawCrosshair`, `CG_CalcFov` and many menu methods,
  so every hook address and layout the port uses would have to be found again for this build.
- **Multiplayer:** 12,565 functions against 14,159: 10,863 the same, 885 changed, 2,411 gone and 817
  new, most of the difference being a newer SpeedTree library (1,554 + 552 functions out, 494 in). A
  newer Demonware session layer too (new session and heartbeat messages: `bdSession::handleUpdatePeerList`,
  "Session has been BD_SESSION_NOT_READY for too long ... calling onForceLeave"; the connection and
  session classes changed, while `bdPacket::serialize`/`deserialize` and `bdDataChunk::serialize` are
  the same code), new map names (LA BOURGADE, PRIERS, FRANCE) and `dlc%i` content paths. Whether a
  Japanese console joins a USA one is open.
- **Data:** extra folders `sp/mace1` and `sp/ad_night` (one speech bank each; no level of that name), and developer configs left on the disc (`config/barnes.cfg`, `miked.cfg`, ...).

What it means: the Japanese disc is a different program for the recomp (its own codegen config and
hooks). Its changes are worth reading when a USA bug is investigated: a fix may already be there.
The changed-function lists with the USA names are in `dev/x360-family/cod3_regions/jp/`.

## The later games: CoD4, Modern Warfare 2, Black Ops, CoD Classic

Each game's two programs were decompiled with World at War's Xbox 360 pipeline (`run_x360.sh`:
`xex2pe.py`, Ghidra with the Xenon language) and named from the public reconstruction closest to it
(names only, by shared strings, `{ "name", handler }` tables and the call graph:
`naming_common.py`'s passes):

| Game | Programs | Functions | Named | Named from |
|---|---|---|---|---|
| Call of Duty 4 | `default.xex`, `default_mp.xex` | 8,890 + 9,062 | 1,465 + 1,275 (and 1,020 + 1,098 globals) | KisakCOD (a reconstruction of CoD4), plus 31 names World at War's Xbox 360 build gives with strong evidence |
| Modern Warfare 2 | `default.xex`, `default_mp.xex` | 14,223 + 14,678 | 1,748 + 1,594 (1,281 + 1,460 globals) | KisakCOD (IW4 grew out of CoD4's engine) |
| Black Ops | `default.xex`, `default_mp.xex` | 17,905 + 19,532 | 2,672 + 2,935 (2,528 + 2,822 globals) | KisakBlack (a reconstruction of the Black Ops multiplayer program) |
| CoD Classic (Xbox Live Arcade) | `default.xex`, `cod_mp.xex` | 6,513 + 6,600 | 327 + 356 (OpenCoD1 reconstructs about 2,800 functions, most of them multiplayer) | OpenCoD1 (a reconstruction of CoD 1.1 multiplayer) |

CoD Classic is an Xbox Live package (STFS), not a disc: `tools/x360_family/stfs_get.py` copies files
out of one (read only), the way `xiso_get.py` does for discs.

### What they give Call of Duty 3

Less than the closer relatives, as expected: CoD3 is Treyarch's branch of the engine, grown from
CoD2's Xbox 360 code, while CoD4 and MW2 are Infinity Ward's next engine and Black Ops grew out of
World at War.

- **Strings:** CoD3 shares about as many distinctive strings (8 characters or more) with each of them
  (600 to 1,070) as with CoD2 and World at War, but most are library and engine-wide text, so few are
  used by exactly one function on both sides: 50 to 90 usable string seeds per game.
- **Pairing by strings and the call graph** (`match_builds.py`): 900 to 1,400 CoD3 campaign functions
  per game, but with so few seeds the pairs are only as good as a second witness. Where CoD3, CoD4
  and World at War are paired with each other, the three pairs agree 93% of the time in the campaign
  and 96% in multiplayer (the same as CoD3-CoD2-World at War). The agreeing triangles carry a CoD4 name
  for 161 CoD3 functions in each program (campaign + multiplayer): 101 + 54 confirm the project's own
  names, 23 + 16 differ from them only in style (CoD3's `Broc_*` builtins against CoD4's `GScr_*`),
  36 + 88 confirm a World at War-based candidate in `dev/waw-decomp/cod3/cod3_relatives.tsv`, and only
  1 + 3 are new.
- **Identical code** (`same_code.py`: the same instructions once addresses are taken out): CoD3 shares
  50 to 400 such functions with each of them (246-399 with CoD2 and CoD4, whose development kits were
  the closest in time; World at War 93-154, MW2 54-66, Black Ops 65-95, CoD Classic 71-84), almost all
  library code (the C runtime; the Xbox 360 Direct3D library, CoD3 campaign 0x822B0000-0x822FFFFF)
  with a few small Quake III helpers (`Com_sprintf`, `Info_RemoveKey`, `generateHashValue`, `vtos`),
  and almost none named on either side. Between two programs of one game the same test pairs
  thousands (CoD3 campaign and multiplayer: 4,573); across games, even engine code that came from the
  same source was compiled differently. Inside one game it is a good check of the names: of the 4,573
  campaign/multiplayer twins of CoD3, 20 carry different names, mostly naming style (`Cmd_Texturesize_f`
  against `Scr_texturesize`) or World at War-pass names in `dev/waw-decomp/cod3/mp/names.tsv` that the
  project's own names already overrule (`SV_DumpServerCommands` for names_mp.toml's `SV_AddServerCommand`);
  one is a wrong name: multiplayer 0x82443628, `MSS_InitFailed` there, is the campaign's `R_Register`
  (0x82426A28) instruction for instruction. World at War and Black Ops share 820 to 970 identical
  functions, which is how close Treyarch's two games are.

So for CoD3 these games are confirmation, not a new source of names. Their own decompiles are
references in their own right: CoD4 and MW2 for the Rust engine (IW4L, its base, is an MW2-era
engine), Black Ops for zombies (the World at War maps it remastered) and Treyarch's later code.

## Tools (`tools/x360_family/`)

| Tool | What it does |
|---|---|
| `build_diff.py` | Two builds of one program with the same functions (a regional rebuild): every function's address in both, the changed ones lined up instruction by instruction, `.data`/`.rdata` moves from the pointers both store, and `--addresses` carried from one build to the other (names TOMLs, address lists). |
| `rebuild_diff.py` | Two builds that are not the same set of functions (a later rebuild): the function lists lined up in link order by masked-code fingerprints; same / changed / gone / new, with the first build's names. |
| `carry_names.py` | A rebuild gets the first build's names through those pairs, including leaf code without a `.pdata` entry (placed from the function before it, instructions checked). |
| `same_code.py` | Functions with identical machine code (branch offsets and addresses built with `lis` taken out) across any number of builds; unique matches only. |
| `family_names.py` | For one build: each function's identical twins elsewhere, their names, how many games agree, and whether the build's own name agrees. |
| `merge_names.py` | Joins two names tables for one build: the first trusted, the second only filling gaps from its stronger passes. |
| `stfs_get.py` | Copies files out of an Xbox 360 STFS package (Xbox Live Arcade games, title updates, DLC), read only. |
| `nslb.py` | Reads Call of Duty 3's NSLB sound banks (list, JSON, extract); see [COD3_AUDIO.md](COD3_AUDIO.md). |
| `cod3_d3d_layout.py` | Where each GPU register lives in CoD3's Direct3D device object and which pending bit covers it; decodes a dump of the device; see [COD3_D3D.md](COD3_D3D.md). |
