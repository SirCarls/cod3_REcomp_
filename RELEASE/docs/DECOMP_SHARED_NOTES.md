# What each decomp learned that the others can use

Four decompiles run side by side in this project, by different chats and helpers:

| Decomp | Game and build | Method | Details |
|---|---|---|---|
| CoD3 | Call of Duty 3, Xbox 360 `default.xex` + `codmp_xenonf.xex` (PowerPC) | Rust ports checked against the original in a CPU emulator | [DECOMP.md](DECOMP.md), [MULTIPLAYER.md](MULTIPLAYER.md) |
| World at War | PC 1.7 (`CoDWaW.exe`, `CoDWaWmp.exe`) and Xbox 360 (`default.xex`, `default_mp.xex`); also CoD2's Xbox 360 builds, and CoD3's paired with both | Ghidra C with names; Xbox 360 functions paired with PC ones | [WAW_DECOMP.md](WAW_DECOMP.md) |
| Call of Duty 2, United Offensive (and Call of Duty 1.5) | PC: CoD2 1.3, UO 1.51, CoD 1.5 (x86), all nine executables and game modules; CoD2 Xbox 360 by the World at War chat (`dev/waw-decomp/cod2_360/`) | Both: Ghidra C with names, and Rust ports checked in an emulator (the PC network layer: `iw_pc::net`) | [tools/decomp/x86/README.md](../tools/decomp/x86/README.md), [tools/pc_decomp/README.md](../tools/pc_decomp/README.md), [PC_MULTIPLAYER.md](PC_MULTIPLAYER.md), [NEW_COPY.md](NEW_COPY.md) |
| Xbox 360 relatives | CoD3's other discs (Germany, Europe Fr/Es/It: the same campaign code; Japan: a later build, 2007-01-31) and the Xbox 360 programs of CoD4, MW2, Black Ops and CoD Classic | World at War's Xbox 360 pipeline; regional builds lined up function by function (`tools/x360_family/build_diff.py`, `rebuild_diff.py`); identical-code matching across games (`same_code.py`); names from KisakCOD, KisakBlack, OpenCoD1 | [X360_FAMILY.md](X360_FAMILY.md) |

The games are one family. Call of Duty (2003) and United Offensive are built on Quake III Arena's
engine; Call of Duty 2 is their successor; Call of Duty 3's Xbox 360 engine comes from the same
line (its multiplayer program has the same game layer: snapshots, player commands, Huffman-coded
messages); World at War is built on Call of Duty 4's engine. So a fact found in one game is worth
checking in the others, and this page collects those facts. Each says where it was found; "check"
means it hasn't been confirmed in the other game yet. Add to it when you learn something another
decomp can use; keep each entry short and exact.

## Shared functions: what is the same and what isn't

Ports from one game were run against another game's originals (`pcdecomp.py check --against cod3`,
300 random cases each, 2026-10-13):

| Function | CoD3 (360) vs CoD2 1.3 and UO 1.51 (PC) |
|---|---|
| `FS_HashFileName`, `generateHashValue`, `Info_RemoveKey`, `Q_strlwr`, `Q_strcat` | identical |
| `Q_stricmp` | **different.** CoD3's returns 0/-1 and folds to upper case; CoD2's and UO's return -1/0/1. CoD3's behaves exactly like an unnamed UO function, `sub_0044FAA0`. |
| `Q_stricmpn` | **different:** CoD3 returns the difference of the first unequal characters, the PC games -1/0/1. |
| `Info_ValueForKey`, `Info_RemoveKey_Big`, `Info_SetValueForKey(_Big)` | the same on most inputs, different at the edges (3 to 22% of random cases) |

So never assume a port carries over: check it against each game, as `iw_pc` does (it re-exports a
`cod3_game` port only where the checker shows the PC original behaves the same).

## Emulator (the "oracle")

