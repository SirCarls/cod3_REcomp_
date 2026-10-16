"""Checks for cod3_game::net: network messages (msg_t) and addresses (netadr_t)."""
from __future__ import annotations

import struct

import gen
from harness import Bytes, BytesInRegisters, CStr, Message, Msg, RetCStr, RetI32, RetVoid, spec

MAX_MSGLEN = 3072       # every message buffer the game makes
MAX_STRING_CHARS = 256  # a string in a message has fewer characters than this
# What strings in messages hold: commands and their arguments, player names, chat. With
# '%' and bytes above 127, which Quake III filtered and CoD3 doesn't.
STRING_CHARS = gen.LETTERS + gen.DIGITS + gen.PUNCTUATION + b'"\\;\n'


def message_string(rng) -> bytes:
    """A string for a message: mostly short, sometimes right at the 256-character limit or past it."""
    r = rng.random()
    if r < 0.08:
        return b""
    if r < 0.3:
        length = rng.choice([254, 255, 256, 257, 300, 1000])
        return gen.text(rng, length, STRING_CHARS, min_len=length, high=0.1)
    return gen.text(rng, rng.choice([8, 40, 120]), STRING_CHARS, high=0.1)


def buffer_bytes(rng, size: int) -> bytes:
    """What a buffer holds before the call: zeros (MSG_Init clears it), an old message, or noise."""
    r = rng.random()
    if r < 0.3:
        return bytes(size)
    if r < 0.5:
        return bytes([rng.choice([0x41, 0xCD, 0xFF])]) * size
    return rng.randbytes(size)


def other_fields(rng, cursize: int) -> dict:
    """The fields neither function should touch: random now and then, to show they don't."""
    return {
        "maxsize": None if rng.random() < 0.8 else rng.choice([0, cursize, MAX_MSGLEN, rng.randint(-5, 5000)]),
        "overflowed": 0 if rng.random() < 0.9 else rng.choice([1, -1, 0x12345678]),
    }


def _write_cases(rng):
    while True:
        text = None if rng.random() < 0.03 else message_string(rng)
        written = 1 if text is None or len(text) >= MAX_STRING_CHARS else len(text) + 1
        # The buffer: the game's size, or a smaller one, often just big enough.
        if rng.random() < 0.15:
            size = MAX_MSGLEN
        else:
            size = written + rng.choice([0, 1, 7, rng.randint(0, 600), rng.randint(0, 600)])
        # Anywhere the string still fits (the original has no size check, so it must
        # fit), often exactly at the end: an almost full buffer.
        room = size - written
        r = rng.random()
        cursize = room if r < 0.25 else 0 if r < 0.35 else rng.randint(0, room)
        readcount = rng.randint(0, cursize)
        message = Message(buffer_bytes(rng, size), cursize, readcount, **other_fields(rng, cursize))
        yield [message, text]


def _read_cases(rng):
    while True:
        # A message as the game writes it: a few numbers, then strings, then whatever was there.
        head = rng.randbytes(rng.choice([0, 0, 4, 5, 9]))
        strings = [message_string(rng) for _ in range(rng.randint(1, 4))]
        starts, body = [], head
        for s in strings:
            starts.append(len(body))
            body += s + b"\0"
        body += rng.randbytes(rng.choice([0, 0, 3, 20]))
        size = MAX_MSGLEN if rng.random() < 0.15 and len(body) < MAX_MSGLEN else len(body) + rng.choice([0, 1, 30])
        data = bytearray(body + buffer_bytes(rng, size - len(body)))
        # The buffer always ends with a 0, so the original's search for a terminator stays
        # inside it (past the end it would read whatever follows the buffer).
        data[-1] = 0
        ends = [start + len(s) for start, s in zip(starts, strings)]  # where each terminator is
        r = rng.random()
        if r < 0.45:
            cursize = ends[-1] + 1 if rng.random() < 0.8 else len(body)  # the end of the strings
        elif r < 0.6:
            cursize = rng.choice(ends) + rng.choice([0, 1])  # the terminator just outside, or just inside
        elif r < 0.7:
            cursize = size  # a full buffer
        elif r < 0.97:
            cursize = rng.randint(0, size)  # cuts a string anywhere
        else:
            cursize = rng.randint(-20, -1)
        r = rng.random()
        if r < 0.6:
            readcount = rng.choice(starts)
        elif r < 0.75:
            readcount = min(max(cursize, 0), size - 1)  # at the end of the data
        elif r < 0.85:
            readcount = rng.randint(min(max(cursize, 0), size - 1), size - 1)  # at or past the end
        else:
            readcount = rng.randint(0, size - 1)  # anywhere inside the buffer
        yield [Message(bytes(data), cursize, readcount, **other_fields(rng, cursize))]


