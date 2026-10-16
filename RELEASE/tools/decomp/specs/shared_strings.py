"""Checks for cod3_game::shared::strings."""
from __future__ import annotations

import gen
from harness import Buffer, CStr, Int, RetCStr, RetI32, RetVoid, spec


def _pairs(rng):
    """String pairs: equal, equal ignoring case, near misses, unrelated, high bytes, prefixes."""
    while True:
        a = gen.text(rng, 20, high=0.1)
        k = rng.random()
        if k < 0.25:
            b = gen.case_variant(rng, a)
        elif k < 0.5:
            b = gen.mutate(rng, a)
        elif k < 0.6:
            b = a[:rng.randint(0, len(a))]
        elif k < 0.7:
            b = a
        else:
            b = gen.text(rng, 20, high=0.1)
        yield a, b


def _stricmpn_cases(rng):
    for a, b in _pairs(rng):
        n = rng.choice([0, 1, 2, 3, 5, 99999, -1, rng.randint(0, 25)])
        yield [a, b, n]


spec(name="Q_stricmpn", rust="shared::strings::stricmpn", args=[CStr(), CStr(), Int()], ret=RetI32(),
     cases=_stricmpn_cases)
spec(name="Q_stricmp", rust="shared::strings::stricmp", args=[CStr(), CStr()], ret=RetI32(),
     cases=lambda rng: ([a, b] for a, b in _pairs(rng)))
spec(name="Q_strlwr", rust="shared::strings::strlwr", args=[Buffer(size=64, fill=0x55)], ret=RetCStr(),
     cases=lambda rng: ([gen.text(rng, 40, gen.LETTERS + gen.DIGITS + gen.PUNCTUATION, high=0.15)]
                        for _ in iter(int, 1)))

PATH_CHARS = gen.LETTERS + gen.DIGITS + b"./\\_-"


def _paths(rng, min_len=0):
    while True:
        yield gen.text(rng, 40, PATH_CHARS, min_len=min_len)


spec(name="COM_GetExtension", rust="shared::strings::get_extension", args=[CStr()], ret=RetCStr(),
     cases=lambda rng: ([p] for p in _paths(rng)))
# The original reads before the buffer for an empty name, so names have at least one character.
spec(name="COM_DefaultExtension", rust="shared::strings::default_extension_cfg",
     args=[Buffer(size=160, fill=0x77)], ret=RetVoid(),
     cases=lambda rng: ([p if rng.random() < 0.9 else p * 4] for p in _paths(rng, min_len=1)))


def _strcat_cases(rng):
    while True:
        size = rng.choice([1, 2, 8, 16, 32, 64])
        start = gen.text(rng, size + 2)
        src = gen.text(rng, 40)
        yield [start, size, src]


spec(name="Q_strcat", rust="shared::strings::strcat", args=[Buffer(size=72, fill=0x33), Int(), CStr()],
     ret=RetVoid(), cases=_strcat_cases,
     notes="the buffer is 72 bytes; the function is told `size`, and must not touch bytes past it")
