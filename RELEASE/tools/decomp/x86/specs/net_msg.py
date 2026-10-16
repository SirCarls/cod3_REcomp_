"""Checks for iw_pc::net::msg and iw_pc::net::huffman (both games).

Message buffers come as the harness's `Msg` argument kind: a whole `msg_t`
with its data. The states are the ones the games reach and their edges:
reads and writes at and past `cursize` and `maxsize`, bit positions at and
inside a byte, bits and bytes interleaved, an already overflowed message.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

import gen
from harness_x86 import (F32, Arg, Buffer, CStr, GlobalU32, Int, Msg, RetCStr, RetF32, RetI32, RetPrinted,
                         RetVoid, spec)

forever = iter(int, 1)
IMAGES = ("cod2mp", "uomp")
SLACK = 64  # Msg's default: bytes after maxsize that United Offensive's readers may reach

# Per game: the static buffers the string readers return, the Huffman globals.
STATIC = {
    "cod2mp": {"MSG_ReadString": 0x019A6390, "MSG_ReadBigString": 0x019A4390, "MSG_ReadStringLine": 0x019A3F90,
               "compressor": 0x00C76AB0, "decompressor": 0x00C7DACC, "bloc": 0x019A1B98},
    "uomp": {"MSG_ReadString": 0x009A9BA8, "MSG_ReadBigString": 0x009A77A8, "MSG_ReadStringLine": 0x009A97A8,
             "compressor": 0x009A9FA8, "decompressor": 0x009B0FC4, "bloc": 0x0389FD5C},
}
HUFF_TREE = 8  # huffman_t.tree: the root node pointer


# --- argument kinds this area needs ----------------------------------------------

@dataclass
class RawBytes(Arg):
    """Bytes the function only reads (`v:`), then `zeros_after` zero bytes, as
    a port that reads past the end of its slice sees zeros."""
    zeros_after: int = 64

    def words(self, oracle, value):
        address = oracle.put(bytes(value), 16)
        oracle.alloc(self.zeros_after, 1)
        return [address]

    def token(self, value):
        return "v:" + bytes(value).hex()


@dataclass
class GlobalBytes(Arg):
    """A global array (a static string buffer): set to the value (padded with
    zeros to `size`) before the call, compared whole afterwards (`b:`)."""
    address: int = 0
    size: int = 0
    at: str = "none"
    mutable = True

    def initial(self, value) -> bytes:
        return (bytes(value) + bytes(self.size))[:self.size]

    def words(self, oracle, value):
        oracle.write(self.address, self.initial(value))
        return [self.address]

    def token(self, value):
        return "b:" + self.initial(value).hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(self.address, self.size)


@dataclass
class Address(Arg):
    """A fixed guest address (a global structure); the Rust side gets i:0."""
    address: int = 0

    def words(self, oracle, value):
        return [self.address]

    def token(self, value):
        return "i:0"


@dataclass
class PointerAt(Arg):
    """The pointer stored at `address` when the call starts (a tree's root)."""
    address: int = 0

    def words(self, oracle, value):
        return [oracle.read_u32(self.address)]

    def token(self, value):
        return "i:0"


# --- message states ----------------------------------------------------------------

def _int32(rng) -> int:
    k = rng.random()
    if k < 0.1:
        return rng.choice([0, 1, -1, 2**31 - 1, -2**31, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, -0x8000])
    if k < 0.4:
        return rng.randint(-300, 300)
    if k < 0.6:
        return rng.randint(-0x10000, 0x10000)
    return rng.randint(-2**31, 2**31 - 1)


def msg_state(rng, max_size: int | None = None, slack: int = SLACK):
    """(overflowed, data, maxsize, cursize, readcount, bit): any state the
    games reach, edges first."""
    if max_size is None:
        k = rng.random()
        maxsize = rng.randint(0, 12) if k < 0.45 else rng.randint(12, 80) if k < 0.92 else rng.randint(80, 1400)
    else:
        maxsize = max_size
    k = rng.random()
    if k < 0.18:
        cursize = maxsize
    elif k < 0.38:
        cursize = max(0, maxsize - rng.randint(1, 5))
    elif k < 0.41:
        cursize = maxsize + rng.randint(1, 3)  # only a write of more than 32 bits gets here
    else:
        cursize = rng.randint(0, maxsize)
    k = rng.random()
    if k < 0.2:
        readcount = cursize
    elif k < 0.4:
        readcount = max(0, cursize - rng.randint(1, 4))
    elif k < 0.44:
        readcount = cursize + rng.randint(1, 3)
    else:
        readcount = rng.randint(0, cursize)
    last = max(cursize, readcount)
    if last == 0 or rng.random() < 0.4:
        bit = 8 * rng.randint(0, last)                          # the next bit starts a new byte
    else:
        bit = 8 * rng.randint(0, last - 1) + rng.randint(1, 7)  # inside the bit writer's/reader's byte
    size = maxsize + slack
    data = bytes(rng.getrandbits(8) for _ in range(size)) if rng.random() < 0.7 else bytes(size)
    overflowed = 1 if rng.random() < 0.1 else 0
    return (overflowed, data, maxsize, cursize, readcount, bit)


def _bit_count(rng) -> int:
    return rng.choice([0, 1, 2, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, rng.randint(0, 33), rng.randint(0, 33)])


def _msg_only(rng):
    for _ in forever:
        yield [msg_state(rng)]


def _write_bits(rng):
    for _ in forever:
        yield [msg_state(rng), _int32(rng), _bit_count(rng)]


def _read_bits(rng):
    for _ in forever:
        bits = _bit_count(rng) if rng.random() < 0.95 else rng.choice([-1, -7, -2**31])
        yield [msg_state(rng), bits]


def _write_int(rng):
    for _ in forever:
        yield [msg_state(rng), _int32(rng)]


def _write_data(rng):
    for _ in forever:
        m = msg_state(rng)
        room = m[2] - m[3]
        length = max(0, rng.choice([room, room + 1, room - 1, rng.randint(0, 20), rng.randint(0, max(room, 0) + 2)]))
        yield [m, bytes(rng.getrandbits(8) for _ in range(length)), length]


def _read_data(rng):
    for _ in forever:
        m = msg_state(rng)
        left = m[3] - m[4]
        length = max(0, rng.choice([left, left + 1, left - 1, rng.randint(0, 20), rng.randint(0, max(left, 0) + 2)]))
        yield [m, b"", length]


READ_DATA_OUT = 1600

for image in IMAGES:
    common = dict(image=image)
    if image == "cod2mp":   # United Offensive has these inline
        spec(name="MSG_BeginReading", rust="net::msg::Msg::begin_reading", args=[Msg(at="eax")], ret=RetVoid(),
             cases=_msg_only, **common)
        spec(name="MSG_GetUsedBitCount", rust="net::msg::Msg::used_bit_count", args=[Msg(at="eax")],
             ret=RetI32(), cases=_msg_only, **common)
    spec(name="MSG_WriteBits", rust="net::msg::Msg::write_bits", args=[Msg(at="eax"), Int(at="edx"), Int(at="ecx")],
         ret=RetVoid(), cases=_write_bits, **common)
    spec(name="MSG_WriteBit0", rust="net::msg::Msg::write_bit0", args=[Msg(at="eax")], ret=RetVoid(),
         cases=_msg_only, **common)
    spec(name="MSG_WriteBit1", rust="net::msg::Msg::write_bit1", args=[Msg(at="eax")], ret=RetVoid(),
         cases=_msg_only, **common)
    spec(name="MSG_ReadBits", rust="net::msg::Msg::read_bits", args=[Msg(at="edx"), Int()], ret=RetI32(),
         cases=_read_bits, **common)
    spec(name="MSG_ReadBit", rust="net::msg::Msg::read_bit", args=[Msg(at="edx")], ret=RetI32(),
         cases=_msg_only, **common)
    spec(name="MSG_WriteByte", rust="net::msg::Msg::write_byte", args=[Msg(at="eax"), Int()], ret=RetVoid(),
         cases=_write_int, **common)
    spec(name="MSG_WriteData", rust="net::msg::Msg::write_data", args=[Msg(at="eax"), RawBytes(), Int(at="ecx")],
         ret=RetVoid(), cases=_write_data, **common)
    spec(name="MSG_WriteShort", rust="net::msg::Msg::write_short", args=[Msg(at="eax"), Int()], ret=RetVoid(),
         cases=_write_int, **common)
    spec(name="MSG_WriteLong", rust="net::msg::Msg::write_long", args=[Msg(at="eax"), Int(at="edi")],
         ret=RetVoid(), cases=_write_int, **common)
    spec(name="MSG_ReadByte", rust="net::msg::Msg::read_byte", args=[Msg(at="ecx")], ret=RetI32(),
         cases=_msg_only, **common)
    spec(name="MSG_ReadShort", rust="net::msg::Msg::read_short", args=[Msg(at="eax")], ret=RetI32(),
         cases=_msg_only, **common)
    spec(name="MSG_ReadLong", rust="net::msg::Msg::read_long", args=[Msg(at="ecx")], ret=RetI32(),
         cases=_msg_only, **common)
    spec(name="MSG_ReadData", rust="net::msg::Msg::read_data",
         args=[Msg(at="eax"), Buffer(size=READ_DATA_OUT, fill=0x5A, terminated=False, at="edx"), Int(at="ecx")],
         ret=RetVoid(), cases=_read_data, **common)


# --- strings -----------------------------------------------------------------------

STRING_CHARS = gen.LETTERS + gen.DIGITS + gen.PUNCTUATION + b"%\n\x92\x7f\x01\x14\x15"


def _string(rng, limit: int) -> bytes:
    """Text with the bytes the games change (0x92, %, above 127, newlines), and
    lengths around `limit`."""
    k = rng.random()
    if k < 0.12:
        n = limit + rng.randint(-3, 2)
    elif k < 0.16:
        n = rng.randint(limit, limit + 300)
    else:
        n = rng.randint(0, 60)
    return gen.text(rng, n, STRING_CHARS, min_len=n, high=0.08)


def _write_string(limit: int, null: bool):
    def cases(rng):
        for _ in forever:
            s = _string(rng, limit)
            if null and rng.random() < 0.03:
                s = None
            length = 0 if s is None else len(s)
            k = rng.random()
            if k < 0.5:
                maxsize = rng.randint(0, 40) + (length if length < limit else 0)
            else:
                maxsize = (length if length < limit else 0) + rng.randint(-2, 2) + rng.randint(0, 6)
            m = msg_state(rng, max(0, maxsize))
            yield [s, m]
    return cases


def _message_with_strings(rng, limit: int):
    """A message holding strings (and other bytes) to read: terminated, cut
    off by cursize, longer than the limit, lines."""
    parts = bytearray()
    for _ in range(rng.randint(0, 3)):
        parts += bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 3)))
        parts += _string(rng, limit)
        if rng.random() < 0.85:
            parts += b"\0"
    maxsize = len(parts) + rng.randint(0, 8)
    m = list(msg_state(rng, maxsize))
    data = bytearray(m[1])
    start = rng.randint(0, maxsize - len(parts)) if maxsize > len(parts) else 0
    data[start:start + len(parts)] = parts
    m[1] = bytes(data)
    k = rng.random()
    m[3] = maxsize if k < 0.3 else rng.randint(start, maxsize)          # cursize
    m[4] = start if rng.random() < 0.8 else rng.randint(0, m[3] + 1)    # readcount
    return tuple(m)


