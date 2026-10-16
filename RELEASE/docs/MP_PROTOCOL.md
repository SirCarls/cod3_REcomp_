# CoD3 multiplayer: what goes over the network

What Call of Duty 3's multiplayer program (`codmp_xenonf.xex`) sends, as far as it is mapped, for the owner's goal of one match across a custom Xbox 360 copy, the recomp and the Rust engine ([MULTIPLAYER.md](MULTIPLAYER.md) 4.9). Every address is the multiplayer program's. Found by reading its PowerPC code (`tools/decomp/decomp.py dis/calls/callers --image codmp`) and by running single functions in the oracle ([DECOMP.md](DECOMP.md)); the evidence for each statement is in the Rust decomp's private notes (`dev/agents/rust-decomp/protocol/`). The function names are in the multiplayer program's symbol map (`tools/name_mp_functions.py`).

**Status (2026-10-13, evening):** both layers mapped at the top. Ported to Rust and matching the original exactly: the bit encoding of the messages consoles exchange (`cod3_game::net::bit_buffer`, 27 functions), the field encoders on top of it (`net::fields`: positions, angles, directions, velocities, coordinate deltas; 21), and the three player state messages, each both ways (writer and reader): the position update 0x23 (`net::position`), the view update 0x24 (`net::view`) and the vehicle update 0x4C (`net::vehicle`). The other ~80 message types are listed but not decoded.

## 1. Two layers, and only one reaches the network

1. **Quake III's layer stays inside one console.** The netchan, the server's messages to clients (`svc`) and the clients' messages to the server (`clc`) exist, but `Netchan_Transmit` 0x82484490 and `NET_SendPacket` 0x824841A8 can only reach `NET_SendLoopPacket` 0x8247ED40, which copies into in-memory queues, and nothing ever queues a packet from a socket. Every console runs its own server, and this layer links that server to the console's own players (up to 4 split-screen) only. User commands aren't even sent: the server reads them from the client's memory (`SV_GetUsercmd` 0x82530270). Section 3.
2. **Between consoles: Demonware's peer-to-peer session.** Every console sends the state of **its own** players to every other console, as Demonware messages (a type byte and a bit-packed payload) over `bdSession` connections. There is no central server streaming the world: the host of the session has extra jobs (joining, game state), but player positions come from the console that owns the player. Section 2.

So a Rust engine that plays with consoles speaks section 2. Section 3 matters only for running the console's own local server and split-screen clients the same way.

## 2. Between consoles: Demonware peer messages

### 2.1 Transport

