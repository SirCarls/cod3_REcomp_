# Cross-play: the PC games (CB copy) and the Rust version

**Status (2026-10-13): research done, nothing ported yet.** This page records how
Call of Duty 2 1.3 and Call of Duty: United Offensive 1.51 talk to each other over the
network, read from the two multiplayer executables (`CoD2MP_s.exe` and `CoDUOMP.exe`), and
the plan for the Rust version to speak exactly the same protocol. The goal (owner,
2026-10-13): "allow for cross platform play between cb copy and the rust version". Xbox 360
cross-play for these two games is out of scope for now. The Xbox 360 plan for Call of
Duty 3 is in [MULTIPLAYER.md](MULTIPLAYER.md); CoD3's console engine is the same family.

The evidence (addresses, disassembly, field tables, scripts, test vectors) is private, in
`dev/cod2-decomp/net/`. The executables were only read; one function at a time was run
in the Unicorn emulator to confirm the Huffman coder, as the decomp checker does.

## 1. What cross-play needs

- **Rust server, CB clients (first):** a player with the original game (CoD2 with CoD2x
  1.4.6.8 as its `mss32.dll`, or UO 1.51) sees the Rust server in the LAN list or joins it by
  address, and plays. The Rust server must answer the same packets, in the same bytes, as a
  CB dedicated server of the same version.
- **Rust client, CB server (later):** the same protocol from the other side.
- **The server decides.** Both games are client-server: the server runs the game, clients
  send their inputs and predict their own movement. The movement code must match for smooth
  play, but a small mismatch only causes a correction, never a split game (section 9).
- **No Activision service is ever contacted.** A server decides by itself whether to check
  a player's CD key with Activision; the Rust server never does (section 4). The client's own
  key handling is never touched.

## 2. The numbers

| | Call of Duty 2 1.3 | United Offensive 1.51 |
|---|---|---|
| Protocol number | **118** | **22** (CoD 1.5 is 6) |
| Version text a server sends back on a protocol mismatch | `1.3` | `1.51` |
| Default port (`net_port`) | 28960 | 28960 |
| LAN search | broadcast `getinfo xxx` to ports 28960 to 28963, twice (CoD2x: 28960 to 28991) | same, 28960 to 28963 |
| Authorize server (never used by us) | `cod2master.activision.com` port 20700 | `coduoauthorize.activision.com` port 20600 |
| Master server (server list on the internet) | `cod2master.activision.com` port 20710 | `coduomaster.activision.com` port 20610 |
| Largest message | 128 KiB | 32 KiB |
| Reliable commands kept | 128 | 64 |
| Config strings | 2048 (0 = server info, 1 = system info) | 2048 (same) |

A client with the wrong protocol gets `error\nEXE_SERVER_IS_DIFFERENT_VER\x15<version>\n` and
the server logs "rejected connect from protocol version %i (should be %i)" (CoD2
`SV_DirectConnect` 0x453C20, UO 0x459B20).

## 3. Packets without a connection ("out of band")

These start with the four bytes `FF FF FF FF`, then a text command. The receiver reads the
first line and splits it into words like a console command.

### 3.1 What a server answers

| Command | Answer | Notes |
|---|---|---|
| `getinfo <token>` | `infoResponse\n<info string>` | The LAN list and the server browser. Keys in 3.4. |
| `getstatus <token>` | `statusResponse\n<server info>\n<score> <ping> "<name>"\n` per player | |
| `getchallenge` | `challengeResponse <number>` | CoD2 clients send `getchallenge 0 "<32 hex digits>"` (a hash of their CD key, used only by PunkBuster and the authorize server). UO servers add a second number when `sv_onlyVisibleClients` is set. |
| `connect "<userinfo>"` | `connectResponse` or `error\n<reason>` | The userinfo carries `protocol`, `challenge`, `qport`, the player's settings, and (CoD2x) `protocol_cod2x`, `cl_hwid`, `cl_hwid2`. |
| `rcon <password> <command>` | `print\n<output>` | Remote console. |
| `ipAuthorize ...` | (none) | Only from the authorize server; the Rust server ignores it. |
| `v ...` | | CoD2 voice chat. |
| `pb_...` (any case) | | PunkBuster. Ignored when PunkBuster is off. |

