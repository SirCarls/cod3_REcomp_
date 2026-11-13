# Movement profiles: every player's own movement, without desync

The owner, 2026-11-07: "Ok please make a robust system where every player could potentially have different movements without desync. This will be applied to all games." Then: "no rust engine. it's C++".

This page is the design. The code is `COD_3_WAW_Setup-tool/recomp/src/movesync/` (`movesync.h`, `movesync.cpp`), plain C++ with no game address. Each game hooks it up as section 8 says.

Status (2026-11-07): the core is built and tested on Linux and Windows. No game uses it yet.

## 1. What can go wrong, by kind of game

Two kinds of game share the project, and they fail in different ways.

| | Who moves a player | What a different movement can break |
|---|---|---|
| **The owner moves** (CoD3 multiplayer on the console and in the recomp: every console runs its own server and moves only its own players, [MP_PROTOCOL.md](MP_PROTOCOL.md) 1, 2) | The player's own machine. Other machines draw the player from position updates (0x23, 125 to 1000 ms apart) and never re-simulate them. | Nothing rubber-bands. The risks are **agreement** (every machine must know who moves how), **fairness** (a machine could claim one movement and use another), and changes made while a player is moving. |
| **The authority moves** (the PC games' client-server model: Call of Duty 2, United Offensive, Big Red One ports; dedicated servers) | The server moves everyone. A client predicts its own player from its unacknowledged commands. | The client must run **exactly** the profile the server runs, command by command. Otherwise every step misses and the client rubber-bands. An original PC copy can only predict the stock movement. |

One system covers both. Its rules hold in either kind of game.

## 2. The rules

1. **One profile per player at a time, named by its bytes.** A profile is every number and switch of one movement (section 3). Its id is the FNV-1a hash of its exact 56 bytes, so two machines with the same id run the same numbers.
2. **Nobody negotiates; everyone computes.** The host's policy (section 5) and each player's facts (request, team, what their machine can run) go into `Resolve`, a pure function. Every machine gets the same answer from the same inputs.
3. **Changes happen only at a spawn or a round start, and each one is numbered.** The owner or the authority promotes the new profile and gives it the next epoch (8 bits). Everyone keeps the last 8 epochs with their start times (`Timeline`). A late packet or a prediction replay therefore finds the profile that was in force at that command.
4. **A player only gets a profile their own machine runs.** Machines announce `Capabilities` (protocol, styles). An original PC copy, an Xbox 360, an older build, or a build that fails the arithmetic check is stock-only. `Resolve` never gives it anything else.
5. **Same bits on every build.** The kernel uses only f32, in a fixed order, without fused multiply-add. `ArithmeticCheck()` runs six contraction-sensitive steps at startup; a build that fails it announces stock-only. `-ffast-math` and `/fp:fast` builds don't compile (section 9 shows why).
6. **One digest says "we agree".** Each machine hashes the policy and every player's slot, team, epoch and effective profile. Machines compare digests. In an owner-moves game, a difference that lasts past the grace time (3 s) moves everyone stock until the next round start (`Agreement`).
7. **Everything received is untrusted.** Decoders are strict: exact sizes, known values only, every number finite and in range, canonical bytes (re-encoding gives the same bytes). Text is never logged or used as a format string.
8. **Players check in with the host.** Every machine keeps a checked view of every player (`PeerView`), and the host's verdicts are the ones that count. Each remote player's movement is checked against the profile the policy gives them, never one they claim (`MotionChecker`; section 10 has the details). The host decides what to do about a player whose score passes 5.

## 3. Profiles

| Style | Runs | Numbers |
|---|---|---|
| `stock` | the game's own movement, unchanged | none (the game's) |
| `cod4` | the recomp's CoD4 style (game/movement/rules.h `Parts`) | none |
| `quakeworld`, `quake3`, `source` | the kernel (section 4) inside the game's own collision code | all twelve |

**Presets.** For the kernel styles, these are the published default values:

| | QuakeWorld | Quake III | Source (Counter-Strike) |
|---|---|---|---|
| max speed | 320 | 320 | 320 |
| accel / air accel / air wish cap | 10 / 10 / 30 | 10 / 1 / none | 5.5 / 12 / 30 |
| friction / stop speed / edge friction | 4 / 100 / 2 | 6 / 100 / 1 | 5.2 / 80 / 1 |
| gravity / jump | 800 / +270 (adds) | 800 / 270 | 800 / 301.993377 |
| step / crouch scale / max velocity | 18 / game's / 2000 | 18 / 0.25 / 4000 (Quake III has none; every profile here must have one) | 18 / 0.34 / 3500 |
| steps, switches | per command, no fall damage | 8 ms steps, velocity snapped | per command |

