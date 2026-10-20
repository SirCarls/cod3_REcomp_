"""Checks for iw_pc::net::delta (both games).

States are random structures whose fields are made for their kind in the
game's own field table (read from the executable): floats around the
whole-number range of the delta coder (+-4096, +-512 for Call of Duty 2's HUD
positions), -0.0, NaN, infinities, fractions; angles; integers of the field's
width and wider; equal and unequal pairs, and states that share most fields.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass

import pe
from harness_x86 import Arg, Buffer, Int, Msg, RetI32, RetVoid, spec
from specs.net_msg import msg_state

forever = iter(int, 1)
IMAGES = ("cod2mp", "uomp")

# (address, entries) of each field table, per game (docs/PC_MULTIPLAYER.md section 12, item 6).
TABLES = {
    "cod2mp": {"entity": (0x0059BDA0, 59), "client": (0x0059C398, 22), "player": (0x0059C4A0, 105),
               "archived": (0x0059C068, 68), "hud": (0x0059C9D8, 32), "objective": (0x0059C990, 6)},
    "uomp": {"entity": (0x0058EC68, 60), "entity12": (0x0058EF38, 60), "client": (0x0058F538, 22),
             "player": (0x0058F640, 114), "archived": (0x0058F208, 68), "hud": (0x005C5768, 30),
             "objective": (0x005C5720, 6)},
}
CLIENT_STATE_SIZE = 0x5C

_fields_cache: dict[tuple[str, str], list[tuple[int, int, int]]] = {}


def fields(image: str, table: str) -> list[tuple[int, int, int]]:
    """(entry address, offset, bits) of each field of a table, read from the
    executable (never stored here: game data)."""
    key = (image, table)
    if key not in _fields_cache:
        address, count = TABLES[image][table]
        raw = pe.load(image, with_names=False).read(address, count * 12)
        # netField_t: name pointer, offset, bits.
        _fields_cache[key] = [(address + 12 * i,) + struct.unpack_from("<ii", raw, 12 * i + 4)
                              for i in range(count)]
    return _fields_cache[key]


def struct_size(image: str, table: str) -> int:
    return max(offset for _, offset, _ in fields(image, table)) + 4


# --- values -------------------------------------------------------------------------

def f32_word(x: float) -> int:
    return struct.unpack("<i", struct.pack("<f", x))[0]


def _i32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= 1 << 31 else v


def _float_value(rng, bias: int) -> int:
    k = rng.random()
    if k < 0.12:
        return f32_word(rng.choice([0.0, -0.0, 1.0, -1.0, float(bias - 1), float(-bias), float(bias),
                                    float(-bias - 1), float(bias) - 0.5, 0.5, -0.5]))
    if k < 0.4:
        return f32_word(float(rng.randint(-bias - 8, bias + 8)))
    if k < 0.55:
        return f32_word(rng.uniform(-bias * 2, bias * 2))
    if k < 0.62:
        return f32_word(rng.choice([math.inf, -math.inf, math.nan, 2.0 ** 31, -2.0 ** 31, 2.0 ** 32 + 5,
                                    1e30, -1e30, 1e-30, 4294967296.0]))
    if k < 0.72:
        return _i32(rng.choice([0x7FC00000, 0xFFC00000, 0x7F800001, 1, 0x80000001, 0xFF800000]))
    return f32_word(rng.uniform(-1e6, 1e6))


def _int_value(rng, bits: int) -> int:
    width = abs(bits) or 32
    k = rng.random()
    if k < 0.15:
        v = rng.choice([0, 1, -1, (1 << min(width, 31)) - 1, 1 << min(width, 31)])
    elif k < 0.75:
        v = rng.getrandbits(min(width, 32))
    else:
        v = rng.getrandbits(32)
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >= 1 << 31 else v


def field_value(rng, image: str, bits: int) -> int:
    if bits == 0:
        return _float_value(rng, 4096)
    if bits == -99 and image == "cod2mp":
        return _float_value(rng, 512)
    if bits == -100:
        k = rng.random()
        return 0 if k < 0.3 else f32_word(-0.0) if k < 0.35 else f32_word(rng.uniform(-720, 720))
    return _int_value(rng, bits)


def random_state(rng, image: str, table: str, size: int) -> bytearray:
    data = bytearray(rng.getrandbits(8) for _ in range(size)) if rng.random() < 0.5 else bytearray(size)
    for _, offset, bits in fields(image, table):
        struct.pack_into("<i", data, offset, field_value(rng, image, bits))
    return data


def changed_from(rng, image: str, table: str, base: bytes) -> bytearray:
    """`base` with a few fields changed (sometimes none, sometimes all)."""
    data = bytearray(base)
    k = rng.random()
    if k < 0.15:
        return data
    table_fields = fields(image, table)
    n = len(table_fields) if k > 0.9 else rng.randint(1, 4)
    for _, offset, bits in rng.sample(table_fields, min(n, len(table_fields))):
        struct.pack_into("<i", data, offset, field_value(rng, image, bits))
    return data


# --- argument kinds -------------------------------------------------------------------

@dataclass
class State(Arg):
    """A structure the function only reads; None = a NULL pointer. Rust gets
    its bytes as `s:`."""

    def words(self, oracle, value):
        return [0 if value is None else oracle.put(bytes(value), 16)]

    def token(self, value):
        return "s:-" if value is None else "s:" + bytes(value).hex()


@dataclass
class Field(Arg):
    """A pointer to one entry of the executable's own field table; Rust gets
    the entry's offset and bits."""

    def words(self, oracle, value):
        return [value[0]]

    def token(self, value):
        return "v:" + struct.pack("<ii", value[1], value[2]).hex()


