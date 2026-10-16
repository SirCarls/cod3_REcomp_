"""Checks for iw_pc::shared::strings (both games)."""
from __future__ import annotations

import gen
from harness_x86 import Buffer, CStr, Int, RetCStr, RetI32, RetVoid, spec

forever = iter(int, 1)


def _pair(rng):
    """Two strings: equal, equal ignoring case, near misses, prefixes, unrelated, high bytes,
    and now and then 100,000 equal characters (past United Offensive's 99999 limit)."""
    if rng.random() < 0.01:
        a = gen.text(rng, 8, min_len=1) * 12_600
        tail = gen.text(rng, 4, min_len=1)
        return a + tail, gen.case_variant(rng, a) + gen.mutate(rng, tail)
    a = gen.text(rng, 20, gen.LETTERS + gen.DIGITS + gen.PUNCTUATION, high=0.1)
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
        b = gen.text(rng, 20, gen.LETTERS + gen.DIGITS + gen.PUNCTUATION, high=0.1)
    return a, b


def _with_n(rng):
    while True:
        a, b = _pair(rng)
        yield [a, b, rng.choice([0, 1, 2, 3, 5, 99999, 2**31 - 1, -1, rng.randint(0, 25)])]


def _pairs(rng, null: bool = False):
    while True:
        a, b = _pair(rng)
        if null and rng.random() < 0.04:
            a, b = rng.choice([(None, b), (a, None), (None, None)])
        yield [a, b]


for image in ("cod2mp", "uomp"):
    spec(name="Q_stricmpn", image=image, rust="shared::strings::stricmpn", cod3="Q_stricmpn",
         args=[CStr(at="edx"), CStr(at="ecx"), Int(at="eax")], ret=RetI32(), cases=_with_n)
    spec(name="Q_strncmp", image=image, rust="shared::strings::strncmp",
         args=[CStr(at="edx"), CStr(at="ecx"), Int(at="eax")], ret=RetI32(), cases=_with_n)

spec(name="Q_stricmp", image="cod2mp", rust="shared::strings::stricmp", cod3="Q_stricmp",
     args=[CStr(), CStr()], ret=RetI32(), cases=_pairs)
spec(name="Q_strcmp", image="cod2mp", rust="shared::strings::strcmp", args=[CStr(), CStr()], ret=RetI32(),
     cases=_pairs)
spec(name="Q_stricmp", image="uomp", rust="shared::strings::stricmp_nullable", cod3="Q_stricmp",
     args=[CStr(at="edx"), CStr(at="ecx")], ret=RetI32(), cases=lambda rng: _pairs(rng, null=True))
spec(name="sub_0044FAA0", image="uomp", rust="shared::strings::stricmp_equal_or_minus_one", cod3="Q_stricmp",
     args=[CStr(at="edx"), CStr(at="ecx")], ret=RetI32(), cases=_pairs,
     notes="an unreferenced second Q_stricmp (0 or -1); the ancestor of Call of Duty 3's Q_stricmp")


def _strncpyz_cases(rng):
    while True:
        size = rng.choice([1, 2, 3, 8, 16, 32, 64, 72, rng.randint(1, 72)])
        yield [gen.text(rng, 70), gen.text(rng, 90, high=0.1), size]


def _case_cases(rng):
    for _ in forever:
        yield [gen.text(rng, 40, gen.LETTERS + gen.DIGITS + gen.PUNCTUATION, high=0.15)]


def _colored(rng):
    """Text with color codes, near misses (^^, ^ at the end, ^x) and bytes outside 0x20-0x7E."""
    out = bytearray()
    for _ in range(rng.randint(0, 12)):
        k = rng.random()
        if k < 0.3:
            out += b"^" + bytes([rng.choice(gen.DIGITS)])
        elif k < 0.4:
            out += rng.choice([b"^^", b"^", b"^a", b"^ ", b"^\x80", b"\x7f", b"\t", b"\x01"])
        else:
            out += gen.text(rng, 5, gen.LETTERS + gen.PUNCTUATION, high=0.1)
    return bytes(out)


