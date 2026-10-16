# The Rust decomp: Call of Duty 3's code, function by function

The project's long-term goal is a native Rust remake (see [REQUIREMENTS.md](REQUIREMENTS.md), section 1). The static recompilation plays the game today, but its code is machine output: C++ that does what each PowerPC instruction did. The Rust decomp turns that into readable Rust, one function at a time, and **proves each function right**: a checker runs the game's own machine code and the Rust port on thousands of inputs and compares every result and every byte written.

What comes out is the crate `engine/crates/cod3_game`: the game's logic in Rust, ready for the engine to use.

## Two layers: the console's behavior, without the console's limits

The remake must not be held back by the Xbox 360 (owner, 2026-10-13: "this rust port shouldn't be restricted by xbox 360"; and earlier: "make sure functions aren't limited by the capabilities of the xbox 360"). So `cod3_game` has two layers:

- **The console layer (`cod3_game::shared`, `math`, `script`): the exact record.** Ports checked against the original machine code, bit for bit, with the console's limits as they are: 1024-byte info strings, 128-byte paths, 27-character name copies, angles snapped to 16 bits, fatal errors where a buffer would overflow. It is the exact record, and it is also what an Xbox 360 or recomp player runs: so the network code and the game rules every player shares (movement, anything predicted on both ends) use this layer, within its limits, or the platforms couldn't play together (owner, 2026-10-13: multiplayer between a custom 360 copy, the recomp and the Rust engine; see [MULTIPLAYER.md](MULTIPLAYER.md)).
- **The engine layer (`cod3_game::engine`): the same results, no 360 limits.** The remake uses this layer for everything that stays on one machine (menus, the view, local files, tools). Each engine function is tested against its console version on every input the console accepts (`cargo test`), and simply keeps working beyond them: `InfoString` grows as needed, `Name` keeps the whole text, paths and compares have no length cap, angles keep full `f32` precision.

What a limit is, and what isn't:

