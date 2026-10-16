"""Checks for cod3_game::net::vehicle: the vehicle update (message 0x4C).

`MPPeer::WriteVehicleUpdate` is checked on vehicles without a physics body
(the vehicle info's +0x518 is 0): with one, it gathers the values with
VMX128 loads the emulator can't run. The checker builds the entry, its
entity (+4), the entity's vehicle info (+0x20C) and the kind table entry
the info's +0x178 picks (0x82B2DC90), from the case's packing.

`MPPeer::ReadVehicleUpdate` is checked with no entity, or an entity
without a physics body; 0x823EA990 (whether the entity refuses the update)
answers the case's value and 0x823DD170 (which tells the game the entry
changed) does nothing. The pointers the checker puts into the entry and the
entity are left out of the comparison (`MaskedBuffer`).
"""
from __future__ import annotations

import random
import struct
from dataclasses import dataclass, field

from harness import Buffer, Bytes, GlobalU8, Int, RetU32, RetVoid, spec
from specs.net_fields import READ_STATUS, REF, _coord_min, angle, coordinate, replace_sin_cos, velocity
from specs.net_position import _long_state

KINDS = 0x82B2DC90
REFUSES = 0x823EA990
CHANGED = 0x823DD170
# The checker's packing of the writer's sources: origin, angles (entity +0x150,
# +0x160), velocity, second velocity (info +0x100, +0x110), info +0x1F9, +0x1FA,
# kind (the kind entry's +0x20), info +0x3D8, info +0x215, +0x216, entity +0x308,
# and the info's +0x178 (which kind entry).
SOURCE = struct.Struct(">12fBBHfBBih")
ENTRY_SIZE = 0x70
ENTITY_SIZE = 0x310


@dataclass
class MaskedBuffer(Buffer):
    """A buffer whose bytes at `masked` (offset, size) pairs aren't compared:
    the checker puts guest pointers there."""
    masked: list = field(default_factory=list)

    def read_back(self, oracle, pointer, value):
        data = bytearray(super().read_back(oracle, pointer, value))
        initial = self.initial(value)
        for offset, size in self.masked:
            data[offset:offset + size] = initial[offset:offset + size]
        return bytes(data)


def _build_writer(oracle, values, pointers):
    _coord_min(oracle, values)
    entry = pointers[0]
    fields = SOURCE.unpack(values[2])
    origin, angles, vel, second = fields[0:3], fields[3:6], fields[6:9], fields[9:12]
    f1f9, f1fa, kind, v3d8, f215, f216, field308, kind_index = fields[12:]
    entity = oracle.alloc(0x320, 16)
    info = oracle.alloc(0x520, 16)
    oracle.write(entry + 4, struct.pack(">I", entity))
    oracle.write(entity + 0x150, struct.pack(">3f", *origin))
    oracle.write(entity + 0x160, struct.pack(">3f", *angles))
    oracle.write(entity + 0x20C, struct.pack(">I", info))
    oracle.write(entity + 0x308, struct.pack(">i", field308))
    oracle.write(info + 0x100, struct.pack(">3f", *vel))
    oracle.write(info + 0x110, struct.pack(">3f", *second))
    oracle.write(info + 0x178, struct.pack(">h", kind_index))
    oracle.write(info + 0x1F9, bytes([f1f9, f1fa]))
    oracle.write(info + 0x215, bytes([f215, f216]))
    oracle.write(info + 0x3D8, struct.pack(">f", v3d8))
    kind_entry = oracle.alloc(0x30, 16)
    oracle.write(kind_entry + 0x20, struct.pack(">H", kind))
    oracle.write(KINDS + 4 * kind_index, struct.pack(">I", kind_entry))


def _writer_cases(rng: random.Random):
    while True:
        kind = rng.choice([0, 1, 1, 2])
        source = SOURCE.pack(*[coordinate(rng) for _ in range(3)], *[angle(rng) for _ in range(3)],
                             *velocity(rng), *velocity(rng), rng.random() < 0.2, rng.random() < 0.2, kind,
                             rng.choice([0.0, 0.5, 0.50001, 1.0, -1.0, float("nan"), rng.uniform(-2, 2)]),
                             rng.random() < 0.3, rng.random() < 0.5,
                             rng.choice([-1, 0, 77, 0x1004, 0x1005, -2, rng.randint(-10, 5000)]),
                             rng.randint(0, 40))
        yield [rng.randbytes(ENTRY_SIZE), _long_state(rng), source]


spec(name="MPPeer::WriteVehicleUpdate", rust="net::vehicle::write_vehicle_update",
     args=[MaskedBuffer(size=ENTRY_SIZE, terminated=False, masked=[(4, 4)]), REF, Bytes()],
     ret=RetVoid(), cases=_writer_cases, setup=_build_writer, image="codmp", golden=True, golden_cases=15)


def _build_reader(oracle, values, pointers):
    _coord_min(oracle, values)
    replace_sin_cos(oracle, values)
    has_entity, refuses = values[5][0], values[5][1]
    entry, entity = pointers[0], pointers[3]
    oracle.write(entry + 4, struct.pack(">I", entity if has_entity else 0))
    info = oracle.alloc(0x520, 16)
    oracle.write(entity + 0x20C, struct.pack(">I", info))
    oracle.replace(REFUSES, lambda o, *_: refuses)
    oracle.replace(CHANGED, lambda o, *_: 0)


def _reader_cases(rng: random.Random):
    while True:
        has_entity = rng.random() < 0.5
        yield [rng.randbytes(ENTRY_SIZE), _long_state(rng), 0, rng.randbytes(ENTITY_SIZE), rng.choice([0, 1, 1]),
               bytes([has_entity, rng.random() < 0.2])]


spec(name="MPPeer::ReadVehicleUpdate", rust="net::vehicle::read_vehicle_update",
     args=[MaskedBuffer(size=ENTRY_SIZE, terminated=False, masked=[(4, 4)]), REF, Int(),
           MaskedBuffer(size=ENTITY_SIZE, terminated=False, masked=[(0x20C, 4)]), GlobalU8(address=READ_STATUS), Bytes()],
     ret=RetU32(), cases=_reader_cases, setup=_build_reader, image="codmp", golden=True, golden_cases=15)
