# The Rust LAN server for Call of Duty 2 and United Offensive

`iw-server` (`engine/tools/iw_server`, library `iw_pc::server`) is a game server written in
Rust that the games' own PC clients can find and join: a CB or Steam copy of **Call of Duty 2
1.3** or **United Offensive 1.51**, on Windows, or on Linux under Proton or Wine. It is the first
step of cross-play between the original PC games and the Rust version
([PC_MULTIPLAYER.md](PC_MULTIPLAYER.md)).

Everything it puts on the wire goes through the ported network code, and every one of those
functions is checked against the game's own machine code (`tools/decomp/x86/`): message bits,
Huffman coding, out-of-band packets, the challenge and connect checks, the netchan with its
scrambling, the delta coding of entities, clients, the player state and player commands, and
the writers of a client's reliable commands and of a snapshot's entity and client lists, the
snapshot writer and the game state sender (`SV_SendClientGameState`, whole: the client primed,
the message, the fragments around it).

## What works and what doesn't yet

| | |
|---|---|
| LAN browser (`getinfo`), server status (`getstatus`) | yes |
| Joining: `getchallenge`, `connect`, the game state, entering the game on the first player command | yes (tested end to end against a stand-in client built from the checked pieces, and over UDP with `iw-join`, the Rust client; not yet against a real CB client) |
| Snapshots: the player's own state, the player list, each a change from the last one the client acknowledged | yes; the player is a still spectator at the map's origin. The stand-in client reads them twice: by hand with the checked delta readers, and with the client's own message parser (`iw_pc::client`, checked against both games), which must find the same; a second run sends the client's messages with the client's own sender (`client::write_packet`, checked too) |
| Reliable commands both ways, `disconnect`, time-outs | yes |
| The game itself: spawning, movement, weapons, scores, game types | **no**: that is the game module (scripts and game code), still to be ported |
| Entities (other players moving, doors, items) | no (nothing runs that would make them) |
| Downloads, `sv_pure`, PunkBuster, master server, authorize server | no (LAN and direct `connect` only; every player has identity 0) |
| United Offensive's config string numbers 3-14 | taken from Call of Duty 2's; to check against UO's game module |

So today a CB client should list the server, connect, load the map and float there as a
spectator, staying connected. Anything past that needs the game module.

## Running it

```
cargo build --release -p iw_server            # in engine/
iw-server --game cod2 --map mp_toujane
iw-server --game uo --map mp_arnhem --port 28960 --hostname "Rust UO" --max-clients 8
```

Options: `--port` (28960 by default), `--hostname`, `--gametype` (`dm` by default), `--max-clients`,
`--motd`, `--set <key> <value>` (adds or replaces a server info key), `--config-string <index> <text>`,
`--exe <path>`.

**The game's executable** (`CoD2MP_s.exe`, `CoDUOMP.exe`) is needed for the protocol's tables
(the Huffman frequencies and the delta field tables are game data, so they are read from your
copy at start and checked by hash, never shipped). It is only read, never run, copied or
changed. Without `--exe`, `iw-server` looks in the folders in `IW_PC_INPUT`, then in your Steam
libraries (Windows and Linux).

## Testing with your own CB copy

You run the game; nothing here ever does.

1. Start `iw-server` on any machine on your network (or the same one).
2. Start the game's multiplayer (`CoD2MP_s.exe` / `CoDUOMP.exe`). On Linux, run it with
   Proton (GE-Proton, Proton-CachyOS) or Wine as you normally do; `newcopy find` lists the Proton
   builds it sees.
3. Join: the LAN tab of the server browser should show the server's name; or open the console
   (`~`) and type `connect <address>:28960` (`connect 127.0.0.1:28960` on the same machine).
4. What to report back if it fails: the last lines of the game's console (`condump file.txt`
   writes them to a file) and the server's output.

