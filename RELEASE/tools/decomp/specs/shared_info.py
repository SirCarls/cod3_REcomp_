"""Checks for cod3_game::shared::info."""
from __future__ import annotations

import gen
from harness import Buffer, CStr, RetCStr, RetVoid, spec

KEY_CHARS = gen.LETTERS + gen.DIGITS + b"_"
VALUE_CHARS = gen.LETTERS + gen.DIGITS + b" _-.:"


def info_string(rng, pairs: int | None = None, long: bool = False) -> tuple[bytes, list[bytes]]:
    """A `\\key\\value` string and its keys, sometimes malformed the ways real ones are."""
    n = rng.randint(0, 10) if pairs is None else pairs
    keys, out = [], b""
    for _ in range(n):
        key = gen.text(rng, 12, KEY_CHARS, min_len=0 if rng.random() < 0.05 else 1)
        value = gen.text(rng, 120 if long else 16, VALUE_CHARS)
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


def pick_key(rng, keys: list[bytes]) -> bytes:
    if keys and rng.random() < 0.7:
        k = rng.choice(keys)
        return gen.case_variant(rng, k) if rng.random() < 0.3 else k
    return gen.text(rng, 10, KEY_CHARS)


def _lookup_cases(rng):
    while True:
        r = rng.random()
        if r < 0.03:
            text, keys = info_string(rng, 12, long=True)  # often past 1024: the oversize error
        else:
            text, keys = info_string(rng)
        key = pick_key(rng, keys)
        if rng.random() < 0.02:
            yield [None, key]
        else:
            yield [text, key]


def _remove_cases(rng):
    while True:
        text, keys = info_string(rng, 12, long=True) if rng.random() < 0.03 else info_string(rng)
        key = pick_key(rng, keys)
        if rng.random() < 0.05:
            key += b"\\x"
        yield [text, key]


def _set_cases(rng):
    while True:
        text, keys = info_string(rng, 12, long=True) if rng.random() < 0.05 else info_string(rng)
        key = pick_key(rng, keys)
        r = rng.random()
        if r < 0.03:
            key += rng.choice([b"\\", b";", b'"'])  # refused keys: the game's error
        value = gen.text(rng, 30, VALUE_CHARS + b'\\;"')
        if rng.random() < 0.05:
            value = b""
        if rng.random() < 0.03:
            value = gen.text(rng, 1100, VALUE_CHARS, min_len=900)  # longer than the 1023 kept
        yield [text, key, value]


spec(name="Info_ValueForKey", rust="shared::info::value_for_key", args=[CStr(), CStr()], ret=RetCStr(),
     cases=_lookup_cases)
spec(name="Info_RemoveKey", rust="shared::info::remove_key", args=[Buffer(size=2600, fill=0x41), CStr()],
     ret=RetVoid(), cases=_remove_cases)
spec(name="Info_RemoveKey_Big", rust="shared::info::remove_key_big",
     args=[Buffer(size=2600, fill=0x41), CStr()], ret=RetVoid(), cases=_remove_cases)
spec(name="Info_SetValueForKey", rust="shared::info::set_value_for_key",
     args=[Buffer(size=4096, fill=0x41), CStr(), CStr()], ret=RetVoid(), cases=_set_cases)
spec(name="Info_SetValueForKey_Big", rust="shared::info::set_value_for_key_big",
     args=[Buffer(size=4096, fill=0x41), CStr(), CStr()], ret=RetVoid(), cases=_set_cases)