# --- cases ------------------------------------------------------------------------------

def _message(rng):
    k = rng.random()
    maxsize = rng.randint(0, 12) if k < 0.2 else rng.randint(12, 64) if k < 0.6 else rng.randint(64, 1400)
    return msg_state(rng, maxsize)


def _field_cases(image: str):
    tables = [t for t in TABLES[image] if t != "objective"] + ["objective"]

    def cases(rng):
        for _ in forever:
            table = rng.choice(tables)
            entry = rng.choice(fields(image, table))
            size = struct_size(image, table)
            old = random_state(rng, image, table, size)
            new = changed_from(rng, image, table, old) if rng.random() < 0.7 else random_state(rng, image, table, size)
            if rng.random() < 0.1:   # the same value as raw bits, or +0.0 against -0.0
                struct.pack_into("<i", new, entry[1], struct.unpack_from("<i", old, entry[1])[0])
            elif rng.random() < 0.05:
                struct.pack_into("<i", old, entry[1], f32_word(0.0))
                struct.pack_into("<i", new, entry[1], f32_word(-0.0))
            yield [bytes(old), bytes(new), _message(rng), entry]
    return cases


def _entity_cases(image: str):
    def cases(rng):
        for _ in forever:
            table = "entity12" if image == "uomp" and rng.random() < 0.3 else "entity"
            size = max(struct_size(image, "entity"), struct_size(image, "entity12") if image == "uomp" else 0)
            old = random_state(rng, image, table, size)
            new = changed_from(rng, image, table, old) if rng.random() < 0.75 else random_state(rng, image, table, size)
            struct.pack_into("<i", new, 0, struct.unpack_from("<i", old, 0)[0] if rng.random() < 0.8
                             else rng.randint(0, 1023))
            if image == "uomp":   # the type picks the table
                struct.pack_into("<i", new, 4, 12 if table == "entity12" else rng.choice([0, 1, 2, 11, 13, 12]))
            to = None if rng.random() < 0.08 else bytes(new)
            yield [_message(rng), bytes(old), to, rng.choice([0, 0, 1])]
    return cases


def _client_cases(image: str):
    def cases(rng):
        for _ in forever:
            old = random_state(rng, image, "client", CLIENT_STATE_SIZE)
            new = changed_from(rng, image, "client", old) if rng.random() < 0.75 else random_state(
                rng, image, "client", CLIENT_STATE_SIZE)
            struct.pack_into("<i", new, 0, rng.randint(0, 63))
            struct.pack_into("<i", old, 0, rng.randint(0, 63))
            frm = None if rng.random() < 0.15 else bytes(old)
            to = None if (rng.random() < 0.08 and frm is not None) else bytes(new)
            yield [_message(rng), frm, to, rng.choice([0, 0, 1])]
    return cases


for image in IMAGES:
    cod2 = image == "cod2mp"
    spec(name="MSG_WriteDeltaField", image=image, rust="net::delta::write_delta_field",
         args=[State(at="eax"), State(at="ecx"), Msg(at="edx"), Field()], ret=RetVoid(),
         cases=_field_cases(image))
    if cod2:
        spec(name="MSG_WriteDeltaEntity", image=image, rust="net::delta::write_delta_entity",
             args=[Msg(at="esi"), State(at="edx"), State(at="ecx"), Int(at="eax")], ret=RetVoid(),
             cases=_entity_cases(image))
    else:
        spec(name="MSG_WriteDeltaEntity", image=image, rust="net::delta::write_delta_entity",
             args=[Msg(), State(at="edx"), State(at="eax"), Int(at="ecx")], ret=RetVoid(),
             cases=_entity_cases(image))
    spec(name="MSG_WriteDeltaClient", image=image, rust="net::delta::write_delta_client",
         args=[Msg(at="esi"), State(at="edx"), State(), Int()], ret=RetVoid(),
         cases=_client_cases(image))


