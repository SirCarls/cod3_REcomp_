"""Checks for cod3_game::net::position: the position update (message 0x23).

`MPPlayer::WritePositionUpdate` reads the player's entity and client from
the game's structures: the entity from a table of pointers (a global,
0x82BCC7FC; entry index + 1), the client from the entity (+0x200). The
checker builds both from the case's snapshot before every call, at the
offsets the function reads, and compares the `MPPlayer` (counter and
baseline) and the buffer afterwards.

The "use code" comes from 0x823DDB08 (weapon class and state bits; not
ported yet). For this check a call to that function returns the snapshot's
value instead (`Oracle.replace`), so the writer is checked on its own.
"""
from __future__ import annotations

import random
import struct

from harness import Buffer, Bytes, GlobalU32, GlobalU8, Int, RetBool, RetU32, spec
from specs.net_bit_buffer import State, random_state
from specs.net_fields import READ_STATUS, REF, _coord_min, angle, coordinate, f32, replace_sin_cos, velocity

ENTITY_TABLE = 0x82BCC7FC
GAME_TIME = 0x82C2826C
USE_CODE_FUNCTION = 0x823DDB08
PLAYER_SIZE = 0x140
# The checker's packing of a snapshot: origin, velocity, angles; weapon, flags, use code, stance, state.
SNAPSHOT = struct.Struct(">9fiIIIi")


def _build_player(oracle, values):
    _coord_min(oracle, values)  # MSGBB_WriteOrigin's lower bound, set when the program starts
    player, snapshot = values[0], values[4]
    index = struct.unpack_from(">i", player, 8)[0]
    if index == -1:
        return
    fields = SNAPSHOT.unpack(snapshot)
    origin, velocity_, angles = fields[0:3], fields[3:6], fields[6:9]
    weapon, flags, use_code, stance, state = fields[9:]
    client = oracle.alloc(0x800, 16)
    oracle.write(client + 0x10, struct.pack(">3f", *velocity_))
    oracle.write(client + 0x2C, struct.pack(">I", flags))
    oracle.write(client + 0xA4, struct.pack(">i", weapon))
    oracle.write(client + 0xCC, struct.pack(">3f", *angles))
    oracle.write(client + 0x520, struct.pack(">I", stance))
    oracle.write(client + 0x784, struct.pack(">i", state))
    entity = oracle.alloc(0x210, 16)
    oracle.write(entity + 0x150, struct.pack(">3f", *origin))
    oracle.write(entity + 0x200, struct.pack(">I", client))
    table = oracle.alloc(4 * (index + 2), 16)
    oracle.write(table + 4 * (index + 1), struct.pack(">I", entity))
    oracle.write(ENTITY_TABLE, struct.pack(">I", table))
    oracle.replace(USE_CODE_FUNCTION, lambda o, *_: use_code)


def _near(rng, base: float) -> float:
    r = rng.random()
    if r < 0.25:
        return base
    if r < 0.45:
        return f32(base + rng.uniform(-1.5, 1.5))
    if r < 0.85:
        return f32(base + rng.uniform(-600.0, 600.0))
    return coordinate(rng)


def _same_or(rng, value, other, same: float):
    return value if rng.random() < same else other


def _cases(rng: random.Random):
    while True:
        counter = rng.getrandbits(8)
        base_origin = [coordinate(rng) for _ in range(3)]
        base_angles = [angle(rng) for _ in range(3)]
        base_velocity = velocity(rng)
        base_use = rng.randint(0, 0x8000)
        base_weapon = rng.randint(-128, 127)
        base_flags = rng.getrandbits(32)
        base_stance = rng.randint(0, 25)

        origin = [_near(rng, c) for c in base_origin]
        velocity_ = _same_or(rng, list(base_velocity), velocity(rng), 0.4)
        angles = [base_angles[0], base_angles[1], angle(rng)] if rng.random() < 0.4 else \
            [angle(rng) for _ in range(3)]
        weapon = _same_or(rng, base_weapon, rng.choice([rng.randint(0, 60), rng.randint(128, 300), -5]), 0.5)
        flags = _same_or(rng, base_flags, rng.getrandbits(32), 0.8)
        use_code = _same_or(rng, base_use, rng.choice([rng.randint(0, 0x8000), 0x8001, 0xFFFF]), 0.5)
        stance = _same_or(rng, base_stance, rng.randint(0, 31), 0.5)
        state = rng.choice([0, 1, 2, 3, 3, 4])

        player = bytearray(rng.randbytes(PLAYER_SIZE))  # bytes the function must leave alone
        index = -1 if rng.random() < 0.05 else rng.randint(0, 23)
        struct.pack_into(">i", player, 8, index)
        player[0xF0] = counter
        struct.pack_into(">3f", player, 0x100, *base_origin)
        struct.pack_into(">3f", player, 0x110, *base_angles)
        struct.pack_into(">3f", player, 0x120, *base_velocity)
        struct.pack_into(">I", player, 0x130, base_use)
        struct.pack_into(">b", player, 0x134, base_weapon)
        struct.pack_into(">I", player, 0x138, base_flags)
        struct.pack_into(">I", player, 0x13C, base_stance)
        snapshot = SNAPSHOT.pack(*origin, *velocity_, *angles, weapon, flags, use_code, stance, state)
        yield [bytes(player), random_state(rng), rng.choice([0, 0, 0, 1]), rng.choice([0, 1]), snapshot]