Demonware sockets (`bdSocket`), UDP with a 2-byte length header for the game data (the console's VDP format, sent as plain UDP by the recomp's SDK patch 0025), then Demonware's own connection layer (`bdUnicastConnection`: an SCTP-like handshake and data chunks), then `bdPacket`. Ports: 3074 (Demonware's default; not checked whether CoD3 changes it) and 50000 for LAN discovery. Details in [MULTIPLAYER.md](MULTIPLAYER.md) section 3. **Not mapped yet:** how a message is framed inside a `bdPacket` (where the type byte and the payload's length go; start at `bdPacket::serialize` and `bdDataChunk`).

### 2.2 Messages

A `bdMessage` (constructor 0x821B1E38) is a type byte (+8) and a payload (+0xC), a `bdBitBuffer`. Game messages are made with type checking off (2.3). `bdSession::accept` 0x821C3510 handles Demonware's own types itself (0x0A join request 0x821C27C8, 0x0B join reply 0x821C2948, 0x0C consistency 0x821C3028) and passes the others, from known peers only, to the game's listener `MPPlayerManager::vfunc_1` 0x823D8C08. That one calls the handler for the type from a table the `MPPlayerManager` constructor 0x82405D50 fills (`this + 0x8210 + type * 8`). Until the player manager is ready it queues messages (up to 512), except that it handles 0x59, 0x5B, 0x1E, 0x1F, 0x26, 0x72 (and 0x20 from one address, probably the host) at once and drops 0x23, 0x24, 0x4C and 0x64 (the frequent state updates).

Sending: `MPPeer::SendAll` 0x823D8F08 (to every connection of the session, 47 callers), `MPPeer::SendToPlayer` 0x823DCCA8 (one player slot's connection), `MPPeer::SendToAllRemote` 0x823DCF10 (every used player slot that isn't local: 24 slots of 0x310 bytes at `MPPeer + 0x1010`).

### 2.3 The payload: Demonware's bit buffer

Ported: `cod3_game::net::bit_buffer` (every function below matches the original on 4,000 to 10,000 random cases).

- **Bits go in least significant first:** bit `n` of the stream is bit `n % 8` of byte `n / 8`; values of several bytes go in little-endian, so an N-bit value is its N low bits in order. On a big-endian console this means the game swaps every value's bytes before writing it.
- **One header bit:** the buffer's first bit says whether it is type-checked. A type-checked buffer writes a 5-bit type tag before every value (1 bool, 2/3 signed/unsigned 8-bit, 5/6 16-bit, 7/8 32-bit, 10 64-bit, 11/12 ranged signed/unsigned, 13 float, 15 ranged float, 19 blob) and the range before every ranged value. **Game messages are not type-checked**, so their first bit is 0 and they carry no tags.
- **Ranged integers** take `highBitNumber(hi - lo) + 1` bits (`highBitNumber` = index of the top set bit): `value - lo` after clamping to `[lo, hi]`. Readers clamp what they read too (a range that isn't a power of two minus one leaves bit patterns past `hi`).
- **Ranged floats** are quantized: `steps = (hi - lo) * (1 / |precision|)` (single precision), the bits are those of `trunc(steps)`, and the value sent is `trunc((clamp(v) - lo) * (1 / |precision|) + 0.5)` (fused multiply-add), at most `steps`. More than 2³² steps sends the plain 32-bit float instead. The reader computes `steps` as `(hi - lo) / |precision|` (one division) and the value as `step * |precision| + lo`; the writer's and the reader's step counts can round differently, so a range where that changes the number of bits would put the two out of step. The game's ranges don't seem to hit it; a Rust peer copies both formulas as they are.
- Reads past the end fail, set a "read failed" flag that stays set, and leave the output as it was.

### 2.4 Player state: who sends what, when

`MPPeer::SendLocalPlayerUpdates` 0x82403840 runs every frame for each local player, unreliable, on three timers (per local player):

| Timer | What goes out |
|---|---|
| 1000 ms since the last full update (or the clock went back) | A **full** position update (0x23) to nearby players, committed as the new baseline, and a full one, not committed, to the others. The vehicle updates (0x4C) go out on this pass too. |
| 125 ms, when 1 to 5 enemies are near (0x823E57D0) | A **delta** position update to nearby players. |
| 250 ms | A delta position update to the others (and the 0x4C updates). |

"Nearby" (0x823E5978): within 512 units and, in team games, on another team. A delta that turns out to need a full update goes to everybody. When the player is in a vehicle or on a turret (player slot byte +0x44), a view update (0x24) goes instead of the position update, and the position travels in 0x4C.

**0x23, position update** (the caller writes the slot and "near"; `MPPlayer::WritePositionUpdate` 0x823F2E20 the rest; the handler `MPPeer::HandlePositionUpdate` 0x823F9528 reads the slot and `MPPlayer::ReadPositionUpdate` 0x823F78B0 the rest), in bit order after the header bit:

```
8   player slot index
1   near (sent by proximity; the receiver expects the next update in 125 ms if set, else 250)
1   player state is 3 (client +0x784 == 3; probably dead or spectating)
1   full
8   baseline counter (incremented first when the update is full and committed)
full:
    origin    3 x ranged float (-8191, 8191, precision 1) = 3 x 14 bits
    velocity  1 bit "moving horizontally"; if set 8 bits of direction (yaw / 360 * 255)
              and a ranged float (horizontal speed capped at 1024; -1024..1024, 1) = 12 bits;
              then vertical speed ranged (-1024, 1024, 1) = 12 bits
    angles    pitch, yaw: 16 bits each, (int)(angle * 182.044) & 0xFFFF
    8   weapon
    32  flags word (client +0x2C)
    16  ranged unsigned (0, 0x8000), tagged: the "use code" (0x823DDB08, not ported yet): a
        bitfield, low bits a weapon class 0-9 from the weapon's definition, 0x20 a flags bit,
        0x100 and 0x1000 client states
    5   ranged unsigned (0, 25): client +0x520 (probably stance or movement state)
delta (against the last committed full update):
    8 bits: x/y moved, z moved, velocity, angles, weapon, use code, flags, +0x520 changed
    then, for each set bit in that order: x and y as ranged (-512, 512, 1) = 2 x 11 bits;
    z the same; velocity, angles, weapon, flags (32), use code (16), +0x520 (5) as above
```

"Moved" means more than 1 unit from the baseline on that axis. A full update is forced when the flags word differs from the baseline or the origin is 512 units or more from it, so the flags never travel in a delta.

**The two ends keep different baselines.** The sender commits the exact values it read from its player; the receiver keeps what the wire delivered (whole units, 16-bit angle steps, a velocity rebuilt from its direction byte). A delta is computed against the first and applied to the second, so the receiver's copy is off by the rounding until the next full update, at most a second later. A Rust peer must keep both baselines the console's way to stay in step with it.

**What the receiver does** (`ReadPositionUpdate`): every part is read only when everything before it was. A full update becomes the receiver's baseline at once, even when a read in it failed. Then the update is dropped when the player is in state 3 on the receiving console and the update's state bit isn't set, or when it is a delta and its counter isn't the receiver's baseline counter; recorded as failed (only the time, 0x82C2826C into `MPPlayer` +0x48) when a read failed; else applied: position, velocity and view into the `MPPlayer` (+0xA0 to +0xC8), the use code (+0x50), and, with an entity, flags, stance and weapon into the entity and its client (game code, not ported yet).

**The velocity's direction** comes back from its byte through a four-lane sine and cosine (0x820B3920) made of the Xbox 360's VMX128 vector instructions, which the checking emulator can't run yet; it is the one step of these messages not checked against the original (both sides of the check use the same stand-in for it).

**0x24, view update** (`MPPlayer::WriteViewUpdate` 0x823E6770, read by `MPPlayer::ReadViewUpdate` 0x823F7638 from the handler 0x823F9440): no player index (the receiver knows the sender's connection). Nothing is sent for a slot without a client or a client whose +0xAE0 is set. Then:

```
1   has angles: what 0x820B12D8 says of the entity, unless the client's turret entry
    (+0xAF4 picks a 0x1F0-byte entry at 0x82C77E30) has +0x114 below +0x118
32  pitch, yaw (16 bits each), when it has angles
2   ranged int (0, 3): the seat, 2 or 1 from the linked vehicle's flags (+0x215, +0x216 of
    its +0x20C) when the entity's +0x1B0 link is good and the player is in it, else 0
8   weapon (client +0xA4)
```

The receiver puts the angles into the `MPPlayer`'s view (+0xC0 yaw, +0xC4 pitch) and hands the seat to the vehicle and the weapon to the game (when the `MPPlayer`'s +0x40 says the player is in a vehicle, or in states 2 to 5).

**0x4C, vehicle update** (`MPPeer::WriteVehicleUpdate` 0x823F2288, read by `MPPeer::ReadVehicleUpdate` 0x823F6048 from the handler 0x823F8658): one per entry of a list at `MPPeer + 0x59B0` (entries 0x210 bytes):

```
5   ranged (0, 20): the entry's index (the caller)
42  position, as in 0x23
13+ velocity, as in 0x23; then a second velocity the same way (probably angular)
48  angles, 16 bits each (all three)
19  four controls, probably steering, throttle, brake and one more:
    ranged floats (-1, 1, 0.05), (-1, 1, 0.1), (0, 1, 0.1), (0, 1, 0.1); the reader
    snaps the second to 0 inside (-0.001, 0.001); all 0 for a vehicle without physics
2   ranged int (0, 3): motion state (2 if info +0x1F9, else 1 if +0x1FA or a vehicle of
    kind 1 whose +0x3D8 is above 0.5, else 0)
2   ranged int (0, 3): the seat, as in 0x24
13  ranged int (-1, 0x1004): the entity's +0x308 (probably an entity number)
1   driven by this console's player (the caller), then that player's 0x24 if set
```

The values come from the vehicle's physics body when it has one (VMX128 loads, so that path isn't checked yet) and from its entity otherwise.

### 2.5 All message types

Handlers from the `MPPlayerManager` constructor 0x82405D50 (type: handler); senders are the functions that construct each type.

| Types | Handlers |
|---|---|
| 0x1E-0x27 | 1E 823E4D80, 1F 823F42A0, 20 82405A88, 21 82405158 (HandlePlayerSetup), 22 823EC2E8, 23 823F9528, 24 823F9440, 25 823FE998, 26 823E1CD8, 27 82400308 |
| 0x28-0x31 | 28 823FFEB0, 29 823FFDF0, 2A 823F8F88, 2B 823E4560, 2C 823FCD18, 2D 823FC5B8, 2E 823FBF78, 2F 823E34F0, 30 82404DD0, 31 823EC538 |
| 0x32-0x3B | 32 823E3448, 33 823F38F0, 34 823F9E48, 35 823FBC28, 36 823D86C8, 37 823E31E8, 38 823FDAD8, 39 823FBB48, 3A 823E2DB8, 3B 823E28F0 |
| 0x3C-0x45 | 3C 823E23A8, 3D 823E21C0, 3E 823E1EC8, 3F 823E2630, 40 823E2CB8, 41 823E33B8, 42 823F8BE0, 43 823EB8F0, 44 823F8B28, 45 823F7060 |
| 0x46-0x4F | 46 823E3298, 47 82403610, 48 82403478, 49 823FE1C0, 4A 823FDE18, 4B 823FFC18, 4C 823F8658, 4D 823FE748, 4E 823FC930, 4F 823FA0F0 |
| 0x50-0x59 | 50 823D8828, 51 823E41F0, 52 823EF610, 53 823EBFA8, 54 823EF4F8, 55 823E3CE0, 56 823E3B88, 57 823E3A78, 58 823E39C0, 59 823DCC00 |
| 0x5A-0x63 | 5A 823EBF08, 5B 823F4200, 5C 824049E8, 5D 823EBBE0, 5E 823D4048, 5F 823E4358, 60 823EC140, 61 823FB298, 62 823F53F8, 63 823F5280 |
| 0x64-0x72 | 64 823F3C10, 65 823FF680, 66 823E3F18, 67 824004F8, 68 823E3DC8, 69 823F87A8, 6A 823E3860, 6B 823E36E8, 6C 823E4708, 6D 823E0E28, 6E 823E0DE0, 6F 823D8788, 71 823E0EE0, 72 823FF020 (no 0x70) |

0x59 (sent by `MPPeer::vfunc_7` 0x823ED0E0) carries two blobs of 8 and 16 bytes, probably the session's id and key. The host's game-state messages (scores, round start: "*COMMON* Sending gamestate ...", 0x82202E20) are not traced yet.

## 3. Inside one console: Quake III's layer

Kept exactly as the console has it in `cod3_game::net::{msg, adr}` (checked against both programs), since the Rust engine's own local server and split-screen clients can work the same way.

- **`msg_t`**: +0 overflowed (never read), +4 data, +8 maxsize (`MSG_Init` 0x8247F2B0 always sets 0xC00), +0xC cursize, +0x10 readcount, +0x14 bit (unused). Integers are written and read inline, big-endian, a byte at a time; no Huffman coding, no bit packing. **No write checks the buffer's size.** Reads past `cursize` give -1 (long), 0xFFFF (short, unsigned) or 0xFF (byte). `MSG_WriteString` 0x82484660 writes the bytes unfiltered (Quake III replaced `%` and bytes above 127) and a single 0 for a string of 256 characters or more; `MSG_ReadString` 0x8247F020 searches for the terminator without a bound, returns one shared static buffer, and on a string that is too long or not terminated before `cursize` returns an empty string without moving on.
- **Netchan header** (big-endian): a 32-bit sequence; from clients a 16-bit "qport" that is really the local client index (0-3), which the server uses as the client slot without checking the address. No fragments, no acknowledgment in the header; at most 2,971 bytes of payload. `Netchan_Process` 0x82484308 drops any packet whose sequence isn't higher than the last (signed comparison).
- **Server → client:** a 32-bit reliable acknowledgment, then ops: 1 nop, 5 server command (32-bit sequence + string; 64 slots, the client keeps 127 characters), 7 snapshot (32-bit server time + a flags byte; no entities or player state: `CL_ParseSnapshot` 0x82528258 copies the player state straight out of the server's memory), 8 end. Anything else is "Illegible server message".
- **Client → server** (`CL_WritePacket` 0x82525088): server id, message acknowledgment, reliable acknowledgment (32-bit each), then op 2 (32-bit sequence + string) for every unacknowledged command, then op 3. No move op.
- **`usercmd_t`** (0x30 bytes, memory only): server time, buttons, weapon, three angles as `(int)(angle * 182.044 + 0.5) & 0xFFFF` in 32-bit words, forward and right moves as clamped signed bytes, five floats of unknown meaning.

## 4. What a Rust peer must match, and what it must not copy

Match exactly: the bit order and the header bit, every range and precision and both rounding formulas (2.3), the timers and the baseline counter (2.4), single-precision arithmetic with truncating conversions (the console's `fctiwz`), the 16-bit angle formula.

Don't copy (check instead, and treat the console's behavior as something to defend against):
- the position update's handler takes the player index from the payload and only refuses local players; it doesn't check that the index belongs to the console that sent it;
- read errors set one global flag (0x82B793E6) that later reads test, so one bad message can affect the next: a position delta read while it is false reads no coordinates and leaves each moved coordinate at twice the baseline's;
- the Quake III layer's missing size checks, unbounded string search and static string buffer (3).

## 5. Not mapped yet

- `bdPacket` / `bdDataChunk` framing of a message, and how game data and voice share a datagram.
- The payloads of the other ~80 message types, especially the host's game state, damage and kills (who decides a hit is not known yet), chat, votes and session control.
- The rest of the use code (0x823DDB08), the meaning of the flags word (client +0x2C), client +0x520 and the "1 to 5 enemies near" rule (0x823E57D0).
- The four-lane sine and cosine (0x820B3920) and the vehicle physics path: VMX128 code, which needs VMX128 in the checking emulator first.
- How joining works end to end (0x0A/0x0B and the game's 0x1E-0x21).