# --- the player's own state -------------------------------------------------------------

LAYOUT = {   # the structure's parts as MSG_WriteDeltaPlayerstate walks them (iw_pc::net::delta)
    "cod2mp": dict(size=0x26A8, stats=0x12C, ammo=0x144, clip=0x344, objectives=0x5E4, hud=0x7A8,
                   hud_count=31, hud_size=0x80),
    "uomp": dict(size=0x4504, stats=0x11C, ammo=0x134, clip=0x334, objectives=0x638, hud=0x7F8,
                 hud_count=63, hud_size=0x7C),
}


def _hud_elem(rng, image: str, size: int, in_use: bool) -> bytearray:
    elem = random_state(rng, image, "hud", size) if rng.random() < 0.7 else bytearray(size)
    struct.pack_into("<i", elem, 0, rng.randint(1, 9) if in_use else 0)
    return elem


def player_state(rng, image: str) -> bytearray:
    lay = LAYOUT[image]
    ps = bytearray(lay["size"])
    if rng.random() < 0.85:
        for _, offset, bits in fields(image, "player"):
            if rng.random() < 0.6:
                struct.pack_into("<i", ps, offset, field_value(rng, image, bits))
    for i in range(6):
        if rng.random() < 0.5:
            struct.pack_into("<i", ps, lay["stats"] + 4 * i, _int_value(rng, rng.choice([8, 16, 32])))
    for base in (lay["ammo"], lay["clip"]):
        for j in range(64):
            if rng.random() < 0.2:
                struct.pack_into("<i", ps, base + 4 * j, _int_value(rng, rng.choice([8, 16, 32])))
    for k in range(16):
        at = lay["objectives"] + 28 * k
        if rng.random() < 0.3:
            ps[at:at + 28] = random_state(rng, image, "objective", 28)[:28]
            struct.pack_into("<i", ps, at, rng.randint(0, 9))
    size, count = lay["hud_size"], lay["hud_count"]
    for array in range(2):
        used = rng.choice([0, 0, 1, 2, 5, count, rng.randint(0, count)])
        for i in range(count):
            at = lay["hud"] + array * count * size + i * size
            if i < used:
                ps[at:at + size] = _hud_elem(rng, image, size, True)
            elif rng.random() < 0.05:   # an element after the first unused one
                ps[at:at + size] = _hud_elem(rng, image, size, True)
    return ps


def changed_player_state(rng, image: str, base: bytes) -> bytearray:
    lay = LAYOUT[image]
    ps = bytearray(base)
    k = rng.random()
    if k < 0.1:
        return ps
    if k > 0.9:
        return player_state(rng, image)
    for _ in range(rng.randint(1, 6)):
        part = rng.choice(["field", "field", "stat", "ammo", "clip", "objective", "hud"])
        if part == "field":
            _, offset, bits = rng.choice(fields(image, "player"))
            struct.pack_into("<i", ps, offset, field_value(rng, image, bits))
        elif part == "stat":
            struct.pack_into("<i", ps, lay["stats"] + 4 * rng.randint(0, 5), _int_value(rng, 32))
        elif part in ("ammo", "clip"):
            struct.pack_into("<i", ps, lay[part] + 4 * rng.randint(0, 63), _int_value(rng, 16))
        elif part == "objective":
            k2 = rng.randint(0, 15)
            at = lay["objectives"] + 28 * k2
            if rng.random() < 0.5:
                struct.pack_into("<i", ps, at, rng.randint(0, 9))
            else:
                _, offset, bits = rng.choice(fields(image, "objective"))
                struct.pack_into("<i", ps, at + offset, field_value(rng, image, bits))
        else:
            size, count = lay["hud_size"], lay["hud_count"]
            i = rng.randint(0, min(count - 1, 6))
            at = lay["hud"] + rng.randint(0, 1) * count * size + i * size
            _, offset, bits = rng.choice(fields(image, "hud"))
            struct.pack_into("<i", ps, at + offset, field_value(rng, image, bits))
    return ps


def _playerstate_cases(image: str):
    def cases(rng):
        for _ in forever:
            old = player_state(rng, image)
            new = changed_player_state(rng, image, bytes(old))
            frm = None if rng.random() < 0.1 else bytes(old)
            k = rng.random()
            maxsize = rng.randint(0, 40) if k < 0.15 else rng.randint(40, 400) if k < 0.4 else rng.randint(400, 1400)
            yield [msg_state(rng, maxsize), frm, bytes(new)]
    return cases


for image in IMAGES:
    stack_msg = image == "uomp"
    spec(name="MSG_WriteDeltaPlayerstate", image=image, rust="net::delta::write_delta_playerstate",
         args=[Msg() if stack_msg else Msg(at="ebx"), State(), State()], ret=RetVoid(),
         cases=_playerstate_cases(image))


