"""Checks for cod3_game::shared::hash."""
from __future__ import annotations

import gen
from harness import Buffer, CStr, Int, RetU32, RetVoid, spec

NAME_CHARS = gen.LETTERS + gen.DIGITS + b"_ .:/\\-"
forever = iter(int, 1)


def _names(rng):
    while True:
        yield gen.text(rng, 40, NAME_CHARS, high=0.08)


spec(name="NameHash", rust="shared::hash::name_hash", args=[CStr()], ret=RetU32(),
     cases=lambda rng: ([None if rng.random() < 0.02 else n] for n in _names(rng)))
spec(name="NameHash_Init", rust="shared::hash::name_key",
     args=[Buffer(size=32, fill=0xEE, terminated=False), CStr()], ret=RetVoid(),
     cases=lambda rng: ([b"", n] for n in _names(rng)))
spec(name="generateHashValue", rust="shared::hash::cvar_hash", args=[CStr()], ret=RetU32(),
     cases=lambda rng: ([n] for n in _names(rng)))
spec(name="FS_HashFileName", rust="shared::hash::file_name_hash", args=[CStr(), Int()], ret=RetU32(),
     cases=lambda rng: ([n, rng.choice([1, 2, 64, 256, 512, 1024, 4096, 65536])] for n in _names(rng)))
