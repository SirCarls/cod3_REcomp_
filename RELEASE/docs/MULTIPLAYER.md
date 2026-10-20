# Multiplayer: what the disc has and the plan

**Status (2026-10-16): the multiplayer program is built.** The setup tool
now recompiles `codmp_xenonf.xex` as a second program, `cod3mp`, next to the
campaign (section 4.1), and the campaign's Multiplayer item starts it; its
own way back starts the campaign. System link uses the SDK's network layer
(patch 0025). It compiles and starts in the cloud; it has **not yet been run
with the disc's multiplayer files** (the cloud copy has no `assets/mp`), so
the first match is still to be played (section 4.11). Before this version,
picking Multiplayer showed "Multiplayer is not available in this version
yet." This page records what Call of Duty 3's multiplayer is made of, what
works without Xbox Live, what the SDK does with its network calls, and the
plan to get Windows and Linux players into the same match. The addresses and
evidence behind each statement are in `dev/agents/multiplayer/NOTES.md`.

The owner's rules for multiplayer (2026-10-10/11):

- Windows and Linux players play together, peer-to-peer or on servers.
- The same network bytes on both systems.
- A version check on the game and the port's build, not on the operating
  system.
- Only the host (admin) can turn `sv_cheats` on in a match, and when they
  do, every player is told in a column of its own next to the kill feed.
- (2026-10-13) One match mixes a custom Xbox 360 copy, the recomp and the
  Rust engine, and the pool of players is as large as possible (section 4.9).

## 1. What multiplayer is on the disc

Multiplayer is a separate program, `codmp_xenonf.xex` (SHA-1
`907985e2f1bdea255b52226ef39acf627c2d7ffc`, built 2006-10-09, same title
ID as the campaign). It is loaded at the same address as the campaign's
`default.xex` (0x82000000), so the two can never share one process: on
the console one program replaces the other, and in the port they will be two
programs that start each other.

Inside, it is the same engine as the campaign (Quake III style `Cmd_`,
`Cvar_`, `SV_`, `CL_`, `Netchan_Transmit`, server snapshots, the same script
thread scheduler and CRT) with three additions:

- **Demonware** (`bdNet`, `bdSession`, `bdUnicastConnection`, `bdSocket`,
  LAN discovery `bdDiscoveryClient` / `bdDiscoveryServer`, `bdQoSProbe`): the
  session and transport layer under the game's netchan.
- **Xbox Live front end**: player and ranked matches, lobby, leaderboards,
  stats, achievements, invites, voice.
- **Game modes** compiled into the program (no map DLLs, unlike the
  campaign's levels): Deathmatch (`mp_dm`), Team Deathmatch (`mp_tdm`),
  Capture the Flag (`mp_ctf`), Single-Flag CTF (`mp_scf`), Headquarters
  (`mp_hq`) and War (`mp_war`). Maps are data (`mp\global.cod`, one `.cod`
  per map).

**Network model.** One player hosts a listen server: the host runs the game
and its modes ("must be host" to accept a join, `Host_PickInitialPoints`),
the other players are clients that send their input and receive snapshots
(`SV_BuildClientSnapshot`, `CL_GetSnapshot`, `CL_ParseServerMessage`). All
traffic is UDP; the program has no TCP at all (only `socket`, `bind`,
`sendto`, `recvfrom`, `setsockopt`, `ioctlsocket`). The host is the
authority, so clients never need to simulate the same thing bit for bit.

**Correction (2026-10-13, the Rust decomp's network mapping,
[MP_PROTOCOL.md](MP_PROTOCOL.md)):** that Quake III client/server layer
never leaves the console. Every console runs its own server, and the
netchan only links it to that console's own split-screen players through
in-memory queues. Between consoles, Demonware's peer-to-peer session
carries bit-packed messages, and every console sends the state of its own
players to every other console. The host's extra role (joining, game
state) is still being mapped.

**The hand-over from the campaign.** The main menu's Multiplayer item runs
`load_mp_exe`: the campaign stores the player's 664-byte controls block with
`XamLoaderSetLaunchData` and asks for `d:\codmp_xenonf.xex`. Multiplayer
reads that block back at start (0x82415AA0), and its own `reboot` (0x82531E20)
does the reverse: it stores the block and asks for `default.xex`.

## 2. What works without Xbox Live

| Mode | What it needs | In the port |
|---|---|---|
| **Splitscreen** (up to 4 on one screen) | Nothing from the network | The first thing to run once the multiplayer program runs. |
| **System link** (LAN, 24 players) | UDP sockets, a LAN broadcast to find games, the console's secure-address and key calls (`XNetCreateKey`, `XNetRegisterKey`, `XNetXnAddrToInAddr`, `XNetConnect`), and the session calls, which need no server for a local session | **The route to cross-platform play.** No Microsoft or Demonware server is involved. |
| **Xbox Live** (player and ranked match, lobby, leaderboards, stats, achievements, voice, gamer cards, marketplace) | Microsoft's matchmaking, arbitration and stats servers, and Demonware's STUN servers | **Won't work.** These servers are gone, and the port never contacts them. Those menus get a "not available" message. |

Everything you play (maps, modes, weapons, netcode) is shared by system link
and Live; only the Live menus, lobby, leaderboards and stats are Live-only.
Counting the game's named C++ classes as a rough measure: about 290 methods
belong to the Live menus and leaderboards, about 100 to the system link and
splitscreen menus, and about 430 to the shared session, scoreboard and
in-game screens.

## 3. What the SDK does with the network calls today