def _read_string(limit: int, size: int):
    def cases(rng):
        for _ in forever:
            m = _message_with_strings(rng, limit)
            stale = bytes(rng.getrandbits(8) for _ in range(size)) if rng.random() < 0.5 else b""
            yield [m, stale]
    return cases


for image in IMAGES:
    cod2 = image == "cod2mp"
    spec(name="MSG_WriteString", image=image, rust="net::msg::Msg::write_string",
         args=[CStr(at="eax" if cod2 else "ecx"), Msg(at="ebx")], ret=RetPrinted(),
         cases=_write_string(1024, null=not cod2))
    spec(name="MSG_WriteBigString", image=image, rust="net::msg::Msg::write_big_string",
         args=[CStr(at="ecx" if cod2 else "edx"), Msg(at="ebx")], ret=RetPrinted(),
         cases=_write_string(8192, null=not cod2))
    for name, size, register in (("MSG_ReadString", 1024, "edx" if cod2 else "esi"),
                                 ("MSG_ReadBigString", 8192, "edx"), ("MSG_ReadStringLine", 1024, "edx")):
        spec(name=name, image=image, rust="net::msg::Msg::" + {"MSG_ReadString": "read_string",
                                                             "MSG_ReadBigString": "read_big_string",
                                                             "MSG_ReadStringLine": "read_string_line"}[name],
             args=[Msg(at=register), GlobalBytes(address=STATIC[image][name], size=size)], ret=RetCStr(),
             cases=_read_string(size - 1, size))