### 3.2 What a client accepts

`challengeResponse` (only while it is asking for a challenge; it then takes the sender's
address as the server's), `connectResponse` (only while connecting, from the same IP),
`infoResponse`, `statusResponse`, `print`, `error` (from its server: ends the connection
with that message), `disconnect`, `loadingnewmap` (two lines: map and game type),
`needcdkey`, `echo`, `getserversResponse`, `updateResponse`, `keyAuthorize` (ignored), and
UO's `motd`. CoD2 also takes `v` (voice) and PunkBuster's `PB_` packets.

### 3.3 The connect packet

- **CoD2:** `FF FF FF FF connect "<userinfo>"`, plain text, no terminating zero.
- **UO:** the same text, but everything after the first 12 bytes (`FF FF FF FF connect `)
  is compressed with Quake III's *adaptive* Huffman coder (a 2-byte big-endian length, then
  the codes). This is the only place UO uses the adaptive coder.

### 3.4 What the LAN list needs

The only key a client requires in `infoResponse` is `protocol` (118 or 22; CoD2x clients also
accept 120). Everything else is shown if present: `hostname`, `mapname`, `clients`,
`sv_maxclients`, `gametype`, `game` (mod folder), `pure`, `minPing`, `maxPing`, `pswrd`
(password set), `ff` (friendly fire), `kc` (killcam), `hw` (2 = dedicated server; a
listen server sends 5 in CoD2, 4 in UO), `mod`, `pb` (PunkBuster), `sv_allowAnonymous`,
`challenge` (echo of the request's token). CoD2 adds `con_disabled` and `voice`; UO adds
`timeoutsAllowed`, `jps` (jeeps) and `tnk` (tanks). The order of keys in a CB server's
answer is recorded in the private notes, for byte-identical answers.

## 4. Letting players in without Activision's servers

What a CB server does on `getchallenge` (CoD2 0x452DE0, UO 0x458C50):

1. It remembers a challenge number for the player's address (1024 slots, oldest reused):
   `((rand() << 16) ^ rand()) ^ server_time`.
2. **A LAN player is answered at once** (unless `net_lanauthorize` is set).
3. Otherwise it asks the authorize server about the player's address (`getIpAuthorize`) and
   waits for its `ipAuthorize ... accept/deny` answer.
4. If the authorize server is silent, it gives up and answers anyway: UO after the player has
   asked for 5 seconds; CoD2 after 7 seconds, but only if the master server has not been heard
   from in the last 20 minutes (the clock starts with the server at 0, so never in a CoD2
   server's first 20 minutes).

On `connect`, the server checks the protocol, then the challenge (same address, port and
number), ping limits, the password and free slots. A player whose CD key was never checked
gets the identity number 0, which only prints "Connecting player #%i has a zero GUID": it is
not refused. `sv_allowAnonymous` is only passed on to the authorize server and shown in the
server info.

**So the Rust server answers every `getchallenge` at once, as a CB server does for a LAN
player.** This is a choice the server is free to make; the client is not changed. The client
still sends its own key check to Activision's authorize server when the server is not on its
LAN; it does not wait for an answer, so this works whether or not that service exists.

**PunkBuster:** with `sv_punkbuster 0` nothing PunkBuster-related is sent; a client's
`cl_punkbuster` setting only matters to a PunkBuster server. The Rust server never runs
PunkBuster and reports `pb 0`.

## 5. The connection ("netchan")

Every packet of a connection starts with a header, all numbers little-endian:

| Field | Size | Notes |
|---|---|---|
| sequence | 32 bits | top bit set = this packet is a fragment |
| qport | 16 bits | client to server only; tells the server which player it is if the port changes |
| fragment start | **CoD2: 32 bits, UO: 16 bits** | fragments only |
| fragment length | 16 bits | fragments only |

A message of 1300 bytes or more is sent in 1300-byte fragments (a message that ends exactly on
a fragment boundary gets one empty fragment more). Packets that arrive late or twice are
dropped (sequence not newer than the last one); the receiver counts the gap as lost packets.
Fragments must arrive in order. This is Quake III's netchan with no change except the CoD2
fragment start. There is no challenge in the header.

After the header:

- **Server to client:** 4 bytes (the last client command the server received), then the
  compressed message (section 6).
- **Client to server:** 1 byte (server id), 4 bytes (last server message received), 4 bytes
  (last server command received), then the compressed message.

The compressed part is then scrambled with a simple key, as in Quake III 1.32: a running
XOR byte made from the challenge number, a sequence number and the text of the last
reliable command the other side acknowledged. Both games do the same.

## 6. How messages are coded

**Huffman.** Messages are compressed with Quake III's message Huffman tree: the frequency
table in both executables is **identical, entry for entry, to Quake III's** (same 256 values),
and the tree is built the same way. This was checked by running each game's own compressor and
decompressor in the emulator on 64 test buffers against an independent model of the Quake III
coder: identical every time. The difference is *how* it is used: Quake III compresses each
byte as it is written, mixed with raw bits; CoD2 and UO write the whole message uncompressed
first and compress it in one go at the end (and decompress the whole message on arrival).
Decompression stops at the end of the last byte, so the padding bits usually decode into one
extra byte at the end of the message; the message's end marker makes it harmless, but an
exact port must reproduce it.

**Bits and bytes mixed.** Inside a message, single bits and whole bytes are interleaved in an
unusual way: a bit write that needs a new byte starts one at the end of the message, and the
next bits fill that byte; a byte, short, long or string write is appended raw at the end,
after it. Bits are written lowest bit first.

**Strings.**

| | CoD2 | UO |
|---|---|---|
| Normal string limit | 1023 characters (a longer one is sent empty) | same |
| Big string limit (config strings) | 8191 | 8191 |
| Characters changed when writing | `0x92` becomes `'` | `0x92` becomes `'`, every byte above 127 becomes `.` |
| Characters changed when reading | `0x92`; `%` becomes `.` in lines and big strings | the same, plus above 127 becomes `.` |
| A string longer than the limit when reading | kept to 1023, the rest read and dropped | reading stops at 1023 (the rest stays in the message) |

Quake III changes `%` and every byte above 127 when writing; CoD2 does neither.

**Reading past the end:** CoD2 marks the message as broken; UO's bit reader keeps reading old
bytes without noticing. Exact ports keep both behaviors.

## 7. What is in the messages

### 7.1 Message types

| | CoD2 | UO (Quake III's numbers) |
|---|---|---|
| Server to client | nop 0, gamestate 1, configstring 2, baseline 3, serverCommand 4, download 5, snapshot 6, end 7 (one byte each) | bad 0, nop 1, gamestate 2, configstring 3, baseline 4, serverCommand 5, download 6, snapshot 7, end 8 |
| Client to server | move 0, move without delta 1, client command 2, end 3, **in 3 bits** | the same numbers **in 2 bits** |

**Game state** (sent when a player connects or the map changes): the last client command,
pending server commands, then the game state type, the server's command number, every
non-empty config string (type, 16-bit index, big string), every entity baseline, the end
marker, the player's client number, the checksum seed (`checksumFeed`, used by the pure
check) and the end marker again.

**Snapshot:** server time (32 bits), which earlier snapshot it is relative to (one byte,
0 = none), flags (one byte), the player's state, the entities (ended by number 1023 in 10
bits), and the other players' client info (each starts with a 1 bit; a 0 bit ends the
list). Unlike Quake III there is no area mask.