**Update 2026-10-12:** SDK patch 0025 fixes everything in the "missing or
wrong" list below except voice and the other Live features: socket options,
`FIONBIO`/`FIONREAD` and Winsock error codes on Linux and Windows; `sendto`
and `recvfrom` addresses (they were byte-reversed on every host, so a send to
192.168.1.20 went to 20.1.168.192); the PC's real LAN address as its system
link address (the `net_interface` setting picks the interface, otherwise the
default route's), with the hardware-address field made from the IP so every
PC computes the same identity for a peer; working address, key, connect and
QoS calls for system link; `XNetRandom` from the system's secure random
source; a link reported while an interface is up. Traffic is LAN-only by
default (`net_lan_only`): only loopback, private, link-local and the chosen
subnet's addresses. Every guest pointer and length is checked. It is built
and unit-tested on Linux and compiled for Windows; it can only be tried in a
match once `cod3mp` exists. Steps 1-3 of milestone 1 below are this patch.

The SDK (Xenia's kernel code) maps the console's network library onto the
host's sockets. What exists:

- `socket`, `bind`, `sendto`, `recvfrom`, `closesocket`, `setsockopt`,
  `ioctlsocket`, `WSAStartup` are implemented on host sockets. The console's
  VDP protocol (254) becomes plain UDP; the console's socket encryption is
  skipped (both ends are the port).
- The session calls (`XSessionCreate`, join, start, search, write stats ...)
  are logged and report success without doing anything.

What is missing or wrong:

- **Linux: socket options and modes.** The SDK passes the console's Winsock
  numbers straight to the host. Multiplayer asks for `SO_BROADCAST`
  (`setsockopt(s, 0xFFFF, 0x20)`) and a non-blocking socket
  (`ioctlsocket(s, 0x8004667E)`, FIONBIO). That is right on Windows, whose
  numbers are the console's, but on Linux both calls fail: no broadcast (no
  LAN discovery) and a blocking socket (the network code would wait forever
  on an empty socket). After a failed call the Linux build also reports
  error 0 instead of "would block". **This has to be fixed for Linux players
  to play at all** (an SDK patch translating the options, the ioctl and the
  error codes; Windows unchanged).
- **The player's address.** `XNetGetTitleXnAddr` answers 127.0.0.1 with the
  same made-up hardware address for everyone, so all players look alike.
- **Secure addresses and keys.** `XNetXnAddrToInAddr` / `XNetInAddrToXnAddr`
  report failure; `XNetCreateKey`, `XNetRegisterKey`, `XNetUnregisterKey`,
  `XNetConnect`, `XNetGetConnectStatus`, `XNetTsAddrToInAddr`,
  `XNetUnregisterInAddr`, `XNetQosLookup` are empty stubs that return an
  error value; `XNetQosListen` fails.
- **Random numbers.** `XNetRandom` fills every buffer with 0xBB, so every
  player would make the same session keys and nonces.
- **Network cable.** `XNetGetEthernetLinkStatus` says there is no link; the
  system link menus check it every second (0x823DF538).
- Voice, gamer cards, stats enumeration and marketplace are stubs (fine to
  leave: they are Live features).

## 4. The plan

### 4.1 Two programs: `cod3` and `cod3mp`

- The setup tool recompiles `codmp_xenonf.xex` as a second target,
  **`cod3mp`**, next to `cod3`, for Windows and Linux, from its own manifest
  and config (`recomp/config/codmp.toml`, readable names from
  `tools/name_mp_functions.py`). The port's own parts (settings window,
  console, HUD, input, logging) are shared; the campaign's hooks are not,
  because every address is different in the multiplayer program. Each one it
  needs is found again in the multiplayer image. The two that maps can't
  load without are already found: the script thread scheduler (0x824C5298,
  instruction for instruction the campaign's, with setjmp 0x82322010 and
  longjmp 0x8231E400) and the job wake-up (0x82141E40, the campaign's
  0x8212A100). Frame limiter, field of view, mouse aim and the rest come
  after.
