"""Checks for iw_pc::shared::checksum (both games): the MD4 block checksums
archives are compared by. The data is random bytes or, as the file system
uses them, lists of CRC-32 words; lengths across MD4's 64-byte blocks and
its 56-byte padding edge."""
from __future__ import annotations

import struct

from harness_x86 import Int, RetI32, spec
from specs.net_msg import RawBytes


def _data(rng) -> bytes:
    if rng.random() < 0.4:   # an archive's CRC list
        return b"".join(struct.pack("<I", rng.getrandbits(32)) for _ in range(rng.choice([0, 1, 14, 16, rng.randint(0, 600)])))
    length = rng.choice([0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 128, rng.randint(0, 3000)])
    return bytes(rng.getrandbits(8) for _ in range(length))


def _cases(keyed: bool):
    def cases(rng):
        while True:
            data = _data(rng)
            # The length passed: the data's, or a shorter one (the function reads only that much).
            length = len(data) if rng.random() < 0.9 else rng.randint(0, len(data))
            if keyed:
                yield [data, length, rng.choice([0, -1, 1, rng.getrandbits(31), -rng.getrandbits(31)])]
            else:
                yield [data, length]
    return cases


for image in ("cod2mp", "uomp"):
    spec(name="Com_BlockChecksum", image=image, rust="shared::checksum::block_checksum",
         args=[RawBytes(), Int()], ret=RetI32(), cases=_cases(False))
    spec(name="Com_BlockChecksumKey", image=image, rust="shared::checksum::block_checksum_key",
         args=[RawBytes(), Int(), Int()], ret=RetI32(), cases=_cases(True))