**Server commands** use one letter instead of Quake III's words: `d` = config string
changed (`d <index> <text>`), `x`, `y`, `z` = pieces of a long config string, `w` =
disconnected with a reason, `B` = map restart, `n` = map restart keeping players' data. The
game adds its own (for example `v` = set a client setting).

**Server id:** one byte. A new map adds 16 (skipping 0 in the high half); a map restart
counts up the low half. A client sending an old id with the same high half is put back into
the game; a different high half gets the game state again.

### 7.2 Player commands

Each client packet carries up to 32 player commands, each coded against the previous one with
a key: the checksum seed, the server message number and a hash of the last server command.

| | CoD2 | UO |
|---|---|---|
| Size in memory | 28 bytes | 24 bytes |
| Buttons | 19 bits | 7 bits |
| Moves | forward, right (each reduced to "pressed forward/back/none" in the key) | forward, right, up |
| Weapon, off-hand | 7 bits each | weapon byte, one more 7-bit field |
| Angles | 3 × 16 bits | 3 × 16 bits |

A command where only the time changed costs a few bits; two coding branches for "small
change" and "full change" mix the key with the command's time in different places. These
details are exact in the private notes and are the first thing the oracle will check.

### 7.3 Entity and player state

Changes are sent field by field from tables of fields (name, offset, size in bits). Each
field: one bit "changed"; for a changed number: one bit "zero or not", then the value. CoD2
and UO share these rules:

- **Whole numbers:** the low (size mod 8) bits as bits, then whole bytes raw.
- **Decimals:** if the value is a whole number between -4096 and 4095, 13 bits (5 as bits,
  8 as a raw byte); otherwise the raw 32-bit value. Same idea as Quake III's 13-bit trick.
- **Angles:** 16 bits (`angle × 65536 / 360`, cut toward zero).
- **CoD2 only:** screen positions with a 10-bit version of the decimal rule.
- **Structure:** an entity starts with its number (10 bits), a "removed" bit and a "has
  changes" bit (a client-info entry has a 1 bit before its 6-bit number); then the index of
  the last changed field as a raw byte, then the fields up to it. The player state is the
  index byte and the fields.

| Table | CoD2 | UO |
|---|---|---|
| Entity | 59 fields | 60 fields; a second 60-field table for entity type 12 |
| Player | 105 fields | 114 fields (UO adds vehicles, speed scales, view heights) |
| Client info | 22 fields | 22 fields |
| Archived entity (killcam) | 68 | 68 |
| HUD element | 32 | 30 |
| Objective | 6 | 6 |

The tables differ in order, sizes and fields between the games, so each game has its own.

## 8. Pure servers (`sv_pure`)

A pure server makes clients use only the game archives it has (`.iwd` in CoD2, `.pk3` in UO):

