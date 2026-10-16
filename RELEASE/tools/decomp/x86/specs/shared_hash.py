"""Checks for iw_pc::shared::hash (both games)."""
from __future__ import annotations

import gen
from harness_x86 import CStr, Int, RetI32, RetU32, spec

NAME_CHARS = gen.LETTERS + gen.DIGITS + b"_ .:/\\-"
forever = iter(int, 1)


def _names(rng):
    while True:
        yield gen.text(rng, 40, NAME_CHARS, high=0.08)


for image in ("cod2mp", "uomp"):
    spec(name="FS_HashFileName", image=image, rust="shared::hash::file_name_hash", cod3="FS_HashFileName",
         args=[CStr(at="eax"), Int()], ret=RetU32(),
         cases=lambda rng: ([n, rng.choice([1, 2, 64, 256, 512, 1024, 4096, 65536])] for n in _names(rng)))
    spec(name="generateHashValue", image=image, rust="shared::hash::cvar_hash", cod3="generateHashValue",
         args=[CStr(at="eax")], ret=RetU32(),
         cases=lambda rng: ([None if rng.random() < 0.02 else n] for n in _names(rng)))
    spec(name="Com_HashKey", image=image, rust="shared::hash::hash_key", args=[CStr(at="eax"), Int(at="ebx")],
         ret=RetI32(),
         cases=lambda rng: ([n, rng.choice([-1, 0, 1, 2, 5, 16, 64, 1024, rng.randint(-5, 45)])] for n in _names(rng)))