**Text form** (settings, logs): `name[,key=value]*`, at most 200 characters, no spaces.

- **Names:** `stock`, `stock+sprint`, `cod4`, `cod4:abcd`, `quakeworld` or `qw`, `quake3` or `q3`, `source` or `cs`. The codes `3` and `4abcd` that rules.h already sends are read as `stock` and `cod4:abcd`.
- **Keys** (kernel styles only):
  - `maxspeed accel airaccel aircap friction stopspeed edgefriction gravity jumpspeed stepsize crouchscale maxvelocity`: plain decimals, at most 12 characters;
  - `stepms` and `jumpcooldown` (`game` for the game's own);
  - `autobhop snap nofalldamage jumpadds`: 0 or 1.
- **Example:** `q3,gravity=400,autobhop=1`.

**Bytes** (format 1, little-endian, 56 bytes):

| Offset | Field |
|---|---|
| 0 | format (1) |
| 1 | style |
| 2 | flags, u16 |
| 4 | twelve f32: max speed, accel, air accel, air wish cap, friction, stop speed, edge friction, gravity, jump speed, step size, crouch scale, max velocity (-0.0 is written as 0.0) |
| 52 | step ms, u16 |
| 54 | jump cooldown ms, u16 (0xFFFF = the game's) |

Text armor: base64url (RFC 4648 section 5, no padding), 75 characters.

**Ranges.** These are what `Validate` refuses outside of:

| Field | Range |
|---|---|
| max speed | 50-600 |
| accel | 0-100 |
| air accel | 0-1000 |
| air wish cap | 0 or 1-1000 |
| friction | 0-20 |
| stop speed | 0-500 |
| edge friction | 1-4 |
| gravity | 100-4000 |
| jump speed | 0-600 |
| step size | 0 or 1-32 |
| crouch scale | 0 or above 0 up to 1 |
| max velocity | 100-4000 (required) |
| step ms | 0-50 |
| jump cooldown | 0-2000 or the game's |

Stock and cod4 must have every number 0 and only their own flags.

## 4. The kernel (one movement step's velocity)

Written from the published formulas (Quake, QuakeWorld, Quake III; no code copied). `dt = msec / 1000`.

```
wish = min(wish_speed, max_speed); crouched: wish *= crouch_scale
on ground and jump and (not held or autobhop):  v.z = jump (QuakeWorld: v.z += jump); leave the ground
on ground:  speed = |v.xy|; under 1: v.xy = 0
            else drop = (max(speed, stop_speed) * friction) * dt (x edge friction near a drop)
                 v *= max(speed - drop, 0) / speed
            add = wish - v.wishdir; add > 0: v += min((accel * dt) * wish, add) * wishdir
in the air: add = min(wish, air_wish_cap) - v.wishdir
            add > 0: v += min((air_accel * dt) * wish, add) * wishdir   (the full wish: air strafing)
max velocity: |v.xy| capped
gravity for the move: GravityEnd(v.z) = v.z - gravity * dt; snap: SnapToUnit (halves to even)
```

The game's own step slide move does the collision, as every hook in game/movement already does.

## 5. The host's policy

| Mode | Each player gets |
|---|---|
| everyone stock | the game as shipped (the default) |
| everyone same | the policy's `fallback` |
| players choose | their request when it is in `allowed`, else `fallback` |
| per team | `team[t]`, else `fallback` |
| assigned | what the host names for that player, else `fallback` |

Then the capability rule: if the player's machine can't run the result, they get `fallback`; if it can't run that either, stock.

**Timing:** at spawn (default) or at round start only.

**Bytes:**

| Offset | Field |
|---|---|
| 0 | format (1) |
| 1 | mode |
| 2 | timing |
| 3 | table count |
| 4 | allowed count |
| 5 | three zero bytes |
| 8 | fallback, u64 |
| 16 | team[4], u64 each |
| 48 | allowed ids, u64 each, sorted |
| after those | the table: up to 8 whole profiles of 56 bytes, sorted by id |

The policy carries every profile it names **by value**. No machine depends on its own presets matching another build's: the stock profile is the only one known without the table.

## 6. Records, digest, agreement

**Player record** (23 bytes):

| Offset | Field |
|---|---|
| 0 | slot (under 64) |
| 1 | team (under 4) |
| 2 | epoch |
| 3 | requested, u64 |
| 11 | effective, u64 (never 0) |
| 19 | from ms, i32 |

**Digest:** SHA-256 over `MSD2`, the policy's bytes, then per record (in slot order) its slot, team, epoch and effective id. `requested` and `from_ms` are left out: they may differ while in flight.

**Agreement** (owner-moves games):

- Digests from peers are compared every frame.
- A difference shorter than the grace time is the normal moment around an epoch change.
- A longer one means everyone moves stock until the next round start, logged once.

## 7. How a match runs

**Owner moves (cod3mp):**

1. The host sends the policy.
2. Each machine sends its capabilities, and its players' requests and teams.
3. Every machine resolves every player.
4. The owner of a player promotes the result at that player's next spawn (or round start), and sends the record (slot, epoch, effective, from).
5. Every machine applies the record to its timeline, and puts that profile's limits into the player's motion checker.
6. Digests are compared.
7. The owner's movement hooks move its own players with their current profile.

**Authority moves (PC ports, dedicated servers):**

1. The server resolves and promotes, the same way.
2. It moves each client with that client's profile; the Pmove wrapper puts the rules of the client being moved in a thread-local, as game/movement already does.
3. It puts the epoch in a spare networked player-state field. Which field is the adapter's choice per game.
4. It sends the records reliably (a config string).
5. A client predicts each command with `Timeline::ByEpoch(epoch in the snapshot)`. If it doesn't have that epoch yet, it doesn't predict until the record arrives.
6. Original clients are stock-only, so the server moves them stock.

**Late joiners** get the policy and every record in full (`Timeline::Reset`). **A gap in epochs** (`Apply` reports it) asks the owner or authority for a full record.

## 8. Hooking it up (each needs its owner's claim)

| Game, part | What to add | Whose area |
|---|---|---|
| Both programs' builds | `movesync.cpp` in the cod3 and cod3mp targets. No special flag is needed: the file turns off contraction itself and refuses fast math. Call `ArithmeticCheck()` once at startup and log the result. | recomp CMakeLists (shared) |
| cod3 (campaign) | The one player's profile from the settings (`ParseProfile`); `rules.h`'s `MoveRules` made from a stock or cod4 profile; the kernel styles' hooks (QUAKE_MOVEMENT.md 4) | movement chat, claude-setup, claude-5d |
| cod3mp, same machine (split screen) | Each local player's profile (`cod3mp_movement`, `_2` to `_4`, now also any text form). The Pmove wrapper (mp-movement-design 1, 5) sets the profile of the client being moved. | claude-9b (stages 1, 2) |
| cod3mp, between consoles | The transport for the policy, capabilities and records: still to be mapped (mp-movement-design 4, stage 3). It must ride something stock consoles ignore, so the bytes are base64url text. The motion checker goes on the position update handler (0x823F9528). Faster position updates for fast profiles: stage 4's `cod3mp_net_update_hz`, per player. | claude-9b, mp-network |
| PC ports (CoD2, United Offensive, Big Red One) and dedicated servers | The server's per-client Pmove wrapper, the epoch field, the records in a config string, prediction by epoch | cod2-uo-decomp, acct2-bro, mp-network |

## 9. Tests

`dev/agents/acct2-decomp/tests/test_movesync.cpp` (private, like game/movement's `test_rules.cpp`) has two parts:

- **Unit checks (527).** Including every attack in section 10, and: hash test values, base64url strictness, presets and their text forms, refused text and bytes, policies and `Resolve`, timelines with wraps, gaps and stale records, the digest, agreement timing, motion checks, kernel cases, and `ArithmeticCheck`.
- **Vectors** (`tests/vectors.txt`, 2,361 lines). Profiles, resolutions, digests, 2,000 kernel steps over five profiles, 300 snap and gravity values, and 22 motion sequences, with every float as its bits. One build writes them; every other build must reproduce them exactly.

**Results, 2026-11-07** (the counts are for the vectors):

| Build | Result |
|---|---|
| GCC 13 in the cloud chat, at -O0, -O2, and -O3 with x86-64-v3 | all equal |
| GCC 13 on the owner's PC | all equal |
| Clang 18 | all equal |
| Windows, built with the project's llvm-mingw (Clang 23, x86-64-v3) and run under Wine | all equal |
| Clang with `-ffp-contract=fast` (fused multiply-add) | 133 of 2,000 kernel steps and 2 of 300 gravity values differ; `ArithmeticCheck` fails |
| `-ffast-math` | also let a NaN profile through; now refused at compile time |

That last pair is the reason for rule 5.

## 10. Abuse: what this stops, and what needs the host

The owner, 2026-11-07: "Please double check this can't be used for malicious behaviour IE speed hacks or forcefully teleporting." "like they need to check in with the host." "or aimbot or DMA or memory injections. Basically we need this to be a safe environment."

**What movesync itself stops** (each one has a test in `test_movesync.cpp`):

| Attack | What stops it |
|---|---|
| Declare a fast profile so your own checks loosen | `PeerView` checks every claim against `Resolve`. A claimed other profile is reported (`kWrongProfile`) and its limits are never used. |
| Speak for another player, or send a policy without being the host | Only the session's owner of a slot may speak for it (`kNotOwner`), only the host for the policy (`kNotHost`). Owners come from the session layer, never from a movesync message. |
| Speed hack in an owner-moves game | Per update: reported speed, rise, distance and climb (`kSpeed`, `kRise`, `kDistance`, `kClimb`). Over 2 s or more: total distance (`kSustained`) catches a hack 10 % over the ceiling that each update's slack would hide. The test catches a 20 % hack that even reports a legal speed, and passes a fair top-speed sprint with 80 ms of arrival jitter. |
| Teleport | `kDistance` on the update. Nothing the player sends can excuse a jump: only events the checking machine saw itself (a spawn, a vehicle exit, knockback it applied), at most one a second; asking more is reported (`kExcuseFlood`). |
| Switch to a looser profile mid-life | New, looser limits start only at a spawn the checking machine saw; until then the stricter of old and new apply. A new epoch without a seen spawn is reported (`kChangeWithoutSpawn`). Requests change at most once per 5 s (`kTooSoon`). |
| Timescale speed hack against a server (client-server games) | `CommandClock` grants each command at most the real time that passed, plus a 500 ms burst. No saving up: idling 10 s buys 500 ms, not 10 s. The test asks for twice real time and gets real time. |
| Hostile numbers | Strict decoders, finite and in-range numbers. Tighter ranges: speed at most 600, jump at most 600, step at most 32, gravity at least 100, a speed ceiling (at most 4000) on every kernel profile. The kernel clamps its inputs: a NaN, infinite or non-unit input moves nowhere, at most 200 ms per step, every velocity component within 8000. |
| A two-faced host (different rules to different players) | The digest is SHA-256 over the whole policy: two different policies can't share a digest, so the players disagree and everyone moves stock. |
| A miscompiled build | `ArithmeticCheck`, plus fast math refused at compile time: such a build is stock-only. |

**What no check on a player's own machine can stop.** In CoD3's peer-to-peer model, each console is the authority for its own players, and today also for its own shots:

- the shooter decides hits;
- the victim's console applies the damage number from a 0x28 message as sent (claude-9b, `c9b-think/mp-lag-compensation-design.md` 1).

A modified console can lie within the checks' slack and decide its own hits. **An aimbot, a wallhack that reads memory (including from a DMA card, which reads RAM from outside the operating system), and injected code all run on the cheater's own machine.** Software on that machine can always be bypassed, and the project doesn't touch DRM or ship kernel drivers. The defence has to be that **the cheater's machine decides nothing that matters and receives nothing it shouldn't see**:

1. **Movement checked by the host** (built here: `PeerView`, `MotionChecker`). In host-authority mode, the host moves everyone from their inputs and clients predict (sections 1, 7, and `CommandClock`). Then a modified client can only send inputs. Matches are PC only, so all peers run our builds and such a mode is possible.
2. **Shots checked by the host.** claude-9b's design: the shooter sends the shot to the host. The host re-traces it against its timed history of positions with a bounded rewind, and checks the fire rate, ammo and spread. The victim's console stops obeying raw 0x28 damage. This stops magic bullets, silent aim, impossible shots and forged kills.
3. **Information hiding.** The host sends each player only the players they could see or hear, so a wallhack, even over DMA, has nothing to show.
4. **Aimbots** produce legal inputs, so no system can block them outright. The host can only flag patterns for review: view snaps faster than a hand, reaction times, hit rates through smoke.

Items 2 to 4 are netcode: claude-9b's and mp-network's area, and claims are needed. Until they exist, a match with another console should stay stock and be treated as trusting the other players. That is mp-movement-design's rule already.

## 11. Open

- The cod3mp transport between consoles (rule 6 and section 7 need it). Until it exists, matches with another console stay stock, as mp-movement-design already decides.
- Per game: the spare player-state field for the epoch (PC ports).
- The kernel styles' hooks in cod3 and cod3mp (QUAKE_MOVEMENT.md).
- `GameLimits` for each game: cod3mp's numbers are in the test.
- Check the Source preset against the CS2 values read from the installed game on the owner's PC (private; they never leave it).
