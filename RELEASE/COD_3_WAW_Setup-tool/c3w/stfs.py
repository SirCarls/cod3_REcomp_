"""[acct2-decomp] Read Xbox 360 STFS packages (marketplace downloads such as
map packs: files that start with "LIVE", "PIRS" or "CON "). Read only: the
package is never changed, and nothing in it is ever run.

Built to take untrusted files: every number read from the package is checked
before it is used (block numbers against the package's own block count and
the file's size, name lengths, the file table's size, folder depth), block
chains can't loop, and file names are checked before they become paths.
Anything wrong raises StfsError; nothing is written for a bad package.

The layout follows the public descriptions of STFS (Free60 wiki "STFS"; the
same rules as the open-source Velocity and wxPirs tools), as in
tools/x360_family/stfs_get.py: 4 KB blocks after the header, hash-table
blocks every 170 data blocks (and every 170*170 at the next level); a file is
a run of blocks, consecutive or linked through the hash tables.
"""
from __future__ import annotations

import os
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Callable, Dict, List, Optional

BLOCK = 0x1000
MAGICS = (b"LIVE", b"PIRS", b"CON ")
HEADER_READ = 0x971A          # enough for every header field used here
MAX_HEADER = 0xB000           # real headers are 0x971A rounded up; refuse anything far bigger
MAX_TABLE_BLOCKS = 64         # 64 blocks = 4096 entries
MAX_ENTRIES = 4096
MAX_DEPTH = 8
MAX_NAME = 40                 # the field is 0x28 bytes
END_OF_CHAIN = 0xFFFFFF
# Characters a file name in a package may use. The real packages use letters,
# digits, "_", "-" and "."; anything else (slashes, colons, control bytes,
# Windows' reserved characters) is refused rather than changed.
NAME_OK = re.compile(r"^[A-Za-z0-9_.\-]+$")
WINDOWS_RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)), *(f"lpt{i}" for i in range(1, 10))}


class StfsError(Exception):
    """The file is not an STFS package, or it is damaged or built to mislead."""


def _u24le(b: bytes, o: int) -> int:
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def _u24be(b: bytes, o: int) -> int:
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


def check_name(name: str) -> str:
    """A single file or folder name from a package, or StfsError."""
    if not name or len(name) > MAX_NAME or not NAME_OK.match(name) or name in (".", ".."):
        raise StfsError(f"unsafe name in the package: {name!r}")
    if name.endswith(".") or name.split(".")[0].lower() in WINDOWS_RESERVED:
        raise StfsError(f"unsafe name in the package: {name!r}")
    return name


@dataclass(frozen=True)
class Entry:
    index: int
    path: str          # "mp/mp_farm/mp_farm.cod", always "/" separated, checked
    is_dir: bool
    size: int
    blocks: int
    start: int
    consecutive: bool