def _reliable_command(rng):
    for _ in forever:
        size = rng.choice([1, 2, 3, 16, 64, 1024, 1024, 1024, rng.randint(1, 1100)])
        k = rng.random()
        if k < 0.1:
            text = b""
        elif k < 0.3:
            n = max(0, size + rng.randint(-2, 2))
            text = gen.text(rng, n, STRING_CHARS, min_len=n, high=0.08)
        else:
            text = _string(rng, 1024)
        yield [text, b"", size]


RELIABLE_OUT = 1200
for image in IMAGES:
    spec(name="MSG_WriteReliableCommandToBuffer", image=image,
         rust="net::msg::write_reliable_command_to_buffer",
         args=[CStr(at="eax"), Buffer(size=RELIABLE_OUT, fill=0x41, terminated=False), Int(at="ebx")],
         ret=RetPrinted(), cases=_reliable_command)


# --- angles ------------------------------------------------------------------------

ANGLE16_SCALE = struct.unpack("<f", struct.pack("<f", 65536.0 / 360.0))[0]
ANGLE8_SCALE = struct.unpack("<f", struct.pack("<f", 256.0 / 360.0))[0]


def _near_steps(rng, scale: float) -> float:
    """An angle whose scaled value lands on or next to a whole number: where
    24-bit and 53-bit products truncate differently."""
    k = rng.randint(-70000, 70000) if rng.random() < 0.8 else rng.randint(-2**31, 2**31)
    v = gen.f32(k / scale)
    bits = struct.unpack("<I", struct.pack("<f", v))[0]
    bits = (bits + rng.randint(-3, 3)) & 0xFFFFFFFF
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def _write_angle(scale: float):
    def cases(rng):
        for _ in forever:
            k = rng.random()
            if k < 0.45:
                a = _near_steps(rng, scale)
            elif k < 0.55:
                a = gen.coordinate(rng)
            else:
                a = gen.angle(rng)
            m = msg_state(rng, rng.choice([rng.randint(0, 8), rng.randint(0, 40)]))
            yield [m, a]
    return cases