`iw-join` (the Rust client's network code, [PC_MULTIPLAYER.md](PC_MULTIPLAYER.md)) joins a
server and stays in the game, printing each step: `iw-join --game cod2 --server
127.0.0.1:28960 --seconds 30`. Against `iw-server` it shows the server side works without
the game; against your CB copy's dedicated server it is the other half of cross-play. It
sends `cp` with no archives after the game state, which a pure Call of Duty 2 server (the
default, `sv_pure 1`) accepts, so the CB server needs no `sv_pure 0` for it; a pure United
Offensive server wants more (PC_MULTIPLAYER.md, section 8), so start that one with
`+set sv_pure 0`.

**Optional, most useful:** a packet capture of a real session with the original server, so the
Rust server can be compared byte for byte. Start the original dedicated server
(`CoD2MP_s.exe +set dedicated 1 +set sv_pure 0 +map mp_toujane`), join it with the client, and
record the UDP traffic on port 28960 with Wireshark or `tcpdump -i any -w cod2.pcap udp port
28960`. The capture holds only the game's own messages on your network.

## How it works

`iw_pc::server::Server` has no sockets: `packet()` takes a packet that arrived, `frame()`
advances the clock (20 server frames a second, `sv_fps`), and both hand the packets to send to
a callback. The binary is a UDP loop around it; `crates/iw_pc/tests/server_session.rs` drives a
whole session with a stand-in client, which reads the game state and every snapshot with the
same checked code a Rust client will use (`iw_pc::net::delta`'s readers).

The session, as the originals' `sv_client_mp` and `sv_snapshot_mp` run it:

1. `getchallenge` → `challengeResponse <n>` (the challenge table of `SV_GetChallenge`).
2. `connect "<userinfo>"` (United Offensive Huffman-codes it) → the checks of
   `SV_DirectConnect` (protocol, challenge, slots) → `connectResponse`; the slot is *connected*.
3. The client's first message carries server id 0: the server sends the game state (config
   strings, the client number, the checksum feed), the slot is *primed*.
4. While the client loads the map, each frame sends only the last client command and the end
   marker (as `SV_SendClientSnapshot` does for a primed client).
5. The client's first player command for this map makes the slot *active*; snapshots follow,
   each a change from the last one the client acknowledged.

Player commands are keyed with the checksum feed, the message acknowledge and the hash of the
acknowledged server command (`Com_HashKey`); client messages are scrambled with the challenge,
server id and message acknowledge, and the acknowledged command's text
([PC_MULTIPLAYER.md](PC_MULTIPLAYER.md), `iw_pc::net::netchan`).

### Config strings sent

| Index | What | Evidence |
|---|---|---|
| 0 | server info: `g_gametype`, `gamename`, `mapname`, `protocol`, `shortversion`, `sv_hostname`, `sv_maxclients` and the usual `sv_*` keys | read by `CL_ServerInfoPacket`, `CL_DownloadsComplete` (`mapname`, `g_gametype`) and the client game |
| 1 | system info: `sv_serverid`, `sv_pure 0`, empty archive lists (`sv_iwds` ... in CoD2, `sv_paks` ... in UO), `sv_cheats 0` | the keys `CL_SystemInfoChanged` reads |
| 2 | `cod` | `CG_Init` stops with "Client/Server game mismatch" otherwise (both games) |
| 11, 13, 14 | north yaw `0`, level start time, message of the day | set by the game module's world spawn; indices from the CoD2 server reconstruction (understanding only), to verify |

## Next

- Check with a real CB client (the owner) and compare against a capture of the original server.
- The game module: spawning, `ClientThink` (movement: `PM_*`/`BG_*`), weapons, game types.
- Entity snapshots (`SV_BuildClientSnapshot`: visibility from the map's PVS).
- United Offensive's config string numbers from its game module.
- The client side is ported and checked ([PC_MULTIPLAYER.md](PC_MULTIPLAYER.md), section 12,
  item 8): `iw-join` joins `iw-server` and sends `cp` for pure servers; next, joining a CB
  server (the owner).
- Config string changes for the clients (`SV_SetConfigstring`): `d <index> <text>` with the
  text unquoted (the client takes the rest of the line as it is), long ones as `x`, `y`, `z`.