spec(name="MPPlayer::WritePositionUpdate", rust="net::position::write_position_update",
     args=[Buffer(size=PLAYER_SIZE, terminated=False), REF, Int(bits=8), Int(bits=8), Bytes()],
     ret=RetBool(), cases=_cases, setup=_build_player, image="codmp", golden=True, golden_cases=15)


# --- the reader ---------------------------------------------------------
#
# `MPPlayer::ReadPositionUpdate` reads the update into the receiving
# console's MPPlayer, then applies it to the player's entity when the
# MPPlayer has a client (+8 >= 0). That part calls into the game's entity
# code, which the check leaves out: most cases have no client; the others
# have a client in state 3 and an update without the "state 3" bit, which
# the reader drops before it gets there. Calls to the VMX128 sine and cosine
# get the stand-in of specs/net_fields.py on both sides.
READER_PLAYER_SIZE = 0x290


def _long_state(rng: random.Random) -> State:
    """Random bits, long enough for a full update (about 180 bits) most of the time."""
    state = random_state(rng)
    if rng.random() < 0.7:
        written = rng.randint(state.read_position + 20, state.read_position + 400)
        data = bytearray(rng.randbytes((written + 7) // 8))
        data[0] = (data[0] & 0xFE) | state.type_checked
        if written % 8:
            data[-1] &= (1 << (written % 8)) - 1
        state = State(bytes(data), written, written, state.read_position, state.read_failed, state.type_checked)
    return state


def _without_state3_bit(state: State) -> State:
    """The same bits with the update's "state 3" bit (the second) cleared."""
    if state.type_checked:
        return state
    data = bytearray(state.data)
    bit = state.read_position + 1
    if bit < state.written:
        data[bit // 8] &= ~(1 << (bit % 8)) & 0xFF
    return State(bytes(data), state.write_position, state.written, state.read_position, state.read_failed, False)


def _build_reader(oracle, values):
    _coord_min(oracle, values)
    replace_sin_cos(oracle, values)
    player, entity_state = values[0], struct.unpack(">i", values[4])[0]
    index = struct.unpack_from(">i", player, 8)[0]
    if index < 0:
        return
    client = oracle.alloc(0x800, 16)
    oracle.write(client + 0x784, struct.pack(">i", entity_state))
    entity = oracle.alloc(0x210, 16)
    oracle.write(entity + 0x200, struct.pack(">I", client))
    table = oracle.alloc(4 * (index + 2), 16)
    oracle.write(table + 4 * (index + 1), struct.pack(">I", entity))
    oracle.write(ENTITY_TABLE, struct.pack(">I", table))


def _reader_cases(rng: random.Random):
    while True:
        player = bytearray(rng.randbytes(READER_PLAYER_SIZE))  # bytes the function must leave alone
        with_client = rng.random() < 0.15
        struct.pack_into(">i", player, 8, rng.randint(0, 23) if with_client else rng.choice([-1, -1, -2]))
        player[0xF0] = rng.choice([0, 0, rng.getrandbits(8)])
        struct.pack_into(">3f", player, 0x100, *[coordinate(rng) for _ in range(3)])
        struct.pack_into(">3f", player, 0x110, *[angle(rng) for _ in range(3)])
        struct.pack_into(">3f", player, 0x120, *velocity(rng))
        player[0x280] = rng.choice([0, 0, 1])
        state = _long_state(rng)
        if with_client:
            state = _without_state3_bit(state)
        time = rng.getrandbits(32)
        yield [bytes(player), state, rng.choice([0, 1, 1]), time, struct.pack(">i", 3)]


spec(name="MPPlayer::ReadPositionUpdate", rust="net::position::read_position_update",
     args=[Buffer(size=READER_PLAYER_SIZE, terminated=False), REF, GlobalU8(address=READ_STATUS),
           GlobalU32(address=GAME_TIME), Bytes()],
     ret=RetU32(), cases=_reader_cases, setup=_build_reader, image="codmp", golden=True, golden_cases=15)