for image in IMAGES:
    spec(name="MSG_WriteAngle", image=image, rust="net::msg::Msg::write_angle", args=[Msg(at="esi"), F32()],
         ret=RetVoid(), cases=_write_angle(ANGLE8_SCALE))
    spec(name="MSG_WriteAngle16", image=image, rust="net::msg::Msg::write_angle16", args=[Msg(at="esi"), F32()],
         ret=RetVoid(), cases=_write_angle(ANGLE16_SCALE))
    spec(name="MSG_ReadAngle16", image=image, rust="net::msg::Msg::read_angle16", args=[Msg(at="eax")],
         ret=RetF32(), cases=_msg_only)


# --- key-coded values ----------------------------------------------------------------

def _pair_values(rng, mask: int):
    old = _int32(rng)
    k = rng.random()
    if k < 0.3:
        new = old
    elif k < 0.45:
        new = old ^ (rng.getrandbits(32) & ~mask)   # differs only outside the bits written
    else:
        new = _int32(rng)
    return old, new


def _write_delta_key(rng):
    for _ in forever:
        bits = rng.choice([1, 2, 4, 7, 8, 16, 18, 31, 32, rng.randint(1, 32)])
        old, new = _pair_values(rng, (1 << bits) - 1)
        yield [msg_state(rng), _int32(rng), old, new, bits]


def _read_delta_key(rng):
    for _ in forever:
        yield [msg_state(rng), _int32(rng), _int32(rng), rng.choice([0, 1, 2, 4, 8, 16, 18, 31, 32,
                                                                    rng.randint(0, 32)])]


def _write_delta_key_sized(mask: int):
    def cases(rng):
        for _ in forever:
            old, new = _pair_values(rng, mask)
            yield [msg_state(rng), _int32(rng), old, new]
    return cases


def _read_delta_key_sized(rng):
    for _ in forever:
        yield [msg_state(rng), _int32(rng), _int32(rng)]


