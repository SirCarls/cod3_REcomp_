# The x86 decomp: Call of Duty 2, United Offensive and Call of Duty (PC)

The same method as the Xbox 360 decomp (`tools/decomp/`, [docs/DECOMP.md](../../../docs/DECOMP.md)),
for the PC games' x86 code: each Rust port in `engine/crates/iw_pc` is checked against the
game's own function, run in a CPU emulator on thousands of inputs, results and every byte
written compared.

The executables are only read. One function at a time runs inside the emulator; the game
itself is never started, patched or changed, and nothing here touches copy protection
(a protected file, such as a SafeDisc `CoDSP.exe`, is simply not supported).

## Setting up

- Python 3.10+, `pip install unicorn==2.1.4 capstone pefile` (stock Unicorn: x86 needs no fixes).
- Rust (the workspace in `engine/`).
- The game files, from your own installs: set `IW_PC_INPUT` to the folder(s) holding them
  (several separated like `PATH`, for games on different drives), or copy them into
  `dev/decomp-input/pc/`. Files are found by name, up to two folders down, in any case.
  Each build is identified by its SHA-1 (`pe.py`): another build of the same file is refused,
  because its addresses differ.

| `--image` | File | Build |
|---|---|---|
| `cod2mp` | `CoD2MP_s.exe` | Call of Duty 2 1.3 multiplayer |
| `uomp` | `CoDUOMP.exe` | United Offensive 1.51 multiplayer |
| `codmp` | `CoDMP.exe` | Call of Duty 1.5 multiplayer |
| `uo_game_mp` | `uo/uo_game_mp_x86.dll` | United Offensive 1.51 server game module |
| `cod2_gfx_mp` | `gfx_d3d_mp_x86_s.dll` | Call of Duty 2 1.3 renderer (read only) |

```sh
python3 tools/decomp/x86/pcdecomp.py setup                 # finds the files, checks the emulator
python3 tools/decomp/x86/pcdecomp.py status                # what is ported, per executable
python3 tools/decomp/x86/pcdecomp.py check --image all     # every port against its originals
python3 tools/decomp/x86/pcdecomp.py check Q_stricmp --image cod2mp --cases 4000
python3 tools/decomp/x86/pcdecomp.py check --impl cpp      # the C++ version (engine/cpp) against the same originals
python3 tools/decomp/x86/pcdecomp.py check --shard 2/4 --no-build   # every 4th function from the 2nd: four of these side by side cover all
python3 tools/decomp/x86/pcdecomp.py dis Com_Frame --image cod2mp
python3 tools/decomp/x86/pcdecomp.py call Q_stricmp s:abc s:ABD --image uomp
python3 tools/decomp/x86/pcdecomp.py find "challengeResponse" --image uomp
```

## Files

| File | What it does |
|---|---|
| `pe.py` | Loads a PE32 image at its preferred base: sections (zero-filled past their file size), imports, exports, our names. |
| `oracle_x86.py` | Unicorn in 32-bit protected mode: stack, test heap, `fs:` (a zeroed thread block), import slots that stop the call, stand-ins for the few imports a shared function reaches, the game's error exits as results, every written page put back after each call. |
| `harness_x86.py` | Specs (arguments, where each goes, what to compare), the line protocol to `engine/tools/pc_check` (the same as `tools/decomp/harness.py`'s), and the comparison. `--impl cpp` talks the same protocol to `pc_check_cpp`, the C++ version's answerer (`engine/cpp`, built with CMake into `engine/cpp/build` or `IW_CPP_BUILD_DIR`). |
| `specs/*.py` | One file per area (`shared_strings`, `shared_info`, `shared_hash`, `shared_filter`; the network layer: `net_msg`, `net_oob`, `net_netchan`, `net_delta`; the client's message readers: `net_client`): the inputs, edge cases included. |
| `names.py`, `names/<image>.tsv` | Our verified names (address, name, evidence) for the functions the ports use. |
| `disasm_x86.py` | Capstone disassembly annotated with names, strings and imports. |
| `pcdecomp.py` | The command line. |

## What the x86 code needs that PowerPC didn't

- **Arguments in registers.** MSVC 7.1 with whole-program optimization passed most arguments
  of the games' internal functions in registers (`eax`, `ecx`, `edx`, `ebx`, `esi`, `edi`) in
  no fixed order. Each spec argument says where it goes (`at=`). The checker also reports a
  function that changes `ebx`, `esi`, `edi` or `ebp` without restoring them.
- **x87 precision.** Call of Duty 2 sets 24-bit precision itself (`Com_Frame` calls
  `_controlfp(_PC_24, _MCW_PC)` every frame); United Offensive and Call of Duty keep the C
  runtime's 53-bit default. Each image's functions run at their game's precision
  (`GAME_FPU`), a spec may ask for another. Unicorn honors the precision bits.
- **Inexact instructions.** QEMU computes `fsin`, `fcos`, `fpatan`, `fyl2x`, `f2xm1`, `fprem`
  and relatives with the host's double-precision library, not like an x87. A call that runs
  one is marked, and the checker never counts it as a bit-exact match.
- **Errors as results.** `Com_Error` and the games' warning printers end the call with their
  level and message; the Rust port must return the same error at the same point.
- **Whole structures of globals.** A function that keeps its state in large globals (the
  client's `cl`, 1.6 MB) gets them as a `Region` argument: written into the game's memory
  before the call, compared whole after it, sent to the answerers sparse-coded (`g:`: the
  size, then the runs of non-zero bytes). The oracle doesn't track writes to a region page
  by page during the call (`untracked`), since the test wrote all of it anyway. The cases
  fill only what the port models, so the rest stays zero on both sides.
- **Where a function goes next.** A function that hands over to another part of the game
  (the client's game state reader applying the system info) ends the call at that function
  (`stops`, a `RetStop` result naming it); the port returns the same name.
- **What the caller provides.** Where a function calls the rest of the game and goes on
  (console variables, the file system, the command buffer), those calls run as stand-ins
  (`stand_ins`, `oracle_x86.StandIn`): each is recorded with its arguments as read (numbers,
  or the strings they point to) and returns a value at once; `effect` writes what the real
  one would leave behind for the caller to read (a cvar's new value). A `CallLog` argument
  compares the record with what the answerer builds from the port's result, which hands
  those things to its caller instead. Values a case needs from the game itself (the info
  string its variables make, a key's hash) come from a call of the original before the
  case's (`GuestUserinfo`, `GuestKeyHash` in `specs/net_client.py`). Imports (the
  shell's `ShellExecuteA` in a download redirect) can be stand-ins too
  (`import_stand_ins`, by `dll!name`); an effect may also rewrite what was recorded (a
  buffer's bytes instead of its pointer: `specs/net_download.py`).
- **Hooks and translated code.** Unicorn keeps code it has translated, and a hook added
  later (a stop or a stand-in of a spec that runs after another) doesn't reach it: adding
  one drops the cache for that address (`ctl_remove_cache`). Writes to unmapped memory mark
  their page before the fault; `reset()` skips such pages.
- **Long differences.** A failed case names the first long token that differs and the byte
  where it does, with the bytes around it.

Names come from our own analysis and published facts (the CoD2x project's address tables for
CoD2 1.3, Quake III's function names); no code is copied from any source. Sources are in
[docs/SOURCES.md](../../../docs/SOURCES.md).
