"""Checks for iw_pc::shared::info (both games)."""
from __future__ import annotations

import gen
from harness_x86 import Buffer, CStr, RetCStr, RetI32, RetPrinted, RetVoid, spec

KEY_CHARS = gen.LETTERS + gen.DIGITS + b"_"
VALUE_CHARS = gen.LETTERS + gen.DIGITS + b" _-.:"


def info_string(rng, pairs: int | None = None, value_len: int = 16) -> tuple[bytes, list[bytes]]:
    """A `\\key\\value` string and its keys, sometimes malformed the ways real ones are."""
    n = rng.randint(0, 10) if pairs is None else pairs
    keys, out = [], b""
    for _ in range(n):
        key = gen.text(rng, 12, KEY_CHARS, min_len=0 if rng.random() < 0.05 else 1, high=0.02)
        value = gen.text(rng, value_len, VALUE_CHARS, high=0.02)
        keys.append(key)
        out += b"\\" + key + b"\\" + value
    r = rng.random()
    if r < 0.1 and out:
        out = out[1:]  # no leading backslash
    elif r < 0.15:
        out += b"\\" + gen.text(rng, 6, KEY_CHARS)  # a trailing key without a value
    elif r < 0.2:
        out += b"\\\\"
    return out, keys


def sized_info(rng, around: int) -> tuple[bytes, list[bytes]]:
    """An info string a little under or over `around` bytes (the limits)."""
    text, keys = info_string(rng, 0)
    while len(text) < around - 120:
        more, more_keys = info_string(rng, 1, value_len=100)
        text += more if more.startswith(b"\\") else b"\\" + more
        keys += more_keys
    target = around + rng.randint(-40, 40)
    text = (text + b"x" * target)[:max(target, 1)]
    return text, keys


def pick_key(rng, keys: list[bytes]) -> bytes:
    if keys and rng.random() < 0.7:
        k = rng.choice(keys)
        return gen.case_variant(rng, k) if rng.random() < 0.3 else k
    return gen.text(rng, 10, KEY_CHARS)


def _lookup_cases(rng):
    while True:
        r = rng.random()
        text, keys = sized_info(rng, 8192) if r < 0.04 else info_string(rng)
        key = pick_key(rng, keys)
        k = rng.random()
        if k < 0.02:
            yield [None, key]
        elif k < 0.03:
            yield [text, None]
        else:
            yield [text, key]


def _remove_cases(limit: int):
    def cases(rng):
        while True:
            text, keys = sized_info(rng, limit) if rng.random() < 0.05 else info_string(rng)
            key = pick_key(rng, keys)
            if rng.random() < 0.05:
                key += b"\\x"
            yield [text, key]
    return cases


def _set_cases(limit: int, big: bool):
    def cases(rng):
        while True:
            r = rng.random()
            if r < 0.06:
                text, keys = sized_info(rng, limit)            # around the size the function accepts
            elif r < 0.12 and big:
                text, keys = sized_info(rng, 1024)             # around Call of Duty 2's 1024 cap
            else:
                text, keys = info_string(rng)
            key = pick_key(rng, keys)
            if rng.random() < 0.04:
                key += rng.choice([b"\\", b";", b'"'])        # refused keys
            value = gen.text(rng, 30, VALUE_CHARS + b'\\;"', high=0.02)
            k = rng.random()
            if k < 0.05:
                value = b""
            elif k < 0.08:
                value = gen.text(rng, 1100, VALUE_CHARS + b";", min_len=900)   # past the 1023 kept
            elif k < 0.1:
                key = gen.text(rng, 1100, KEY_CHARS, min_len=1000)           # a pair over 1024 bytes
            elif k < 0.11:
                # `\key\value` of 1023, 1024 (no room for the terminator) and 1025 characters
                n = max(1, rng.choice([1021, 1022, 1023]) - len(key))
                value = gen.text(rng, n, VALUE_CHARS, min_len=n)
            elif big and k < 0.14:
                value = gen.text(rng, 8300, VALUE_CHARS, min_len=7000)
            yield [text, key, value]
    return cases


for image, s_reg, key_reg, remove_reg in (("cod2mp", "ecx", "stack", "eax"), ("uomp", "ecx", "ebx", "ecx")):
    spec(name="Info_ValueForKey", image=image, rust="shared::info::value_for_key", cod3="Info_ValueForKey",
         args=[CStr(at=s_reg), CStr(at=key_reg)], ret=RetCStr(), cases=_lookup_cases)
    spec(name="Info_RemoveKey", image=image, rust="shared::info::remove_key", cod3="Info_RemoveKey",
         args=[Buffer(size=2600, fill=0x41, at=remove_reg), CStr(at="ebx")], ret=RetVoid(),
         cases=_remove_cases(1024))
    spec(name="Info_RemoveKey_Big", image=image, rust="shared::info::remove_key_big", cod3="Info_RemoveKey_Big",
         args=[Buffer(size=8400, fill=0x41, at="ecx"), CStr(at="ebx")], ret=RetVoid(),
         cases=_remove_cases(8192))
    spec(name="Info_Validate", image=image, rust="shared::info::validate", args=[CStr(at="esi")], ret=RetI32(),
         cases=lambda rng: ([gen.text(rng, 30, VALUE_CHARS + b';"\\', high=0.02)] for _ in iter(int, 1)))

spec(name="Info_SetValueForKey", image="cod2mp", rust="shared::info::set_value_for_key", cod3="Info_SetValueForKey",
     args=[Buffer(size=2600, fill=0x41), CStr(at="eax"), CStr()], ret=RetPrinted(), cases=_set_cases(1024, False))
spec(name="Info_SetValueForKey", image="uomp", rust="shared::info::set_value_for_key", cod3="Info_SetValueForKey",
     args=[Buffer(size=2600, fill=0x41), CStr(at="ecx"), CStr()], ret=RetPrinted(), cases=_set_cases(1024, False))
for image in ("cod2mp", "uomp"):
    spec(name="Info_SetValueForKey_Big", image=image, rust="shared::info::set_value_for_key_big",
         cod3="Info_SetValueForKey_Big", args=[Buffer(size=8500, fill=0x41), CStr(at="ecx"), CStr()],
         ret=RetPrinted(), cases=_set_cases(8192, True))
