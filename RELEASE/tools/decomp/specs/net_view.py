"""Checks for cod3_game::net::view: the view update (message 0x24).

`MPPlayer::WriteViewUpdate` decides what to send from the game's
structures; the checker builds them from the case's `ViewSource` before
every call: the entity (from the table at 0x82BCC7FC, entry index + 1),
its client, the client's turret entry (0x1F0 bytes each, from
0x82C77E30), and, for a player in a vehicle, the vehicle the entity is
linked to (+0x1B0: a handle into the 8-byte entries at 0x82BD1FC0, index
in the low 14 bits, a generation above) and the vehicle's seat flags. Two
game functions it asks are replaced by the case's answers: 0x820B12D8
(whether the view counts) and 0x823D9B50 (whether the player is in the
vehicle). When the case has no vehicle, one of the conditions fails, picked
at random.

`MPPlayer::ReadViewUpdate` is checked with a client index and an entity
always there; the MPPlayer's +0x40 is kept outside the values that hand
the seat and the weapon to the game's vehicle and weapon code.
"""
from __future__ import annotations

import random
import struct

from harness import Buffer, Bytes, GlobalU32, GlobalU8, RetU32, RetVoid, spec
from specs.net_fields import READ_STATUS, REF, angle
from specs.net_position import ENTITY_TABLE, GAME_TIME, READER_PLAYER_SIZE, _long_state

VIEW_COUNTS = 0x820B12D8
IN_VEHICLE = 0x823D9B50
TURRETS = 0x82C77E30
TURRET_SIZE = 0x1F0
LINKS = 0x82BD1FC0
# The checker's packing of a ViewSource: client index, frozen, angles, view counts,
# turret limits, vehicle seat flags, weapon; then how the guest is built: the turret
# entry's index and, without a vehicle, which condition fails (0: none).
SOURCE = struct.Struct(">iB3fB2fBBiBB")
WRITER_PLAYER_SIZE = 0x50
NO_LINK, OLD_LINK, NO_VEHICLE, NOT_IN_IT, NOT_STATE_1 = 1, 2, 3, 4, 5


def _build_writer(oracle, values):
    (index, frozen, pitch, yaw, roll, view_counts, limit0, limit1, flag215, flag216, weapon, turret,
     fault) = SOURCE.unpack(values[2])
    if index < 0:
        return
    client = oracle.alloc(0xB00, 16)
    oracle.write(client + 0xAE0, bytes([frozen]))
    oracle.write(client + 0xAF4, struct.pack(">i", turret))
    oracle.write(client + 0xCC, struct.pack(">3f", pitch, yaw, roll))
    oracle.write(client + 0xA4, struct.pack(">i", weapon))
    oracle.write(TURRETS + turret * TURRET_SIZE + 0x114, struct.pack(">2f", limit0, limit1))
    entity = oracle.alloc(0x210, 16)
    oracle.write(entity + 0x200, struct.pack(">I", client))
    table = oracle.alloc(4 * (index + 2), 16)
    oracle.write(table + 4 * (index + 1), struct.pack(">I", entity))
    oracle.write(ENTITY_TABLE, struct.pack(">I", table))
    oracle.replace(VIEW_COUNTS, lambda o, *_: view_counts)

    link = 0x1234 if fault == NO_LINK else 0x123  # the low 14 bits: 0x1000 and up isn't an entry
    generation = 0x2A5
    oracle.write(entity + 0x1B0, struct.pack(">I", generation << 14 | link))
    vehicle = oracle.alloc(0x210, 16)
    seats = oracle.alloc(0x220, 16)
    oracle.write(vehicle + 0x20C, struct.pack(">I", seats))
    oracle.write(seats + 0x215, bytes([flag215, flag216]))
    oracle.write(LINKS + 8 * (link & 0xFFF), struct.pack(">II", 0 if fault == NO_VEHICLE else vehicle,
                                                        generation ^ (fault == OLD_LINK)))
    oracle.replace(IN_VEHICLE, lambda o, *_: fault != NOT_IN_IT)


def _writer_cases(rng: random.Random):
    while True:
        index = -1 if rng.random() < 0.05 else rng.randint(0, 23)
        frozen = 1 if rng.random() < 0.1 else 0
        angles = [angle(rng) for _ in range(3)]
        limits = rng.choice([[0.0, 0.0], [1.0, 2.0], [2.0, 1.0], [float("nan"), 1.0], [angle(rng), angle(rng)]])
        fault = 0 if rng.random() < 0.5 else rng.choice([NO_LINK, OLD_LINK, NO_VEHICLE, NOT_IN_IT, NOT_STATE_1])
        flags = [rng.choice([0, 1]), rng.choice([0, 1])]
        weapon = rng.choice([rng.randint(0, 60), rng.randint(128, 400), -3])
        source = SOURCE.pack(index, frozen, *angles, rng.choice([0, 1, 1]), *limits, *flags, weapon,
                             rng.randint(0, 3), fault)
        player = bytearray(rng.randbytes(WRITER_PLAYER_SIZE))
        struct.pack_into(">i", player, 8, index)
        struct.pack_into(">i", player, 0x40, rng.choice([0, 2, 5]) if fault == NOT_STATE_1 else 1)
        yield [bytes(player), _long_state(rng), source]


spec(name="MPPlayer::WriteViewUpdate", rust="net::view::write_view_update",
     args=[Buffer(size=WRITER_PLAYER_SIZE, terminated=False), REF, Bytes()],
     ret=RetVoid(), cases=_writer_cases, setup=_build_writer, image="codmp", golden=True, golden_cases=15)


def _build_reader(oracle, values):
    player, frozen = values[0], values[4][0]
    index = struct.unpack_from(">i", player, 8)[0]
    client = oracle.alloc(0xB00, 16)
    oracle.write(client + 0xAE0, bytes([frozen]))
    entity = oracle.alloc(0x210, 16)
    oracle.write(entity + 0x200, struct.pack(">I", client))
    table = oracle.alloc(4 * (index + 2), 16)
    oracle.write(table + 4 * (index + 1), struct.pack(">I", entity))
    oracle.write(ENTITY_TABLE, struct.pack(">I", table))


def _reader_cases(rng: random.Random):
    while True:
        player = bytearray(rng.randbytes(READER_PLAYER_SIZE))
        struct.pack_into(">i", player, 8, rng.randint(0, 23))
        struct.pack_into(">i", player, 0x40, rng.choice([0, 6, 7, -1]))
        player[0x280] = rng.choice([0, 0, 1])
        frozen = 1 if rng.random() < 0.2 else 0
        yield [bytes(player), _long_state(rng), rng.choice([0, 1, 1]), rng.getrandbits(32), bytes([frozen])]


spec(name="MPPlayer::ReadViewUpdate", rust="net::view::read_view_update",
     args=[Buffer(size=READER_PLAYER_SIZE, terminated=False), REF, GlobalU8(address=READ_STATUS),
           GlobalU32(address=GAME_TIME), Bytes()],
     ret=RetU32(), cases=_reader_cases, setup=_build_reader, image="codmp", golden=True, golden_cases=15)
