"""Checks for iw_pc::shared::filter (Com_Filter, Com_FilterPath; the games differ)."""
from __future__ import annotations

import gen
from harness_x86 import CStr, Int, RetBool8, RetI32, spec

NAME_CHARS = gen.LETTERS + gen.DIGITS + b"_.-/\\:"
CLASS_CHARS = gen.LETTERS + gen.DIGITS + b"_.-"


def _class(rng, c: int, well_formed: bool) -> bytes:
    """A `[...]` class that may or may not hold the character `c`."""
    inner = bytearray()
    for _ in range(rng.randint(1, 3)):
        k = rng.random()
        if k < 0.4:
            inner.append(c if rng.random() < 0.6 else rng.choice(CLASS_CHARS))
        elif k < 0.8:
            lo, hi = sorted(rng.choice(CLASS_CHARS) for _ in range(2))
            if rng.random() < 0.3:
                lo, hi = hi, lo  # an empty range
            inner += bytes([lo]) + b"-" + bytes([hi])
        elif not well_formed:
            inner += rng.choice([b"]]", b"-", b"a-", b"-]", b"[", b"a-]]"])
        else:
            inner += bytes([gen.case_variant(rng, bytes([c]))[0]])
    if well_formed:
        return b"[" + bytes(inner) + b"]"
    return b"[" + bytes(inner) + rng.choice([b"]", b"]", b"]", b"", b"]]"])


def _filter_for(rng, name: bytes, well_formed: bool) -> bytes:
    """A filter made from a name: some characters become `?`, a class or a `*`, cases change."""
    out = bytearray()
    i = 0
    while i < len(name):
        k = rng.random()
        if k < 0.1:
            out += b"?"
            i += 1
        elif k < 0.17:
            out += b"*"
            i += rng.randint(0, 6)
        elif k < 0.25:
            out += _class(rng, name[i], well_formed)
            i += 1
        else:
            c = name[i:i + 1]
            out += gen.case_variant(rng, c) if rng.random() < 0.2 else c
            i += 1
    r = rng.random()
    if r < 0.1:
        out += b"*"
    elif r < 0.15:
        out += b"?"
    elif r < 0.25 and out and not well_formed:
        out = bytearray(gen.mutate(rng, bytes(out)))
    elif r < 0.3 and not well_formed:
        out += rng.choice([b"[", b"[[a]", b"[a", b"*[", b"]"])
    if rng.random() < 0.08:
        out = bytearray(b"*" + gen.text(rng, 6, NAME_CHARS))  # "*suffix" (Call of Duty 2 matches suffixes only)
    return bytes(out)


def _cases(max_name: int, well_formed: bool, max_filter: int):
    def cases(rng):
        while True:
            name = gen.text(rng, max_name, NAME_CHARS, high=0.03)
            source = name if rng.random() < 0.85 else gen.text(rng, max_name, NAME_CHARS)
            filt = _filter_for(rng, source, well_formed)
            while len(filt) > max_filter:  # shorter, rather than cut (a cut could leave a class open)
                source = source[:len(source) // 2]
                filt = _filter_for(rng, source, well_formed)
            yield [filt, name, rng.choice([0, 1, 1, 0, 2])]
    return cases


# Com_Filter reads past the end of the filter or the name in some cases; the oracle
# leaves zeros there (CStr padding), as the port assumes.
spec(name="Com_Filter", image="cod2mp", rust="shared::filter::filter_cod2",
     args=[CStr(at="eax"), CStr(), Int()], ret=RetBool8(), cases=_cases(40, False, 200))
spec(name="Com_Filter", image="uomp", rust="shared::filter::filter",
     args=[CStr(at="ecx"), CStr(), Int()], ret=RetI32(), cases=_cases(40, False, 200))
# Com_FilterPath copies both into 64-byte arrays on its stack; a class the filter doesn't
# close would make Com_Filter read past the copy (uninitialized stack in the game), so its
# filters close every class and stay within the 63 characters copied.
spec(name="Com_FilterPath", image="cod2mp", rust="shared::filter::filter_path_cod2",
     args=[CStr(at="edx"), CStr(at="eax"), Int()], ret=RetBool8(), cases=_cases(90, True, 60))
spec(name="Com_FilterPath", image="uomp", rust="shared::filter::filter_path",
     args=[CStr(at="edx"), CStr(), Int()], ret=RetI32(), cases=_cases(90, True, 60))