class StfsPackage:
    """An open STFS package. Use as a context manager, or call close()."""

    def __init__(self, path: Path):
        self.path = Path(path)
        self.f: BinaryIO = open(self.path, "rb")
        try:
            self.file_size = os.fstat(self.f.fileno()).st_size
            self._read_header()
        except Exception:
            self.f.close()
            raise

    def __enter__(self) -> "StfsPackage":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self.f.close()

    # ------------------------------------------------------------ header
    def _read(self, off: int, n: int) -> bytes:
        if off < 0 or n < 0 or off + n > self.file_size:
            raise StfsError(f"the package is cut short (wanted {n} bytes at {off:#x}, it has {self.file_size})")
        self.f.seek(off)
        data = self.f.read(n)
        if len(data) != n:
            raise StfsError("the package is cut short")
        return data

    def _read_header(self) -> None:
        if self.file_size < HEADER_READ:
            raise StfsError("too small to be an STFS package")
        head = self._read(0, HEADER_READ)
        self.magic = head[:4]
        if self.magic not in MAGICS:
            raise StfsError("not an Xbox 360 package (it doesn't start with LIVE, PIRS or CON)")
        self.header_size, = struct.unpack_from(">I", head, 0x340)
        if not 0x971A <= self.header_size <= MAX_HEADER:
            raise StfsError(f"odd header size {self.header_size:#x}")
        self.content_type, = struct.unpack_from(">I", head, 0x344)
        self.title_id, = struct.unpack_from(">I", head, 0x360)
        desc_type, = struct.unpack_from(">I", head, 0x3A9)
        if desc_type != 0:
            raise StfsError("this package is SVOD (disc-sized content), not STFS")
        vd = head[0x379:0x379 + 0x24]
        if vd[0] != 0x24:
            raise StfsError("odd volume descriptor")
        self.block_sep = vd[2]
        self.table_blocks, = struct.unpack_from("<H", vd, 3)
        self.table_start = _u24le(vd, 5)
        self.alloc_blocks, = struct.unpack_from(">I", vd, 0x1C)
        if not 1 <= self.table_blocks <= MAX_TABLE_BLOCKS:
            raise StfsError(f"odd file table size ({self.table_blocks} blocks)")
        # One copy of each hash table in read-only ("female") packages, two otherwise.
        self.shift = (~self.block_sep) & 1
        self.first = (self.header_size + 0xFFF) & ~0xFFF
        # The highest data block the file can hold, whatever the header claims.
        self.max_block = min(self.alloc_blocks, (self.file_size - self.first) // BLOCK + 1)
        if self.max_block <= 0:
            raise StfsError("no data blocks")
        name = head[0x411:0x411 + 0x80].decode("utf-16-be", "replace").split("\0")[0]
        self.display_name = "".join(c for c in name if c.isprintable())[:64]

    # ------------------------------------------------------------ blocks
    def _check_block(self, n: int) -> None:
        if not 0 <= n < self.max_block:
            raise StfsError(f"block {n} is outside the package")

    def _backing(self, n: int) -> int:
        r = (((n + 0xAA) // 0xAA) << self.shift) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << self.shift)
        return (1 << self.shift) + r + (((n + 0x70E4) // 0x70E4) << self.shift)

    def _addr(self, n: int) -> int:
        self._check_block(n)
        return (self._backing(n) << 12) + self.first

    def _next(self, n: int) -> int:
        step0 = 0xAB if self.shift == 0 else 0xAC
        num = (n // 0xAA) * step0
        if n >= 0xAA:
            num += ((n // 0x70E4) + 1) << self.shift
            if n >= 0x70E4:
                num += 1 << self.shift
        a = (num << 12) + self.first + (n % 0xAA) * 0x18
        if self.shift == 1 and self.block_sep & 2:
            a += 0x1000
        e = self._read(a, 0x18)
        return _u24be(e, 21)

    def _chain(self, start: int, count: int, consecutive: bool) -> List[int]:
        if not 0 <= count <= self.max_block:
            raise StfsError(f"odd block count {count}")
        out: List[int] = []
        seen = set()
        n = start
        for _ in range(count):
            self._check_block(n)
            if n in seen:
                raise StfsError("a block chain loops")
            seen.add(n)
            out.append(n)
            n = n + 1 if consecutive else self._next(n)
            if n == END_OF_CHAIN:
                break
        return out

    # ------------------------------------------------------------ files
    def entries(self) -> List[Entry]:
        raw = b"".join(self._read(self._addr(b), BLOCK)
                       for b in self._chain(self.table_start, self.table_blocks, False))
        rows = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            flags = e[0x28]
            nlen = flags & 0x3F
            if nlen == 0:
                continue
            if len(rows) >= MAX_ENTRIES:
                raise StfsError("too many files in the package")
            if nlen > MAX_NAME:
                raise StfsError("a file name is too long")
            name = check_name(e[:nlen].decode("latin-1"))
            rows.append((i // 0x40, name, bool(flags & 0x80), bool(flags & 0x40), _u24le(e, 0x29),
                         _u24le(e, 0x2F), struct.unpack_from(">h", e, 0x32)[0], struct.unpack_from(">I", e, 0x34)[0]))
        by_index = {r[0]: r for r in rows}
        out: List[Entry] = []
        seen_paths: Dict[str, int] = {}
        for idx, name, is_dir, consecutive, blocks, start, parent, size in rows:
            parts = [name]
            p, depth = parent, 0
            while p != -1:
                if p not in by_index or not by_index[p][2]:
                    raise StfsError(f"{name}: its folder is missing from the file table")
                depth += 1
                if depth > MAX_DEPTH:
                    raise StfsError(f"{name}: folders too deep (or looping)")
                parts.append(by_index[p][1])
                p = by_index[p][6]
            path = "/".join(reversed(parts))
            key = path.lower()                # one name per folder on Windows too
            if key in seen_paths:
                raise StfsError(f"two files named {path} (letter case aside)")
            seen_paths[key] = idx
            if not is_dir:
                if size > blocks * BLOCK or blocks > self.max_block:
                    raise StfsError(f"{path}: its size doesn't match its blocks")
            out.append(Entry(idx, path, is_dir, 0 if is_dir else size, blocks, start, consecutive))
        return out

    def read_file(self, e: Entry, limit: int = 1 << 20) -> bytes:
        """A small file's bytes (dlmaps.txt); refuses anything over `limit`."""
        if e.is_dir or e.size > limit:
            raise StfsError(f"{e.path}: too big to read whole")
        chunks = []
        self.copy_file(e, chunks.append)
        return b"".join(chunks)

    def copy_file(self, e: Entry, write: Callable[[bytes], object],
                  check_cancel: Optional[Callable[[], None]] = None) -> None:
        left = e.size
        for b in self._chain(e.start, e.blocks, e.consecutive):
            if left <= 0:
                break
            if check_cancel:
                check_cancel()
            buf = self._read(self._addr(b), min(BLOCK, left))
            write(buf)
            left -= len(buf)
        if left > 0:
            raise StfsError(f"{e.path}: its blocks end {left} bytes early")