| Lifted (the 360's memory, bandwidth or buffers) | Kept (it's how the game behaves, or it must match data) |
|---|---|
| Fixed buffers and length caps, overflow errors | The name hash (scripts and ripped data refer to names by it) |
| 16-bit angle steps (Quake III's network format; coarser than a high-resolution mouse), on one machine only: the wire and the shared game rules keep them | The level scripts' random sequence (scripted events play out the same) |
| Text copies cut to 27 characters | The deterministic `sin`/`cos` and the order of float operations (identical results on every PC, which Windows/Linux multiplayer needs) |
| Static rotating result buffers | Format rules (an info string key can't contain `\`, `;` or `"`) |

When a new area is ported, the console version comes first (checked against the original), then the engine version (checked against the console version), with a line in its doc comment saying which limit it lifts. Gameplay rules (damage, timing, movement) stay the console's unless the owner decides otherwise.

## How it works

```
 default.xex (your disc) ──decrypt──► PowerPC image ──► oracle (CPU emulator) ──┐
                                                                                ├─► compare: result,
 engine/crates/cod3_game (Rust port) ──► engine/tools/decomp_check ─────────────┘   buffers, errors
                       ▲
       tools/decomp/specs/<area>.py: how to call the function, random inputs
```

- **The oracle** (`tools/decomp/oracle.py`) loads the decrypted executable into the Unicorn CPU emulator (a PowerPC 970FX, the closest relative of the Xbox 360's Xenon, with 32-bit addresses like the console) and calls one function with the arguments a test case gives. It reports the result registers and lets the checker read memory back. When the original calls `Com_Error`, that is a result too: the port must refuse the same input.
- **The port** is a plain Rust function in `cod3_game`. Its doc comment says which original it replaces: ``/// Original: `Info_ValueForKey` at 0x82457570``.
- **The checker** (`python3 tools/decomp/decomp.py check`) runs both on random inputs from the spec (edge cases included: empty strings, high bytes, NaN, ±0, huge numbers) and prints `ok` or the first differences.

## Status

`python3 tools/decomp/decomp.py status` prints the current table. On 2026-10-13:

| Area | Rust module | Functions | Checked |
|---|---|---|---|
| Strings | `shared::strings`; engine: `engine::strings` | `Q_stricmp`, `Q_stricmpn`, `Q_strlwr`, `Q_strcat`, `COM_GetExtension`, `COM_DefaultExtension` | yes |
| Info strings (`\key\value`) | `shared::info`; engine: `engine::info::InfoString` | `Info_ValueForKey`, `Info_RemoveKey(_Big)`, `Info_SetValueForKey(_Big)` | yes |
| Name, dvar and file hashes | `shared::hash`; engine: `engine::names` | `NameHash`, `NameHash_Init`, `generateHashValue`, `FS_HashFileName` | yes |
| Angles | `math::angles`; engine: `engine::math::angles` (full precision) | `AngleNormalize180`, `AngleSubtract`, `AngleDelta`, `AnglesSubtract`; multiplayer: `AngleWrap180` | yes |
| Fast yaw (multiplayer) | `math::yaw` | `vectoyaw`, `vectoyaw_signed`: an arcsine series instead of `atan2` | yes |
| Vectors | `math::vector` (engine: the same) | `VectorNormalize`, `VectorNormalize2`, `VectorNormalizeFast`, `Vec2Normalize`, `Vec2Distance(Sq)`, `Distance` | yes |
| C runtime trig | `math::crt` (engine: the same) | `sin`, `cos` (bit-exact double precision) | yes |
| Level script built-ins | `script::{builtins, random}` (engine: the same) | `abs`, `sin`, `cos`, `sqrt`, `length(squared)`, `vectordot`, `distance(squared)`, `closer`, `vectornormalize`, `randomint(range)`, `randomfloat(range)` | yes |
| Network messages | `net::{msg, adr}` (console layer only: what goes over the network) | `MSG_WriteString`, `MSG_ReadString`, `NET_CompareAdrSigned`, `NET_AdrToString` | yes, in both programs |
| Demonware bit buffer (multiplayer) | `net::bit_buffer` (console layer only) | `bdBitBuffer`: `writeBits`, `readBits`, the typed writers and readers (bool, 8/16/32/64-bit, float), ranged integers and floats, type tags; `bdBitOperations::highBitNumber` (27) | yes, 4,000-10,000 cases each |
| Message fields (multiplayer) | `net::fields` (console layer only) | `MSGBB_Write`/`Read` `Origin`, `OriginInt`, `CoordDelta`, `Angle(s2/3)`, `Vector`, `Dir`, `Velocity`; `DirToByte` (22, with 5 identical copies) | yes, 4,000 cases each |
| Player state messages (multiplayer) | `net::{position, view, vehicle}` (console layer only) | the position update 0x23, the view update 0x24, the vehicle update 0x4C: writer and reader each (6) | yes, 1,000-2,000 cases each¹ |

105 functions, each matching the original on 1,000 random cases or more (4,000 or more but the six message functions). `default.xex` has 19,083 functions; 2,247 have names; `codmp_xenonf.xex` 3,067 named.

¹ The messages are checked with three kinds of stand-ins, on both sides: the game structures they read are built from each case (entity, client, vehicle); game functions they ask that aren't ported yet answer the case's value (`Oracle.replace`: the use code 0x823DDB08, 0x820B12D8, 0x823D9B50, 0x823EA990); and the VMX128 sine and cosine (0x820B3920), which the emulator can't run, is the same simple function on both sides. Paths that need VMX128 (a vehicle with a physics body) or the game's entity code (applying an update to the player's entity) aren't checked yet.

**Next, in order** (multiplayer between the 360, the recomp and the engine comes first; [MULTIPLAYER.md](MULTIPLAYER.md)):

0. **The multiplayer program in the oracle** (done 2026-10-13; [The multiplayer program](#the-multiplayer-program)), and **what crosses the network** (mapped 2026-10-13, [MP_PROTOCOL.md](MP_PROTOCOL.md)): Quake III's netchan stays inside one console; consoles exchange Demonware peer messages whose payloads are `bdBitBuffer`s (ported).
1. **The player messages** (done 2026-10-13: 0x23, 0x24 and 0x4C, writers and readers, `net::{fields, position, view, vehicle}`). Left: **VMX128 in the oracle** (the 360's vector instructions: the sine and cosine 0x820B3920 the velocity reader uses, the vehicle physics path, and the vector math library everywhere), the use code (0x823DDB08), and the handlers around the readers (0x823F9528, 0x823F9440, 0x823F8658: slot lookup, the "driven" bit).
2. **The Demonware layer around them**: `bdPacket` / `bdDataChunk` framing, `bdUnicastConnection`'s handshake and chunks, `bdSession`'s join messages (0x0A-0x0C), then the game's other message types (the host's game state first).
3. **The rest of `q_math`**: `AngleVectors`, `vectoangles`, `AnglesToForward`, `RadiusFromBounds` and the unnamed helpers next to them (0x82526B88 to 0x82531400); movement needs them.
4. **Moving things**: `BG_EvaluateTrajectory`, `BG_EvaluateTrajectoryDelta` (needs the `trajectory_t` layout).
5. **Player movement** (`PM_*`, `bg_pmove`): what every client predicts, so it must be exact; needs `pmove_t`.
6. **The dvar system** (`Cvar_*`): stateful; needs a guest-memory model of `cvar_t`.
7. **The level interface table** (about 1,000 script built-ins): names first, then ports, area by area.

For names and understanding, the network areas can lean on Call of Duty: United Offensive's PC executables (plain x86, laid out like Quake III; the owner has them) the same way as on Quake III's source: names and understanding only, nothing copied or run. The World at War decomp (another chat, [WAW_DECOMP.md](WAW_DECOMP.md)) is a study aid in the same way.

## Speed

The engine layer runs inside the frame loop, so the owner's rule (2026-10-13: "make sure the loops are really fast and don't choke") becomes:

- **No allocation per call** on a hot path; a function that returns new text allocates once, for it.
- **Time in proportion to the input, at most.** Nothing quadratic: building an info string key by key, comparing two names, walking a list. A function that can be handed a big input (anything from the network, a file or a mod) is tested at sizes far past the 360's.
- **Exact and fast are not in conflict.** The console's arithmetic costs nothing extra: plain `f32` operations are the PowerPC's, and the fused multiply-add (`ppc::fmadds`) is the CPU's instruction when the build allows it, or an exact `f64` method of a few nanoseconds, never the C library's (slower, and its result could depend on the C library installed).
- **Watch for denormal numbers** (floats below about 1e-38): x86 CPUs take about 100 times longer on them. A value that shrinks every frame (a velocity under friction, a fading weight) should snap to zero below a small threshold, as Quake III does for velocities, when the console does the same; the console's results are not changed to avoid them.
- **Every engine function has a row** in `engine/crates/cod3_game/examples/bench.rs`, at a usual size and at a size far past the 360's limits; run it before and after a change:

  ```sh
  cargo run --release -p cod3_game --example bench          # in engine/
  ```

**Built for the player's CPU.** `RUSTFLAGS="-C target-cpu=native"` lets the compiler use everything the CPU has (fused multiply-add, AVX2). The results don't change (the checker passes all 105 with both builds; Rust never reorders float arithmetic), and the math gets 2 to 5 times faster. A build made on the player's own PC by the setup tool can use it; a build handed to other PCs can't.

Numbers on 2026-10-13 (a 2-core cloud machine, Xeon at 2.1 GHz; runs vary by about 30%):

| What | Before | After | After, built for the CPU |
|---|---|---|---|
| Info string: look up the first key, 10 pairs | 250 ns | 20 ns | 16 ns |
| Info string: look up the last key, 10 pairs | 340 ns | 165 ns | 143 ns |
| Info string: look up the first key, 10,000 pairs (260 KB) | 196 µs | 36 ns | 57 ns |
| Info string: look up the last key, 10,000 pairs | 219 µs | 147 µs | 189 µs |
| Info string: look up the last key, 100 values of 10,000 characters | 569 µs | 97 µs | 98 µs |
| Info string: build 10,000 pairs one `set` at a time | 965 ms | 2.2 ms | 2.1 ms |
| `name_hash`, 8 / 4,096 characters | 12.8 ns / 5.1 µs | 8.5 ns / 3.7 µs | 8.5 ns / 3.1 µs |
| `equal_ignore_case`, 8 characters | 12.1 ns | 7.2 ns | |
| `compare_ignore_case`, 64 / 4,096 characters | 70.6 ns / 4.4 µs | 12.8 ns / 0.57 µs | / 0.23 µs |
| `angles::normalize_180` | 8.8 ns¹ | 4.5 ns | 4.3 ns |
| `math::normalize` (a 3D vector) | 150.7 ns² | 21.9 ns | 4.1 ns |
| `math::length_squared` (two fused multiply-adds) | | 11.9 ns | 1.7 ns |
| `math::sin` (the console's, deterministic) | 40.5 ns | 42.7 ns | 11.0 ns |
| `f64::sin` (the platform's, for comparison) | 14.1 ns | 10.0 ns | 12.1 ns |
| Script `randomintrange` / `randomfloatrange` | 2.1 ns / | 2.1 ns / 5.0 ns | / 2.3 ns |
| Position update 0x23: write a full one / a delta (console layer, no allocation) | | 350-390 ns / 220 ns | 400 ns / |
| Position update 0x23: read a full one | | about 120 ns | |

¹ The old row also timed the benchmark's own remainder. ² The old row's vector shrank every round until it was denormal: the 150 ns was the CPU's denormal penalty, which is why that rule is above.

The network rows (2026-10-13 evening) are the console layer, which every platform must run the same: a player's full update is about 180 bits written in 25 fields, so a console with 4 players sending to 23 peers at 8 updates a second spends well under a millisecond per second on them. A one-word fast path for the bit writer made no measurable difference and was left out.

What changed: info strings walk their text without building a list, find each `\` eight bytes at a time (`cod3_game::bytes`, plain safe Rust) and, past 1,024 bytes, keep an index of the keys present, so a missing key costs nothing and adding keys grows linearly; name compares and string lengths work eight bytes at a time; the name hash takes four characters per step; the fused multiply-add is exact without the C library; `normalize_180` skips the remainder for angles under a turn. Looking up a key near the end of a huge info string is still a walk through it (about 15 ns per pair): fine for anything a game sends, and big strings from other players are capped at the network boundary anyway.

## Setting up (once)

You need Python 3.11 or newer, Rust (the same toolchain as the rest of `engine/`), Git, CMake and a C compiler. The setup tool's dependency check installs all of them.

1. **Your game files.** The checker reads the executable from your own disc, never from the repository. Copy them into `dev/decomp-input/` in the project folder (`dev/` is never published). From the folder the setup tool built (its `assets/`):

   ```sh
   # Linux (in the project folder)
   mkdir -p dev/decomp-input
   cp assets/default.xex assets/codmp_xenonf.xex dev/decomp-input/
   for d in assets/sp/*/; do n=$(basename "$d"); [ -f "$d$n.dll" ] && cp "$d$n.dll" dev/decomp-input/; done
   ```

   ```bat
   rem Windows (in the project folder)
   mkdir dev\decomp-input
   copy assets\default.xex dev\decomp-input\
   copy assets\codmp_xenonf.xex dev\decomp-input\
   for /d %d in (assets\sp\*) do if exist "%d\%~nxd.dll" copy "%d\%~nxd.dll" dev\decomp-input\
   ```

   Plain copies, not links: the Claude desktop app won't read hard-linked files. Or set `COD3_DECOMP_INPUT` to any folder holding them.
2. **The emulator:** `python3 tools/decomp/unicorn_build.py`. It asks before downloading, then builds Unicorn 2.1.4 (GPL-2.0) into `work/tools/unicorn/` with four fixes (below). About a minute.
3. **Optional, for reading code:** `pip install capstone` (BSD), which the `dis` command uses.
4. **Check:** `python3 tools/decomp/decomp.py setup` decrypts the images into `dev/decomp/` and tests the emulator; then `python3 tools/decomp/decomp.py check` runs every spec.

## The multiplayer program

Multiplayer is its own program, `codmp_xenonf.xex` ([MULTIPLAYER.md](MULTIPLAYER.md)), and its network code is what the Rust engine has to match. The oracle runs it like the campaign:

- `decomp.py setup` decrypts it with the rest (`dev/decomp/codmp.pe`).
- **Its names** (private, made from your copy): `python3 tools/xex_imports.py dev/decomp-input/codmp_xenonf.xex dev/decomp/codmp.pe <rexglue-sdk checkout> > dev/decomp/codmp.imports`, then `python3 tools/name_mp_functions.py dev/decomp/codmp.pe --imports dev/decomp/codmp.imports --q3-src <Quake-III-Arena checkout> -o dev/decomp/symbols_mp.toml`. With `dev/decomp/default.pe` and the campaign's symbol map next to it, the tool also gives every function with exactly the same instructions as a named campaign function (and unique on both sides) the campaign's name, and the globals they address the campaign's global names (6,557 functions match one to one; 323 named that way). Its hand-checked list holds the network functions the protocol mapping named ([MP_PROTOCOL.md](MP_PROTOCOL.md)); 3,067 functions named in all on 2026-10-13 (evening), with the message fields and readers this decomp found. The file is reproduced byte for byte when nothing changed. `image.py` loads that file for `codmp`.
- **In the oracle:** `oracle.for_image("codmp")`. The C runtime's per-thread data has its own addresses there (`CRT_THREAD_STATE["codmp"]`, found by matching its C runtime to the campaign's), and `Com_Error` is at 0x825447E0. Demonware allocates through two function pointers the game fills at start (0x82A97E94, 0x82A97E98); the oracle points them at host-side stand-ins (`PROGRAM_HOOKS`), so `bdBitBuffer` and friends can grow.
- **Who calls what:** `decomp.py calls NAME --image codmp` lists a function's calls and tail jumps, `decomp.py callers NAME --image codmp` every call to it.
- **Specs** say `spec(image="codmp", ...)`; `dis` and `call` take `--image codmp`.
- **`decomp.py check --image codmp`** runs the campaign's specs against the multiplayer program's copies of the same functions (by name). On 2026-10-13, 19 of 19 match: the info strings, string compares, hashes and `sin`/`cos` the network code uses are the ports already checked. Specs that write campaign globals (the script random seed) aren't moved over.

## The same bits on every platform

Players on Windows, Linux, an Xbox 360 or an emulator must compute the same floats wherever the game shares them ([MULTIPLAYER.md](MULTIPLAYER.md) 4.10). The checker proves the ports right on the machine that runs the oracle; two `cargo test` checks prove every other machine computes the same, without game files or the emulator:

- **Golden answers:** `python3 tools/decomp/decomp.py golden` records the original's replies for every spec with floats (100 cases each, a fixed seed; fewer for big ones, `golden_cases`) into `engine/tools/decomp_check/golden/<area>.txt`, and `cargo test -p decomp_check` replays them through the ports. A spec with `golden=True` is recorded too though its floats are inside a structure (the player messages: the whole wire path), and `golden=False` keeps out one whose cases carry the game's own data (the direction table). Re-record after porting or changing a float function (it rewrites whole areas; existing lines stay the same), and commit the files with the port.
- **Hard fused multiply-add cases:** `tools/decomp/fma_cases.py` writes `engine/crates/cod3_game/src/ppc/fma_cases.rs`: cases where rounding twice gives another answer, with exact answers. `ppc::fmadds` and the platform's `f64::mul_add` (used by `math::crt`) must get every one right.

Run `cargo test` on every platform players use: Windows, Linux, and macOS or another ARM machine. (The GitHub workflows can't be written from a cloud chat; adding Windows and macOS runners to their matrix is a job for an agent on the owner's PC.)

## Porting a function

1. **Claim the area** on the board (`dev/agents/BOARD.md`, see [Working in parallel](#working-in-parallel)).
2. **Read the original:** `python3 tools/decomp/decomp.py dis Info_ValueForKey`. Calls, strings, globals and float constants are annotated. To try it on an input: `decomp.py call Q_stricmp s:abc s:ABC`.
3. **Write the port** in `engine/crates/cod3_game/src/<area>.rs`, with its `/// Original:` line. The rules below keep it exact. Then the engine version in `src/engine/` without the console's limits, if it has any (see [Two layers](#two-layers-the-consoles-behavior-without-the-consoles-limits)), with a `cargo test` comparing the two within the console's limits.
4. **Answer the checker:** add the function to `engine/tools/decomp_check/src/<area>.rs` (decode the arguments, call the port, return the result and changed buffers).
5. **Describe the call:** a `spec(...)` in `tools/decomp/specs/<area>.py`: the argument kinds (`Int`, `F32`, `F64`, `CStr`, `Buffer`, `Floats`, `FloatBuffer`, `OutFloats`, `GlobalU32`, `GlobalU8`, `Bytes`, `Msg`; in the network specs `BitBufferState`, `BitBufferRef`, `MaskedBuffer`), the result kind and a generator of random cases. Think about what the game really passes, then add edge cases.
6. **Check:** `python3 tools/decomp/decomp.py check Info_ValueForKey --cases 10000`. Fix until it says `ok`. A different `--seed` tries new inputs.
7. **Name what you found.** A function you identified gets a line in `CURATED_FUNCTIONS` in `tools/name_functions.py`, then regenerate the symbol map:

   ```sh
   python3 tools/xex_imports.py dev/decomp-input/default.xex dev/decomp/default.pe <rexglue-sdk checkout> > dev/decomp/default.imports
   python3 tools/name_functions.py dev/decomp/default.pe --imports dev/decomp/default.imports \
       --q3-src <Quake-III-Arena checkout> -o COD_3_WAW_Setup-tool/recomp/config/symbols.toml
   ```

   Without new names it reproduces the file byte for byte, which is a good check before you add one.
8. **Test and tidy:** `cargo fmt`, `cargo clippy`, `cargo test` in `engine/`; if the function takes or returns floats, `python3 tools/decomp/decomp.py golden` to record its answers for the other platforms; then update this page's table.

### Rules for exact ports

- **Floats follow the PowerPC, not the C source you'd expect.** Single-precision instructions are `f32` operations, in the original's order. A fused multiply-add is `ppc::fmadds` for single precision (`fmadds`) and `f64::mul_add` for double (`fmadd`), never `a * b + c`: the order of a sum of squares matters (`y*y` first, then `x` and `z` fused in, for example). Conversions go through `cod3_game::ppc` (`fctiwz`, `fctid`, `fsel`), which saturate and treat NaN like the hardware.
- **Characters are signed** where the code uses `extsb`: bytes 0x80 and up are negative numbers in hashes and compares (`cstr::signed_at`).
- **The game's `tolower`/`toupper`/`isalpha` only know ASCII letters.**
- **Strings are bytes** (`&[u8]`). A string in a fixed buffer is `&mut [u8]` with the buffer's real size, and the port writes exactly the bytes the original writes, including padding (`strncpy`) and the always-zeroed last byte of `Com_sprintf`.
- **`Com_Error` becomes `Err(GameError)`**, at the same point, with the same level (`Drop` or `Fatal`).
- **Static buffers become borrows:** `Info_ValueForKey` copies into one of two rotating static buffers; the port returns a slice of the input instead.
- **Quirks stay and get a sentence:** CoD3's `Q_stricmp` returns -1 for any difference; `Info_SetValueForKey` has no length check; angles snap to 1/65536 of a turn. Document what differs from Quake III.
- **Undefined behavior isn't copied:** where the original reads outside its buffer (an empty name in `COM_DefaultExtension`), the port does something sane and says so, and the spec doesn't test it.
- **Quake III's source is for names and understanding, never for code.** It's GPL; this crate is MIT. Write every port from the game's machine code.

## The oracle's emulator

Unicorn 2.1.4 (QEMU 5's PowerPC code) as released runs Xbox 360 code wrong in four ways. `unicorn_build.py` fixes them; each fix is a few lines and explained in the script:

1. It can't select a 64-bit CPU model, so floating point fails. (Model number not offset.)
2. Float-to-integer conversions (`fctiwz` ...) give 0x80000000 for every out-of-range value; the architecture saturates (+3e9 → 0x7FFFFFFF). The game's angle code depends on it.
3. `or rX,rX,rX` halts the CPU. The Xbox 360 compiler marks `__try` blocks with `mr r8,r8`.
4. Single-precision fused multiply-adds are rounded twice (to double, then to single). The architecture rounds once; off by one in the last bit about once in 3,000.

What the oracle can't run, and how specs live with it:

- **VMX128** (the 360's extended vector instructions): the call stops with "illegal instruction". Such functions need a hand-written reference, or a check in the recomp. A function that only *calls* one can be checked with that call replaced (next point).
- **A call it shouldn't make**: `oracle.replace(address, stand_in)` (in a spec's `setup`) makes every call to that game function run a Python stand-in until the next case; the Rust side's check uses the same stand-in. For a function not ported yet that the one under test asks (the use code, "is the player in the vehicle"), or one the emulator can't run. Writes into a program's code also work: the oracle drops Unicorn's translations of that code then and when the case ends.
- **Kernel calls**: the call stops, except the few the C runtime makes for itself (thread-local storage, the heap, critical sections), which have stand-ins. The C runtime's per-thread data is seeded as its startup would leave it.
- **Globals** start as in the executable file (zero for most). A spec writes the ones a function reads (`GlobalU32`, `GlobalU8`, or `setup=`), like the level scripts' random seed. Values the program sets when it starts count too: the multiplayer position's lower bound (-8191 at 0x82C9DD00) is 0 in the file, so the field and message specs set it. A `setup` with three parameters also gets where each argument was placed, to build structures that point at them.
- **Arguments in registers** follow the 360's convention: every argument has a slot in both register files, so an integer after a float goes one general register further (`MSGBB_ReadCoordDelta(buffer, float base, float *out)`: r3, f1, r5); the checker does the same.
- **NaN bits**: a PowerPC makes 0x7FC00000 where an x86 makes 0xFFC00000 for the same invalid operation; any NaN matches any NaN.

## Working in parallel

Several agents (Claude chats, their helpers, coding agents on your PC) can port at the same time if each one owns an **area**: one Rust module (console version and engine version), one dispatch file, one spec file. For example `math/trajectory.rs` + `engine/math/trajectory.rs` + `decomp_check/src/trajectory.rs` + `specs/trajectory.py`.

- **Claim it first** on `dev/agents/BOARD.md` (private), and read the board's rules. Don't edit an area someone else has claimed.
- **Shared files get one-line additions only:** the `mod` lines in `cod3_game/src/lib.rs` or a `mod.rs`, the `mod` and `dispatch` lines in `decomp_check/src/main.rs`, your own block of names at the end of `CURATED_FUNCTIONS`. Merge, don't overwrite, when someone else added theirs.
- **Builds:** one at a time on a shared machine. The checker builds `decomp_check` itself (`--no-build` to skip).
- **Done means checked:** an area is done when `decomp.py check --area <module>` passes with 10,000 cases, `cargo clippy` and `cargo test` are clean, and the table above is updated.

## Files

| Path | What it is |
|---|---|
| `engine/crates/cod3_game/` | The ports (MIT, no dependencies): `src/shared`, `math`, `script` exact (the console layer), `src/engine/` without the 360's limits, `src/bytes.rs` (eight-bytes-at-a-time loops), `examples/bench.rs` (the speed table). |
| `engine/crates/cod3_game/src/net/` | Network messages (`msg_t`), addresses (`netadr_t`), Demonware's bit buffer (`bit_buffer.rs`), the multiplayer message fields (`fields.rs`) and the player state messages (`position.rs`, `view.rs`, `vehicle.rs`): console layer only, since every platform in a match must send the same bytes. |
| `tools/decomp/specs/net_bit_buffer.py`, `engine/tools/decomp_check/src/bit_buffer.rs` | The bit buffer's checks: the buffer travels as its whole state (flags, positions, bytes). |
| `tools/decomp/specs/net_fields.py`, `net_position.py`, `net_view.py`, `net_vehicle.py`; `engine/tools/decomp_check/src/fields.rs`, `position.rs`, `vehicle.rs` | The field and message checks: the buffer as a `bdReference` cell, the read status and the time as globals, the game structures each message reads built per case, stand-ins for what isn't ported. |
| `engine/tools/decomp_check/` | Runs the ports for the checker (line protocol in `tools/decomp/harness.py`). `golden/` and `tests/golden.rs`: the original's recorded answers, replayed by `cargo test` on any platform. |
| `engine/crates/cod3_game/src/ppc/fma_cases.rs`, `tools/decomp/fma_cases.py` | Hard fused multiply-add cases with exact answers, and their generator. |
| `tools/name_mp_functions.py` | The multiplayer program's symbol map (written to `dev/decomp/symbols_mp.toml` for the oracle). |
| `tools/decomp/decomp.py` | The command line: `setup`, `dis`, `call`, `check`, `status`, `asm`. |
| `tools/decomp/oracle.py` | The emulator around the original code. |
| `tools/decomp/harness.py` | Argument kinds, specs, comparing answers. |
| `tools/decomp/specs/*.py` | One file of specs per area. |
| `tools/decomp/gen.py` | Random strings, angles, coordinates. |
| `tools/decomp/image.py`, `xex.py`, `aes.py` | Finding, decrypting and reading the executables (no openssl needed). |
| `tools/decomp/disasm.py` | Annotated disassembly (Capstone). |
| `tools/decomp/unicorn_build.py` | Builds the fixed emulator. |
| `dev/decomp-input/`, `dev/decomp/` | Your game files and the decrypted images: private, never published. |