NA_LOOPBACK, NA_IP, NA_IPX = 2, 4, 5
NETADR = struct.Struct(">i4s10sH")  # netadr_t: type, ip, ipx, port (20 bytes)
PORTS = [0, 1, 28960, 28961, 32767, 32768, 40000, 65535]


def random_address(rng) -> list:
    """A netadr_t's fields: mostly IP, some IPX and loopback, now and then a type the code doesn't know."""
    r = rng.random()
    if r < 0.5:
        kind = NA_IP
    elif r < 0.7:
        kind = NA_IPX
    elif r < 0.8:
        kind = NA_LOOPBACK
    else:
        kind = rng.choice([0, 1, 3, 6, 7, -1, 1000, -0x80000000, 0x7FFFFFFF])
    ip = rng.choice([bytes([127, 0, 0, 1]), bytes(4), b"\xff" * 4]) if rng.random() < 0.2 else rng.randbytes(4)
    ipx = bytes(10) if rng.random() < 0.3 else rng.randbytes(10)
    port = rng.choice(PORTS) if rng.random() < 0.4 else rng.randint(0, 0xFFFF)
    return [kind, ip, ipx, port]


def nudge(rng, fields: list) -> list:
    """The same address with one field changed a little."""
    kind, ip, ipx, port = fields
    which = rng.randrange(5)
    if which == 0:
        port = (port + rng.choice([1, -1, 256, 0x8000])) & 0xFFFF
    elif which == 1:
        i = rng.randrange(4)
        ip = ip[:i] + bytes([rng.randrange(256)]) + ip[i + 1:]
    elif which == 2:
        i = rng.randrange(10)
        ipx = ipx[:i] + bytes([rng.randrange(256)]) + ipx[i + 1:]
    elif which == 3:
        kind = rng.choice([NA_LOOPBACK, NA_IP, NA_IPX, kind + 1, kind - 1])
        kind = (kind + 0x80000000) % 0x100000000 - 0x80000000
    else:
        ip, ipx = rng.randbytes(4), rng.randbytes(10)  # only the bytes this type doesn't use, often
    return [kind, ip, ipx, port]


def _compare_cases(rng):
    while True:
        a = random_address(rng)
        r = rng.random()
        b = a if r < 0.15 else nudge(rng, a) if r < 0.8 else random_address(rng)
        yield [NETADR.pack(*a), NETADR.pack(*b)]


spec(name="MSG_WriteString", rust="net::msg::Msg::write_string", args=[Msg(), CStr()], ret=RetVoid(),
     cases=_write_cases)
spec(name="MSG_ReadString", rust="net::msg::Msg::read_string", args=[Msg()], ret=RetCStr(),
     cases=_read_cases)
spec(name="NET_CompareAdrSigned", rust="net::adr::compare_signed", args=[Bytes(), Bytes()], ret=RetI32(),
     cases=_compare_cases)
# NET_AdrToString calls BigShort through the pointer Swap_Init fills at startup.
spec(name="NET_AdrToString", rust="net::adr::adr_to_string", args=[BytesInRegisters()], ret=RetCStr(),
     cases=lambda rng: ([NETADR.pack(*random_address(rng))] for _ in iter(int, 1)), startup=("Swap_Init",))
