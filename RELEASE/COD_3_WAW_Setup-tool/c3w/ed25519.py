"""Ed25519 signatures (RFC 8032), in plain Python: what the updater checks
releases with (c3w/update.py) and what tools/sign_release.py signs them with.

It follows the RFC's own reference code (section 6), with the checks its
verification asks for: the public key and R must be valid encodings of curve
points (y below p), and S must be below the group order. Python's standard
library has no Ed25519, and the setup tool needs no packages beyond it.

Speed: a check takes a few tens of milliseconds, once per update. Signing
isn't hardened against timing measurements, which doesn't matter for a key
used on its owner's own PC.

The RFC's code is under the Simplified BSD License (IETF Trust and the
persons identified as the document authors, 2017).
"""
from __future__ import annotations

import hashlib
from typing import Optional, Tuple

Point = Tuple[int, int, int, int]   # extended coordinates (X, Y, Z, T): x = X/Z, y = Y/Z, xy = T/Z

P = 2 ** 255 - 19
Q = 2 ** 252 + 27742317777372353535851937790883648493   # the group order (L)


def _inv(x: int) -> int:
    return pow(x, P - 2, P)


D = -121665 * _inv(121666) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _sha512_mod_q(data: bytes) -> int:
    return int.from_bytes(hashlib.sha512(data).digest(), "little") % Q


def _add(a: Point, b: Point) -> Point:
    pa = (a[1] - a[0]) * (b[1] - b[0]) % P
    pb = (a[1] + a[0]) * (b[1] + b[0]) % P
    pc = 2 * a[3] * b[3] * D % P
    pd = 2 * a[2] * b[2] % P
    e, f, g, h = pb - pa, pd - pc, pd + pc, pb + pa
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s: int, point: Point) -> Point:
    result: Point = (0, 1, 1, 0)   # the neutral element
    while s > 0:
        if s & 1:
            result = _add(result, point)
        point = _add(point, point)
        s >>= 1
    return result


def _equal(a: Point, b: Point) -> bool:
    return (a[0] * b[2] - b[0] * a[2]) % P == 0 and (a[1] * b[2] - b[1] * a[2]) % P == 0


def _recover_x(y: int, sign: int) -> Optional[int]:
    if y >= P:
        return None
    x2 = (y * y - 1) * _inv(D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * _inv(5) % P
_GX = _recover_x(_GY, 0)
assert _GX is not None
G: Point = (_GX, _GY, 1, _GX * _GY % P)


def _compress(point: Point) -> bytes:
    zinv = _inv(point[2])
    x = point[0] * zinv % P
    y = point[1] * zinv % P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decompress(data: bytes) -> Optional[Point]:
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _expand(secret: bytes) -> Tuple[int, bytes]:
    if len(secret) != 32:
        raise ValueError("an Ed25519 secret key is 32 bytes")
    h = hashlib.sha512(secret).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(secret: bytes) -> bytes:
    """The 32-byte public key of a 32-byte secret key (seed)."""
    a, _ = _expand(secret)
    return _compress(_mul(a, G))


def sign(secret: bytes, message: bytes) -> bytes:
    """The 64-byte signature of `message`."""
    a, prefix = _expand(secret)
    pub = _compress(_mul(a, G))
    r = _sha512_mod_q(prefix + message)
    rs = _compress(_mul(r, G))
    h = _sha512_mod_q(rs + pub + message)
    s = (r + h * a) % Q
    return rs + s.to_bytes(32, "little")


def verify(public: bytes, message: bytes, signature: bytes) -> bool:
    """True only for a valid signature of `message` by `public`. Never raises
    on bad input (wrong lengths, invalid points): it's False."""
    if not isinstance(public, (bytes, bytearray)) or not isinstance(signature, (bytes, bytearray)):
        return False
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decompress(bytes(public))
    if a is None:
        return False
    rs = bytes(signature[:32])
    r = _decompress(rs)
    if r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= Q:
        return False
    h = _sha512_mod_q(rs + bytes(public) + message)
    return _equal(_mul(s, G), _add(r, _mul(h, a)))