# --- player commands ------------------------------------------------------------------------

USERCMD = {   # size, buttons, weapon, second byte, angles, forward (iw_pc::net::delta)
    "cod2mp": dict(size=28, buttons=4, weapon=8, second=9, angles=0xC, forward=0x18),
    "uomp": dict(size=24, buttons=4, weapon=5, second=6, angles=8, forward=0x14),
}


def usercmd(rng, image: str) -> bytearray:
    lay = USERCMD[image]
    cmd = bytearray(rng.getrandbits(8) for _ in range(lay["size"])) if rng.random() < 0.6 else bytearray(lay["size"])
    struct.pack_into("<i", cmd, 0, _int_value(rng, 32))
    for i in range(3):
        struct.pack_into("<i", cmd, lay["angles"] + 4 * i, rng.choice([0, rng.randint(0, 65535), _int_value(rng, 32)]))
    for j in range(3):
        cmd[lay["forward"] + j] = rng.choice([0, 10, 11, 0xF6, 0xF5, 127, 0x81, rng.getrandbits(8)])
    return cmd


def changed_usercmd(rng, image: str, base: bytes) -> bytearray:
    lay = USERCMD[image]
    cmd = bytearray(base)
    t = struct.unpack_from("<i", cmd, 0)[0]
    struct.pack_into("<i", cmd, 0, _i32(t + rng.choice([0, 1, 50, 255, 256, 1000, -1, rng.getrandbits(31)])))
    for _ in range(rng.choice([0, 0, 1, 1, 2, 4])):
        part = rng.choice(["b0", "b", "w", "s", "a0", "a1", "a2", "f", "r", "u"])
        if part == "b0":
            cmd[lay["buttons"]] ^= 1
        elif part == "b":
            cmd[lay["buttons"] + rng.randint(0, 3 if image == "cod2mp" else 0)] ^= 1 << rng.randint(0, 7)
        elif part == "w":
            cmd[lay["weapon"]] = rng.getrandbits(8)
        elif part == "s":
            cmd[lay["second"]] = rng.getrandbits(8)
        elif part[0] == "a":
            struct.pack_into("<i", cmd, lay["angles"] + 4 * int(part[1]), rng.randint(0, 65535))
        else:
            cmd[lay["forward"] + "fru".index(part)] = rng.choice([0, 11, 0xF5, 127, 0x81, rng.getrandbits(8)])
    return cmd


def _usercmd_message(rng):
    k = rng.random()
    return msg_state(rng, rng.randint(0, 8) if k < 0.2 else rng.randint(8, 60))


def _write_usercmd_cases(image: str):
    def cases(rng):
        for _ in forever:
            old = usercmd(rng, image)
            yield [_usercmd_message(rng), bytes(old), bytes(changed_usercmd(rng, image, bytes(old))), _int_value(rng, 32)]
    return cases


def _read_usercmd_cases(image: str):
    """Messages holding what the writer would write, cut short or mixed."""
    def cases(rng):
        for _ in forever:
            old = usercmd(rng, image)
            m = list(_usercmd_message(rng))
            m[4] = rng.randint(0, m[3]) if rng.random() < 0.3 else 0          # readcount
            m[5] = 8 * m[4] if rng.random() < 0.7 else 8 * m[4] + rng.randint(1, 7)  # bit
            stale = bytes(rng.getrandbits(8) for _ in range(USERCMD[image]["size"]))
            yield [tuple(m), _int_value(rng, 32), bytes(old), stale]
    return cases


for image in IMAGES:
    cod2 = image == "cod2mp"
    spec(name="MSG_WriteDeltaUsercmdKey", image=image, rust="net::delta::write_delta_usercmd_key",
         args=[Msg(at="eax"), State(at="edx"), State(at="ecx"), Int()], ret=RetVoid(),
         cases=_write_usercmd_cases(image))
    out = Buffer(size=USERCMD[image]["size"], fill=0, terminated=False, at="stack" if cod2 else "esi")
    spec(name="MSG_ReadDeltaUsercmdKey", image=image, rust="net::delta::read_delta_usercmd_key",
         args=[Msg(at="ebx" if cod2 else "edi"), Int(at="stack" if cod2 else "eax"), State(), out],
         ret=RetVoid(), cases=_read_usercmd_cases(image))



# --- the readers ----------------------------------------------------------------------------
#
# The readers get messages built the way the writers build them, with random content: every
# kind of field in every form (unchanged, zero, short and long floats, angles, integers),
# counts anywhere in the table (and past it), removed and unchanged structures, every part of
# the player state. Call of Duty 2's messages are also cut short anywhere (its readers stop
# at the end); United Offensive's readers read bits on past the end, so only its field and
# structure readers get cut messages. What is never generated: inputs on which the originals
# read or write outside the structures (docs/PC_MULTIPLAYER.md, section 12, item 6: a field
# count missing where the entry in front of the table holds an offset outside the
# structure, a player state count past its table, United Offensive HUD indexes 30 and 31).