def _strcat_cases(rng):
    while True:
        size = rng.choice([1, 2, 8, 16, 32, 64, 72])
        yield [gen.text(rng, size + 2), size, gen.text(rng, 80, high=0.05)]


PATH_CHARS = gen.LETTERS + gen.DIGITS + b"./\\_-:"


def _paths(rng, min_len=0):
    while True:
        yield gen.text(rng, 40, PATH_CHARS, min_len=min_len, high=0.03)


def _default_extension_cases(rng):
    for path in _paths(rng, min_len=1):
        if rng.random() < 0.1:
            path = (path * 4)[:150]  # past the 63 characters kept
        extension = rng.choice([b".cfg", b".iwd", b".gsc", b"", b".a.b", gen.text(rng, 8, PATH_CHARS)])
        size = rng.choice([1, 2, 8, 16, 32, 64, 128, 160, rng.randint(1, 160)])
        yield [path, size, extension]


for image, reg in (("cod2mp", "eax"), ("uomp", "edi")):
    spec(name="Q_strncpyz", image=image, rust="shared::strings::strncpyz",
         args=[Buffer(size=72, fill=0x55, at="edi"), CStr(at="ecx"), Int(at="esi")], ret=RetVoid(),
         cases=_strncpyz_cases)
    spec(name="Q_strlwr", image=image, rust="shared::strings::strlwr", cod3="Q_strlwr",
         args=[Buffer(size=64, fill=0x55, at=reg)], ret=RetCStr(), cases=_case_cases)
    spec(name="Q_strupr", image=image, rust="shared::strings::strupr",
         args=[Buffer(size=64, fill=0x55, at=reg)], ret=RetCStr(), cases=_case_cases)
    spec(name="Q_strcat", image=image, rust="shared::strings::strcat", cod3="Q_strcat",
         args=[Buffer(size=72, fill=0x33, at="ebx"), Int(at="eax"), CStr()], ret=RetVoid(), cases=_strcat_cases,
         notes="the buffer is 72 bytes; the function is told `size`, and must not touch bytes past it")
    spec(name="Q_PrintStrlen", image=image, rust="shared::strings::print_strlen", args=[CStr(at="edx")],
         ret=RetI32(), cases=lambda rng: ([_colored(rng)] for _ in forever))
    spec(name="Q_CleanStr", image=image, rust="shared::strings::clean_str",
         args=[Buffer(size=64, fill=0x55, at="eax")], ret=RetCStr(),
         cases=lambda rng: ([_colored(rng)[:60]] for _ in forever))
    spec(name="COM_SkipPath", image=image, rust="shared::strings::skip_path", args=[CStr(at="edx")],
         ret=RetCStr(), cases=lambda rng: ([p] for p in _paths(rng)))

spec(name="Com_GetExtensionSubString", image="cod2mp", rust="shared::strings::extension_with_dot",
     args=[CStr(at="eax")], ret=RetCStr(), cases=lambda rng: ([p] for p in _paths(rng)))
spec(name="COM_StripExtension", image="cod2mp", rust="shared::strings::strip_extension",
     args=[CStr(at="ecx"), Buffer(size=64, fill=0x77, at="eax")], ret=RetVoid(),
     cases=lambda rng: ([p, b""] for p in _paths(rng)))
spec(name="COM_StripExtension", image="uomp", rust="shared::strings::strip_extension_at_first_dot",
     args=[CStr(at="edx"), Buffer(size=64, fill=0x77, at="ecx")], ret=RetVoid(),
     cases=lambda rng: ([p, b""] for p in _paths(rng)))
# The original reads before the array for an empty name, so names have at least one character.
spec(name="COM_DefaultExtension", image="cod2mp", rust="shared::strings::default_extension",
     args=[Buffer(size=160, fill=0x77, at="eax"), Int(), CStr()], ret=RetVoid(), cases=_default_extension_cases)
spec(name="COM_DefaultExtension", image="uomp", rust="shared::strings::default_extension",
     args=[Buffer(size=160, fill=0x77, at="ecx"), Int(), CStr()], ret=RetVoid(), cases=_default_extension_cases)