- **Campaign to multiplayer:** `cod3::mp::MultiplayerAvailable()` is
  true when `cod3mp` is installed next to `cod3` and the game data has
  `mp\global.cod` (otherwise the game's message box says which is missing). The game's own
  `load_mp_exe` then runs, and the port starts `cod3mp` with the launch data
  the same way it already restarts itself for the game's reboot
  (`game/relaunch.h`, `userdata/launch_data.bin`), and ends the campaign.
- **Multiplayer to campaign:** the multiplayer program's own `reboot` asks
  for `default.xex`; `cod3mp` starts `cod3` the same way.
- **Dry run done (2026-10-11):** the SDK's codegen translates
  `codmp_xenonf.xex` completely after one config entry (a C++ "this"
  adjustor thunk, 0x82298D38): 23,358 functions, 90 C++ files, 92 MB, in
  20 to 30 seconds. The campaign is 95 MB, so a full build with multiplayer takes
  roughly twice as long to compile.
- **Built (2026-10-16).** What is in place:
  - `recomp/mp/`: the manifest (`cod3mp_manifest.toml`, reading the
    project's `assets/codmp_xenonf.xex`), `config/codmp.toml` (setjmp, 24
    function starts the analyzer can't find, the script thread resume hooks
    at the campaign's offsets, the `return` after the game modes' two yield
    calls), `config/names_mp.toml` (2,228 names from
    `tools/name_mp_functions.py`) and its own `CMakeLists.txt` and generated
    glue (the codegen writes `generated/rexglue.cmake` next to its
    manifest, so each program has a folder). `recomp/CMakeLists.txt` adds it
    when `assets/codmp_xenonf.xex` is there.
  - Game code shared with the campaign: script threads on host fibers
    (`game/script_threads.cpp`, the same hooks by the same names),
    `game/level_hooks.cpp`, the relaunch (`game/relaunch.cpp`, now for both
    programs), the scripted test controller and the interface theme.
    Multiplayer's own: `cod3mp_app.cpp` (paths, settings, exit) and
    `game/mp_hooks.cpp` (the job wake-up fix at 0x82141E40). A shared file
    may only hook a function by a name `tools/name_mp_functions.py` gives
    the same function in both programs, never by a `sub_` address (a test
    in `tests/test_multiplayer_build.py` checks it).
  - Settings: `cod3mp` reads the campaign's `cod3.toml` (keys, window,
    video, audio, network) and keeps its own changes in `cod3mp.toml`, so it
    never rewrites the campaign's file. The newer file wins (2026-10-16, the
    owner's "last change wins" rule): when `cod3.toml` was saved after
    `cod3mp.toml` (a change in the campaign or the setup tool), its values
    replace `cod3mp.toml`'s; otherwise only settings still at their default
    take them. The GPU plugin's settings are taken in a second pass once the
    plugin is loaded. Never taken: the campaign's display shape and render
    scales, texture packs (multiplayer allows only the insignia list), the
    1000 Hz vblank and the display gamma (both programs derive it from
    `cod3_color_space`). Its log is `userdata/logs/cod3mp.log`.
  - Controls: the SDK's keyboard and mouse mode with the campaign's binds;
    the mouse moves the right stick until the campaign's raw mouse aim is
    hooked here too. The campaign's F2 window, game console, frame limiter,
    native resolution and the rest hook `default.xex` and come one by one.
  - The setup tool copies `codmp_xenonf.xex` into the project when it is the
    known build (SHA-1 above), translates and compiles both programs, and
    packs `cod3mp` next to `cod3` (plain folder: `bin/cod3mp` and
    "Call of Duty 3 Multiplayer.sh"/".lnk"; AppImage: one file whose AppRun
    starts `cod3mp` when `COD3_PROGRAM=cod3mp`).

### 4.2 Milestone 1: system link between Windows and Linux, connect by IP

Goal: two PCs, any mix of Windows and Linux, on the same network (or one
with a forwarded UDP port) play a system link match.

1. **SDK socket layer** (patch): translate the console's socket options,
   `FIONBIO` / `FIONREAD` and error codes on Linux, so both systems behave
   like the console. Datagrams are copied byte for byte, never re-encoded.
2. **Network identity** (patch or `cod3mp` hooks): `XNetGetTitleXnAddr`
   gives the PC's real LAN address and port, plus a random id made once per
   install (in `userdata/`), in the fields the game uses to tell players
   apart.
3. **Secure addresses and keys**: system link between consoles already uses
   plain IPv4 addresses, so `XNetXnAddrToInAddr` hands back the peer's real
   address, and `XNetCreateKey` / `XNetRegisterKey` / `XNetConnect` succeed
   (keys from a real random source). `XNetRandom` uses the system's random
   source; `XNetGetEthernetLinkStatus` reports a link.
4. **Finding a game**: LAN discovery is a broadcast and finds games on the
   same network by itself once broadcast works. For anything else, a
   `connect <address>[:port]` console command in `cod3mp` asks that host
   directly and puts the answer into the game's own system link list, so
   joining still goes through the game's menus.
5. **Splitscreen** is tested at the same time (no network).

### 4.3 Later: play over the internet

In order of effort:

- **Direct by IP with a forwarded port** (milestone 1 already does this).
- **NAT traversal**: UDP hole punching through a small meeting server that
  only exchanges addresses. Someone has to run that server; the game asks
  the player before using any address it is given.
- **Relay**: a server that forwards the packets when hole punching fails.
  It always works but costs the server's bandwidth.
- **Dedicated server**: the Xbox 360 build has no dedicated server mode;
  the host is always a player. A headless `cod3mp` host that joins as a
  spectator may be possible. **Unknown** until the program runs.

No Microsoft or Demonware server is ever contacted. The program names
Demonware's STUN servers (`stun.*.demonware.net`), but it doesn't import the
console's DNS lookup, so it can't reach them by name; any address lookup the
port adds will refuse those names.

### 4.4 The same bytes on both systems

- The packets are written by the recompiled game code in its own big-endian
  memory, exactly as on the console, and the socket layer copies them
  unchanged. A Windows build and a Linux build of the same release send the
  same bytes. Nothing the port adds goes into the game's packets.
- The host decides everything that matters (hits, scores, flags), so small
  floating-point differences between machines can't split the game; clients
  only predict their own movement, as on the console. Anything the port
  ever adds to the simulation must be integer or otherwise exact on both
  systems. (Correction 2026-10-13: each console is the authority for its
  own players' movement and sends it to the others; who decides hits,
  scores and flags is not mapped yet. [MP_PROTOCOL.md](MP_PROTOCOL.md).)
- The socket fixes in 4.2 make Winsock and POSIX sockets behave alike.

### 4.5 Version check

- The game has its own check: a client compares the server's game version
  string with its own ("Client/Server game mismatch: %s/%s"), which only
  tells different game discs apart.
