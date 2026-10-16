# PC decompile: Call of Duty 2 and United Offensive

Readable C with names for the PC executables of Call of Duty 2 (1.3) and Call of Duty:
United Offensive (1.51), made with Ghidra (headless), for whoever ports a system to Rust.
The same method as World at War's (`tools/waw_decomp/`, `docs/WAW_DECOMP.md`).

The executables are only read: never run, never patched. The output is derived from the
games, so it goes to `dev/` (private): `dev/cod2-decomp/{mp,sp,gfx_mp}/` and
`dev/coduo-decomp/{mp,sp,game_mp}/`.

## Running it

Needs Ghidra 11.4.2 (Java 21), Python 3 with `pefile` and `capstone`, and three reference
checkouts that give names only: CoD2x (its symbol tables for CoD2 1.3 and the Mac 1.3
decompile), Quake III Arena's source, and optionally CoD2rev_Server (script function tables).

```sh
GHIDRA_HOME=<ghidra> COD2X_SRC=<CoD2x> Q3_SRC=<Quake-III-Arena> COD2REV_SRC=<CoD2rev_Server> \
  sh tools/pc_decomp/run_all.sh <cod2 game folder> <uo game folder> <work folder> dev
```

`run_all.sh` runs the images in order (CoD2 MP first: the others are named by matching
against it). `run_pc.sh` does one image; a step whose output exists is skipped.

## Files

| File | What it does |
|---|---|
| `run_all.sh`, `run_pc.sh` | The pipeline: import, analysis, function search, naming, decompile, tables. |
| `ghidra_scripts/PcSetup.java` | Analysis options (Decompiler Parameter ID for register arguments); drops copy-protection sections before analysis. |
| `ghidra_scripts/PcFindFunctions.java` | Finds the functions Ghidra misses: pointers in data, code addresses in operands, call targets, tail-jump targets glued to the caller, code after padding. |
| `ghidra_scripts/PcApplyNames.java` | Applies `names.tsv`. |
| `ghidra_scripts/PcExport.java` | Tables (functions, calls, strings, globals) and the C of every function. |
| `name_pc.py` | The naming passes (below). |
| `finish_pc.py` | The output tables and `stats.txt`. |

Shared with the World at War tools: `naming_common.py` (the name table) and
`kisak_strings.py` (strings and calls per function of a C source tree).

## Naming passes

Strongest first; an address or a name is taken by the first pass that claims it, and every
name keeps its evidence. A later pass that disagrees is noted in the evidence ("also mac: X").

| Pass | Evidence |
|---|---|
| `cod2x` | CoD2x's symbol tables for exactly these builds (CoD2 MP and its renderer DLL): address, length, name. |
| `selfname` | The function's own name in its messages: `"SV_DirectConnect()\n"`, `"G_CallSpawn: NULL classname"`, or `"%s: ..."` with the name as an argument. |
| `cmd` | `Cmd_AddCommand("name", handler)` → `Cmd_<name>_f`. |
| `dvar` / `cvar` | `g = Dvar_Register*("name", ...)` / `Cvar_Get("name", ...)` → `dvar_<name>` / `cvar_<name>`; game modules: `Cvar_Register(&vmCvar, "name", ...)` and `{ &vmCvar, "name", ... }` tables. |
| `q3` | Distinctive strings used by exactly one Quake III Arena function and one function here, both ways. |
| `match` | United Offensive and CoD2 singleplayer: the same code (masked instructions), the same strings, or a call in step from a matched function, against a named function of another build; strings and named callees must agree. |
| `mac` | Like `q3`, against the decompile of the Mac 1.3 build (real symbol names); then its call graph where the callees of both functions agree. |
| `table` | `{ "name", handler }` arrays that match a reference table (script builtins, menu keywords). |

Ghidra's own Function ID names the C runtime; those functions are listed but their C is left
out.