- **Unicorn's `count=` argument costs a hook per instruction.** The x86 oracle got 14 times faster
  by dropping it and stopping runaway calls with `timeout=` instead. The PowerPC oracle
  (`tools/decomp/oracle.py`) still passes `count=max_instructions`: likely the same gain there.
  Also, `count=N` stops silently after N instructions (Quake III-style `MSG_initHuffman` runs about
  10^8), so a result after a stop that wasn't the return address is not a result. (CoD2/UO)
- **Stock Unicorn runs x86 correctly** for this work: it honors the x87 precision bits; `fs:`
  needs a GDT entry in 32-bit mode. Its x87 `fsin`, `fcos`, `fpatan`, `fyl2x`, `f2xm1`, `fprem`
  and relatives use the host's double-precision library, so the x86 checker never calls a function
  that runs one bit-exact. PowerPC needed four fixes ([DECOMP.md](DECOMP.md)). (CoD2/UO, CoD3)
- **The x86 oracle's float path is verified** (2026-10-14): CoD2's real `MSG_WriteAngle16` run on
  one input at 24-, 53- and 64-bit precision sends 5, 4 and 4 steps, as the model predicts, so the
  two PC games really put different angles on the wire for some inputs. Float arguments on the
  stack, results in `st(0)` and the x87 stack-depth check all work. (CoD2/UO)
- **Unicorn's `mem_write` refuses a slice of a `bytearray`** kept between cases: pass `bytes(...)`.
  It crashed every case of a spec that runs a game function once to set up state. (CoD2/UO)
- **Decision functions can be checked without their whole world:** end the call at the first of a
  set of addresses (the reply it would send, the slot it would take) and compare which one it
  reached and the registers there (`Spec.stops`); record what it would `sendto` instead of
  sending. (CoD2/UO)
- **A game's error exit is a result.** Both oracles end the call at `Com_Error` (and the PC
  checker at the games' warning printers) and the port must return the same error. (CoD3, CoD2/UO)

## Floating point

- **Each platform's float rules are part of the game.** CoD3 (360): single-rounded `fmadds`,
  saturating `fctiwz`, denormal handling ([DECOMP.md](DECOMP.md)). PC: the x87's precision
  setting. **Call of Duty 2 sets 24-bit precision itself every frame** (`Com_Frame` calls
  `_controlfp(_PC_24, _MCW_PC)`, client and dedicated server alike); United Offensive and Call of
  Duty 1.5 keep the C runtime's 53-bit default. Search for the C runtime call
  (`push 0x30000; push 0x20000; call`), not only `fldcw`: an earlier search for `fldcw` alone
  concluded wrongly that neither game sets it. (CoD2/UO) Check: World at War PC (CoD4 engine).