for image in IMAGES:
    spec(name="MSG_WriteDeltaKey", image=image, rust="net::msg::Msg::write_delta_key",
         args=[Msg(at="eax"), Int(), Int(at="ecx"), Int(at="esi"), Int()], ret=RetVoid(), cases=_write_delta_key)
    spec(name="MSG_ReadDeltaKey", image=image, rust="net::msg::Msg::read_delta_key",
         args=[Msg(at="edx"), Int(), Int(), Int(at="eax")], ret=RetI32(), cases=_read_delta_key)
    spec(name="MSG_WriteDeltaKeyByte", image=image, rust="net::msg::Msg::write_delta_key_byte",
         args=[Msg(at="eax"), Int(), Int(), Int()], ret=RetVoid(), cases=_write_delta_key_sized(0xFF))
    spec(name="MSG_ReadDeltaKeyByte", image=image, rust="net::msg::Msg::read_delta_key_byte",
         args=[Msg(at="eax"), Int(), Int()], ret=RetI32(), cases=_read_delta_key_sized)
    spec(name="MSG_WriteDeltaKeyShort", image=image, rust="net::msg::Msg::write_delta_key_short",
         args=[Msg(at="eax"), Int(at="edx"), Int(), Int(at="ecx")], ret=RetVoid(),
         cases=_write_delta_key_sized(0xFFFF))
    spec(name="MSG_ReadDeltaKeyShort", image=image, rust="net::msg::Msg::read_delta_key_short",
         args=[Msg(at="eax"), Int(), Int()], ret=RetI32(), cases=_read_delta_key_sized)


# --- Huffman -----------------------------------------------------------------------
# The message tree is the game's own, built by its MSG_initHuffman once per image
# (prepare); the Rust side builds its tree from the frequency table it reads from the
# same executable (iw_pc::net::protocol_data). Coding every symbol (Huff_offsetTransmit
# over 0..256) and decoding any bits (Huff_offsetReceive) against the game's trees
# shows the two trees are the same.

HUFFMAN = ("MSG_initHuffman",)
TEXT_BYTES = gen.LETTERS + gen.DIGITS + b" \\\"\n\0\0\0\x01\x02\xff"


def _message_bytes(rng, max_len: int = 200) -> bytes:
    """What messages hold: many zeros and small numbers, text, sometimes noise."""
    n = rng.choice([0, 1, 2, 3, rng.randint(0, 40), rng.randint(0, max_len)])
    k = rng.random()
    if k < 0.4:
        return bytes(rng.choice(TEXT_BYTES) for _ in range(n))
    if k < 0.7:
        return bytes(min(255, int(rng.expovariate(0.15))) for _ in range(n))
    return bytes(rng.getrandbits(8) for _ in range(n))


def _write_compress(rng):
    for _ in forever:
        data = _message_bytes(rng)
        out = bytes(rng.getrandbits(8) for _ in range(3 * len(data) + 32))
        yield [data, out, len(data)]


def _read_compress(rng):
    for _ in forever:
        data = _message_bytes(rng, 120) if rng.random() < 0.5 else bytes(rng.getrandbits(8)
                                                                         for _ in range(rng.randint(0, 120)))
        out = bytes(rng.getrandbits(8) for _ in range(8 * len(data) + 16))
        yield [data, out, len(data)]


def _int_bytes(v: int) -> bytes:
    return struct.pack("<i", v)


def _offset_transmit(rng):
    for i in forever:
        symbol = i % 257 if i < 257 * 2 else rng.randint(0, 256)
        offset = rng.choice([0, 1, 7, 8, rng.randint(0, 300)])
        out = bytes(rng.getrandbits(8) for _ in range(64))
        yield [None, symbol, out, _int_bytes(offset), rng.getrandbits(32)]


def _offset_receive(rng):
    for _ in forever:
        data = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 24)))
        offset = rng.randint(0, max(0, 8 * len(data) - 1)) if data and rng.random() < 0.9 else rng.randint(0, 200)
        yield [None, _int_bytes(rng.getrandbits(31)), data, _int_bytes(offset), rng.getrandbits(32)]


def _transmit(rng):
    for i in forever:
        symbol = i % 257 if i < 257 else rng.randint(0, 256)
        out = bytes(rng.getrandbits(8) for _ in range(64))
        yield [None, symbol, out, rng.randint(0, 300)]


def _receive(rng):
    for _ in forever:
        data = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 24)))
        yield [None, _int_bytes(rng.getrandbits(31)), data, rng.randint(0, max(0, 8 * len(data)))]