1. For each archive, the CRC-32 of every file in it (in the order of the zip directory,
   skipping empty files) is put in a list. Its **checksum** is MD4 of that list, folded to 32
   bits (Quake III's `Com_BlockChecksum`); its **pure checksum** is the same with the map's
   random `checksumFeed` hashed first. This is Quake III's method, unchanged.
2. The server lists its archives in the system info: CoD2 `sv_iwds`, `sv_iwdNames`,
   `sv_referencedIwds`, `sv_referencedIwdNames`; UO `sv_paks`, `sv_pakNames`,
   `sv_referencedPaks`, `sv_referencedPakNames`.
3. The client answers with a `cp` command: CoD2 `cp @ <pure checksums of the archives it
   used> <check number>`; UO `cp <cgame archive> <ui archive> @ <...> <check number>` (the
   archives holding the client game and menu DLLs, as Quake III's virtual machines).
4. The server checks that every listed archive is one of its own and that the check number
   (`checksumFeed` XOR all checksums XOR their count) is right. Only a failed `cp` drops the
   player ("EXE_UNPURECLIENTDETECTED"), and only with `sv_pure` on.

**For the Rust server:** with `sv_pure 0` none of this is needed. With `sv_pure 1` it reads
the same archives from the player's installation, computes both checksums and checks `cp`
the same way. UO's pure check also needs to know which archives hold the client DLLs (open
question).

## 9. Bit-exact movement prediction

The client predicts its own movement with the same code the server runs (`PM_*`, `BG_*`), in
x87 floating point, so the precision the x87 runs at decides the last bits of every result:

- **Call of Duty 2 sets 24-bit precision itself, every frame.** `Com_Frame` (0x435240) and
  `Com_Init` call the C runtime's `_controlfp(_PC_24, _MCW_PC)` before anything else, in the
  client and in a dedicated server alike (one executable). Its Direct3D device is also created
  without `D3DCREATE_FPU_PRESERVE`, which would set 24 bits anyway. So a CB client and a CB
  server compute movement the same way: like `f32` arithmetic, except that values beyond
  `f32`'s range stay finite until they are stored.
- **United Offensive never changes it** (an OpenGL game: nothing sets it behind its back), so
  client and server run at the C runtime's 53-bit default: intermediate results round like
  `f64`, and storing them to a `float` rounds a second time.

The server's result wins either way and is sent as raw 32-bit floats. For the Rust server to
behave like a CB server, its movement and its float-to-integer conversions (angles, the
"whole number" test in deltas, which turns out-of-range values into -2147483648) follow the
x87 results of each game's precision exactly; the oracle runs every original function at the
precision its game uses (`GAME_FPU` in `tools/decomp/x86/harness_x86.py`), and the ports match it.
Corrected 2026-10-13 15:40: an earlier version of this page said neither game sets the precision;
the oracle helper's search for `_controlfp` calls (not only `fldcw`) found CoD2's.

## 10. CoD2x (the CB copy's `mss32.dll`)

From CoD2x 1.4.6.8's source (facts only):

- The client still connects with protocol 118 and adds `protocol_cod2x 6`, `cl_hwid`,
  `cl_hwid2` (and `match_login`) to its userinfo. A plain 1.3 server ignores extra keys.
- It accepts servers that report protocol 118 or 120 (CoD2x servers report 120) and scans
  ports 28960 to 28991 on the LAN.
- A CoD2x server sets `g_cod2x` in the system info. **Without it the client prints "CoD2x:
  Changes turned off, using legacy CoD2 1.3"**: its gameplay fixes stay off and it does not
  load CoD2x's own archive, so its pure list is a plain 1.3 list.
- A CoD2x server refuses clients without CoD2x and checks `cl_hwid2`.

**So the Rust server speaks plain 1.3:** protocol 118, never sets `g_cod2x`, and keeps the
extra userinfo keys as given. A CoD2x client then joins in legacy 1.3 mode, exactly as it
joins a plain 1.3 server.

## 11. Same as Quake III, and different

| Same as Quake III | Different |
|---|---|
| Out-of-band packets and their names; challenge, connect and authorize flow | The CD key hash in CoD2's `getchallenge`; UO's extra challenge number; letter server commands |
| Netchan header, fragment size, ordering rules | CoD2's 32-bit fragment start; larger messages |
| Huffman frequency table and tree | Whole-message compression; raw bytes mixed with bits; CoD2 sends `connect` uncompressed |
| XOR scrambling of messages (1.32) | Client header has a 1-byte server id |
| Key-coded player commands, Quake III's command hash | Command layouts, two coding branches |
| 13-bit whole-number trick for floats | Split as 5 bits + raw byte; angles as raw shorts; CoD2's 10-bit variant |
| Pure checksums (MD4 over CRC lists, keyed by `checksumFeed`) | No server id in `cp`; CoD2 has no client-DLL checksums |
| Configstrings, baselines, snapshots | CoD2 message numbers shifted by one; no area mask; client info list; server id scheme |

## 12. Porting plan

**Status (2026-10-15):** items 1-4 and 6 are ported and checked against both games
(`iw_pc::net::{huffman, msg, protocol_data, oob, netchan, delta}`; 165 + 22 functions match,
the delta writers and readers both), and the game state and snapshot bodies are written from
them (`iw_pc::net::game_state`).
Item 7, the server state machine, is `iw_pc::server` with the `iw-server` binary
([PC_SERVER.md](PC_SERVER.md)): the first milestone below, waiting for a test with a real CB
client. Two things found while porting item 6: the player state's own field coder has no "is
zero" bit for floats and integers (Quake III's player-state rule; angles keep theirs, unlike
the entity coder), and player commands carry only the *direction* of movement (four bits:
forward, back, right, left; United Offensive two more for up and down), which arrives as 127,
-127 or 0. And from the readers: United Offensive doesn't extend the sign of integer fields
with a negative bit count (Call of Duty 2 does), so a -1 in an 8-bit field arrives as 255; a
field count past the table makes Call of Duty 2 mark the message overflowed and stop, while
United Offensive reads the whole table.

