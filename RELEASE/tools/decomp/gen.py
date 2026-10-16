"""Random inputs for the decomp checker: strings and floats with the edge cases games hit."""
from __future__ import annotations

import math
import random
import struct

LETTERS = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
DIGITS = b"0123456789"
PUNCTUATION = b" !#$%&'()*+,-./:<=>?@[]^_`{|}~"
HIGH = bytes(range(0x80, 0x100))


def text(rng: random.Random, max_len: int = 24, alphabet: bytes = LETTERS + DIGITS, min_len: int = 0,
         high: float = 0.0) -> bytes:
    """A string without NULs; `high` is the chance of each byte being 0x80-0xFF."""
    n = rng.randint(min_len, max_len)
    out = bytearray()
    for _ in range(n):
        out.append(rng.choice(HIGH) if rng.random() < high else rng.choice(alphabet))
    return bytes(out)


def case_variant(rng: random.Random, s: bytes) -> bytes:
    """The same text with random letters in the other case."""
    return bytes(c ^ 0x20 if chr(c).isalpha() and c < 0x80 and rng.random() < 0.5 else c for c in s)


def mutate(rng: random.Random, s: bytes) -> bytes:
    """A near miss: one byte changed, removed or added."""
    if not s:
        return text(rng, 3, min_len=1)
    i = rng.randrange(len(s))
    k = rng.random()
    if k < 0.4:
        return s[:i] + bytes([rng.choice(LETTERS + DIGITS + HIGH)]) + s[i + 1:]
    if k < 0.7:
        return s[:i] + s[i + 1:]
    return s[:i] + bytes([rng.choice(LETTERS)]) + s[i:]


SPECIAL_FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 90.0, -90.0, 180.0, -180.0, 360.0, -360.0, 720.0, 1e-7, -1e-7,
                  1e30, -1e30, 3.4e38, -3.4e38, float("inf"), float("-inf"), float("nan"), 1.17549435e-38,
                  1e-45, 179.99, 180.01, -179.99, -180.01, 359.99, 0.0027465820, 0.0054931640625]


def f32(v: float) -> float:
    """`v` rounded to single precision (out of range: infinity, like the FPU's frsp)."""
    try:
        return struct.unpack(">f", struct.pack(">f", v))[0]
    except OverflowError:
        return math.copysign(math.inf, v)


def angle(rng: random.Random) -> float:
    """Angles as the game sees them: mostly ordinary, sometimes extreme or special."""
    r = rng.random()
    if r < 0.15:
        return rng.choice(SPECIAL_FLOATS)
    if r < 0.75:
        return f32(rng.uniform(-720.0, 720.0))
    if r < 0.9:
        return f32(rng.uniform(-1e6, 1e6))
    return f32(rng.randint(-1440, 1440) * 0.5)


def coordinate(rng: random.Random) -> float:
    """World coordinates and vector parts: units to tens of thousands, plus special values."""
    r = rng.random()
    if r < 0.08:
        return rng.choice(SPECIAL_FLOATS)
    if r < 0.6:
        return f32(rng.uniform(-100.0, 100.0))
    if r < 0.9:
        return f32(rng.uniform(-40000.0, 40000.0))
    # Any finite float, by its bits.
    while True:
        v = struct.unpack(">f", struct.pack(">I", rng.getrandbits(32)))[0]
        if not math.isnan(v):
            return v


def vector(rng: random.Random, n: int = 3) -> list[float]:
    if rng.random() < 0.05:
        return [0.0] * n
    return [coordinate(rng) for _ in range(n)]