class Wire:
    """A message written the games' way (iw_pc::net::msg): a bit that starts
    a new byte takes the message's next byte; raw bytes follow it."""

    def __init__(self):
        self.data = bytearray()
        self.bit = 0

    def bits(self, value: int, count: int) -> None:
        for i in range(count):
            if self.bit & 7 == 0:
                self.bit = 8 * len(self.data)
                self.data.append(0)
            if value >> i & 1:
                self.data[self.bit >> 3] |= 1 << (self.bit & 7)
            self.bit += 1

    def byte(self, value: int) -> None:
        self.data.append(value & 0xFF)

    def short(self, value: int) -> None:
        self.data += struct.pack("<H", value & 0xFFFF)

    def long(self, value: int) -> None:
        self.data += struct.pack("<I", value & 0xFFFFFFFF)


def put_field(w: Wire, rng, image: str, bits: int, player: bool = False) -> None:
    """One field as MSG_WriteDeltaField (or the player state's field writer:
    no "is zero" bit for floats and integers) sends it, with random content."""
    if rng.random() < 0.3:
        w.bits(0, 1)                      # unchanged
        return
    w.bits(1, 1)
    if bits == -100:
        if rng.random() < 0.2:
            w.bits(0, 1)
        else:
            w.bits(1, 1)
            w.short(rng.getrandbits(16))
        return
    if not player:
        if rng.random() < 0.15:
            w.bits(0, 1)                  # zero
            return
        w.bits(1, 1)
    if bits == 0 or (bits == -99 and image == "cod2mp" and not player):
        low = 2 if bits == -99 else 5
        if rng.random() < 0.6:
            w.bits(0, 1)
            w.bits(rng.getrandbits(low), low)
            w.byte(rng.getrandbits(8))
        else:
            w.bits(1, 1)
            w.long(rng.getrandbits(32))
        return
    width = abs(bits)
    if width & 7:
        w.bits(rng.getrandbits(width & 7), width & 7)
    for _ in range(width & 7, width, 8):
        w.byte(rng.getrandbits(8))


def put_struct(w: Wire, rng, image: str, table: str, flag: bool) -> int | None:
    """A structure as MSG_WriteDeltaStruct sends it after the number (United
    Offensive's entities: the table flag first). Returns where the field
    count is in the message, if there is one."""
    if flag:
        w.bits(1 if table == "entity12" else 0, 1)
    if rng.random() < 0.08:
        w.bits(1, 1)                      # removed
        return None
    w.bits(0, 1)
    if rng.random() < 0.15:
        w.bits(0, 1)                      # unchanged
        return None
    w.bits(1, 1)
    table_fields = fields(image, table)
    n = len(table_fields)
    k = rng.random()
    count = rng.randint(0, n) if k < 0.8 else n if k < 0.9 else rng.choice([0, 1]) if k < 0.95 \
        else rng.randint(n + 1, 255)
    at = len(w.data)
    w.byte(count)
    for _, _, bits in table_fields[:min(count, n)]:
        put_field(w, rng, image, bits)
    return at


def _short_group(w: Wire, rng) -> None:
    mask = rng.choice([1, 0x8000, 0xFFFF, rng.getrandbits(16), 1 << rng.randint(0, 15)])
    w.short(mask)
    for j in range(16):
        if mask >> j & 1:
            w.short(rng.getrandbits(16))