The ports go into `engine/crates/iw_pc/src/net/`, one module per area, each function ported
from both executables and checked against them with the x86 oracle (one function run in the
emulator on many inputs, results compared). Each function keeps `/// Original:` lines for both
games, for example ``/// Original: `MSG_WriteBits` at 0x00443E50 (CoD2MP_s.exe 1.3), 0x004495F0
(CoDUOMP.exe 1.51)``. One Rust function serves both games when the machine code agrees; a
`Game` value (`Cod2` / `Uo`) picks the variant where it doesn't.

**Game data stays out of the code.** The Huffman frequency table (256 numbers) and the field
tables (entity, player, client, HUD) are needed by the protocol, but they are game data (and
the Huffman table is also Quake III's GPL source). The Rust code reads them at start from the
player's own executable, at the addresses below, and checks a hash of the bytes; a different
executable is refused with a clear message. Only small constants (protocol numbers, ports,
sizes) are written in the code.

Many of these functions take their arguments in registers (MSVC 7.1 builds); the "calls"
column below and the private notes say which, for the oracle.

### Order and contents

1. **`huffman.rs`** — the fixed message tree and the whole-buffer coder; UO's adaptive coder
   for `connect`.

   | Function | CoD2 1.3 | UO 1.51 | Calls (CoD2) |
   |---|---|---|---|
   | `MSG_initHuffman` | 0x447670 | 0x44CF80 | none |
   | `Huff_Init` | 0x43D4E0 | 0x4420F0 | esi = tree pair |
   | `Huff_addRef` | 0x43CCC0 | 0x441870 | eax = tree, stack: symbol |
   | `send` (Quake III's static bit writer) | 0x43CF30 | 0x441AE0 | stack |
   | `Huff_transmit` | 0x43CFC0 | 0x441B70 | |
   | `Huff_offsetTransmit` | 0x43D160 | 0x441D10 | |
   | `Huff_Receive` | 0x43CE60 | 0x441A10 | |
   | `Huff_offsetReceive` | 0x43CEC0 | 0x441A70 | eax = tree, ebx = &bitpos, edi = in |
   | `MSG_WriteBitsCompress` | 0x444000 | 0x449780 | eax = in, ebx = out, ecx = length |
   | `MSG_ReadBitsCompress` | 0x444050 | 0x4497D0 | eax = in, stack: out, length |
   | `Huff_Compress` (adaptive) | — | 0x441FA0 | |
   | `Huff_Decompress` (adaptive) | — | 0x441D40 | |
   | frequency table (data) | 0x59CB58 | 0x5C58D0 | 256 × int32 |

2. **`msg.rs`** — the message buffer: bits, bytes, strings, angles, key-coded values.

   | Function | CoD2 1.3 | UO 1.51 |
   |---|---|---|
   | `MSG_Init` | 0x443DE0 | (inlined) |
   | `MSG_BeginReading`, `MSG_GetUsedBitCount` | 0x443E20, 0x443E30 | (inlined) |
   | `MSG_WriteBits`, `MSG_WriteBit0`, `MSG_WriteBit1` | 0x443E50, 0x443EC0, 0x443EF0 | 0x4495F0, 0x449660, 0x449690 |
   | `MSG_ReadBits`, `MSG_ReadBit` | 0x443F40, 0x443FB0 | 0x4496E0, 0x449740 |
   | `MSG_WriteByte`, `MSG_WriteData`, `MSG_WriteShort`, `MSG_WriteLong` | 0x4440B0, 0x4440D0, 0x444110, 0x444140 | 0x449830, 0x449850, 0x449890, 0x4498C0 |
   | `MSG_WriteString`, `MSG_WriteBigString` | 0x4441B0, 0x444270 | 0x4498F0, 0x4499F0 |
   | `MSG_WriteAngle`, `MSG_WriteAngle16` | 0x444350, 0x444380 | 0x449B50, 0x449B80 |
   | `MSG_ReadByte`, `MSG_ReadShort`, `MSG_ReadLong`, `MSG_ReadData` | 0x4443C0, 0x4443E0, 0x444410, 0x444600 | 0x449BC0, 0x449BE0, 0x449C10, 0x449D90 |
   | `MSG_ReadString`, `MSG_ReadBigString`, `MSG_ReadStringLine`, `MSG_ReadAngle16` | 0x444480, 0x4444E0, 0x444540, 0x4445B0 | 0x449C30, 0x449C80, 0x449CE0, 0x449D40 |
   | `MSG_WriteDeltaKey`, `MSG_ReadDeltaKey` | 0x444650, 0x4446A0 | 0x449E90, 0x449EE0 |
   | `MSG_WriteDeltaKeyByte`, `MSG_ReadDeltaKeyByte` | 0x444750, 0x4447B0 | 0x449F80, 0x449FE0 |
   | `MSG_WriteDeltaKeyShort`, `MSG_ReadDeltaKeyShort` | 0x444840, 0x4448B0 | 0x44A060, 0x44A0D0 |
   | `MSG_WriteReliableCommandToBuffer` | 0x444940 | 0x44A150 |

3. **`netchan.rs`** — header, fragments, ordering, the XOR scrambling.

   | Function | CoD2 1.3 | UO 1.51 |
   |---|---|---|
   | `Netchan_Setup` | 0x447D50 | 0x44D530 |
   | `Netchan_TransmitNextFragment` | 0x447DB0 | 0x44D590 |
   | `Netchan_Transmit` | 0x447FB0 | 0x44D790 |
   | `Netchan_Process` | 0x4481B0 | 0x44D970 |
   | `SV_Netchan_Encode`, `SV_Netchan_Decode` | 0x45C8C0, 0x45C920 | 0x463640, 0x4636A0 |
   | `CL_Netchan_Encode`, `CL_Netchan_Decode` | 0x4127F0, 0x412860 | 0x4173F0, 0x417460 |
   | `SV_SendMessageToClient` (header, compress, scramble, send) | 0x45F370 | 0x465DA0 |

4. **`oob.rs`** — connectionless packets, info strings, the challenge table, the connect
   check. Info strings are ported here unless an `iw_pc` info module exists by then.

   | Function | CoD2 1.3 | UO 1.51 |
   |---|---|---|
   | `NET_OutOfBandPrint`, `NET_OutOfBandData` | 0x448910, 0x448A70 | 0x44E140, 0x44E240 |
   | `SV_ConnectionlessPacket` | 0x45B430 | 0x4623C0 |
   | `SVC_Info`, `SVC_Status` | 0x45AE80, 0x45A880 | 0x4616F0, 0x461190 |
   | `SV_GetChallenge` | 0x452DE0 | 0x458C50 |
   | `SV_DirectConnect` | 0x453C20 | 0x459B20 |
   | `CL_ConnectionlessPacket`, `CL_CheckForResend`, `CL_ServerInfoPacket` | 0x40E750, 0x40E100, 0x4B34E0 | 0x412880, 0x411E60, 0x4155F0 |
   | `Info_ValueForKey`, `Info_SetValueForKey` | 0x44AA90, 0x44AE10 | 0x44FB70, 0x44FF50 |

5. **`pure.rs`** — MD4 checksums of archives, the system info lists, the `cp` check.

   | Function | CoD2 1.3 | UO 1.51 |
   |---|---|---|
   | `Com_BlockChecksum`, `Com_BlockChecksumKey` | 0x443A70, 0x443AE0 | 0x449470, 0x4494F0 |
   | MD4 update, final | 0x443230, 0x4432D0 | (called by the above) |
   | `FS_LoadZipFile` (CRC list) | 0x4234E0 | 0x42E970 |
   | `FS_LoadedIwdPureChecksums` | 0x43C000 | (to find) |
   | `SV_VerifyIwds_f` / `SV_VerifyPaks_f` (`cp`) | 0x4558F0 | 0x45B860 |
   | `FS_PureServerSetLoadedIwds` (client) | 0x43C550 | (to find) |

6. **`delta.rs`** — player commands, field deltas, entity, player and HUD states, game state
   and snapshot order.

   | Function | CoD2 1.3 | UO 1.51 |
   |---|---|---|
   | `MSG_WriteDeltaUsercmdKey`, `MSG_ReadDeltaUsercmdKey` | 0x444B10, 0x444EC0 | 0x44A360, 0x44A770 |
   | `MSG_WriteDeltaField`, `MSG_ReadDeltaField` | 0x445180, 0x445700 | 0x44AA60, 0x44B030 |
   | `MSG_WriteDeltaStruct`, `MSG_ReadDeltaStruct` | 0x445480, 0x445A40 | 0x44ACD0, 0x44B280 |
   | `MSG_WriteDeltaEntity` (table choice), `MSG_ReadDeltaEntity` | 0x445680, 0x445BE0 | 0x44AF50, 0x44B420 |
   | `MSG_ReadDeltaArchivedEntity` | 0x445C00 | 0x44B4A0 |
   | `MSG_WriteDeltaClient`, `MSG_ReadDeltaClient` | 0x4456C0, 0x445C20 | 0x44AFE0, 0x44B4C0 |
   | `MSG_ReadDeltaFields` (objectives) | 0x445990 | 0x44B1E0 (inline in the player state reader) |
   | `MSG_WriteDeltaPlayerstate`, `MSG_ReadDeltaPlayerstate` | 0x445EC0, 0x446CA0 | 0x44B7B0, 0x44C5C0 |
   | HUD elements write, read | 0x445C60, 0x445DB0 | 0x44B510, 0x44B690 |
   | `SV_SendClientGameState` | 0x4549A0 | 0x45A8F0 |
   | `SV_WriteSnapshotToClient` | 0x45D7C0 | (to find) |
   | `SV_ExecuteClientMessage`, `SV_UserMove` | 0x456300, 0x4560B0 | 0x45C230, 0x45C010 |
   | `CL_WritePacket`, `CL_ParseServerMessage`, `CL_ParseSnapshot` | 0x408E80, 0x4143E0, 0x4136C0 | 0x40CC80, 0x419500, 0x418870 |
   | field tables (data): entity, player, client, archived, HUD, objective | 0x59BDA0, 0x59C4A0, 0x59C398, 0x59C068, 0x59C9D8, 0x59C990 | 0x58EC68 (+0x58EF38), 0x58F640, 0x58F538, 0x58F208, 0x5C5768, 0x5C5720 |

   **Broken messages.** The readers trust their input more than they should. Where a message
   ends just before a structure's field count, the count reads as -1 and the structure reader
   copies "field -1": the entry in front of the table, which is the end of whatever precedes
   it in the executable (an offset inside the structure for some tables, United Offensive's
   entity number for one, far outside it for others). A player state count past its table,
   or a United Offensive HUD field index of 30 or 31 (5 bits for a 30-field table), has the
   originals read entries past their tables (the Huffman frequencies follow the HUD table) as
   fields, and write outside the structure. The writers never send any of this. The port
   copies field -1 when it lies inside the structure (the offset is read from the executable
   with the tables) and otherwise reads only the table and marks the message overflowed;
   the checks don't generate the cases that would make the originals write outside.

7. **Server state machine** (after the six modules): challenge table, client slots, the
   connect sequence (challenge → connect → game state → `cp` → snapshots), reliable command
   queues, timeouts. Built on the ported functions, with the game itself (scripts, physics)
   coming from the rest of the Rust decomp.

8. **Client side** (later): the same modules from the other side, then the CB-server join.

First milestone: a Rust LAN server that a CB client lists, joins and stays connected to on
an empty map (`sv_pure 0`): items 1 to 4, the game state and empty snapshots from item 6,
and item 7's connect sequence.

## 13. Open questions

- **UO pure check:** which `.pk3` holds `uo_cgame_mp_x86.dll` and `uo_ui_mp_x86.dll` on a
  normal installation (the server needs their pure checksums when `sv_pure` is on). Needs the
  owner's archive list or the files.
- **Exact `cp` timing:** whether a client sends `cp` when the server is not pure (the server
  ignores it either way).
- **UO details still to port and check:** its `CL_Netchan_Decode`, `SV_WriteSnapshotToClient`
  and the pure-list functions are not yet located; the meaning of UO's second
  `challengeResponse` number and of entity type 12.
- **Game scripts and player identity:** players get identity number 0 without the authorize
  server; game scripts that use it (bans, stats) see 0 for everyone.
- **PunkBuster's own packets** were not studied (never used by the Rust server).
- **Internet play:** reporting the Rust server to a master server (Activision's or CoD2x's)
  is a separate decision; LAN and direct `connect <address>` need none.
