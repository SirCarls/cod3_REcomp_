"""Checks for iw_pc::fs (both games): the list of referenced archives' pure
checksums a client sends a pure server. The search path is built in guest
memory as the game keeps it: a linked list of search path entries (next,
archive, folder, localized, language), each archive a structure with its
pure checksum at +0x308 and its referenced bytes at +0x310 (United
Offensive: +0x311 the user interface module, +0x312 the client game's). The
rest of each structure is random bytes the functions mustn't read."""
from __future__ import annotations

import struct
from dataclasses import dataclass

from harness_x86 import Arg, RetCStr, spec

# Where each game keeps the search path and the two numbers the list uses.
FS = {
    "cod2mp": dict(name="FS_ReferencedIwdPureChecksums", search_paths=0x019A1B6C, feed=0x00B1A5B4,
                   fake=0x00B1A5C4),
    "uomp": dict(name="FS_ReferencedPakPureChecksums", search_paths=0x0389FCFC, feed=0x049311E8,
                 fake=0x04935308),
}
ARCHIVE_SIZE = 0x320   # past the referenced bytes; the archives' real structure is larger
PURE_CHECKSUM = 0x308
REFERENCED = 0x310
SEARCH_PATH_SIZE = 0x14


def _i32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


@dataclass
class SearchPathEntry:
    archive: bool
    localized: int          # the search path's word at +0xC (any non-zero: localized)
    referenced: bytes       # the archive's bytes at +0x310, +0x311, +0x312
    pure_checksum: int
    noise: int              # seeds the structures' other bytes


@dataclass
class SearchPaths(Arg):
    """The search path (a list of SearchPathEntry), written to guest memory
    and fs_searchpaths pointed at it. The answerer gets a v: token: for each
    entry a flags byte (1 an archive, 2 localized, 4 any file referenced, 8
    the user interface's, 16 the client game's) and the pure checksum (i32,
    big-endian)."""
    image: str = ""
    at: str = "none"

    def words(self, oracle, value):
        import random
        pointer = 0
        for entry in reversed(value):
            noise = random.Random(entry.noise)
            node = bytearray(noise.getrandbits(8) for _ in range(SEARCH_PATH_SIZE))
            struct.pack_into("<I", node, 0, pointer)
            archive = 0
            if entry.archive:
                data = bytearray(noise.getrandbits(8) for _ in range(ARCHIVE_SIZE))
                struct.pack_into("<i", data, PURE_CHECKSUM, entry.pure_checksum)
                data[REFERENCED:REFERENCED + 3] = entry.referenced
                archive = oracle.put(bytes(data), 16)
            struct.pack_into("<I", node, 4, archive)
            struct.pack_into("<i", node, 0xC, entry.localized)
            pointer = oracle.put(bytes(node), 4)
        oracle.write(FS[self.image]["search_paths"], struct.pack("<I", pointer))
        return [0]

    def token(self, value):
        out = bytearray()
        for e in value:
            flags = (1 if e.archive else 0) | (2 if e.localized else 0)
            for bit, byte in zip((4, 8, 16), e.referenced):
                flags |= bit if byte else 0
            out += bytes([flags]) + struct.pack(">i", e.pure_checksum)
        return "v:" + out.hex()


@dataclass
class FsWord(Arg):
    """A 32-bit global the function reads (fs_checksumFeed, fs_fakeChkSum)."""
    address: int = 0
    at: str = "none"

    def words(self, oracle, value):
        oracle.write(self.address, struct.pack("<i", _i32(value)))
        return [0]

    def token(self, value):
        return f"i:{_i32(value)}"


def random_search_paths(rng, image: str, length: int | None = None) -> list[SearchPathEntry]:
    """A search path: folders, archives (mostly referenced, some localized),
    pure checksums from small to extreme; now and then hundreds of archives,
    so that the 8191-character list is cut."""
    if length is None:
        length = rng.choice([0, 1, 2, 5, 12, 30, rng.randint(0, 60), rng.randint(600, 800)])
    flag = lambda p: rng.choice([1, 0x80, 0xFF, rng.randint(2, 255)]) if rng.random() < p else 0
    module_chance = 0.15 if image == "uomp" else 0.5
    paths = []
    for _ in range(length):
        archive = rng.random() < 0.85
        paths.append(SearchPathEntry(
            archive=archive,
            localized=rng.choice([0, 0, 0, 0, 1, 0x100, -1]),
            referenced=bytes([flag(0.7), flag(module_chance), flag(module_chance)]),
            pure_checksum=_i32(rng.choice([0, 1, -1, rng.getrandbits(32), rng.getrandbits(32), -0x80000000,
                                           rng.randint(-999, 999)])),
            noise=rng.getrandbits(32)))
    return paths


def checksum_numbers(rng) -> tuple[int, int]:
    """The checksum feed and the fake checksum (mostly 0, as the game keeps it)."""
    feed = _i32(rng.choice([0, rng.getrandbits(32), rng.getrandbits(32), -1]))
    fake = 0 if rng.random() < 0.6 else _i32(rng.choice([1, -1, rng.getrandbits(32)]))
    return feed, fake


def _cases(image: str):
    def cases(rng):
        while True:
            feed, fake = checksum_numbers(rng)
            yield [random_search_paths(rng, image), feed, fake]
    return cases


for image, f in FS.items():
    spec(name=f["name"], image=image, rust="fs::referenced_pure_checksums",
         args=[SearchPaths(image=image), FsWord(address=f["feed"]), FsWord(address=f["fake"])],
         ret=RetCStr(), cases=_cases(image))
