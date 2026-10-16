"""AES-128 decryption (ECB and CBC) in plain Python.

The XEX2 container encrypts its image with AES-128 (FIPS-197). `xex.py` uses
`openssl` when the system has it, because it is much faster, and this module
otherwise, so the decomp tools need nothing but Python on Windows too.
Written from the FIPS-197 description; checked against its appendix C.1
example in `tests` below (`python3 aes.py`).
"""
from __future__ import annotations

# The S-box from FIPS-197 section 5.1.1, computed rather than typed in:
# the multiplicative inverse in GF(2^8), then the affine transform.


def _make_sbox() -> tuple[list[int], list[int]]:
    sbox = [0] * 256
    p = q = 1
    while True:
        # p walks through every non-zero element (multiply by 3);
        # q walks through their inverses (divide by 3).
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        rot = lambda v, n: ((v << n) | (v >> (8 - n))) & 0xFF  # noqa: E731
        sbox[p] = q ^ rot(q, 1) ^ rot(q, 2) ^ rot(q, 3) ^ rot(q, 4) ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    inv = [0] * 256
    for i, v in enumerate(sbox):
        inv[v] = i
    return sbox, inv


SBOX, INV_SBOX = _make_sbox()


def _xtime(a: int) -> int:
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _mul(a: int, b: int) -> int:
    out = 0
    while b:
        if b & 1:
            out ^= a
        a = _xtime(a)
        b >>= 1
    return out


# InvMixColumns lookup tables (one per coefficient 9, 11, 13, 14).
_M9 = [_mul(i, 9) for i in range(256)]
_M11 = [_mul(i, 11) for i in range(256)]
_M13 = [_mul(i, 13) for i in range(256)]
_M14 = [_mul(i, 14) for i in range(256)]


def _expand_key(key: bytes) -> list[list[int]]:
    """The 11 round keys of AES-128 (FIPS-197 section 5.2), 16 bytes each."""
    if len(key) != 16:
        raise ValueError("AES-128 needs a 16-byte key")
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        temp = list(words[i - 1])
        if i % 4 == 0:
            temp = temp[1:] + temp[:1]
            temp = [SBOX[b] for b in temp]
            temp[0] ^= rcon
            rcon = _xtime(rcon)
        words.append([a ^ b for a, b in zip(words[i - 4], temp)])
    return [sum(words[r * 4:r * 4 + 4], []) for r in range(11)]


def _decrypt_block(block: bytes, round_keys: list[list[int]]) -> bytes:
    """The inverse cipher (FIPS-197 section 5.3) on one 16-byte block."""
    s = [b ^ k for b, k in zip(block, round_keys[10])]
    for rnd in range(9, -1, -1):
        # InvShiftRows. The state is column-major: byte i is row i % 4, column i // 4,
        # and row r moves r columns to the right.
        s = [s[i % 4 + 4 * ((i // 4 - i % 4) % 4)] for i in range(16)]
        s = [INV_SBOX[b] for b in s]
        s = [b ^ k for b, k in zip(s, round_keys[rnd])]
        if rnd:
            out = []
            for c in range(4):
                a0, a1, a2, a3 = s[c * 4:c * 4 + 4]
                out += [_M14[a0] ^ _M11[a1] ^ _M13[a2] ^ _M9[a3],
                        _M9[a0] ^ _M14[a1] ^ _M11[a2] ^ _M13[a3],
                        _M13[a0] ^ _M9[a1] ^ _M14[a2] ^ _M11[a3],
                        _M11[a0] ^ _M13[a1] ^ _M9[a2] ^ _M14[a3]]
            s = out
    return bytes(s)


def decrypt_ecb(key: bytes, data: bytes) -> bytes:
    keys = _expand_key(key)
    return b"".join(_decrypt_block(data[i:i + 16], keys) for i in range(0, len(data) - 15, 16))


def decrypt_cbc(key: bytes, data: bytes, iv: bytes = bytes(16)) -> bytes:
    keys = _expand_key(key)
    out = bytearray()
    prev = iv
    for i in range(0, len(data) - 15, 16):
        block = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(_decrypt_block(block, keys), prev))
        prev = block
    return bytes(out)


def _self_test() -> None:
    # FIPS-197 appendix C.1 (AES-128).
    key = bytes(range(16))
    cipher = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
    plain = bytes.fromhex("00112233445566778899aabbccddeeff")
    assert decrypt_ecb(key, cipher) == plain, decrypt_ecb(key, cipher).hex()
    print("aes: FIPS-197 C.1 OK")


if __name__ == "__main__":
    _self_test()