- The port adds a **build id**: a hash of the multiplayer program's SHA-1,
  the port's release version and the version of the port's own messages,
  the same for the Windows and Linux builds of one release. Hosts advertise
  it with their game in the system link list (games from another build show
  as "different version" and can't be joined), and the host checks it again
  before accepting a player.

### 4.6 The host's authority and the announcement column

- `cod3::mp::IsMatchRunning()`, `IsHost()` and `Announce(text)`
  (`game/multiplayer.h`) are the hooks the cheat policy
  (`game/cheat_guard.h`) and the texture rules already use. In the campaign
  program a match never runs: no match, this machine counts as the host,
  and an announcement shows only here. In `cod3mp` they will come from the
  game's session: a match runs while the server is up or a session is
  joined, and the host is the machine running the server.
- `sv_cheats`: only the host can change it in a match (`cheat_guard.cpp`);
  the game's server already decides whether cheat commands work, and a
  client can't change the server's dvars.
- `Announce(text)` on the host goes to every player in the match over the
  game's own reliable server-command channel, as a port command that only
  displays text. Each machine shows it in the HUD announcement column (next
  to the kill feed, `ui/announcement_feed.h`) and its console, as plain
  text, length-limited, never run as a command.

### 4.7 Basic protection against bad packets

- **Lengths**: datagrams longer than the game's own maximum or shorter than
  a packet header are dropped before the game sees them. The game already
  refuses oversize packets (`Com_EventLoop`) and oversize info strings.
- **No console commands from the network** except an allow-list: a command
  from another machine goes through the cheat policy as a network source
  (`cheats::Source::kNetwork`); only display commands like the announcement
  are allowed, anything else is dropped and logged.
- **Rate limits** on the host: packets per second per address, and join
  attempts per minute; above them, packets are dropped.
- **Names and announcements** are plain text with a length limit; color
  codes are shown as written.

### 4.8 Texture replacement in a match

Done separately (`docs/TEXTURE_REPLACEMENT.md`, `game/hd_textures.cpp`):
while `IsMatchRunning()` is true, only the world insignia on a fixed list
are replaced, everything else stays the game's own, so a texture pack can't
make walls or bushes see-through. It switches on by itself once `cod3mp`
reports a running match.

### 4.9 Three kinds of players: a custom Xbox 360 copy, the recomp and the Rust engine (2026-10-13)

The owner, 2026-10-13: "multiplayer will need to work between a custom xbox
360 copy, rust, and rexgluesdk"; asked whether that needs a complete
rewrite, or costs performance; "I really want to make the pool of players
as large as possible". Also: World at War weapons and models are not part
of CoD3's multiplayer (the World at War decomp is a study aid), and Call of
Duty: United Offensive's network protocol is a reference, with UO players
joining not a priority (different games).

**No rewrite.** The 360 and the recomp run the console's own multiplayer
code, so they already speak the console's protocol. The Rust engine has to
speak it too: its network code is ported from `codmp_xenonf.xex` function
by function and checked against the original with the decomp checker
([DECOMP.md](DECOMP.md)). What crosses the network is Demonware's
peer-to-peer messages, not Quake III's netchan ([MP_PROTOCOL.md](MP_PROTOCOL.md));
their bit encoding is ported and matches the original exactly.

**What has to match, and what doesn't.** Every console simulates its own
players and tells the others where they are, so these must be the same on
every platform:

1. the bytes on the wire: Demonware's packets and the game's bit-packed
   messages (player positions, views, vehicles, the host's game state ...),
   with the console's ranges and rounding;
2. the game rules that decide anything shared (damage, scores, flags; who
   decides them is not mapped yet) and the movement code, so a Rust
   player moves like a console player;
3. hashes, numbers and names the protocol refers to (config string
   indexes, the game version string of 4.5).

Rendering, sound, effects, menus, frame rate, threads and the rest of the
engine are its own, as fast as the machine allows.

**It doesn't cost performance.** Matching the console exactly costs little:
the exact versions are ported already and measured (2026-10-13): a vector
normalized in 22 ns (4 ns built for the player's CPU), the fused
multiply-add in 3 to 6 ns, an info string lookup in 20 ns. Movement for a
full match is microseconds per frame. Exactness would be expensive only if
the whole game had to run in lockstep, and this netcode doesn't.

**Each Rust player translates for each peer.** (Written for a central
server first; the network turned out to be peer-to-peer, where every
console sends its own players' state to every other one. The idea is the
same, per peer instead of per client.) A Rust peer keeps the full,
unlimited game and writes what it sends to each peer in that peer's own
dialect, decided when it joins (with the build id of 4.5):

| Toward a 360 or recomp player | How |
|---|---|
| Angles | The console's 16-bit form, `round(angle × 65536 / 360) & 0xFFFF` (the loss, 0.0055°, can't be seen). The other way is exact. |
| Info and config strings | A console view: the keys the console knows, values cut to fit, under 1,024 bytes (`cod3_game::engine::info::InfoString::fits_console`). A bigger one makes the console drop the connection. |
| Entity and player numbers | Renumbered per client into the console's ranges. |
| Update rate | A Rust peer may update faster; console peers get position updates on the console's 125 / 250 / 1000 ms timers. |

Rust players get the native dialect: no size limits, full-precision
angles, a faster update rate.

**What isn't translated, so everyone in a match shares it:**

- **The game rules** (movement, weapons, damage, timing). They're the
  console's in a mixed match; they cost microseconds.
- **The map.** Every player needs it; a 360 can only load the maps it has.
- **The console's player limit** (24 on system link) when console players
  are in the match. Hiding extra players from them would be unfair.

**Who hosts.** The 360 build has no dedicated server (4.3), so a Rust
dedicated server is the natural host of a mixed match, if the session's
host role can be played without a player of its own (to be checked once
the host's messages are mapped). When a 360 or recomp player hosts, Rust
players join it in the console dialect.

**The 360's side.** A retail console's system link goes through its secure
network layer (keys, VDP packets); the recomp skips the encryption because
both ends are the port (section 3). With a real console in the match,
every player has to speak what the custom copy speaks: the plan is that the
custom copy sends the same plain UDP packets as the recomp. That change
touches the console's network security, so it is the owner's decision. This
project can write such a patch but never runs it; trying it on a console is
the owner's.

**The custom copy runs on both a console and an emulator** (owner,
2026-10-13 03:32: "the custom xex should work on both xbox 360 and an
emulator (xenia canary can be your testing ground)"). Xenia Canary and a
real console can't play system link together today: the console encrypts
it, Xenia doesn't (TeamXLink's Xenia page lists "Xenia users and Xbox 360
users playing together" under what doesn't work). The custom copy's plain
UDP packets remove that difference for both. One set of changes, written
twice: as a patch file in Xenia Canary's own format for the emulator, and as
the same changes for the console. Xenia Canary's main builds exist for
Windows and Linux (its netplay fork for Windows only).

**Xenia Canary as the test bench** (owner, 2026-10-13 ~03:35, an exception
to "neither game's executable is run"): a pinned Xenia Canary build in the
cloud workspace (`work/tools/xenia-canary/`), running only the multiplayer
program (original and custom copy), only for tests. Running the whole game
in an emulator costs a lot (no GPU in the cloud, 2 cores), so the everyday
tests are the artificial ones in 4.10, and Xenia runs at milestones
(owner, 05:00: "devise tests where you can artificially test these. as
computing this would be expensive").

### 4.10 Testing cross-platform play without running the game (2026-10-13)

Four tiers, cheapest first. Tiers 0 to 2 need no GPU, no maps and no
emulator of the whole game: only the multiplayer executable in
`dev/decomp-input/` and the function oracle of [DECOMP.md](DECOMP.md),
which runs single functions of the original.

**Tier 0: every build, seconds, no game files at all** (`cargo test`, any
machine):

- **Golden answers** (built): `python3 tools/decomp/decomp.py golden`
  records the original's results for every function that touches floats
  (56 functions on 2026-10-13 evening, including the three player
  messages written and read whole; 15 to 100 random cases each) into
  `engine/tools/decomp_check/golden/`; `cargo test -p decomp_check`
  replays them through the Rust ports wherever it runs. A system that
  computes one bit differently (another CPU, compiler, C library) fails
  here instead of drifting out of step in a match.
- **Hard fused multiply-add cases** (built): 48 single- and 48
  double-precision cases where rounding twice gives the wrong answer, with
  exact answers (`tools/decomp/fma_cases.py`). `ppc::fmadds` must get all of
  them right, and so must the platform's `f64::mul_add` that the C runtime's
  `sin`/`cos` port uses. A C library that rounds through the x87's 80-bit
  format fails this.
- **Translation properties** (with the translating host): console → native
  → console is the identity; nothing sent to a console player exceeds its
  limits (`fits_console`, angle and entity number ranges); the same input
  gives the same packet.
- **Packet fuzzing** (with the network ports): random and mutated packets
  never make the Rust parser panic, allocate without bound or take more
  than linear time.

**Tier 1: per change, about a minute** (`decomp.py check`, the original
functions in the oracle):

- **The multiplayer program's own copies** (built: `decomp.py check --image
  codmp`): the ports against `codmp_xenonf.xex`'s copies of the same
  functions, found by `tools/name_mp_functions.py`. 19 of 19 match (info
  strings, string compares, hashes, `sin`/`cos`), so those ports serve the
  network code as they are.
- **Every network function** against the multiplayer program on
  thousands of random inputs. Built: Demonware's bit buffer (27
  functions), the message fields (positions, angles, directions,
  velocities; 22), the position, view and vehicle updates, each writer
  and reader (2026-10-13 evening), and Quake III's message and address
  functions. The game structures a message reads are built per case, and
  the few game functions it asks that aren't ported answer the case's
  value. Next: the rest of [MP_PROTOCOL.md](MP_PROTOCOL.md) 2.5, and
  VMX128 in the oracle (the velocity reader's sine and cosine).
- **Round trips**: the original writer against the Rust reader and the
  Rust writer against the original reader, on random player and vehicle
  states, including malformed messages (both sides must refuse the same
  ones). Started: the Rust writer to the Rust reader for all three
  messages (`cargo test`), and every reader already sees random and
  truncated bit streams in its checks.

**Tier 2: occasionally, minutes, still no game running:**

- **Loopback lab**: three peers on local UDP behind a deterministic
  impairment layer (loss, reordering, duplicates, delay): Demonware's
  connection and session code running in the oracle (its socket imports
  replaced by stand-ins that hand datagrams to the lab), the Rust
  Demonware layer, and the recomp's socket layer (SDK patch 0025 and its
  tests). They must agree on the handshake, the data chunks and the
  messages that find and join a game.
- **Artificial match**: Rust peers with scripted bots, and a "console
  peer" made of the original message handlers in the oracle. Each update
  the console peer must decode what the Rust peers meant to send, after
  the console's quantization (positions to a unit, 16-bit angles, the
  console view of strings). Thousands of updates without a renderer.
- **Movement replays**: the original `Pmove` in the oracle against the Rust
  port, on synthetic worlds (a floor, a step, a slope, a wall) supplied
  through the movement code's trace callback, with random command
  sequences; the player state must match bit for bit after every frame.
- **The custom copy's patch**: static checks (it applies only to the right
  module hash, changes only the intended functions, every branch lands
  inside the module, everything disassembles), then the patched functions
  run in the oracle with their network calls recorded (a plain UDP socket,
  a send to the address given, no secure-key calls).

**Tier 3: milestones only, expensive:** Xenia Canary in the cloud with the
custom copy against the recomp and a Rust host; the owner on a real
console.

**United Offensive as a reference.** The owner has Call of Duty and United
Offensive for PC (`CoDMP.exe`, `CoDUOMP.exe` and the `gamex86`,
`cgamex86`, `uix86` DLLs, plain x86 laid out like Quake III). CoD3's
network code very likely comes from the same line, so they can name and
explain the netchan, packet and snapshot code before CoD3's is ported: names
and understanding only, as with Quake III; nothing copied or run, and
PunkBuster and the key check are left alone.

**Reconstructed sources from the same family** (owner, 2026-10-13): [Open CoD:UO](https://github.com/opencoduo/coduomp) (United Offensive multiplayer client, server and game modules) and [OpenCoD1](https://github.com/opencod1/opencod1) (Call of Duty 1.1 multiplayer). Neither has a license file, so they are read for understanding only, like Quake III, and nothing is copied (SOURCES.md). A first comparison with CoD3's multiplayer program: the game layer matches the family (`CL_ParseServerMessage`, `CL_GetSnapshot`, `CG_ProcessSnapshots`, `CL_GetUserCmd`, Huffman tables), but the join and transport don't: there are no `getchallenge` / `connectResponse` packets; Demonware's peer-to-peer layer (`bdSocket`, `bdUnicastConnection`, "Connected to host (%s), sending join request", "Connected to all peers. Local peer index is %u") joins the players, and then the host runs the game. So these sources help with snapshots, user commands and the game modules; the join follows CoD3's own Demonware code.

### 4.11 Playing system link (2026-10-16)

What to do on each PC, once the setup tool has built both programs:

1. Start the game as usual and pick **Multiplayer** in the main menu (or
   start `Call of Duty 3 Multiplayer` directly). The campaign closes and
   `cod3mp` opens in the same window size.
2. Choose **System Link**: one player creates a game, the others find it in
   the list (LAN discovery is a broadcast on UDP port 50000; the game's own
   traffic goes over UDP port 3074).
3. Back to the campaign: when multiplayer hands back to the campaign (the
   game's `reboot` command, which asks for `default.xex`), `cod3` starts.

Notes:

- **Same network.** Both PCs must be on the same local network. By default
  the port only talks to local addresses (`net_lan_only`, on); the address
  it uses is the default route's interface unless `net_interface` names
  another (an interface name, or an IPv4 address of this PC). Both are SDK
  settings (F4 in `cod3mp`, or `cod3.toml`).
- **Firewall.** Allow UDP 3074 and 50000 in, for `cod3mp` (Windows asks the
  first time a game is hosted).
- **Two copies on one PC** can't both host or find games: both would use
  ports 3074 and 50000.
- **First match not played yet.** The program compiles and starts; it has
  not been run with the disc's multiplayer files, so menus, map loads and a
  match on two PCs are the next things to try. What to send back if
  something goes wrong: `userdata/logs/cod3mp.log` from each PC.

### 4.12 The campaign's port features in `cod3mp` (2026-10-16)

Owner, 03:26: "Please make sure all the patches we made for sp can be used
within mp."

**How.** The same `recomp/src` files are compiled into both programs, but
the two programs put their functions and data at different addresses, so
`cod3mp`'s build takes its addresses from one reviewed table,
`recomp/mp/config/campaign_map.tsv` (tab-separated: symbol, campaign
address, multiplayer address, status, registers, jump, why, MP function).
A row's status is `mp` (its multiplayer counterpart, checked), `same`,
`dead` (campaign only, never runs in `cod3mp`; the why column says why),
`later` (it has a counterpart, but that feature isn't on in `cod3mp` yet) or
`mp-code` (`cod3mp` does this in its own code). Every pair was checked
against both programs' code (function pairing, then by hand where the
pairing wasn't certain); the evidence is in the why column.
`tools/mp_aliases.py --write` makes four files from the table (never edit
them by hand):

- `recomp/src/game/mp/mp_aliases.h`, force-included into `cod3mp`'s sources
  only: each campaign function name a shared file hooks or calls becomes the
  multiplayer function's name.
- `recomp/src/game/mp/addresses_mp.h`: `addresses.h`'s constants with their
  multiplayer values (`addresses.h` uses it when `COD3_MP` is defined). A
  constant with no multiplayer value is left out, so a `cod3mp` file that
  uses it doesn't compile.
- `recomp/src/game/mp/campaign_only.cpp`: a stand-in for each campaign-only
  function a shared file names. It never runs; if it did, the log would say
  so once.
- `recomp/mp/config/codmp_campaign.toml`: the campaign's mid-asm hooks at
  their multiplayer sites (`cod3mp_manifest.toml` includes it).

An address written in a `.cpp` file goes through
`COD3_ADDR(campaign, multiplayer)` (`recomp/src/game/program.h`, which also
gives `cod3::kMultiplayer`). `python tools/mp_aliases.py --check` (also run
by `tests/test_multiplayer_build.py`) fails on a stale generated file, an
unreviewed row, a hook without a row, or a raw campaign address in a file
`cod3mp` compiles; `--report` lists what binds how. After changing the
port code, run `--check`; a new hook or address needs a row first. The
campaign's build doesn't change: its compiled objects were compared byte for
byte before and after.

**In `cod3mp` now** (cloud runs in the front end and a local match on Verdun,
spawned and moved):

- Frame rate limit and pacing (the hook at 0x82557E98), the 1000 Hz guest
  vblank, `com_maxfps` from the console config, the performance line, the
  thread and function profilers, the machine profile, the watchdog at the
  multiplayer program's sites.
- Native render resolution, shadow quality, the ultrawide picture (two
  scene-aspect sites; not tried on a wide window yet).
- Field of view: the campaign's limits (`cg_fov 170` becomes 160 with a
  note), saved per program (`cod3mp_config.cfg`), multiplayer's own default
  of 80. In multiplayer `cg_fov` only exists once a map is loaded (it is a
  cgame dvar there): set it in a match or in F2.
- The port's crosshair (mid-asm hooks at 0x8245DD8C and 0x82452E38).
- F2, the ~ console and the full console, `cod3_report`, `cg_drawFPS`, the
  game's own prints in the log.
- The cheat guard: cheat commands and cheat-protected dvars are refused while
  `sv_cheats` is 0, and the game's own `sv_cheats 1` at every map load
  (`SV_Map_f`) is undone. In a match nobody can turn cheats on yet: every
  console runs its own server (section 1), so "this machine runs the server"
  doesn't make it the host, and until `cod3mp` reads the Demonware session's
  host flag (and has a channel to every player) no machine counts as the
  host (4.6).
- Texture replacement: in a match only the insignia list of the user pack
  (4.8); `cod3_hd_compare` is campaign only.
- Screenshots (`screenshot`, the debug log's shots): the game's own picture
  only.
- The main menu: DOWNLOAD CONTENT becomes QUIT TO DESKTOP (press A twice
  within 20 s), and XBOX LIVE says it isn't part of the PC version.
- Keyboard movement. Raw mouse aim is compiled in but untested: in the cloud
  a match frame takes 5-14 s, longer than the 1.5 s the mouse code allows
  between frames, so the cloud can't show it.

**Not in `cod3mp` yet (why):** the film skip (no films in multiplayer),
battle actions and the QTE spinner (none in multiplayer), the option rows in
the game's own menus (FOV, VIDEO, BRIGHTNESS), menu mouse control and button
prompts (multiplayer's menu classes have other fields and virtual functions,
and its option pages are its own `MP*` classes), Sticky Aim (kept per
controller profile in multiplayer), the voice language (multiplayer's own
sound banks), the decomp trace build, the level DLL hooks (multiplayer has no
level DLLs). F2 has the same settings meanwhile.

**Known problems:**

- **No sound output device:** creating or joining a match stops the game.
  The game's voice and session object (0x82A9ACB8) is made by the sound
  system's start-up and stays empty without a device. `cod3mp` says so (log
  and a HUD line) 15 s after the main menu appears.
- QUIT TO DESKTOP isn't on the pause pages yet; in a match, the console's
  `quit` and the window's close button work.
- The garbage bytes in the log line when multiplayer hands back to the
  campaign came from the SDK (`XamLoaderLaunchTitle` kept a view of a freed
  string): SDK patch 0043; `relaunch.cpp` also prints only the file name now,
  with odd bytes as `\xNN`.

### 4.13 The campaign's keys, controls and crosshair in `cod3mp` (2026-10-19)

Owner, 2026-10-19: "Crosshairs and controls are not being carried over /
Grenade and special grenade's activations get switched in MP". In
`recomp/src/game/mp/controls_mp.cpp` (the plain rules in `controls_rules.h`):

- **Grenade keys.** The keyboard presses controller buttons (the SDK's
  `keybind_*` rows), and the rows are named for the campaign's layout: the
  right shoulder (G) is `+grenadeattack`, the left one (4) `+smokegrenade`.
  Multiplayer runs the same `xenon_<layout>.cfg` and then its own
  `mp_xenon_<layout>.cfg` over it (`Controls_ExecLayoutFiles`, 0x82507AD8),
  which can give the frag and the special grenade the other shoulder; then G
  threw the special grenade. After every change to player 1's shoulder binds
  (`Key_SetBinding` 0x82528550; the bind table is 0x82C35030 + local client *
  2052 + key * 8, the text at +4; the shoulders are game keys 217 and 218),
  the two rows are pointed at the buttons that hold the frag and the special
  grenade, through SDK patch 0056 (`rex::input::mnk::SetButtonRemap`: only the
  keyboard's presses move; a controller keeps the game's layout). F2 names the
  rows "Frag grenade" and "Special grenade" in `cod3mp`. The log has the
  layout each time it changes (`player 1's controller layout A: RT +attack,
  ...`) and what the keys press.
- **Controls.** The campaign's Multiplayer item hands its 664-byte control
  settings (invert, sensitivity, button layout, crosshair ...) to `cod3mp` as
  launch data, and `MP_ReadLaunchData` copies them into the player's
  controller profile (0x82C6B730 + profile * 6984; its first 664 bytes are
  the same block). When that profile's own saved settings are read later
  (`Profile_SettingsReadDone` 0x824178E0, which assigns the whole block with
  `ProfileSettings__Assign` 0x824E6098), the campaign's block is put back on
  top of the first such read of the session, so a switch from the campaign
  keeps the player's controls; a later read (after a change in multiplayer's
  own options) is left alone, and so is a read that failed (the game leaves
  the block and shows a message whose retry reads again). The profile that
  got the block is the one whose block equals the launch data afterwards
  (the game copies it by XUID or profile index, and its +92 into all four).
  Both steps are logged with the values (crosshair, invert aim, sensitivity,
  button layout).
- **Crosshair.** Multiplayer has no `cg_drawCrosshair`: its crosshair is drawn
  while byte +57 of the local player's controller profile is set (the draw
  gate at 0x82452920). At start `cod3mp` copies the campaign's choice
  (`seta cg_drawCrosshair` in `userdata/cod3_config.cfg`, kept there by
  `game/settings_sync.h`) into its own console config; the game then has the
  dvar from the first frame and the same `settings_sync` keeps it and player
  1's profile byte together (and applies it again at every map this machine
  loads as the server). With no choice in either config (the campaign writes
  one only after a change), the first value the game has becomes it: the
  handed-over byte, else player 1's profile as first read. A profile read
  puts the choice back on the byte it loads, so a load is never taken for the
  player's choice; a change made in multiplayer's own options is written
  back into the campaign's console config (also when the program ends or
  hands over), so both programs show the last choice. The F2 crosshair (`cod3_crosshair*`) comes from
  `cod3.toml` like every shared setting (4.11).
- **Not yet:** the campaign's own key actions (binoculars, sprint / hold
  breath on keys of their own, `cod3_binds_kbm_p1`, `game/keybinds/`) are
  campaign only; in `cod3mp` Shift is the controller's left stick click
  (`+sprintbreath`) through `keybind_lstick_press`. Split screen: the
  keyboard is player 1 and only player 1's profile follows the campaign.

Checked in the cloud (no multiplayer data there, so only up to the game
reading `mp\global.cod`): the crosshair choice taken from the campaign's
config (`cod3mp: crosshair off (the campaign's choice, ...)`, and the line in
`cod3mp_config.cfg`); a hand-over block read (`the campaign's control
settings for profile 1: crosshair off, invert aim on, sensitivity 30/40,
button layout C`, the values written into the test's launch data), and with
no choice in either config, the handed-over crosshair taken as the choice
(the campaign's config left as it was); the SDK
remap's unit test; the rules' tests. The layout and profile-read hooks run
only with the disc's multiplayer files (the owner's PC).

### 4.14 Keys that call the game's actions (2026-10-19)

The campaign's key binds (`docs/KEYBINDS.md`) are in `cod3mp` too: while you play, each
keyboard and mouse key calls one of the game's own actions for player 1 (fire `+attack`,
reload `+reload`, use `+activate`, the grenades, stance, weapons ...) instead of pressing a
controller button; in the menus the keys press the `keybind_*` rows' buttons as before. The
same `game/keybinds/` files serve both programs: the handlers are found by name in the game's
command list (both programs register the same names), and `addr::keybinds` has multiplayer's
addresses through `campaign_map.tsv`. Multiplayer's own parts:

- its pad poll (0x82543340, the campaign's `sub_82522138` through `mp_aliases.h`) goes through
  the local clients once a frame, each with its own copy of the game's kbuttons (696 bytes at 0x82B3FFD0 + cl *
  696, copied into 0x82B53E20 around the client's poll and, in `Com_Frame`, its frame): the
  keys' calls copy player 1's set in and out the same way, with `clientActiveIndex` 0;
- its press rule: `in_joystick` (0x82B3ED7C), byte `g_FEManager+50`, the controller's skip byte
  (0x82B3EDB8 + pad), the client's front-end object (`*(g_FEManager+208+cl*4)`, its vfunc at
  +92), and the client in a match (`0x82C7A270 + 184 + cl*3208` == 2: otherwise the game
  gives every press to the menu keys, so the keys press the controller rows there too);
- the pad state hook for interactions: `game/keybinds/pad_state_mp.cpp` (0x82314360).

Keys changed in the campaign carry over (multiplayer reads `cod3.toml`); a controller row the
player changed keeps its keys in play too (once, `cod3mp_binds_version`); the console's `bind`
for actions is the campaign's only so far. To check on the PC: in a
match, R reloads, F uses, G / 4 throw the frag / special grenade, C / CTRL / SPACE change stance,
the wheel and 1 / 2 switch weapons; the menus still take SPACE, ENTER and the arrows;
`cod3mp.log` has `cod3: key binds: 33 of 33 actions call the game's own commands (multiplayer)`.

## 5. Unknowns

- How Xenia Canary's main builds handle the secure-address and key calls a
  system link game makes (its netplay fork adds online play, for Windows
  only). The custom copy's network changes should avoid relying on them
  (4.9).

- Whether system link uses the console's VDP packets or plain UDP (both
  appear where Demonware opens its socket), and the system link port number.
- Whether CoD3 moves the host to another player when the host leaves a
  system link game (the game modes have a host-migration callback; the
  console's `XSessionMigrateHost` is not imported).
- How the multiplayer program behaves on the emulated GPU and in its menus;
  nothing of it has been run yet.
- A dedicated server (4.3).
- How many of the campaign's port features (mouse aim, field of view, frame
  limit, ultrawide, crosshair) are wanted in multiplayer. Decided by the
  owner 2026-10-16: all of them (4.12); the field of view keeps the
  campaign's limits in a match.

## 6. Files

- `recomp/src/game/multiplayer.{h,cpp}`: the Multiplayer item, the
  hand-over hook and the match hooks.
- `tools/name_mp_functions.py`: the multiplayer program's symbol map and
  codegen names (3,071 functions named, 2,231 of them for the codegen; `names_mp.toml` also has role names given by hand, 4.12).
- `recomp/mp/`: the `cod3mp` target (manifest, `config/codmp.toml`,
  `config/names_mp.toml`, `CMakeLists.txt`, `generated/rexglue.cmake`).
- `recomp/src/cod3mp_app.{h,cpp}`, `recomp/src/main_mp.cpp`,
  `recomp/src/game/mp/` (`session`, `menus_mp`, `hud_mp`, `controls_mp`,
  `controls_rules.h`, `campaign_stubs` and the three generated files): the
  multiplayer program's own code;
  `recomp/src/game/keybinds/` (shared with the campaign; `pad_state_mp.cpp` is `cod3mp`'s only,
  4.14);
  `recomp/src/game/program.h`: `COD3_ADDR`, `COD3_TEXT`, `kMultiplayer`;
  `recomp/src/game/relaunch.{h,cpp}`: switching between the two programs.
- `c3w/pipeline.py` (`MP_XEX`, `KNOWN_MP_XEX`, `_copy_mp_xex`), and
  `tests/test_multiplayer_build.py`.
- `recomp/mp/config/campaign_map.tsv` and `tools/mp_aliases.py` (4.12);
  `tools/campaign_map_set.py` applies a file of review decisions to the
  table.
