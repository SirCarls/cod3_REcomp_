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
- XAudio and FMOD (FMOD's reverb kernel is native since 2.12.3, its resampler since 2.12.12, its mix kernels since 2.12.13, its ramped mix since 2.12.14 and its connection mix since 2.12.15, below);
- the movie decoders;
- the C runtime (its busiest functions, nearly all its string and character functions, its math (`exp`, `log`, `pow` and `tan` too), its formatted output and atoi/strtol are native since 2.12.1-2.12.16, below);
- the image libraries.

The game's own code sits at both ends of `.text`, with the libraries between (0x820E8EB8-0x823C0B98). The exception is the game's physics and platform code (0x8217D6E8-0x82195A90) in the middle. Part of the video decoder lives outside `.text`, in seven extra executable sections.

## The map (campaign, in address order)

| Range | What | Kind |
|---|---|---|
| 0x820A0000-0x820E8EB8 | Game code: material classes (`cd*`), memory pools, asset tables, script registration | game |
| 0x820E8EB8-0x82114530 | **aps**: Treyarch's particle library (effects, groups, domains, actions, 10 renderers) | Treyarch |
| 0x82114530-0x8211D440 | **NSL**: Treyarch's sound library over FMOD ([COD3_AUDIO.md](COD3_AUDIO.md)) | Treyarch |
| 0x8211D440-0x82120430 | dlmalloc 2.8 `mspace` (FMOD's heap; called only from NSL) | third-party |
| 0x82120430-0x8212A000 | **tl / jq**: Treyarch's tool library (files, memory, fatal errors, LZO1X decompression jobs (native since 2.12.3, below), asset banks) and the job queue ("JQ Worker Thread %d") | Treyarch |
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

  **Native now (2026-10-20, setup tool 2.12.1; 2.12.2 added the string functions and the multiplayer program):** `memcpy`, `memset`, `memmove`, `XMemCpy`, `_stricmp`, `strncmp`, `strcpy`, `strncpy`, `strchr` and `strstr` run as the PC's own code (`recomp/src/game/crt_native.cpp`, switch `cod3_native_crt`, on by default). Each one runs natively only where that does exactly what the translated code does:
  - no overlap for `memcpy` and `XMemCpy` (the guest code copies forward in blocks, and `XMemCpy` clears destination cache lines first);
  - nothing in the GPU register window (0x7F000000-0x7FFFFFFF) and nothing across 0xE0000000 (on Windows the host view of 0xE0000000 and up sits 4 KB further on);
  - `memmove` keeps its signed compare (`cmpw` picks the direction), so an overlapping move across 0x80000000 is left to the guest code;
  - the length is r5's low 32 bits, and r3 comes back as the guest code leaves it.
  - the string functions read only what the guest code reads (a string that runs past plain guest memory goes to the guest code before anything is written), overlapping `strcpy`/`strncpy` go to the guest code, and their quirks are kept: `_stricmp` returns s1's byte unfolded when s2 ends first, `strncmp` treats n as signed, `strchr` compares c as a 32-bit value.

  At every start a self-check runs both versions on the same scratch guest memory (17,500 random calls: lengths, alignments, overlaps, fill values, strings with their ends in every spot, junk in the registers' high halves) and compares every byte and r3. A function that differs, even once, stays on the game's code. On the owner's PC the check took 55-59 ms and passed for all ten, in both programs. In the first minute of Forest about 2.1 million `memcpy`, 0.45 million `memset` and 0.54 million `XMemCpy` calls (2.4 GB) ran natively, and 5 overlapping copies went to the game's code. The frame rate is the same (the GPU emulation thread sets it); the main thread's CPU is a little lower (benches `claude-setup-crt-ab` and `-ab2`). In multiplayer (`cod3mp`, Verdun) the first minute ran 5.8 million `memcpy`, 0.5 million `_stricmp` and 0.1 million `XMemCpy` calls natively; there `tools/mp_aliases.py` points the names at the multiplayer program's own copies (`mp/config/campaign_map.tsv`: nine are named the same in `names_mp.toml`, `XMemCpy` is 0x82312698), and the self-check runs against those copies.
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

## FMOD's reverb as native code (setup tool 2.12.3)

FMOD Ex's SFX reverb unit (`DSPSfxReverb_read`, 0x82392B50) spends nearly all of its time in one function, 0x823A65A8: a feedback delay network (a 100,000-sample pre-delay, eight delay lines with one-pole filters, mixed by three stages of plane rotations). In function profiles of the FMOD thread (`XThread82387D30`) it was 50-70% of the thread, the largest cost of the game's sound, and the unit's read callback around it (the input split into one plane per channel, the kernel, the planes mixed back into the output with the dry signal) another 6-13%. Both now run as native code (`recomp/src/game/fmod_native.cpp`, switch `cod3_native_fmod`, on by default):

- the same floating-point operations in the same order: the generated code's `fmadds` is a double fma rounded to float, and its single adds, subtracts and multiplies round the same as float operations;
- the same bytes written everywhere: the state, the delay lines, the buffers and the two argument words in the caller's frame;
- only where every address it touches is plain guest memory, the buffers don't overlap the state and the state's values are in the ranges the code keeps them in. Anything else goes to the game's code;
- NaNs: which NaN an operation passes on when two of its operands are NaNs depends on the instruction the compiler picked, so a NaN in the input sends that call, and every later one, to the game's code (NaNs the arithmetic makes itself are the CPU's one default NaN either way).

The first 16 calls of each shape (delay lines, channels) run both ways on the same memory, and every byte either one wrote is compared; after that one call in 2048 is still compared. A difference keeps the game's result and switches the native code off for the session. The read callback is checked the same way, the kernel inside it included. On the owner's PC the checks passed in both programs (8 lines, 6 channels; Forest, Saint-Lo, Verdun). A kernel call took 16.5-17.7 us instead of the game's 81-101 us, and a whole read callback 24 us; the FMOD thread went from about 570 profile samples in 20 s of Forest to 2. Where the CPU has FMA the kernel is built a second time with it (single instructions instead of library calls; both round exactly the same).

The multiplayer program has the same kernel at 0x82375C30: the same 916 instructions apart from branch targets and two constant loads (the input offset and 0.0; its mono mix 0x82375770 reads 1.0 and 0.0 from 0x82001570); its read callback is 0x82362220 (the same 177 instructions). `tools/mp_aliases.py` points the names at them, and `COD3_ADDR` gives the constants' addresses (`mp/config/campaign_map.tsv`).

The XMA decoders behind FMOD's XMA codec are the SDK's (FFmpeg). SDK patch 0066 (setup tool 2.12.3) puts a restarted decoder back as it was just after opening instead of rebuilding its MDCTs and tables at every restart; its first 24 restarts are decoded both ways and compared (`xma_restart_without_reopen`).

## LZO1X as native code (setup tool 2.12.3)

Treyarch's tool library unpacks the level archives' NCH blocks (`.cod`, KAPF; [formats/cod3.md](formats/cod3.md)) with LZO's own `lzo1x_decompress` (0x821258C8; called only by the block job 0x82125BF0 on the job workers): the fast version without bounds checks, compiled to copy one byte at a time. It now runs as native code (`recomp/src/game/lzo_native.cpp`, switch `cod3_native_lzo`, on by default):

- the same stream decoded the same way: an overlapping match copies forwards one byte after another (so it repeats its pattern), and the lengths, offsets, end test, length word and r3 are what the code computes;
- it decodes into a buffer of its own first and only then writes guest memory. A stream that would read past its input (plus 64 bytes), look back before its output, or produce output over its own input or length word goes to the game's code with guest memory untouched (a damaged stream above all, which the code would follow wherever it leads).

The first 32 blocks are decoded both ways and compared (output, length word, r3), then one block in 256; `cod3_native_lzo_check_all` compares every block, for tests, and `cod3_native_lzo_report` (console or `cod3_autoexec`) writes the counters to the log. On the owner's PC every block of the start-up and of the Saint-Lô and Forest loads was the same. The game decompresses less this way than expected: 184 blocks (about 160 MB) for the start-up and those two loads, at 555 MB/s natively against the code's 390-400 MB/s, so about 0.1 s less work per load. A level load is short already ("level saint_lo: loaded in 0.4 s" in the game's log); what follows it before play is the level's intro.

The multiplayer program has the same decoder at 0x82137910 (the same 202 instructions, generated code compared); `tools/mp_aliases.py` points the name at it. A Verdun run decompressed no blocks.

## The C runtime's math as native code (setup tool 2.12.4)

Fifteen more C runtime functions run as native code (`recomp/src/game/crt_math_native.cpp`, switch `cod3_native_math`, on by default): `sin`, `cos`, `atan`, `atan2`, `asin`, `acos` (through its core `_asincos`), `fmod`, `floor`, `ceil`, `_fpclass`, `_sptype`, `_copysign`, `tolower`, `toupper` and `isalpha`. Each one only reads its argument registers and a few of the game's read-only constants and returns one register, so its native version is the translated code's instructions written out in the same order:

- the floating-point ones make the same operations in the same order (the same fused multiply-adds; `fctid` in the current rounding mode and `fctidz` toward zero, as the generated code computes them) and read their constants (the polynomial tables, the limits, the NaN they return) from the game's memory, so they give the same bits;
- no NaN goes through the native arithmetic. A NaN argument, `asin` or `acos` of more than 1 and `atan2` of two infinities go to the game's code, because which NaN an operation passes on when two of its operands are NaNs depends on the instructions the compiler picked (see the FMOD section). `floor`, `ceil`, `fmod` and the class functions handle every argument, NaNs too: they only test, select or shift bits;
- `fmod` is integer long division on the two values' bits, written out step by step with its edges (its NaN constant for an infinite or NaN x or a zero y, x itself for an infinite y, the shifts' behavior for counts of 64 and more);
- `_fpclass` comes back with the high register half its code leaves (`rlwinm` masks that wrap keep high bits), and `isalpha` reads the locale's ctype table only where that is plain guest memory;
- `_asincos` runs natively only with r4 0 or 1 (acos passes 1): its code indexes a table with r4.

At every start, before the first native call, a check runs both versions on 3,000 arguments per function and compares the result register's bits: the edges (zeros of both signs, denormals, infinities, NaNs, the limits and the values a few steps from them, multiples of pi/2, 0.5 and 1 for asin and acos, exact multiples for fmod), then random ones of every size. A function that differs stays on the game's code for the session (logged). The game's first 64 calls of each function run both ways too, then one call in 4096 per thread. `cod3_native_math_check_all` compares every call (tests); `cod3_native_math_test N` (console or `cod3_autoexec`) compares N more random arguments per function, up to 2,000,000, on the next call; `cod3_native_math_report` writes the counts to the log.

On the owner's PC the start-up check took 3-5 ms and passed for all fifteen in both programs. `cod3_native_math_test 2000000` compared about 28 million arguments in Saint-Lô (2.5 s) and `1000000` in Verdun, all the same. In the first minute (start-up, menus and a level) the game made about 255,000 `sin` and 246,000 `cos` calls, 530,000 `tolower` and 490,000 `isalpha`, 7,000 `acos` and 1,600 `atan2`. Speed is not the point: the translated code of these functions is already short and straight-line, so the native versions are about as fast (`sin` 25 against 23 ns a call, `_fpclass` 13 against 6, `fmod` 370 against 280 on random arguments). What it changes is how much of the program is still translated Xbox 360 code.

The multiplayer program has the same functions (the same instructions; only the constants' addresses differ, through `COD3_ADDR`). `sin`, `cos`, `fmod` and `isalpha` have the same names there; `tools/mp_aliases.py` points the other eleven at cod3mp's copies (`mp/config/campaign_map.tsv`). `abs` and `sqrt` are left out: cod3mp has no out-of-line copy of either. `tan`, `pow`, `log` and `exp` stayed translated here: `tan` changes the FPU's control word (`_ctrlfp`) and `pow` looked as if it had `errno` paths. `exp` and `log` turned out not to (neither calls anything); they followed in 2.12.8, below. `pow` turned out not to touch `errno` either; it followed in 2.12.11.

## The C runtime's formatted output as native code (setup tool 2.12.5)

The engine under `sprintf`, `_vsnprintf`, `vsprintf`, `_snprintf`, `printf` and `fprintf` runs as native code (`recomp/src/game/crt_printf_native.cpp`, switch `cod3_native_printf`, on by default). That engine is the C runtime's `_output_l` (0x82353B58, 737 instructions; `sprintf` alone has 90 call sites and `Com_sprintf` 27). The translated code writes every character through a function call. The native version is the same table-driven state machine over the format string:

- each format character's class and the next state come from the game's own table (`__lookuptable`, read from the game's memory as its code reads it);
- the eight states do what the game's code does, quirks included: `*` widths and precisions are read as 32-bit values (a negative width means `-`); an `I` not followed by `64`, `32` or an integer conversion is printed as a character; `ll` means a 64-bit value; width and precision digits wrap at 32 bits; precisions above 512 are cut to 512; a null `%s` prints `(null)`; a zero `%#x` gets no `0x`; `%#o` forces a leading `0`;
- the arguments come from the `va_list` in 8-byte slots (a 32-bit value is the slot's low word);
- the characters go to the string stream that `sprintf` and `_vsnprintf` build on their stack: written at `_ptr`, with `_ptr` and `_cnt` moved on, or only counted when the stream has no buffer.

A call runs natively only where that is provably what the game's code would do. Everything else runs the game's code, decided before anything is written:

- floating-point conversions (`%e`, `%f`, `%g`, `%a` and capitals: the game's own conversion code);
- wide characters and strings;
- `%n`, `%Z` and conversions the game's code has no case for;
- multibyte lead bytes;
- file streams and stdout/stderr;
- output that would not fit `_vsnprintf`'s count (the game's code then sets `errno` and the stream's error flag) or that is longer than 64 KB;
- anything outside plain guest memory, and output that overlaps what the call reads (the format, the argument slots, the strings, the stream, the game's tables) or the stack below the caller.

The native version reads everything first and writes the finished output at the end. A call it gives up on goes to the game's code untouched.

At every start, before the first native call, a check runs the game's code and the native version on 2,500 random formats in two identical blocks of scratch guest memory, then compares every byte and the returned value. The formats cover:

- flags, widths, precisions and sizes, and `*` arguments;
- extreme integers (`INT_MIN`, `-1`, 64-bit limits) and null strings;
- random format soup;
- small, missing and odd buffers (count 0, negative, `INT_MIN`, count-only streams), and output written over the strings it prints.

Before the game's code runs a format, the check works out from the game's own table which argument slots it will read as pointers, and runs only formats where those are real strings or 0. So the check never makes the game's code read a stray pointer.

The start-up check leaves floating-point and wide conversions out. `cod3_native_printf_test N` (console or `cod3_autoexec`, up to 1,000,000) adds them on the next call, and `cod3_native_printf_report` writes the counts and the reasons for the calls left to the game's code. If any format differs, the native version stays off for the session (logged). The game's first 64 native calls run both ways too, then one in 1,024 per thread. `cod3_native_printf_check_all` compares every call (tests).

Results on the owner's PC:

- the start-up check took 7-8 ms and passed in both programs; 2,362 of the 2,500 formats ran natively;
- `cod3_native_printf_test` compared 200,000 random formats in Saint-Lô (0.66 s) and 1,000,000 in Verdun (3.1 s), all the same;
- a call takes about 0.45 us natively against 1.5-1.7 us in the game's code;
- in the first two minutes (start-up, menus and a level) the game made about 5,000-7,000 such calls. All but 50 ran natively (3 floating point, 47 `_vsnprintf` calls whose output didn't fit), and the 70 compared with the game's code were all the same.

As with the math functions, the speed doesn't change the frame rate. What changes is how much of the program is still translated Xbox 360 code.

The multiplayer program has the same function (cod3mp 0x82323758: the same instructions; only its tables' addresses differ, through `COD3_ADDR`). `tools/mp_aliases.py` points the hook at it (`mp/config/campaign_map.tsv`).

## atoi and strtol as native code (setup tool 2.12.6)

The C runtime's string-to-integer conversion runs as native code too (`recomp/src/game/crt_strtol_native.cpp`, switch `cod3_native_strtol`, on by default). The function is `strtoxl` (0x82350BA8, 189 instructions), under `atoi` (74 call sites: dvars, configs, script and menu strings), `strtol` and `strtoul`. The native version walks the string the way the game's code does:

- spaces are skipped by the locale's ctype table (the game's own table, read from its memory), then one `+` or `-`;
- base 0 means 16 after `0x`, 8 after a leading `0`, else 10; base 16 skips a `0x` too;
- digits come from the table's digit bit and letters from its letter bits, each below the base;
- the value is kept in 32 bits, and the result register is what the code leaves (a negative result is the 64-bit negation of the 32-bit value);
- `*endptr` is set to the first byte not used, or to the string itself when there were no digits.

Everything that touches `errno` stays on the game's code, decided before anything is written: a value that doesn't fit (the code sets `ERANGE`), a null string or a base outside 0 and 2-36 (the invalid parameter handler). So do a multibyte locale, a string that runs past plain guest memory, and an `endptr` that overlaps what the call reads or the stack below the caller.

At every start the game's code and the native version run on 4,000 random strings (spaces, signs, prefixes, digits and letters in every base, the 32-bit limits, other bytes, with and without `endptr`, `strtol` and `strtoul`) and the results and `*endptr` are compared. Strings the native version leaves to the game's code are not run, because they would set `errno`. The game's first 64 native calls run both ways too, then one in 1,024 per thread. `cod3_native_strtol_check_all` compares every call; `cod3_native_strtol_test N` and `cod3_native_strtol_report` work as for printf. The multiplayer program's copy is 0x82320008 (the same instructions; only the locale data's address differs).

On the owner's PC the start-up check took 0.8-0.9 ms: 3,629 of the 4,000 strings ran natively and all were the same, in both programs. `cod3_native_strtol_test` compared 180,551 more the same (200,000 asked, Saint-Lo) and 902,594 (1,000,000 asked, Verdun). A call takes about 43 ns instead of the game's 55. In the first minute of a level about 4,500 calls (Saint-Lo) and 5,900 (Verdun) ran natively, none went to the game's code, and the 67-69 calls compared were the same (runs `claude-setup-st3`, `claude-setup-st3-mp`). The game spends little time here, so the gain is less translated code, not speed.

## The rest of the small string and character functions (setup tool 2.12.7)

Ten more of the C runtime's small functions run as native code: `_strnicmp` (0x82353618, 23 call sites), `strrchr` (0x8234E890, 15), `strncat` (0x8234E600, 2), `memchr` (0x8234E650, 4), `strcspn` (0x8234E8E8, 3), `_strlwr` (0x82353528, 4) and `_strupr` (0x82353728, 6) in `recomp/src/game/crt_native.cpp` (switch `cod3_native_crt`), and `isdigit` (0x8234BFF8), `isspace` (0x8234C038) and `isalnum` (0x8234C058) next to `isalpha` in `recomp/src/game/crt_math_native.cpp` (switch `cod3_native_math`). They follow the game's code, quirks included:

- `_strnicmp`, `_strlwr` and `_strupr` check their arguments first: a null pointer or a count above `INT_MAX` sets `errno` and calls the invalid parameter handler, so those calls stay on the game's code. They also make a 96-byte stack frame before reading anything (the return address, `r31` for `_strnicmp`, the back chain); the native versions write the same words, and a string that overlaps that area goes to the game's code;
- `strcspn` builds a 32-byte bitmap below the stack pointer (a bit per byte to stop at, the NUL always set) and leaves it there; the native version writes the same bitmap;
- `strrchr` and `memchr` compare the character as a signed 32-bit value with each zero-extended byte, so a value outside 0-255 is never found; `memchr`'s count and `strncat`'s are `r5`'s low 32 bits, unsigned; a found address keeps `r3`'s high half, as the code's `lbzu` does;
- `strncat` whose source overlaps what it writes goes to the game's code (its byte order is its own);
- `isdigit`, `isspace` and `isalnum` read the same ctype table as `isalpha` (masks 4, 8 and 0x107); only `isalnum`'s `andi.` sets `cr0`.

The start-up checks run them on the same scratch memory as the others (300-500 random calls each, so the whole check stays near its old 55 ms; the stack pointer is put inside the scratch blocks so the frames and the bitmap are compared too, and now and then a string is put where those go). On the owner's PC they passed in both programs; the check found one thing in its own first version: the frame's back chain is the stack pointer itself, which differs between the two scratch blocks, so it is compared relative to each. In the first minute of Verdun about 215,000 `_strnicmp`, 11,000 `strcspn`, 2,000 `_strlwr` and `memchr` calls ran natively, none went to the game's code; in Forest about 41,000 `isdigit` and 82,000 `isspace`. The multiplayer program's copies: `sub_82323220` (its core `sub_823231C0`), `sub_8231DDD0`, `sub_8231DC60`, `sub_8231DCB0`, `sub_8231DE28`, `sub_82323130`, `sub_82323328`, `sub_8231BD48`, `sub_8231BD88` and `sub_8231BDA8`.

Left on the game's code: `abs` (213 call sites, all from the movie decoder; three instructions, nothing to gain), the functions that keep per-thread state through `_getptd` (`rand`, `srand`, `strtok`), the floating-point conversions (`atof`, `strtod`, `sscanf`), the ones that call back into the game (`qsort`, `bsearch`), and `tan`, which changes the floating-point control word (native since 2.12.16, below; `exp` and `log` followed in 2.12.8, `pow` in 2.12.11).

## exp and log as native code (setup tool 2.12.8)

`exp` (0x8234F180; cod3mp 0x823204E8) and `log` (0x8234E980; cod3mp 0x8231DEC0; `log10` at 0x8234EB00 calls it) run as native code in `recomp/src/game/crt_math_native.cpp` (switch `cod3_native_math`). Neither calls anything or touches `errno`: each reads its argument, its own constant table and a few shared constants (1.0, 0.0, 0.5, +inf, the NaN) and returns one register, so the native versions are their instructions written out in the same order, like the 2.12.4 functions:

- `exp` splits x into n times ln 2 plus r (n rounded with `fctid`, r in one fused multiply-subtract), evaluates a rational polynomial in r and adds n+1 to the result's exponent bits directly, with no overflow check there; then two `fsel`s pick +inf above its overflow limit and 0.0 below its underflow limit, so infinities and huge values never depend on the polynomial. A NaN argument goes to the game's code;
- `log` handles its edges first (1.0 gives 0.0; +inf gives itself; +-0 gives -inf; a NaN, -inf or a negative number gives the game's NaN constant with its sign flipped), takes the exponent and a mantissa in [0.5, 1) from the bits (a denormal is scaled up first), splits at sqrt(0.5), evaluates a rational polynomial and adds the exponent times ln 2 in two parts. No NaN goes through its arithmetic, so every argument runs natively;
- both leave a few scratch words below the stack pointer (and `log` a copy of x in its caller's argument area) that nothing reads after they return; as for the other math functions, the native versions only write the result register.

The start-up check compares 3,000 arguments of each: the limits and the values a step either side, multiples of ln 2 (where n changes) for `exp`, the sqrt(0.5) split at many exponents, denormals, the smallest normal and the largest finite for `log`, zeros, infinities and NaNs, then random ones. On the owner's PC the check passed for both in both programs (the whole math check 3.5-3.6 ms; `exp` 2,995 of 3,000 native, the five NaNs on the game's code), and `cod3_native_math_test 1000000` compared another million arguments of each, all the same. `log` is the busier one in the campaign (about 115,000 calls in the first 90 s of Forest, against 7,000 `exp`), `exp` in multiplayer (78,000 in Verdun, against 7,700 `log`); every call ran natively and every compared call was the same. The native versions take 16 ns (`exp`) and 21 ns (`log`) a call against the translated code's 21 and 36.

## tan as native code (setup tool 2.12.16)

`tan` (0x8234BBD8; cod3mp 0x8231B808; 20 call sites in each program) runs as native code in `recomp/src/game/crt_math_native.cpp` (switch `cod3_native_math`). Unlike the other math functions, it changes the floating-point control word: it calls `_ctrlfp` (0x82356780) to set round-to-nearest with every exception masked, does its arithmetic, then calls it again to put the old word back. Its error paths end in `_except1` or `_handle_qnan1`, which set `errno`. In this port the only part of that word the game's code can read or set is the rounding mode (the generated code's `mffs` reads it and `mtfsf` sets it), so:

- the native version runs only when the rounding mode is already round-to-nearest (as the game keeps it). It reads the mode the way the code's `mffs` does, and leaves the state as the code leaves it: the flush mode off, the rounding mode as it was;
- the old word's inexact-exception bit, which decides whether `tan` reports an inexact result through `_except1`, always reads as masked here, so `tan`'s normal path never calls it;
- the arithmetic is Cody and Waite's, in the same order, with the same roundings and the game's own constants (read where its code reads them; the check logs them). n is x times 2/pi rounded to an integer (by adding and subtracting 2^52, its helper 0x82356730). r is x - n pi/2, in two fused steps. Then a rational polynomial in r (r itself below 1.05e-8), and -1/tan(r) for an odd n;
- a NaN or infinite x, |x| above its limit (298,156,826.86: a total loss of precision) and a remainder that is a denormal go to the game's code, because they end in `_except1` or `_handle_qnan1`. +-0 gives x itself;
- as for the other math functions, only the result register is written (not the scratch words below the stack pointer or the copy of x in the caller's argument area).

The start-up check compares 3,000 arguments: zeros, denormals, the small-remainder bound and the limit with their neighbors, multiples of pi/4 and pi/2 a few steps either way, random angles, values of every size, NaNs and infinities. It also compares the rounding and flush state each call of the game's code leaves, with the flush mode switched on before every other call. On the owner's PC it passed in both programs (2,651 of 3,000 arguments native in the campaign; the rest go to the game's code). `cod3_native_math_test 300000` compared another 250,775 arguments in the campaign, all the same. In Forest about 49,000 calls ran natively in the first 100 s, none went to the game's code, and every compared call was the same. The same held for about 28,500 calls in Verdun (multiplayer), where the start-up check gave the same counts and the test compared another 250,700 arguments, all the same; the constants read the same in both programs. A call takes 21 ns, against 89 for the translated code.

## FMOD's connection mix as native code (setup tool 2.12.15)

The kernels and the ramped mix below are called from FMOD's connection mix, 0x823B07C8 (cod3mp 0x8237FE50), once for every sound in every mix block. It does this:

- a ramp in progress (+160) is mixed first with the ramped mix;
- if the connection's volume (+164) is 0, it stops there;
- otherwise it loads the levels into registers: entries [0] and [1] of each of the six level arrays, and entry [k] of the k-th; a null slot gives 0.0. Then it picks a path by output and input count:
  - 2 out, 1 in: frames one at a time (`fmadds`) until the output reaches a 16-byte boundary, then the mono-into-stereo kernel;
  - 2 out, 2 in: if either cross level ([0][1], [1][0]) reaches a threshold constant, the full 2x2 sum per frame (two `fmadds` for each output). Otherwise the same as above, ending in the stereo kernel;
  - 6 out, 1 in: one frame at a time to a boundary, then the mono-into-6 kernel;
  - 6 out, 2 in: if any of the ten left and right levels other than [0][0] and [1][1] reaches the threshold, the full 2x6 sum per frame. Otherwise only left into channel 0 and right into channel 1; the other four channels are left alone;
  - 6 out, 6 in: only at volume 1.0 with the target matrix exactly the identity (0x823AFD30 checks this), one frame at a time to a boundary, then the 6-into-6 kernel with the diagonal levels;
  - anything else: each output plus the sum (from 0.0) of every input times its level.
- It saves and restores f26-f31 with the compiler's helpers, whose `stfd` / `lfd` switch the denormal flush off. So the flush is always off when it returns, even after a kernel that left it on.

The connection mix now runs as native code in the same file and under the same switch. Every path uses the same operations in the same order, with the kernels' own native code inside. A ramp in progress goes to the game's code, whose ramp and kernels are native anyway; that is about 3% of the calls. Everything else is gated the same way as the kernels: at most 6 outputs and 32 inputs, plain aligned memory, the stack pointer 16-byte aligned (for the kernels' gain copies), the kernels' alignment where they run, the output over nothing the call reads, the stack frame over nothing at all, and no NaN or infinity read. Only memory the game's code reads is read.

The self-check now starts every case of all seven functions from a random flush mode, so a function that leaves the flush alone can be told from one that switches it. It runs 400 connection mix cases: random shapes (2/6 outputs with 1/2/6 inputs, and others), levels with and without cross terms, null slots, volume 1 with identity targets, volume 0, and any output alignment. The first build ran 182 such cases from whatever flush state the previous case left and passed. Its first live call then found the restore helper's flush switch (6 into 6, the kernel ending with the flush on), so the native version was switched off, as designed. That was fixed, and the self-check was extended as above.

Results on the owner's PC (runs `claude-setup-conn2`, `-conn2-mp`): the self-check passed for all seven functions in both programs (31 ms; 373 connection mix cases ran). In Forest, 204,372 connection mixes ran natively in about 90 s (1 into 6: 95,573; 2 into 6: 35,857; 6 into 6: 61,182; the rest during ramps), 114 compared, all the same. In Verdun, 93,402 ran, 65 compared, all the same. Only calls with a ramp in progress (6,477 and 638) went to the game's code.

## FMOD's ramped mix as native code (setup tool 2.12.14)

While a sound's volume or pan is changing, FMOD's connection mix doesn't use the kernels below. It uses a ramped matrix mix, 0x823AF9E0 (cod3mp 0x8237F068), which steps every level a little each frame so the change doesn't click:

- `ramp_mix(connection, out, in, outputs, inputs, frames)`: for each frame and each output channel, a sum starts at 0.0 (a float constant) and adds each input times its level (`fmadds`). Each level then gets its step added (`fadds`), and the sum is added to the output (`fadds`). Finally the connection's count of ramp frames left (+160) goes down by the frames mixed. The levels and steps are arrays the connection points to (+52, +76), one per output channel;
- the ramp is set up by 0x823AFB68 (cod3mp 0x8237F1F0) whenever the levels are set. Each step is (target times the connection's volume minus the level now) times a constant (`fmsubs`, then `fmuls`), and the steps' sizes are summed. When the sum is above another constant, the ramp count becomes 64 frames. The function has two paths, one for six levels per output (unrolled, with its own order of additions) and one for any other count.

Both are scalar code with the flush off. They now run as native code in the same file and under the same switch (`cod3_native_fmod_mix`), with the same operations in the same order on the same values. A call runs natively only where that is provably what the game's code does: at most 6 outputs (the connection's slots) and 32 inputs; everything 4-byte aligned plain guest memory; nothing it writes (the output, the levels, the steps, the count) over anything else it reads or writes; and no NaN or infinity in what it reads. Only memory the game's code would read is read. The self-check runs 200 random cases of each at the first call (random connections, level and step arrays, 0 to 300 frames, both setup paths, levels already at their targets), and the first 16 calls in play and then one in 2,048 are compared as for the kernels.

Results on the owner's PC (runs `claude-setup-ramp1`, `-ramp1-mp`): the self-check passed for both in both programs (all six of the file's functions in 22-24 ms). In Forest, 6,409 ramped mixes and 57,867 ramp setups ran natively in the first 90 s, 66 compared, all the same. In Verdun, 596 and 13,527 ran, 42 compared, all the same. None was left to the game's code. The setup runs on the game's main thread (about 650 calls a second in Forest), the mix on FMOD's.

## FMOD's mix kernels as native code (setup tool 2.12.13)

After resampling, FMOD Ex adds every playing sound into the mix buffer with its volume and pan. Its connection mix (0x823B07C8, cod3mp 0x8237FE50) steps the output to a 16-byte boundary one frame at a time, then hands the rest to one of four small kernels, `out += in * gain` on float samples:

- 0x823BD5D8 (cod3mp 0x8238CC60): mono into stereo, the left and right gains in f1 / f2 (doubles);
- 0x823BD6C0 (cod3mp 0x8238CD48): stereo into stereo, f1 / f2;
- 0x823BD7C0 (cod3mp 0x8238CE48): mono into 6 channels, the gains at r6, r7 and r8 (four floats each: the six channels of two frames);
- 0x823BD998 (cod3mp 0x8238D020): 6 channels into 6, the same gains.

Each one copies its gains to the stack as floats, runs groups of 8 frames (4 for the 6-channel ones) in VMX (`vmaddfp`: a multiply, then an add, with the vector unit's denormal flush on), then the remaining frames one at a time with `fmadds` (a double fma rounded to float, the flush off). The single frames read the gains again for each frame: f1 / f2 as doubles, or the first frame's six gains for all of them. One detail of the compiler's: the 6-channel kernel's third channel has its `fmadds` operands the other way round (gain times sample), which only matters for which NaN comes out.

They now run as native code (`recomp/src/game/fmod_mix_native.cpp`, switch `cod3_native_fmod_mix`, on by default): the vector steps with the same SIMD calls, each lane paired with the same gain lane; the single frames as the same `std::fma` on the same doubles; the flush mode switched where the game's code switches it and left as it leaves it (on only when groups ran and no single frames did).

A call runs natively only where that is provably what the game's code does, decided before anything is written: at most 65,536 frames; in, out and the gains 4-byte aligned plain guest memory; with groups, in, out and the stack pointer 16-byte aligned (`lvx` and `stvx` drop the low four address bits, and the gain copies are read back from the stack); the output over none of the input, the gains and the stack the code writes below the stack pointer; no NaN or infinity in what it reads, and f1 / f2 no larger than a float holds. Everything else goes to the game's code. The native version doesn't write the gain copies and saved registers the game's code leaves below the stack pointer; nothing reads them.

The checks: at the first call, 200 random cases of each kernel run both ways on scratch guest memory (frame counts around the group sizes and random ones; samples, gains and gain doubles of every kind but NaN and infinity, denormals, zeros and doubles below a float's range among them), comparing the output, the bytes around it and the flush mode left. In play the first 16 calls of each kernel are compared too, then one call in 2,048. `cod3_native_fmod_mix_report` writes the counts to the log; since 2.12.13 it and `cod3_native_resampler_report` also work from the ~ console and `cod3_autoexec`.

Results on the owner's PC (runs `claude-setup-mix1`, `-mix1-mp`):

- the self-check passed for all four kernels in both programs (14 ms);
- the game mixes to 6 channels, so in play only the two 6-channel kernels ran: in Forest, mono into 6 channels 60,417 calls and 6 into 6 40,894 in the first minute, 81 compared, all the same, none left to the game's code; in Verdun 12,558 and 33,793, 54 compared, all the same;
- about 1.2 ns a frame natively.

## FMOD's resampler as native code (setup tool 2.12.12)

FMOD Ex's software mixer resamples every playing sound through one function, 0x823BA518 (cod3mp 0x82389BA0: the same 3,119 instructions apart from where its constants sit). It interpolates linearly from the sound's buffer into a float buffer. The position is 64-bit fixed point (the source frame in the high word, the fraction in the low word), and a 64-bit speed is added after each frame:

- out = s0 * (1 - f) + s1 * f, with s0 and s1 the frame at the position and the next one, and f the fraction times 2^-32;
- the formats are PCM8, PCM16, PCM24 (little-endian), PCM32 and float, scaled by 1/128, 1/32768, 2^-23, -2^-31 and 1. The PCM32 scale really is negative: such samples come out with their sign flipped, on the console too;
- a speed whose high word is outside two of FMOD's words (0 and 2) is logged, then answered with silence or a zero speed.

The compiler wrote 13 versions of the loop: mono, stereo and any channel count, for each format. They don't all round the same way:

- the scalar ones use one of two fused orders: A, fma(s1, f, s0 * (1 - f)), or B, fma(s0, 1 - f, s1 * f). Each is an `fmadds`, which the generated code computes as a double fma rounded to float;
- the VMX ones multiply and add separately with the vector unit's denormal flush on. These are mono 8 / 16 / float and stereo 16 / float, once the output is 16- or 32-byte aligned;
- which order each channel and frame gets is what the compiler's scheduling left. A small symbolic evaluator over each loop body's instructions printed every output's formula:
  - mono 8 / 16 / float: B up to a 16-byte boundary, then 8 frames at a time in VMX, the rest B;
  - mono 24 / 32: B;
  - stereo 8: in groups of 4 frames, left A and right B, but the 4th frame's right A; the rest A A;
  - stereo 16 / float: B B up to a 32-byte boundary, then 4 frames at a time in VMX, the rest A A;
  - any other channel count (stereo too for 24 and 32): channels in groups of 4, then one at a time. Formats 8, 16 and float use A everywhere; 24 and 32 use A A A B in each group and B for the single channels.

The resampler now runs as native code (`recomp/src/game/fmod_resampler_native.cpp`, switch `cod3_native_resampler`, on by default). It does exactly that:

- each scalar step is the generated code's double operation rounded to float;
- the vector steps use the same SIMD calls, including the SDK's own unsigned-to-float conversion for the fraction;
- the flush mode is switched where the game's code switches it and left as it leaves it.

A call runs natively only where that is provably what the game's code does, decided before anything is written:

- the speed in range, a known format, 1 to 32 channels and 1 to 65,536 frames;
- the output 4-byte aligned (8 for stereo 16 / float, whose vector stores need it) and plain guest memory;
- every source frame the call reads in plain memory, with no 32-bit address wrap;
- nothing it writes (the output, the position) overlapping what it reads (the source, the position, the speed).

A float source with a NaN or an infinity in the frames it reads goes to the game's code.

The checks:

- at the first call, a self-check runs the game's code and the native version on scratch guest memory, 160 random cases per version: positions, speeds, frame counts and output alignments, with random samples (for floats also denormals, zeros and values near the largest). It compares the output, the bytes around it, the position and the flush mode left;
- in play, the first 16 calls of each version are compared too, then one call in 2,048;
- `cod3_native_resampler_report` writes the counts to the log.

Results on the owner's PC:

- the self-check passed for all 13 versions in both programs (11-12 ms);
- in Forest, about 80 s of play ran 248,515 calls natively: mono float 216,065, stereo 16-bit 25,760, stereo float 4,629 and mono 16-bit 2,061. All 185 compared calls were the same, and none was left to the game's code;
- in Verdun, 33,793 mono float calls ran natively, 32 compared, all the same (runs `claude-setup-rs1`, `-rs1-mp`).

About 4 ns a frame natively. The sound thread was barely busy (6% of its 5% was spent here), so this, too, is less translated code rather than speed.

## pow as native code (setup tool 2.12.11)

`pow` (0x8234DD18; cod3mp 0x8231B278, the same 320 instructions apart from its constants' addresses) runs as native code in `recomp/src/game/crt_math_native.cpp` (switch `cod3_native_math`). It has 78 call sites, 17 in the game's own code, and it is busy: about 16,000 calls a second in Forest (1.6 million in 100 s), far fewer in Verdun. It is Cody and Waite's `pow` (their "Software Manual for the Elementary Functions"), and it doesn't touch `errno` after all: an out-of-range result is +-0 or +-inf. The native version is its instructions written out in the same order, its five helpers too (the integer and parity test 0x8234DB28, `frexp` 0x82355B80, the exponent's get and set 0x82355AA8 / 0x82355A80, the infinities 0x8234DB98; written out inside it, not hooked, since `frexp` and the exponent's set have other callers):

- y = +-0 gives 1.0; x = +-0 gives +-0 or +-inf by y's sign and parity (through `_copysign`, native since 2.12.4);
- an infinite x or y gives a constant (its quirk: `pow(+-1, -inf)` is the game's NaN, `pow(+-1, +inf)` is 1);
- a negative x to a y that is not an integer gives the game's NaN; to an odd y the result's sign is flipped;
- |y| above 1e20 gives +-inf, +-0 or +-1;
- a positive integer y up to 128 with an integer x: the mantissa squared and multiplied, the exponents added;
- otherwise log2(x) in sixteenths from the table of 2^(-j/16) (or from `log()` itself when x is within a sixteenth of an octave of 1; that `log` is the native one, checked since 2.12.8), y times it kept exact in pieces rounded to sixteenths with `fctid` in the current rounding mode, 2^w by a polynomial times a table entry, and the exponent put into the bits. Results below 2^-1021 are +-0, not denormals.

Integers are 32-bit as the generated code computes them (the `subf.` record form compares the low word, `cntlzw` twice tests the sign). A NaN x or y goes to the game's code: its last step there is x + y, and which NaN that passes on depends on the instruction the compiler picked. The start-up check compares about 5,700 argument pairs (every pair of 22 x values and 28 y values with both signs: zeros, denormals, 1 and its neighbors, small integers, the 128 and 1e20 limits, 2^63, the infinities; then random pairs: everyday values, x near 1, integer x to integer y up to 140, negative x, results near the overflow and underflow limits, floats, any size, denormal x). On the owner's PC it passed in both programs (5,669 of 5,672 native; the three NaN pairs on the game's code), `cod3_native_math_test 1000000` compared 1,002,668 more pairs the same in each, and every compared call in play was the same (458 in Forest, 77 in Verdun; runs `claude-setup-pow1`, `-pow1-mp`). A call takes 48-54 ns natively against the game's code's 89-108.

## The C heap, the game's pools and SpeedTree's release (setup tool 2.12.10)

The C runtime's heap is the Xbox 360 system library's version of the NT heap: `RtlAllocateHeap` 0x823478F0, `RtlFreeHeap` 0x823481D8, `RtlReAllocateHeap` 0x823484C0, `RtlpCoalesceFreeBlocks` 0x823460D8 (malloc, free, `XMemAlloc` / `XMemFree` and the game's own allocator reach them; the process heap is at 0x40000000, its handle at 0x82BEA9C0). A block starts with a 16-byte header: u16 size and u16 previous size in 16-byte units, u8 segment, u8 flags (0x01 in use, 0x04 fill pattern 0xFEEEFEEE, 0x08 a block of its own in virtual memory, 0x10 last in its segment), u8 unused bytes; a free block's list links are at +8 / +12, the 128 list heads at heap +0x180, the heap's lock (a critical-section pointer) at +0x580, its signature 0xEEFFEEFF at +0x10. The game's own allocator (`Mem_Alloc` 0x82127A20 / `Mem_Free` 0x821276D0, through the hooks it installs at 0x8290E6C8: operator new 0x820CC238 / operator delete 0x820CC468, the free at 0x820CBF58) keeps small-block pools in its .bss from 0x826A2C00; a freed block's first word becomes the pool's free-list link.

`cod3_heap_check` (command line only, `game/heap_check.cpp`, campaign) checks that heap from the outside, under its own lock and without changing it: every block recorded with the guest call stack that asked for it, the block and its neighbors checked at every allocation and free, every block once a second; a damaged header is logged with the block in use before it and, when that memory was freed recently, who had it.

SpeedTree's wrapper (one per tree model) keeps one branch index buffer per level of detail in an array its setup (0x823EFB80, cod3mp 0x824097A0) leaves uncleared for a level of detail without strips. Its release (0x823F04B0, cod3mp 0x8240A0C0) runs twice for every tree at a level's end (the tree manager's shutdown 0x823F0828, then the destructor) and changes none of the fields it reads: the second run releases destroyed buffers, reads the freed arrays and frees them again. `game/speedtree_fix.cpp` clears the slots never written before the first run and the counts and pointers after it, and makes `D3DResource_Release(0)` (0x822F1340, cod3mp 0x822C0060) do nothing: each buffer released once, every array freed once.

## Where the Xbox 360 libraries still spend time (profiles, 2026-10-20)

With setup tool 2.12.8, function profiles (`cod3_profile`, 20-30 s of play) of every busy thread show that little library code is left where the time goes:

- the game's main thread (Forest: 22% of a core; 30% of its samples are the frame limiter's waits; Verdun: 15%) runs the game's own code: no function above 2%, no library function above 1% (the C runtime's busy functions are native);
- the job workers (4% each) spend about 15% of that in Direct3D's draw-time state flush (`DrawIndexedVertices` 0x822F3A28, `DrawVertices` 0x822F3620, the register-run, fetch-constant, float-constant and shader writers 0x822FA360 / 0x822FA5A0 / 0x822FA700 / 0x822FB4B0, the command buffer reserve 0x822F2BB8; see [COD3_D3D.md](COD3_D3D.md)) and 1% in `pow` (native since 2.12.11);
- the FMOD thread (5%): the native reverb about a third of it, the SDK's XMA decoder reopening on format changes about a fifth (restarts no longer reopen, since SDK patch 0066), FMOD's resampler (0x823BA518, 3,119 instructions; native since 2.12.12) 6%; the audio worker had no samples at all;
- the asynchronous loader (`XThread82123DE0`) is idle in play.

A profile of the FMOD thread with 2.12.12 (Forest, 20 s) found it busy 4% of a core in 59 samples: the SDK's XMA decoder reopening on format changes about 40% of that, the native reverb about 17%, and of FMOD's own code its ramped matrix mix (0x823AF9E0, used while a sound's volume or pan changes) 7%.

So the Xbox 360 code that still runs every frame is mostly Direct3D's command writing, which the planned renderer at the game's own level replaces rather than translates.

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