- **World at War PC never sets the precision itself** (no `_controlfp` or `_control87` call; the C
  runtime's 53-bit default stays), **but creates its Direct3D 9 device without
  `D3DCREATE_FPU_PRESERVE`** (behavior flags 0x44: hardware vertex processing, multithreaded;
  KisakCOD's CoD4 passes 0x46, which includes it). Without the flag Direct3D switches the thread
  that creates the device (and threads calling it) to 24-bit, so code on those threads runs at
  24-bit unless something sets it back. (World at War) Check: which World at War threads that
  covers.
- Float-to-integer conversions on the wire (`angle * 65536 / 360` to 16 bits, the "whole number"
  test in deltas) go through the C runtime's `__ftol2` on PC: truncation, and -2147483648 for
  anything out of range. (CoD2/UO) Check the 360's `fctiwz` path in CoD3's `MSG_*`.

## Network messages (Quake III family)

Found in CoD2 1.3 and UO 1.51 (`docs/PC_MULTIPLAYER.md`); **check each in CoD3's
`codmp_xenonf.xex`** when its `MSG_*` functions are ported:

- **Huffman:** Quake III's message frequency table, all 256 entries unchanged, and its tree
  building. To find it in a build, search the data for the table (big-endian ints on the 360) and
  take the one function that reads it (`MSG_initHuffman`). Unlike Quake III, the whole message is
  compressed in one go at the end (`MSG_WriteBitsCompress`), and raw bytes are interleaved with
  bits: a bit write that needs a new byte starts it at the end of the message; byte writes append
  raw. Decompression stops at the end of the last byte, so the padding usually decodes into one
  extra byte, which a bit-exact port must reproduce.
- **Which Huffman table each build has** (searched as 32-bit integers in both byte orders): CoD2 PC
  MP and CoD2 Xbox 360 MP have Quake III's; all four World at War builds (PC and 360, SP and MP)
  have CoD4's (KisakCOD's `msg_hData`, 274054, 68777, ...), not Quake III's; CoD3 has neither, in
  either program, which fits what the CoD3 decomp found: its `msg_t` writes bytes inline with no
  Huffman coding, and the bit packing on the wire is Demonware's bit buffer
  ([MP_PROTOCOL.md](MP_PROTOCOL.md)). So a CoD2 or World at War message coder doesn't carry over
  to CoD3. (World at War, CoD3)
- **Delta coding:** a float field is a 0 bit for 0.0, a whole number in [-4096, 4096) as 5 bits
  plus a raw byte, else raw 32 bits; a field of size -100 is a 16-bit angle; CoD2 adds size -99
  (screen positions). The index of the last changed field + 1 is a raw byte.
- **Server commands are single letters** (`d` config string, `x`/`y`/`z` long config string
  pieces, `w` disconnect reason, `B`/`n` map restart). CoD2 renumbered the message types from
  `svc_nop` = 0; UO keeps Quake III's numbers.
- **Server id is one byte:** +0x10 per map (high nibble never 0), low nibble +1 per map restart.
- **Netchan:** Quake III's, plus CoD2's 32-bit fragment start (UO: 16-bit), and Quake III 1.32's
  XOR scrambling (challenge, sequence, last acknowledged reliable command).
- **Reading past the end:** CoD2's bit reader marks the message broken; UO's keeps reading old
  bytes. Ports keep each game's behavior; don't merge them.
- **Strings that find the network code fast:** `"challengeResponse"`, `"connectResponse"`,
  `"rejected connect from protocol version"`, `"%s:Out of order packet %i at %i"`,
  `"Netchan_Transmit: length = %i"`, `"MSG_WriteString: MAX_STRING_CHARS"`, field-table names
  (`"pos.trTime"`, `"commandTime"`), the `svc_*` name table.
- **Order of a server message on the wire** (`SV_SendMessageToClient`, CoD2 and UO alike): the
  message is written uncompressed with the last client command number as its first long; those 4
  bytes go out raw; the rest is Huffman-coded in one go; only the coded bytes are XOR-scrambled;
  then the netchan header goes in front. The client sends 9 raw bytes (server id, message
  sequence, command sequence) before its coded body. (CoD2/UO) Check: CoD3's in-console netchan.
- **The XOR scrambling is Quake III 1.32's loop without its `%`/>127 substitution:**
  `key ^= c << (i & 1)`, `data[i] ^= key`, the text restarting at its end; 128 reliable commands in
  CoD2, 64 in UO. (CoD2/UO)
- **Fragments:** 1300 bytes; a message of exactly k*1300 bytes ends with an empty fragment;
  reassembly copies up to maxsize + 4 bytes (Quake III's off-by-four). UO doesn't update the
  incoming sequence after a reassembled message (Quake III 1.17's behavior); CoD2 has 1.32's fix.
- **Refusal texts are localization keys** (`EXE_SERVER_IS_DIFFERENT_VER\x15<version>`,
  `EXE_BAD_CHALLENGE`, `EXE_SERVERISFULL`, ...): a server in another engine must send the keys,
  not English. (CoD2/UO)
- **`netadr_t` moved:** UO keeps Quake III's layout (port at +0x12), CoD2 has the port at +8. Read
  the structure from the code before reusing a layout. (CoD2/UO)
- **UO's adaptive Huffman coder copies leftover stack bytes into the message** (the `connect`
  packet's tail); a port writes zeros there and the checker must not compare those bytes. (UO)
- **The player state's field coder differs from the entity one:** in `MSG_WriteDeltaPlayerstate`
  a changed float or integer field has no "is zero" bit (Quake III's player-state rule), only
  angles keep theirs; the entity, client, HUD and objective coder (`MSG_WriteDeltaField`) has it
  for all three. A port that reuses one for the other is wrong on most changed fields. (CoD2/UO)
  Check: CoD3's player state messages.
- **Player commands carry only the direction of movement:** four bits (forward, back, right,
  left; UO two more for up/down), so the server sees 127, -127 or 0, never the stick's value.
  The key is the checksum feed ^ message acknowledge ^ `Com_HashKey` of the acknowledged server
  command (32 characters). (CoD2/UO)
- **The delta readers differ more than the writers:** UO doesn't extend the sign of integer
  fields with a negative bit count (a -1 in an 8-bit field arrives as 255; CoD2 extends it); a
  field count past the table stops CoD2 (message marked overflowed) but is clamped by UO; and
  both take a missing count (-1) as "copy from field -1", the entry in front of the table in the
  executable (outside the structure for some tables). A port must also not trust indexes the
  writer never sends: UO's HUD reader takes a 5-bit field index for a 30-field table and reads
  past it. (CoD2/UO) Check: CoD3's readers for the same three.
- **Random bytes are a poor input for message readers:** they drive the originals into the
  out-of-table reads above. The checks build messages the way the writers do (every field kind
  in every form, counts anywhere in the table, cut short where the reader stops at the end)
  and mutate only within what the originals survive (`tools/decomp/x86/specs/net_delta.py`,
  `Wire`). (CoD2/UO)
- **A second language costs little once the checker speaks a line protocol:** the C++ version
  of the PC decomp (`engine/cpp`, owner 2026-10-15: C++ for developers, Rust stays the full
  version) has its own answerer (`pc_check_cpp`) for the same specs, so every C++ function is
  checked exactly like its Rust twin (`pcdecomp.py check --impl cpp`). Porting from the checked
  Rust (not from the disassembly again) took an evening for 187 functions, and every one matched
  the originals the first time; running the slow specs on new seeds found a bug in a spec
  instead (a setting value too large for a float). A chat that wants C++ for `cod3_game` can do
  the same with `decomp.py`'s protocol. (CoD2/UO)
- **Join and transport differ by platform:** the PC games use Quake III's `getchallenge` /
  `connect`; CoD3 on the 360 joins through Demonware peer to peer (setup chat, 2026-10-13). The
  game layer above it is the shared part.
- **Demonware's packet is the same in CoD3 (2006) and World at War PC (2008)**, so World at War's
  assertion texts explain CoD3's: a 16-bit size of the "encrypted data section", then a 32-bit
  connection tag and the chunks (first byte the type: 2 data, 3 init, 4 init ack, 5 selective ack,
  6/7 heartbeat and ack, 9/10/11 shutdown, ack, complete, 13 cookie echo, 14 cookie ack; SCTP's
  set numbered from 2), then the unencrypted payloads; little-endian on the wire on both
  platforms. A data chunk is type, flags (2 = payload inside, 4 = payload in the unencrypted
  tail), the message's type byte, a 16-bit sequence number and the payload sizes; the message
  type byte is the game's (CoD3's 0x23 position update and the rest). Abort and error chunks are
  "not implemented": the reader stays on them until its 100-chunk limit, so nothing after them is
  read. Details with addresses: `dev/waw-decomp/cod3/mp/demonware_packet.md`. (World at War chat)
- **Demonware names itself:** its log lines start with the method (`bdCore::init`) and its
  assertions carry the compiler's function signature; `tools/waw_decomp/method_names.py` names
  260-275 functions per World at War PC program, about 100 per Xbox 360 program and 104 in CoD3
  MP (85 the project's MP names lacked, `bdPacket::serialize` among them). (World at War chat)

### Call of Duty 3's regional builds (2026-10-16)

The German and the Europe (Fr, Es, It) discs' multiplayer programs have the USA build's network code
unchanged (only two menu panels and one download-map slot differ), so they speak the same protocol.
The Japanese build (2007-01-31) has a newer Demonware session layer: `bdPacket::serialize` /
`deserialize` and `bdDataChunk::serialize` are the same code, the connection and session classes
changed (fields moved, new session checks). Whether it joins a USA console is open. Details:
[X360_FAMILY.md](X360_FAMILY.md). (x360-family)

### Master servers and protocol numbers (checked against the executables, 2026-10-15)

The owner pointed at [cod.pm](https://cod.pm/), a server list for Call of Duty 1 to 4 (PC) with a
community master server. Its numbers, checked against the owner's executables where we have them:

| Build | Protocol | Master server (server list) | Authorize server | Heartbeat | Checked |
|---|---|---|---|---|---|
| CoD 1.1 | 1 | `codmaster.activision.com` 20510 | `codauthorize.activision.com` 20500 | `heartbeat COD-1` | executable (`CoDMP.exe`, SHA-1 `1f4df0c155ad821184dbe1f166111f220fc14dd3`) |
| CoD 1.2, 1.3, 1.4 | 2, 4, 5 | same | same | | cod.pm only |
| CoD 1.5 | 6 | same | same | `heartbeat COD-1` | executable |
| UO 1.41 | 21 | `coduomaster.activision.com` 20610 | `coduoauthorize.activision.com` 20600 | | cod.pm only |
| UO 1.51 | 22 | same | same | `heartbeat COD-1` | executable |
| CoD2 1.0, 1.2 | 115, 117 | `cod2master.activision.com` 20710 | | | cod.pm only |
| CoD2 1.3 | 118 | `cod2master.activision.com` 20710 | `cod2master.activision.com` 20700 | `heartbeat COD-2` | executable |
| CoD4 1.0, 1.3-1.6, 1.7, 1.8 | 1, 2-5, 6, 7 | `cod4master.activision.com` 20810 | | `heartbeat COD-4` (KisakCOD) | cod.pm only |
| World at War 1.7 MP, SP | 101, 62 | see below | | none | executable |

- **cod.pm lists `cod2authorize.activision.com`** for CoD2's authorize server; CoD2 1.3 has no such
  name: it sends `getKeyAuthorize`/`getIpAuthorize` to `cod2master.activision.com` port 20700
  (as `PC_MULTIPLAYER.md` says). Trust the executable.
- **`master.cod.pm`** is a community master on the same four ports that answers the same
  `\xFF\xFF\xFF\xFFgetservers <protocol>` query (protocol 0: every server) and tracks the protocols
  above (CoD4 only 1, 6 and 7). A server is added to it with `set sv_master2 "master.cod.pm"`
  (`sv_master1` is Activision's; CoD1 has up to `sv_master5`). Stock clients ask Activision's master;
  cod.pm's client guide replaces `CoDMP.exe` to make them ask cod.pm's, which this project never
  does. For a Rust server (`PC_MULTIPLAYER.md` section 12): `sv_master2` is the way onto cod.pm's
  list without touching any client. (cod.pm FAQ and server guide)
- **World at War PC keeps CoD4's master settings but not its heartbeat:** the dvars
  `masterServerName` and `authServerName` default to `cod4master.activision.com` and `masterPort`
  to 20810, and the client still reads `getserversResponse`, but neither executable has the
  `heartbeat %s`, `getservers %s` or `flatline` strings CoD1-CoD2 use. Its internet games are found
  through Demonware's matchmaking (`CL_MP_FindSessions`, `bdMatchMaking`); LAN through
  `localservers`. So cod.pm's masters don't see World at War servers, and a Rust World at War server
  reaches a stock client by LAN or by Demonware's protocol, not by a master. (World at War)
- **Text colors** (cod.pm's color guide; the color function is the same in CoD 1.5 `0x405240`,
  UO 1.51 `0x405AA0` and CoD2 1.3 `0x4032E0`): `^0`-`^7` index Quake
  III's eight colors, but `^8` and `^9` are the cvars `g_TeamColor_Allies` and `g_TeamColor_Axis`
  (so they change with the map's factions, as cod.pm says), and any other character after `^`
  gives white, where Quake III's `ColorIndex` would wrap it (`(c - '0') & 7`). CoD 1.1 has no
  team-color cvars; World at War has nine `g_TeamColor_*` cvars. (cod.pm, World at War chat)
- cod.pm's API (`api.cod.pm`, at most one request per second) returns live servers'
  `getinfo`/`getstatus` answers, real examples for a parser test; they carry player names, so keep
  any saved copy private. (cod.pm FAQ)

## Naming

- **Never trust a published name for the calling convention.** MSVC 7.1 builds (CoD2, UO) pass
  most internal arguments in registers in no fixed order; the function a public table calls
  `Q_stricmp` in CoD2 is really `Q_stricmpn` with `n` in `eax`. Read the prologue. (CoD2/UO)
- **Self-naming messages** (`"SV_DirectConnect()\n"`, `"%s: ..."` with the name as an argument),
  console command registrations (`Cmd_AddCommand("name", handler)`), dvar/cvar registrations and
  `{ "name", handler }` tables name thousands of functions in every build. (World at War, CoD2/UO)
- **The same code in two builds** (instructions with addresses masked) carries names between
  singleplayer and multiplayer of one game (World at War: SP 3,459 / MP 3,096 named). Across
  CPUs (CoD2 PC to CoD2 360) only strings and call graphs carry over.
- **Strings, named globals, the call graph and pointer tables pair two builds across CPUs.**
  `tools/waw_decomp/match_builds.py` seeds with unique shared strings, unique named globals
  (`dvar_cg_fov`) and same names, then aligns the call lists of matched pairs, pairs functions
  with the same matched callers and callees, and walks the function-pointer tables around matched
  functions (stopping where the strings next to the pointers differ). Of the pairs it finds again
  with half of the string seeds hidden, 97-99% are right (Xbox 360 to PC; 99.9% SP to MP); on
  script builtins with their names hidden, 99.2-99.8%. World at War 360 SP: 5,484 functions paired
  with PC; CoD3's default.xex: 1,269 functions paired with World at War's 360 builds
  (`dev/waw-decomp/cod3/cod3_waw_pairs.tsv`: 131 name candidates for CoD3's symbol map, and the
  World at War function to read next to each paired CoD3 one). It reads the tables of `WawExport.java`, so it works on any build exported that way.
  Its usual mistake: pairing two functions that each exist in one build only. (World at War)
- **CoD2 Xbox 360 against CoD2 PC** (with the CoD2/UO decomp's PC names): 1,354 functions named
  in SP and 1,305 in MP; the hidden-seed test found 223 of 310 SP pairs again, all right
  (`dev/waw-decomp/cod2_360/`). **CoD3 against both relatives:** where a CoD3 function pairs with a
  CoD2 and a World at War function that are paired with each other, the three pairs agree 93-96%
  of the time; CoD3 MP pairs 8,304 functions with CoD3 SP (98.1% right in the hidden-seed test).
  `dev/waw-decomp/cod3/cod3_relatives.tsv` gives each CoD3 function its addresses in the eight
  other builds and 221 name candidates for the symbol map. (World at War)
- **United Offensive against CoD 1.5 and CoD2 1.3** (asked for by the CoD2/UO decomp): the UO
  multiplayer executable and its game and cgame modules, paired three ways (UO-CoD2, UO-CoD1.5,
  CoD1.5-CoD2), give 684 name candidates (+36% on UO's 1,890 names); where both decomps already
  name a paired function, the names agree on 1,622 of 1,649 (98.4%). UO and CoD 1.5 pair at 100%
  in the hidden-seed test. **A route through a near-identical build is not independent evidence:**
  UO and CoD 1.5 are almost the same code, so a wrong UO-CoD2 pair tends to come back as a wrong
  CoD1.5-CoD2 pair (UO's `GLimp_Shutdown` landed on CoD2's `GScr_LoadConsts` both ways; CoD2 has
  no OpenGL). Pairs between functions that exist in one game only (renderer, platform) are the
  ones to check. (`dev/waw-decomp/coduo/`; World at War chat)
- **Not every `x = F("name", ...)` is a dvar registration.** `Material_RegisterHandle` and a sound
  alias lookup are called the same way and gave about 130 false `dvar_` names per World at War
  build. Real registration functions all reach the same core of the dvar system within a few
  calls; keep only those. (World at War) Check: the CoD2/UO cvar pass.
- **Group a reference source's `{ "name", handler }` entries by the array they are in**, not by
  handler prefix: one array mixes `Scr_` and `GScr_` handlers, and two arrays of one file share
  keywords (menuDef and itemDef). (World at War)
- **A table ends where the loop that reads it stops**: two arrays can sit back to back with no
  name repeated (World at War SP: 20 hud element methods, then 114 player methods); the reader's
  `cmp reg, size * count - 1` gives the split, and the reader's name comes from the reference
  source's function reading the same array. (World at War)
- **Check published addresses against what the code does**, not only that a function starts
  there: T4M's six `*_GetMethod` labels for World at War SP are each on another class's lookup
  (0x4DEEA0, labelled `Player_GetMethod`, loops over the 107 actor methods). (World at War)
- **Xbox 360 register save/restore routines** (`__savegprlr_14` ... `__restfpr_31`: runs of
  `std rN, d(r1)`, `ld rN, d(r1)`, `stfd`/`lfd fN, d(r12)`) have hundreds of callers and look
  alike; name them by their instructions before any call-graph pass, or it names them after game
  functions. The World at War pass agrees with CoD3's symbol map on all 47. (World at War, CoD3)
- **Symbol-rich builds of the same game:** CoD2's Mac 1.3 build has real symbol names; its
  decompile named 458 more CoD2 PC functions by unique shared strings. (CoD2)
- **Function discovery:** Ghidra's analysis alone misses about 40% of the functions in these
  MSVC 7.1 executables (reached only through pointer tables, glued tail jumps, code after
  padding); `tools/pc_decomp/ghidra_scripts/PcFindFunctions.java` finds them (CoD2 MP 5,507 to
  9,415). (CoD2/UO) World at War's finder found about 3,000 the same way.

- **One empty function behind many names:** the linker folds functions with identical code, so a
  `{ "name", handler }` table can point several names at one function. In World at War PC SP, 40
  script builtin entries (file output, rumble, achievements, logging...) and the console command
  `assert` all point at one `ret`: those builtins do nothing in that program, and a name taken from
  one table entry names the shared function after only one of them. (World at War) Check: CoD3's
  builtin tables.

## Fastfiles, mods and custom maps

- **World at War's zone stream** (read from its own loader, PC 1.7 SP, `tools/waw_decomp/zone_loader.py`):
  a pointer 0 is null, -1 means the data follows inline, -2 inline and registered for later
  references, any other value v points at block (v-1)>>29, offset (v-1)&0x1FFFFFFF. Alignment
  applies to where data lands in memory; the file stream has no padding. (World at War) Check:
  CoD3's fastfiles ([formats/cod3.md](formats/cod3.md)).
- **Overriding assets** (World at War `DB_LinkXAssetEntry`, the CoD4 engine's rule): of two zones
  with the same asset, the one with the higher or equal zone flags wins (a mod's `mod.ff` has the
  highest); only types with a default asset name, plus raw files and map entities, may be
  overridden, any other type twice is a fatal error. So a mod can replace weapons, models,
  materials, images, sounds, menus, effects and scripts, not a map's collision or world. (World at War) Check:
  CoD3.
- **Where World at War looks for a zone:** the game's `zone\<language>\` first, then the mod folder
  `<fs_localAppData>\<fs_game>\`, then `<fs_localAppData>\usermaps\<map>\`; `mod.ff` only in the
  mod folder; `fs_localAppData` = the local application data folder + `\Activision\CoDWaW`. A map is
  a zombies map when its arena gametype is `zom` or its name contains `nazi_zombie_`. Details:
  [WAW_DECOMP.md](WAW_DECOMP.md), "Zombies and custom maps". (World at War)
- **Fixed limits a custom map can hit** (World at War PC 1.7 SP, each read from the comparison
  that enforces it): 1,024 game entities (1,018 for the map; KisakCOD SP has 2,176), per script VM
  65,533 variables and 24,573 objects, 48 hint strings, 1,022 localized strings (`setText` takes
  one per different text, never freed), 511 models, 127 materials, 195 effects, 255 effect-on-tag
  pairs, 63 looping/positioned sound aliases, 32 actors, 36 sentients, 31 hud elements sent per
  player, a 2,500 ms infinite-loop timeout (CoD4 5,000). Most index limits end the map with an
  error. The full list (85, private): `dev/waw-decomp/zone/engine_limits.tsv`; summary in
  [WAW_DECOMP.md](WAW_DECOMP.md). (World at War) Check: CoD3's own counts.
- **Sized enums in C++ headers** (`enum X : unsigned __int16`) turned into plain C enums become 4
  bytes and shift every later member; check a generated header's offsets with a compiler set to the
  game's ABI (`t4sp_types.py --check`: clang `--target=i686-pc-windows-msvc`). (World at War)

## Ghidra and tools

- **Ghidra's Xbox 360 language** (`PowerPC:BE:64:Xenon-32addr`) applies an image base of
  0x82000000 twice: load at 0 with the binary loader and move the block; never `setImageBase`.
  VMX128 needs the World at War chat's language extension. (World at War)
- **Xbox 360 sections don't start on page boundaries:** split the memory at every section start,
  or `.pdata` is unreadable (World at War: 128 functions found instead of 10,658). (World at War)
- **Stock Ghidra lacks `vadduws`** (commented out in its AltiVec file); the Xenon language in
  `tools/waw_decomp/ghidra_xenon/` adds it, and `WawFixBadInstructions.java` re-disassembles an
  existing project. With it, both World at War XEXs and both CoD2 XEXs decode completely (CoD3's `default.xex` has
  one undecodable word). (World at War)
- **Helper agents hold the chat that started them until they finish**; long runs go to background
  processes, started from a snapshot copy of their scripts (never edit a running shell script).
  (World at War, CoD2/UO)
- **The cloud machine has 2 cores and 7 GB:** one Ghidra analysis at a time (`MAXMEM=3G`),
  `nice` it, and give each agent its own `CARGO_TARGET_DIR`. (CoD2/UO)

## Copy protection and inputs

- **Steam's wrapper encrypts only the code:** World at War's Steam `CoDWaW.exe` has a `.bind`
  section and code at entropy 8.0, but its `.rdata`, `.data` and `.rsrc` are byte-identical to the
  owner's unwrapped CB copy of the same build. The decomps read the CB copies; the DRM is never
  touched. A SafeDisc file (`CoDSP.exe` 1.5) is skipped. (World at War, CoD2/UO)
- **The Rust versions never need an executable:** new copies hold only the data archives
  (`.pk3`, `.iwd`, `.ff` are not protected), on Windows and Linux, from Steam or a CB folder
  ([NEW_COPY.md](NEW_COPY.md)). (CoD2/UO)
- **Every build is identified by its SHA-1** before an address is used: the same file name in
  another patch has other addresses. (all)
