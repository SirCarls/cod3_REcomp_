# Call of Duty 3's library code (Xbox 360)

This file maps which parts of the campaign program (`default.xex`, the supported USA build) are statically linked libraries and which are the game's own code. For each part it gives the range, what it is, and the facts that matter for the port and for the decompilation.

It was read from the program itself: the PE sections, strings, imports, the static-library list in the XEX header, the call graph and the code's own tables. It was then compared with the multiplayer program. Started 2026-10-16 (owner: "Keep going"), after [COD3_D3D.md](COD3_D3D.md) and [COD3_AUDIO.md](COD3_AUDIO.md).

Function names are candidates with evidence, in the private `dev/x360-family/libs/` (and `d3d/`, `audio/` for Direct3D, FMOD and NSL). Nothing here edits `symbols.toml` or `names.toml`.

## The short version

| Kind | Bytes of code | Share | Functions (Ghidra's count) |
|---|---|---|---|
| The game's own code | 2.20 MB | 42% | ~6,850 |
| Treyarch's shared engine libraries (ngl, NSL, aps, nal, tl/jq) | 0.60 MB | 11% | ~1,770 |
| Microsoft's Xbox 360 SDK libraries | 1.91 MB | 37% | ~4,000 |
| Third-party libraries (FMOD, SpeedTree, dlmalloc; zlib/libpng/libjpeg are counted under D3DX) | 0.50 MB | 10% | ~1,640 |

The total is 5.21 MB: `.text` plus the seven `.embsec_` sections.

So well over half of the program is library code. Most of that is code a host port can replace rather than translate or decompile:

- Direct3D;
- XAudio and FMOD;
- the movie decoders;
- the C runtime;
- the image libraries.

The game's own code sits at both ends of `.text`, with the libraries between (0x820E8EB8-0x823C0B98). The exception is the game's physics and platform code (0x8217D6E8-0x82195A90) in the middle. Part of the video decoder lives outside `.text`, in seven extra executable sections.

## The map (campaign, in address order)

| Range | What | Kind |
|---|---|---|
| 0x820A0000-0x820E8EB8 | Game code: material classes (`cd*`), memory pools, asset tables, script registration | game |
| 0x820E8EB8-0x82114530 | **aps**: Treyarch's particle library (effects, groups, domains, actions, 10 renderers) | Treyarch |
| 0x82114530-0x8211D440 | **NSL**: Treyarch's sound library over FMOD ([COD3_AUDIO.md](COD3_AUDIO.md)) | Treyarch |
| 0x8211D440-0x82120430 | dlmalloc 2.8 `mspace` (FMOD's heap; called only from NSL) | third-party |
| 0x82120430-0x8212A000 | **tl / jq**: Treyarch's tool library (files, memory, fatal errors, LZO1X decompression jobs, asset banks) and the job queue ("JQ Worker Thread %d") | Treyarch |
| 0x8212A000-0x82160000 | **ngl**: Treyarch's renderer (scene lists, textures, meshes, materials, EDRAM, DXSAS FX scripts, render job and back end); mapped in claude-setup's `dev/agents/rust-renderer/NOTES-game-side.md` | Treyarch |
| 0x82160000-0x8217D6E8 | **nal**: Treyarch's animation library (anims, skeletons, scene anims) | Treyarch |
| 0x8217D6E8-0x82194998 | Physics (`phys_*`: bodies, GJK collision and constraint-solver jobs on the job queue) | game |
| 0x82194998-0x82195A90 | Platform layer: asynchronous file I/O and storage helpers | game |
| 0x82195A90-0x821965A0 | **XMP** (the music-player client, used to pause the player's own music during movies) and XAPI/XONLINE wrappers (profile settings, network start-up) | Microsoft |
| 0x821965A0-0x821C5040 | **SpeedTree RT** (Interactive Data Visualization; SPT files tagged `__IdvSpt_02_`) | third-party |
| 0x821C5040-0x821CB4F0 | C++ library template code (iostreams, number formatting), SpeedTree's float helpers and SpeedWind glue (0x821CB0B0) | mixed |
| 0x821CB4F0-0x82216280 | **The movie player ("CAL")** with the Windows Media Audio decoder and an ASF parser; almost certainly Microsoft's **XMEDIA** library (see below) | Microsoft |
| 0x82216280-0x822BF4D8 | **Windows Media Video decoder**: VC-1/WMV3, WMV1/2, the MS-MPEG4 variants, WMV9 Image | Microsoft |
| 0x822BF4D8-0x822E5810 | **XAudio** and its vector mixing kernels | Microsoft |
| 0x822E5810-0x823031A8 | **Direct3D 9** 2.0.3424 ([COD3_D3D.md](COD3_D3D.md)) | Microsoft |
| 0x823031A8-0x823433E0 | **D3DX9** texture file code with **libpng 1.0.5**, **libjpeg 6b** and **zlib 1.1.4** (inflate and deflate) | Microsoft + third-party |
| 0x823433E0-0x82349F18 | **XAPI**: memory copy and fill, the Win32-style wrappers over the kernel (files, threads, waits, events), an NT-style heap, the program's entry point (0x82344D00) | Microsoft |
| 0x82349F18-0x8234B250 | **LIBCPMT**: the Dinkumware C++ library (VC8) | Microsoft |
| 0x8234B250-0x8235F988 | **The C runtime** (VC8, shipped inside XAPILIB) and the compiler's register save/restore helpers | Microsoft |
| 0x8235F988-0x82377470 | **XGraphics / D3DX** image conversion and filtering | Microsoft |
| 0x82377470-0x823C0280 | **FMOD Ex 4.04.18** ([COD3_AUDIO.md](COD3_AUDIO.md)) | third-party |
| 0x823C0280-0x823C0B98 | XAPI: time functions, `RaiseException`, `FlushFileBuffers` | Microsoft |
| 0x823C0B98-0x82578E8C | Game code: AI, vegetation, cgame and HUD, the R layer, assets, levels, front end, server, qcommon | game |
| 0x82579000-0x8259B1B8 | Seven `.embsec_` executable sections (126 KB): more of the WMV decoder | Microsoft |

Most ranges are exact to the function. A few are approximate to within a few functions; their confidence is in the private `regions.tsv`:

- the ends of nal;
- the platform layer;
- the mixed range after SpeedTree;
- the XAudio start.

The header's static-library list (all 2.0.3424.0) is XAPILIB, LIBCPMT, XBOXKRNL, XMP, XONLINE, XMEDIA, XAUD, XNET, D3D9, D3DX9 and XGRAPHC. All of them were found except XNET, which shows up only as the `WSAStartup` glue: the campaign has no networking. There is no separate C runtime library in the list: the C runtime ships inside XAPILIB. Copyright strings confirm the Dinkumware C++ library ("Copyright (c) 1992-2004 by P.J. Plauger") and libjpeg ("Copyright (C) 1998, Thomas G. Lane").

## Treyarch's libraries

Treyarch's shared engine libraries make up about a ninth of the code. Their names survive in strings, RTTI and job-module names:

- **ngl** (renderer)
- **NSL** (sound)
- **aps** / **aeps** (particles)
- **nal** (animation)
- **tl** (tools)
- **jq** (job queue)
- **nfl** (file decompression jobs: `nfl_decompress_lzo1x_module`)

They are engine code, not game code. A port of the game's behaviour to the Rust engine replaces them, rather than decompiling them line by line:

- ngl with the renderer;
- NSL with the sound engine;
- aps and nal with the engine's own particles and animation.

The physics (`phys_*`) runs as job-queue modules and is identical in the multiplayer program. Here it counts as game code.

## The C runtime and XAPI (905 functions named)

What matters for the port:

- **Replaceable hot functions.** These are "pure": no globals, no system calls. Call sites from game code, then from all code:
  - `_stricmp` 467 (484), `memset` 175 (738), `XMemCpy` 112 (120), `_fpclass` 101, `memcpy` 98 (682);
  - `strncpy` 79 (94), `atoi` 71, `sprintf` 67 (90, through the shared printf engine), `strstr` 44;
  - `sin` 35 (92), `strchr` 26, `cos` 25 (91), `_snprintf` 25, `_strnicmp` 19, `pow` 17 (78), `tan` 17.

  Most `memset`/`memcpy` calls come from the movie decoders. Some pure functions have no game callers at all: `abs` (213 calls, all from the WMV decoder) and `floor` (aps and WMV). The same goes for `malloc` and `free`, which only SpeedTree, tl and NSL call.

  All of these could get native versions in the port (with big-endian guest memory in mind). The full list with addresses is in the private `crt_names.tsv`. Hot but not pure, from game code: `atexit` 238, `rand` 125 (per-thread seed), `atof` 79.
- **The C runtime has no out-of-line `strlen`, `strcmp` or `memcmp`.** The compiler inlined them everywhere, so game code can't be sped up by replacing such a function. FMOD carries its own `strlen` (0x8238F188), `strcmp` (0x8238F318) and a counted byte compare (0x8238F4E0).
- **Three allocators.**
  - `malloc`/`free` go to XAPI's NT-style process heap; its size comes from the program header, 1 MB by default, and it can grow.
  - The game's own `XMemAlloc` (0x824BF8A0) sends allocator id 0x8C (the movie player) to a bump arena: frees are ignored, and the arena is released as a whole when the movie ends (0x824C75D8). Everything else goes to XAPI's `XMemAllocDefault`. Only the SDK libraries call `XMemAlloc`; game code doesn't.
  - `operator new`/`delete` are the game's own (0x820CC1D0 / 0x820CC468).
- **Per-thread state.** `errno`, the `rand` seed, `strtok`'s position and the C++ exception state live in a kernel TLS slot. XAPI keeps the last error in the kernel thread object.
- **C locale only.** `setlocale` accepts only "C". stdout and stderr go to `OutputDebugString`.
- **Exit path.** After `Sys_Main` returns, the entry point calls `_cexit` (0x82352618), then `XamLoaderTerminateTitle`. `abort` and `raise` end in `KeBugCheck`.
- **Name corrections.**
  - Two existing names are refined: 0x823582B0 `RtlUnwind_wrapper` is the C runtime's `_local_unwind`, and 0x823C0280 `KeQuerySystemTime_thunk` is `GetSystemTimeAsFileTime`.
  - Fourteen cross-game "maybe" notes from this chat's earlier relatives pass are wrong (e.g. 0x823438C8 is `GetCurrentThreadId`, not `__allshl`). They are listed in the private `crt_NOTES.md`.

## The movie player and its decoders

The movie stack is one Microsoft component, almost certainly **XMEDIA** (its XMV player). That is not certain, but the evidence points that way:

- every allocation in it uses `XMemAlloc` allocator id 0x8C (attributes 0x208C800x and 0x248C800x), which nothing else in the program uses. The other SDK libraries use their own ids: Direct3D 0x80, D3DX 0x81, XAPI 0x83, XGraphics 0x87;
- the game's factory call has the shape of `XMediaCreateXmvPlayer`;
- XMEDIA is in the static-library list;
- its class names (`CCalMoviePlayer`, `CCalChannel`) are Microsoft style.

None of it is in the multiplayer program.

- **How the game drives it.**
  - `Game_PlayCinematic` (0x824EED20) goes through `Game_StartMovie` (0x824D0258).
  - `Game_StartMovie` opens and checks the movie through Treyarch's movie layer (0x82125D30-0x82127268), sets up the 0x8C arena, then pauses the player's own music (XMP).
  - The movie layer calls the player factory at 0x821CBA68 (one call, 0x82126BFC) with the D3D device and a parameter block holding two read callbacks.
  - From then on the game only calls five interface slots (+0x08, +0x50, +0x60, +0x70, +0x78; per frame, status, volume, release).
- **How the player works.**
  - It runs its own threads: ASF demultiplexer, WMV decoder plug-in, a video renderer that draws textured quads and presents from its own thread after taking Direct3D thread ownership, and the WMA decoder.
  - Movie sound goes to its own XAudio voice at 44.1 kHz, not through FMOD.
  - Its plug-in slots hold three byte sources (FileSeek, KeepAlive, LocalBuffer), one ASF demultiplexer and ten codec slots, of which only WMV is filled.
- **The WMV decoder** accepts the FOURCCs WVC1, WMVA, WMV3, WMV2, WMV1, WVP2, WMVP, WMVR, MP4S, MP43 and others.
  - Init is 0x82219BD0, per-frame decode 0x8221E180, output 0x8221DF30.
  - Part of its picture decoding is in the `.embsec_` sections (0x82579000-0x8259B1B8, 126 KB). Those sections are reached only from the decoder: direct calls from 0x8221CB20, 0x82260378 and 0x82265550, and function addresses loaded by a few other decoder functions.
  - The SDK's code generator scans every executable section (`src/codegen/phase_scan.cpp`, with `executable` taken from the section flags in `xex_module.cpp`), so the port should translate them too. I inferred this from the SDK's source and haven't checked it against the generated code. A tool that reads only `.text` would miss them.
- **For the port**, the cleanest place for a host decoder (FFmpeg's VC-1/WMV3, WMA and ASF support, for example) is the player itself: the factory and its five slots. That drops:
  - the whole 0x821CB4F0-0x822BF4D8 stack and the `.embsec_` code;
  - the player's Direct3D thread-ownership and Present path;
  - its XAudio voice and the 0x8C arena.

  The XMP pause and resume around movies stays in game code. Replacing only the decoders and keeping the player is the alternative; the private `codec_NOTES.md` lists both sets of hook points.

## The multiplayer program

`codmp_xenonf.xex` links the same SDK version. Its library list has XHV (voice chat) in place of XMEDIA and XNET. It has no movie player, WMA or WMV code and no `.embsec_` sections. It adds Demonware's networking libraries, which are not mapped here.

Measured by identical functions (immediates masked):

- **Identical builds:** physics (159 of 162), nal, Direct3D (500 of 537, see [COD3_D3D.md](COD3_D3D.md)), the C runtime and XGraphics (98-100%).
- **Mostly identical:** aps, NSL and tl/jq (80-92%).
- **A different build:** SpeedTree. Only 21 of 459 functions match, and multiplayer has 31 SpeedTree strings the campaign lacks.

## Names

All of these are in the private `dev/x360-family/libs/`:

- `regions.tsv`: the map above, with evidence and confidence.
- `crt_names.tsv` and `crt_ranges.tsv`: 905 C runtime, C++ library, XAPI, XMP/XONLINE and compiler-helper functions (552 high, 262 medium, 91 low confidence), each with its game call sites and whether it is pure.
- `codec_regions.tsv` and `codec_names.tsv`: the movie player and its decoders, 221 anchor functions.
- `crt_NOTES.md` and `codec_NOTES.md`: the two helper passes' notes.

Together with Direct3D (537), FMOD (1,471) and NSL (150), that is candidates for about 3,280 library functions that the project's names list didn't cover.

Not done:

- naming inside Treyarch's libraries beyond what the renderer notes and the audio work cover;
- XAudio's functions;
- the multiplayer program's Demonware code.
