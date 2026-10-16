"""Xbox / Xbox 360 disc image (XDVDFS) reader.

Handles redump-style images (XGD1/XGD2/XGD3, which carry a video partition in
front of the game data) and trimmed XISOs. Pure Python, read-only: the ISO is
never modified.
"""
from __future__ import annotations

import struct
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Callable, Iterator, List, Optional

from .common import Cancelled, Log, SetupError, human, is_within

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = [
    ("XISO", 0x00000000),
    ("XGD2", 0x0FD90000),  # most 2006-2010 Xbox 360 titles (CoD3 is XGD2)
    ("XGD3", 0x02080000),
    ("XGD1", 0x18300000),
]
ATTR_DIRECTORY = 0x10
CHUNK = 8 * 1024 * 1024
MAX_DEPTH = 32   # folder levels (game discs use a handful)


@dataclass
class Entry:
    path: str          # forward-slash relative path
    is_dir: bool
    sector: int
    size: int


class XisoImage:
    def __init__(self, path: Path):
        self.path = Path(path)
        self._f: BinaryIO = open(self.path, "rb")
        self.kind, self.base, root_sector, root_size = self._find_partition()
        self.entries: List[Entry] = list(self._walk(root_sector, root_size, ""))

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> "XisoImage":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- layout --------------------------------------------------------------
    def _find_partition(self):
        for kind, off in PARTITIONS:
            self._f.seek(off + 32 * SECTOR)
            hdr = self._f.read(SECTOR)
            if len(hdr) == SECTOR and hdr[:20] == MAGIC and hdr[0x7EC:0x7EC + 20] == MAGIC:
                root_sector, root_size = struct.unpack_from("<II", hdr, 20)
                return kind, off, root_sector, root_size
        raise SetupError(f"{self.path.name} is not an Xbox / Xbox 360 game image (no XDVDFS volume found).")

    def _read_dir(self, sector: int, size: int):
        if size == 0:
            return
        self._f.seek(self.base + sector * SECTOR)
        data = self._f.read(size)
        stack, seen = [0], set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue  # sector padding
            name = data[off + 14:off + 14 + nlen].decode("latin-1")
            yield name, attrs, start, fsize
            for child in (left, right):
                if child and child != 0xFFFF and child * 4 < len(data):
                    stack.append(child * 4)

    def _walk(self, sector: int, size: int, prefix: str, seen: Optional[set] = None) -> Iterator[Entry]:
        # A damaged or crafted image can list a folder inside itself, or one
        # folder in many places: each folder's table is read once, and the
        # folders nest at most MAX_DEPTH deep.
        seen = set() if seen is None else seen
        if sector in seen or prefix.count("/") >= MAX_DEPTH:
            return
        seen.add(sector)
        for name, attrs, start, fsize in sorted(self._read_dir(sector, size), key=lambda e: e[0].lower()):
            safe = name.replace("\\", "_").replace("/", "_").replace(":", "_").replace("\0", "_")
            if safe in ("", ".", ".."):
                continue
            rel = prefix + safe
            if attrs & ATTR_DIRECTORY:
                yield Entry(rel, True, start, fsize)
                yield from self._walk(start, fsize, rel + "/", seen)
            else:
                yield Entry(rel, False, start, fsize)

    # -- access --------------------------------------------------------------
    @property
    def files(self) -> List[Entry]:
        return [e for e in self.entries if not e.is_dir]

    @property
    def total_size(self) -> int:
        return sum(e.size for e in self.files)

    def find(self, rel: str) -> Optional[Entry]:
        rel = rel.replace("\\", "/").lower()
        for e in self.entries:
            if e.path.lower() == rel:
                return e
        return None

    def read(self, rel: str, limit: Optional[int] = None) -> bytes:
        e = self.find(rel)
        if not e or e.is_dir:
            raise SetupError(f"{rel} not found in {self.path.name}")
        self._f.seek(self.base + e.sector * SECTOR)
        return self._f.read(e.size if limit is None else min(limit, e.size))

    def extract(self, dest: Path, log: Log, cancel: threading.Event,
                only: Optional[Callable[[str], bool]] = None) -> None:
        """Copies the image's files into `dest`. `only`, if given, picks the
        files to copy by their relative path."""
        dest = Path(dest)
        dest.mkdir(parents=True, exist_ok=True)
        wanted = [e for e in self.files if only is None or only(e.path)]
        files = sorted(wanted, key=lambda e: (e.path.lower() != "default.xex", e.path.lower()))
        size = sum(e.size for e in files)
        total = size or 1   # progress fractions divide by this
        log.info(f"{self.path.name}: layout {self.kind}, {len(files)} files, {human(size)}")
        for e in self.entries:
            if e.is_dir and (only is None or only(e.path + "/")):
                if not is_within(dest / e.path, dest):
                    raise SetupError(f"Unsafe path in image: {e.path}")
                (dest / e.path).mkdir(parents=True, exist_ok=True)
        done, t0, last = 0, time.time(), 0.0
        for e in files:
            if cancel.is_set():
                raise Cancelled()
            out = dest / e.path
            if not is_within(out, dest):
                raise SetupError(f"Unsafe path in image: {e.path}")
            out.parent.mkdir(parents=True, exist_ok=True)
            if out.exists() and out.stat().st_size == e.size:
                done += e.size
                continue
            tmp = out.with_name(out.name + ".part")
            self._f.seek(self.base + e.sector * SECTOR)
            with open(tmp, "wb") as o:
                left = e.size
                while left:
                    if cancel.is_set():
                        raise Cancelled()
                    buf = self._f.read(min(CHUNK, left))
                    if not buf:
                        raise SetupError(f"The image is truncated (while reading {e.path}).")
                    o.write(buf)
                    left -= len(buf)
                    done += len(buf)
                    now = time.time()
                    if now - last > 0.25:
                        last = now
                        rate = done / max(now - t0, 1e-3)
                        log.progress(done / total, f"{e.path}  ({human(rate)}/s)")
            tmp.replace(out)
        log.progress(1.0, f"extracted {human(size)}")


def looks_like_xiso(path: Path) -> bool:
    try:
        with XisoImage(path):
            return True
    except (SetupError, OSError):
        return False