def put_playerstate(w: Wire, rng, image: str) -> None:
    """A player state as MSG_WriteDeltaPlayerstate sends it, every part
    random."""
    lay = LAYOUT[image]
    player = fields(image, "player")
    count = rng.choice([0, len(player), rng.randint(0, len(player)), rng.randint(0, 12)])
    w.byte(count)
    for _, _, bits in player[:count]:
        put_field(w, rng, image, bits, player=True)
    if rng.random() < 0.5:                 # statistics
        w.bits(0, 1)
    else:
        w.bits(1, 1)
        mask = rng.getrandbits(6)
        w.bits(mask, 6)
        for i in range(6):
            if mask >> i & 1:
                if i == 3:
                    w.bits(rng.getrandbits(6), 6)
                elif i == 5:
                    w.byte(rng.getrandbits(8))
                else:
                    w.short(rng.getrandbits(16))
    if rng.random() < 0.5:                 # ammunition
        w.bits(0, 1)
    else:
        w.bits(1, 1)
        for _ in range(4):
            if rng.random() < 0.5:
                w.bits(0, 1)
            else:
                w.bits(1, 1)
                _short_group(w, rng)
    for _ in range(4):                     # in the clip
        if rng.random() < 0.6:
            w.bits(0, 1)
        else:
            w.bits(1, 1)
            _short_group(w, rng)
    if rng.random() < 0.6:                 # objectives
        w.bits(0, 1)
    else:
        w.bits(1, 1)
        for _ in range(16):
            w.bits(rng.getrandbits(3), 3)
            if rng.random() < 0.7:
                w.bits(0, 1)
            else:
                w.bits(1, 1)
                for _, _, bits in fields(image, "objective"):
                    put_field(w, rng, image, bits)
    if rng.random() < 0.4:                 # HUD elements
        w.bits(0, 1)
    else:
        w.bits(1, 1)
        hud = fields(image, "hud")
        count_bits = 5 if image == "cod2mp" else 6
        for _ in range(2):
            count = rng.choice([0, 1, 2, lay["hud_count"], rng.randint(0, lay["hud_count"])])
            w.bits(count, count_bits)
            for _ in range(count):
                last = rng.choice([0, len(hud) - 1, rng.randint(0, len(hud) - 1)])
                w.bits(last, 5)
                for _, _, bits in hud[:last + 1]:
                    put_field(w, rng, image, bits)


def reader_message(rng, content, cut: bool, keep_count: bool = False):
    """A msg_t for a reader: some earlier content, then what `content`
    writes, then other bytes; cut short (`cut`) anywhere after the reader's
    start (`keep_count`: after the field count `content` says it wrote)."""
    w = Wire()
    for _ in range(rng.choice([0, 0, 1, 2, 5, 12])):
        if rng.random() < 0.5:
            w.bits(rng.getrandbits(8), rng.randint(1, 8))
        else:
            w.byte(rng.getrandbits(8))
    readcount, bit = len(w.data), w.bit
    at = content(w)
    end = len(w.data)
    data = bytes(w.data) + bytes(rng.getrandbits(8) for _ in range(rng.choice([0, 3, 16, 40])))
    cursize = end if rng.random() < 0.8 else len(data)
    if cut:
        first = at + 1 if keep_count and at is not None else readcount
        cursize = rng.randint(min(first, end), end)
    maxsize = len(data) + rng.choice([0, 0, 1, 20])
    return (1 if rng.random() < 0.05 else 0, data, maxsize, cursize, readcount, bit)


def preceding_offset(image: str, table: str) -> int:
    """The offset word in the 12 bytes in front of a table, which the
    structure reader takes as field -1 when the field count is missing."""
    address, _ = TABLES[image][table]
    return struct.unpack("<i", pe.load(image, with_names=False).read(address - 8, 4))[0]


def _read_field_cases(image: str):
    tables = list(TABLES[image])

    def cases(rng):
        for _ in forever:
            table = rng.choice(tables)
            entry = rng.choice(fields(image, table))
            size = struct_size(image, table)
            old = random_state(rng, image, table, size)
            stale = bytes(rng.getrandbits(8) for _ in range(size))
            if rng.random() < 0.3:
                m = _message(rng)
            else:
                m = reader_message(rng, lambda w: put_field(w, rng, image, entry[2]), cut=rng.random() < 0.3)
            yield [m, bytes(old), stale, entry, 0]
    return cases


def _read_struct_cases(image: str, kind: str):
    def cases(rng):
        for _ in forever:
            uo_entity = kind == "entity" and image == "uomp"
            table = "entity12" if uo_entity and rng.random() < 0.3 else kind
            names = ("entity", "entity12") if uo_entity else (table,)
            size = max(max(4 * (len(fields(image, t)) + 1), struct_size(image, t)) for t in names)
            old = random_state(rng, image, table, size)
            stale = bytes(rng.getrandbits(8) for _ in range(size))
            offset = preceding_offset(image, table)
            inside = 0 <= offset <= size - 4
            m = reader_message(rng, lambda w: put_struct(w, rng, image, table, uo_entity),
                               cut=rng.random() < 0.3, keep_count=not inside)
            number = rng.randint(0, 63 if kind == "client" else 1023)
            frm = None if kind == "client" and rng.random() < 0.15 else bytes(old)
            yield [m, frm, stale, number]
    return cases


def _read_playerstate_cases(image: str):
    def cases(rng):
        size = LAYOUT[image]["size"]
        for _ in forever:
            old = player_state(rng, image)
            m = reader_message(rng, lambda w: put_playerstate(w, rng, image),
                               cut=image == "cod2mp" and rng.random() < 0.3)
            yield [m, None if rng.random() < 0.1 else bytes(old), b"\xAA" * size]
    return cases