for image in IMAGES:
    tree = STATIC[image]
    common = dict(image=image, prepare=HUFFMAN)
    spec(name="MSG_WriteBitsCompress", rust="net::huffman::MessageTree::write_bits_compress",
         args=[RawBytes(at="eax"), Buffer(terminated=False, at="ebx"), Int(at="ecx")], ret=RetI32(),
         cases=_write_compress, **common)
    spec(name="MSG_ReadBitsCompress", rust="net::huffman::MessageTree::read_bits_compress",
         args=[RawBytes(at="eax"), Buffer(terminated=False), Int()], ret=RetI32(), cases=_read_compress, **common)
    spec(name="Huff_offsetTransmit", rust="net::huffman::MessageTree::offset_transmit",
         args=[Address(address=tree["compressor"], at="ecx"), Int(at="eax"), Buffer(terminated=False),
               Buffer(terminated=False, at="esi"), GlobalU32(address=tree["bloc"])],
         ret=RetVoid(), cases=_offset_transmit, **common)
    spec(name="Huff_offsetReceive", rust="net::huffman::MessageTree::offset_receive",
         args=[PointerAt(address=tree["decompressor"] + HUFF_TREE, at="eax"), Buffer(terminated=False),
               RawBytes(at="edi"), Buffer(terminated=False, at="ebx"), GlobalU32(address=tree["bloc"])],
         ret=RetVoid(), cases=_offset_receive, **common)
    spec(name="Huff_transmit", rust="net::huffman::HuffmanTree::transmit",
         args=[Address(address=tree["compressor"]), Int(), Buffer(terminated=False),
               GlobalU32(address=tree["bloc"])],
         ret=RetVoid(), cases=_transmit, **common)
    spec(name="Huff_Receive", rust="net::huffman::HuffmanTree::receive",
         args=[PointerAt(address=tree["decompressor"] + HUFF_TREE, at="eax"), Buffer(terminated=False),
               RawBytes(at="edi"), GlobalU32(address=tree["bloc"])],
         ret=RetI32(), cases=_receive, **common)


# United Offensive's adaptive coder (its connect packet): a fresh tree per call.
CONNECT_CHARS = gen.LETTERS + gen.DIGITS + b"\\_ .\"-"


def _adaptive_plain(rng):
    """Connect-like text after the 12-byte header, or other bytes."""
    k = rng.random()
    if k < 0.6:
        body = b"connect \"" + b"".join(b"\\" + gen.text(rng, 12, CONNECT_CHARS, min_len=1) for _ in
                                        range(rng.randint(0, 30))) + b"\""
    elif k < 0.8:
        body = _message_bytes(rng, 600)
    else:
        body = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 1200)))
    return b"\xff\xff\xff\xff" + body


def _compress_adaptive(rng):
    for _ in forever:
        plain = _adaptive_plain(rng)
        maxsize = len(plain) + rng.randint(0, 64)
        m = list(msg_state(rng, maxsize, slack=ADAPTIVE_SLACK))
        data = bytearray(m[1])
        data[:len(plain)] = plain
        m[1], m[3] = bytes(data), len(plain)
        offset = rng.choice([12, 12, 12, 0, 4, len(plain), len(plain) + 1, rng.randint(0, len(plain))])
        yield [tuple(m), offset]


def _decompress_adaptive(rng):
    """Random bits after a length: every bit pattern decodes to something, and
    the length may be right, too long (early stop) or too short."""
    for _ in forever:
        body = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 600)))
        guess = len(body) * 8 // 9
        length = rng.choice([guess, guess + rng.randint(1, 50), max(0, guess - rng.randint(1, 50)), 0,
                             rng.randint(0, 0xFFFF)])
        packet = b"\xff\xff\xff\xffconnect " + struct.pack(">H", length) + body
        maxsize = len(packet) + rng.choice([0, 16, 1200, rng.randint(0, 2000)])
        m = list(msg_state(rng, maxsize))
        data = bytearray(m[1])
        data[:len(packet)] = packet
        m[1], m[3] = bytes(data), len(packet)
        offset = rng.choice([12, 12, 12, 0, len(packet), rng.randint(0, len(packet))])
        yield [tuple(m), offset]


ADAPTIVE_SLACK = 2048  # the compressed text can be longer than maxsize: the original doesn't check
spec(name="Huff_Compress", image="uomp", rust="net::huffman::compress_adaptive",
     args=[Msg(slack=ADAPTIVE_SLACK), Int()], ret=RetVoid(), cases=_compress_adaptive)
spec(name="Huff_Decompress", image="uomp", rust="net::huffman::decompress_adaptive",
     args=[Msg(), Int()], ret=RetVoid(), cases=_decompress_adaptive)
