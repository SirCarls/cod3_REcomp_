"""[acct2-decomp] LZO1X decompression in plain Python (the format of Call of Duty 3's NCH blocks).

Written from the LZO1X stream format (the reference decoder's state machine, as described
in public documentation of the format; no code copied). Every read and every back-reference
is bounds-checked: a damaged or crafted stream raises LzoError, it never reads outside the
input or before the start of the output.
"""
from __future__ import annotations


class LzoError(ValueError):
    pass


def decompress(src: bytes, out_len: int, with_end: bool = False):
    """The decompressed bytes; exactly out_len of them, and the stream must end there.
    With with_end, (bytes, input position just after the end marker)."""
    n = len(src)
    out = bytearray()
    ip = 0

    def byte() -> int:
        nonlocal ip
        if ip >= n:
            raise LzoError("input ends early")
        b = src[ip]
        ip += 1
        return b

    def ext(base: int) -> int:
        # a zero byte adds 255; the first non-zero byte ends the count
        nonlocal ip
        t = 0
        while True:
            if ip >= n:
                raise LzoError("input ends early")
            if src[ip] != 0:
                break
            t += 255
            ip += 1
            if t > out_len + 255:
                raise LzoError("length out of range")
        return t + base + byte()

    def literals(count: int) -> None:
        nonlocal ip
        if ip + count > n:
            raise LzoError("literal run past the input")
        if len(out) + count > out_len:
            raise LzoError("output past its size")
        out.extend(src[ip:ip + count])
        ip += count

    def copy(dist: int, count: int) -> None:
        start = len(out) - dist
        if start < 0 or dist <= 0:
            raise LzoError("match before the start of the output")
        if len(out) + count > out_len:
            raise LzoError("output past its size")
        if dist >= count:
            out.extend(out[start:start + count])
        else:
            for k in range(count):
                out.append(out[start + k])

    state = "loop"
    t = 0
    if n == 0:
        raise LzoError("empty input")
    if src[0] > 17:
        ip = 1
        t = src[0] - 17
        if t < 4:
            state = "match_next"
        else:
            literals(t)
            state = "first_literal_run"
    while True:
        if state == "loop":
            t = byte()
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                t = ext(15)
            literals(t + 3)
            state = "first_literal_run"
            continue
        if state == "first_literal_run":
            t = byte()
            if t >= 16:
                state = "match"
                continue
            dist = 1 + 0x0800 + (t >> 2) + (byte() << 2)
            copy(dist, 3)
            state = "match_done"
            continue
        if state == "match":
            if t >= 64:
                dist = 1 + ((t >> 2) & 7) + (byte() << 3)
                copy(dist, (t >> 5) + 1)
            elif t >= 32:
                t &= 31
                if t == 0:
                    t = ext(31)
                lo = byte()
                hi = byte()
                dist = 1 + ((lo | (hi << 8)) >> 2)
                copy(dist, t + 2)
            elif t >= 16:
                far = (t & 8) << 11
                t &= 7
                if t == 0:
                    t = ext(7)
                lo = byte()
                hi = byte()
                dist = far + ((lo | (hi << 8)) >> 2)
                if dist == 0:
                    break                      # end of stream
                copy(dist + 0x4000, t + 2)
            else:
                dist = 1 + (t >> 2) + (byte() << 2)
                copy(dist, 2)
            state = "match_done"
            continue
        if state == "match_done":
            t = src[ip - 2] & 3
            if t == 0:
                state = "loop"
                continue
            state = "match_next"
            continue
        if state == "match_next":
            literals(t)
            t = byte()
            state = "match"
            continue
    if len(out) != out_len:
        raise LzoError(f"stream ended at {len(out)} bytes, {out_len} expected")
    return (bytes(out), ip) if with_end else bytes(out)