for image in IMAGES:
    cod2 = image == "cod2mp"
    out = Buffer(size=0, terminated=False)
    spec(name="MSG_ReadDeltaField", image=image, rust="net::delta::read_delta_field",
         args=[Msg(at="edx", slack=128), State(), out, Field(), Int()], ret=RetVoid(),
         cases=_read_field_cases(image))
    if cod2:
        entity_args = [Msg(at="ebx", slack=128), State(at="ecx"), out, Int(at="eax")]
        archived_args = entity_args
    else:
        entity_args = [Msg(at="eax", slack=128), State(), Buffer(size=0, terminated=False, at="esi"),
                       Int(at="edx")]
        archived_args = [Msg(slack=128), State(at="edx"), Buffer(size=0, terminated=False, at="ecx"),
                         Int(at="eax")]
    spec(name="MSG_ReadDeltaEntity", image=image, rust="net::delta::read_delta_entity",
         args=entity_args, ret=RetI32(), cases=_read_struct_cases(image, "entity"))
    spec(name="MSG_ReadDeltaArchivedEntity", image=image, rust="net::delta::read_delta_archived_entity",
         args=archived_args, ret=RetI32(), cases=_read_struct_cases(image, "archived"))
    spec(name="MSG_ReadDeltaClient", image=image, rust="net::delta::read_delta_client",
         args=[Msg(at="ebx", slack=128), State(at="edx"), out, Int()], ret=RetI32(),
         cases=_read_struct_cases(image, "client"))
    spec(name="MSG_ReadDeltaPlayerstate", image=image, rust="net::delta::read_delta_playerstate",
         args=[Msg(at="ebx", slack=128), State(), out], ret=RetVoid(),
         cases=_read_playerstate_cases(image))


# --- the structure coders and the HUD arrays on their own -----------------------------------
#
# The callers above pass these their own tables and lengths; checked directly, the structure
# coders get every table, numbers wider than the number's bits, every force/change-bit word
# (any non-zero one counts), and United Offensive's callback or none; the HUD coders get
# arrays with any number of elements in use, and the writer any length up to the full one.

STRUCT_TABLES = {"cod2mp": ("entity", "client", "archived", "player", "hud", "objective"),
                 "uomp": ("entity", "entity12", "client", "archived", "player", "hud", "objective")}
TABLE_FLAG = 0x0044AF00   # United Offensive's MSG_WriteEntityTableFlag, the callback MSG_WriteDeltaEntity passes


@dataclass
class Table(Arg):
    """A pointer to one of the executable's own field tables (the value is its
    name in TABLES); Rust gets the name."""
    image: str = ""

    def words(self, oracle, value):
        return [TABLES[self.image][value][0]]

    def token(self, value):
        return "v:" + value.encode().hex()


def _whole_size(image: str, table: str) -> int:
    """Bytes a structure of `table` needs: its fields, and the words an
    unchanged structure's copy takes (4 per field and one more)."""
    return max(struct_size(image, table), 4 * (len(fields(image, table)) + 1))


def _number(rng) -> int:
    return rng.choice([rng.randint(0, 1023), rng.randint(0, 63), _int_value(rng, 32)])


def _write_struct_cases(image: str):
    def cases(rng):
        for _ in forever:
            table = rng.choice(STRUCT_TABLES[image])
            n = len(fields(image, table))
            size = _whole_size(image, table)
            old = random_state(rng, image, table, size)
            new = changed_from(rng, image, table, old) if rng.random() < 0.75 else random_state(rng, image, table, size)
            struct.pack_into("<i", old, 0, _number(rng))
            struct.pack_into("<i", new, 0, struct.unpack_from("<i", old, 0)[0] if rng.random() < 0.8 else _number(rng))
            if image == "uomp" and rng.random() < 0.5:   # the type the table flag looks at
                struct.pack_into("<i", new, 4, rng.choice([12, 12, 0, 1, 11, 13]))
            to = None if rng.random() < 0.08 else bytes(new)
            count = n if rng.random() < 0.85 else rng.randint(0, n)
            bits = rng.choice([10, 10, 6, 6, rng.randint(1, 16)])
            case = [_message(rng), bytes(old), to, rng.choice([0, 0, 1, 2]), count, bits, table]
            if image == "uomp":
                case.append(TABLE_FLAG if rng.random() < 0.5 else 0)
            case.append(rng.choice([0, 0, 1, 1, 5]))
            yield case
    return cases


def _read_struct_direct_cases(image: str):
    def cases(rng):
        for _ in forever:
            table = rng.choice(STRUCT_TABLES[image])
            size = _whole_size(image, table)
            old = random_state(rng, image, table, size)
            stale = bytes(rng.getrandbits(8) for _ in range(size))
            offset = preceding_offset(image, table)
            inside = 0 <= offset <= size - 4
            m = reader_message(rng, lambda w: put_struct(w, rng, image, table, False),
                               cut=rng.random() < 0.3, keep_count=not inside)
            yield [m, bytes(old), stale, _number(rng), len(fields(image, table)), table]
    return cases


def hud_array(rng, image: str, count: int) -> bytearray:
    """`count` HUD elements: some in use at the start, now and then one after
    the first unused one."""
    size = LAYOUT[image]["hud_size"]
    data = bytearray(count * size)
    used = rng.choice([0, 1, 2, 5, count, rng.randint(0, count)])
    for i in range(count):
        if i < used or rng.random() < 0.05:
            data[i * size:(i + 1) * size] = _hud_elem(rng, image, size, True)
    return data


def changed_hud_array(rng, image: str, base: bytes, count: int) -> bytearray:
    size = LAYOUT[image]["hud_size"]
    data = bytearray(base)
    k = rng.random()
    if k < 0.1:
        return data
    if k > 0.85:
        return hud_array(rng, image, count)
    for _ in range(rng.randint(1, 5)):
        i = rng.randint(0, min(count - 1, 8))
        if rng.random() < 0.15:   # an element taken out of use, or brought in
            struct.pack_into("<i", data, i * size, 0 if rng.random() < 0.5 else rng.randint(1, 9))
        else:
            _, offset, bits = rng.choice(fields(image, "hud"))
            struct.pack_into("<i", data, i * size + offset, field_value(rng, image, bits))
    return data


def put_hud_array(w: Wire, rng, image: str, count: int) -> int:
    """One HUD array as MSG_WriteDeltaHudElems sends it, random. Returns the
    byte with the element count's last bit."""
    hud = fields(image, "hud")
    sent = rng.choice([0, 1, 2, count, rng.randint(0, count)])
    w.bits(sent, 5 if image == "cod2mp" else 6)
    at = (w.bit - 1) >> 3
    for _ in range(sent):
        last = rng.choice([0, len(hud) - 1, rng.randint(0, len(hud) - 1)])
        w.bits(last, 5)
        for _, _, bits in hud[:last + 1]:
            put_field(w, rng, image, bits)
    return at


def _write_hud_cases(image: str):
    full = LAYOUT[image]["hud_count"]

    def cases(rng):
        for _ in forever:
            old = hud_array(rng, image, full)
            new = changed_hud_array(rng, image, bytes(old), full)
            count = full if rng.random() < 0.8 else rng.randint(0, full)
            k = rng.random()
            maxsize = rng.randint(0, 30) if k < 0.15 else rng.randint(30, 300) if k < 0.4 else rng.randint(300, 1400)
            yield [msg_state(rng, maxsize), bytes(old), bytes(new), count]
    return cases


def _read_hud_cases(image: str):
    """The full length (as the player state reader passes it: a shorter one
    with more elements sent has the original write past the array), and Call
    of Duty 2's messages cut only after the count (a missing count has it zero
    the element in front of the array)."""
    full = LAYOUT[image]["hud_count"]
    size = LAYOUT[image]["hud_size"]

    def cases(rng):
        for _ in forever:
            old = hud_array(rng, image, full)
            m = reader_message(rng, lambda w: put_hud_array(w, rng, image, full),
                               cut=image == "cod2mp" and rng.random() < 0.3, keep_count=True)
            stale = bytes(rng.getrandbits(8) for _ in range(full * size)) if rng.random() < 0.5 else b"\xAA" * (full * size)
            yield [m, bytes(old), stale, full]
    return cases


for image in IMAGES:
    cod2 = image == "cod2mp"
    out = Buffer(size=0, terminated=False)
    write_args = [Msg(at="esi"), State(), State(), Int(), Int(at="eax"), Int(), Table(image=image)]
    spec(name="MSG_WriteDeltaStruct", image=image, rust="net::delta::write_delta_struct",
         args=write_args + ([Int()] if cod2 else [Int(), Int()]), ret=RetVoid(),
         cases=_write_struct_cases(image))
    to = Buffer(size=0, terminated=False, at="eax") if cod2 else out
    spec(name="MSG_ReadDeltaStruct", image=image, rust="net::delta::read_delta_struct",
         args=[Msg(at="ebx", slack=128), State(), to, Int(), Int(), Table(image=image)], ret=RetI32(),
         cases=_read_struct_direct_cases(image))
    spec(name="MSG_WriteDeltaHudElems", image=image, rust="net::delta::write_delta_hud_elems",
         args=[Msg(), State() if cod2 else State(at="ebx"), State(), Int(at="ecx")], ret=RetVoid(),
         cases=_write_hud_cases(image))
    spec(name="MSG_ReadDeltaHudElems", image=image, rust="net::delta::read_delta_hud_elems",
         args=[Msg(slack=128), State(), out, Int()], ret=RetVoid(),
         cases=_read_hud_cases(image))
