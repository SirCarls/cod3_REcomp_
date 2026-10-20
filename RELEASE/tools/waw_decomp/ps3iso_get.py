#!/usr/bin/env python3
"""
ps3iso_get.py - list or copy files from a PlayStation 3 game disc image, without
unpacking the whole disc. The image is only read, never changed.

A PS3 game disc holds an ISO 9660 file system: PS3_DISC.SFB, PS3_GAME/PARAM.SFO
(title, title ID, versions), PS3_GAME/USRDIR/EBOOT.BIN (the game program) and the
game's data. Dumps of the disc as it is ("redump" images) keep the disc's sector
encryption; this tool decrypts nothing, so it reads only images that are already
plain, and says so when a file it copies doesn't look plain.

The game program itself (EBOOT.BIN, and *.self / *.sprx modules) is a SELF: an ELF
signed and encrypted for the console. This tool only reports that; it doesn't open it.

Usage (from the project folder, Linux or Windows):
    python3 tools/waw_decomp/ps3iso_get.py "<the game>.iso" <out folder>          program files + PARAM.SFO
    python3 tools/waw_decomp/ps3iso_get.py "<the game>.iso" <out folder> --list   list every file on the disc
    python3 tools/waw_decomp/ps3iso_get.py "<the game>.iso" <out folder> --file PS3_GAME/USRDIR/x.cfg

Options:
    --pattern GLOB   extra file name pattern to copy (repeatable), e.g. --pattern "*.map"
"""
import argparse
import fnmatch
import os
import struct
import sys

SECTOR = 2048
CHUNK = 1 << 20
PROGRAM_PATTERNS = ["eboot.bin", "*.self", "*.sprx", "*.prx", "*.elf"]
INFO_FILES = ["PS3_DISC.SFB", "PS3_GAME/PARAM.SFO"]
MAX_DEPTH = 32


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return str(n)


class Iso9660:
    """The primary volume of an ISO 9660 image: walk its directories, read its files."""

    def __init__(self, f):
        self.f = f
        f.seek(0, os.SEEK_END)
        self.size = f.tell()
        self.root = None
        for sector in range(16, 64):
            d = self.read(sector * SECTOR, SECTOR)
            if len(d) < SECTOR or d[1:6] != b"CD001":
                break
            if d[0] == 1:  # primary volume descriptor
                block = struct.unpack_from("<H", d, 128)[0]
                if block != SECTOR:
                    raise ValueError(f"logical block size {block}, expected {SECTOR}")
                self.root = self.record(d, 156)
                break
            if d[0] == 255:  # set terminator
                break
        if self.root is None:
            raise ValueError("no ISO 9660 primary volume descriptor (not a PS3 disc image, or an encrypted one: "
                             "this tool decrypts nothing)")

    def read(self, offset, n):
        if offset < 0 or offset + n > self.size:
            raise ValueError(f"read past the end of the image (0x{offset:X} + {n})")
        self.f.seek(offset)
        return self.f.read(n)

    @staticmethod
    def record(buf, at):
        """(name, is_dir, more_extents, lba, size) of the directory record at buf[at]."""
        length = buf[at]
        lba = struct.unpack_from("<I", buf, at + 2)[0]
        size = struct.unpack_from("<I", buf, at + 10)[0]
        flags = buf[at + 25]
        name_len = buf[at + 32]
        raw = bytes(buf[at + 33:at + 33 + name_len])
        if raw in (b"\x00", b"\x01"):
            name = raw.decode("latin-1")
        else:
            name = raw.decode("latin-1").split(";")[0]
        return name, bool(flags & 2), bool(flags & 0x80), lba, size, length

    def entries(self, lba, size):
        """The records of one directory, multi-extent files joined: [(name, is_dir, [(lba, size)])]."""
        data = self.read(lba * SECTOR, size)
        out, at = [], 0
        while at < len(data):
            if data[at] == 0:  # records don't cross sectors: the rest of this one is padding
                at = (at // SECTOR + 1) * SECTOR
                continue
            name, is_dir, more, rlba, rsize, length = self.record(data, at)
            if length < 34:
                raise ValueError(f"bad directory record at sector {lba}")
            at += length
            if name in ("\x00", "\x01"):
                continue
            if out and out[-1][3]:  # the previous record said the file goes on in this one
                out[-1][2].append((rlba, rsize))
                out[-1][3] = more
                continue
            out.append([name, is_dir, [(rlba, rsize)], more])
        return [(n, d, ext) for n, d, ext, _ in out]

    def walk(self, lba=None, size=None, prefix="", depth=0):
        """(path, is_dir, extents, total size) for every file and folder, depth first."""
        if depth > MAX_DEPTH:
            raise ValueError("directories nested too deep")
        if lba is None:
            _, _, _, lba, size, _ = self.root
        for name, is_dir, extents in self.entries(lba, size):
            if not name or "/" in name or "\\" in name or name in (".", ".."):
                raise ValueError(f"unsafe name {name!r} on the disc")
            path = prefix + name
            if is_dir:
                yield path, True, extents, 0
                yield from self.walk(extents[0][0], extents[0][1], path + "/", depth + 1)
            else:
                yield path, False, extents, sum(s for _, s in extents)

    def copy(self, extents, dst):
        with open(dst, "wb") as o:
            for lba, size in extents:
                offset, left = lba * SECTOR, size
                while left:
                    buf = self.read(offset, min(CHUNK, left))
                    o.write(buf)
                    offset += len(buf)
                    left -= len(buf)


def kind_of(head):
    """What the first bytes of a copied file say it is."""
    if head[:4] == b"SCE\x00":
        return "SELF (signed and encrypted for the console; not opened here)"
    if head[:4] == b"\x7fELF":
        return "ELF (plain)"
    if head[:4] == b"\x00PSF":
        return "PARAM.SFO"
    if head[:8] == b".SFB\x00\x00\x00\x00" or head[:4] == b".SFB":
        return "PS3_DISC.SFB"
    return "unknown"


def read_sfo(path):
    """PARAM.SFO's keys and values (strings and integers)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x00PSF":
        return {}
    key_table, data_table, count = struct.unpack_from("<III", d, 8)
    out = {}
    for i in range(count):
        key_off, fmt, length, _, data_off = struct.unpack_from("<HHIII", d, 20 + i * 16)
        key = d[key_table + key_off:d.index(b"\x00", key_table + key_off)].decode("latin-1")
        raw = d[data_table + data_off:data_table + data_off + length]
        out[key] = struct.unpack_from("<I", raw)[0] if fmt == 0x0404 and length >= 4 \
            else raw.rstrip(b"\x00").decode("utf-8", "replace")
    return out


def is_ps3_image(path):
    """True when the image has a readable ISO 9660 volume with PS3_GAME/PARAM.SFO in it."""
    try:
        with open(path, "rb") as f:
            iso = Iso9660(f)
            for name, is_dir, extents in iso.entries(iso.root[3], iso.root[4]):
                if is_dir and name.upper() == "PS3_GAME":
                    return any(n.upper() == "PARAM.SFO" for n, _, _ in iso.entries(*extents[0]))
    except (OSError, ValueError, struct.error):
        return False
    return False


def program_files(path):
    """[(path on the disc, size)] of the program files (EBOOT.BIN, *.self, *.sprx, ...)."""
    with open(path, "rb") as f:
        iso = Iso9660(f)
        return [(p, size) for p, is_dir, _, size in iso.walk() if not is_dir and
                any(fnmatch.fnmatch(os.path.basename(p).lower(), pat) for pat in PROGRAM_PATTERNS)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("out_dir")
    ap.add_argument("--list", action="store_true", help="list every file on the disc, copy nothing")
    ap.add_argument("--file", action="append", default=[], help="copy this file (path on the disc)")
    ap.add_argument("--pattern", action="append", default=[], help="extra file name pattern to copy")
    args = ap.parse_args()

    iso_path = os.path.abspath(args.iso)
    out_root = os.path.abspath(args.out_dir)
    if out_root == os.path.dirname(iso_path) or out_root.startswith(iso_path):
        sys.exit("error: choose an output folder that is not the disc image's own folder")

    with open(iso_path, "rb") as f:
        try:
            iso = Iso9660(f)
            files = [(p, ext, size) for p, is_dir, ext, size in iso.walk() if not is_dir]
        except ValueError as e:
            sys.exit(f"error: {e}")
        if args.list:
            for p, _, size in files:
                print(f"{size:>12}  {p}")
            print(f"[ps3iso_get] {len(files)} files, {human(sum(s for _, _, s in files))}")
            return
        wanted_names = {n.upper() for n in INFO_FILES + args.file}
        patterns = PROGRAM_PATTERNS + [p.lower() for p in args.pattern]
        wanted = [(p, ext, size) for p, ext, size in files
                  if p.upper() in wanted_names or (not args.file and
                                                   any(fnmatch.fnmatch(os.path.basename(p).lower(), pat)
                                                       for pat in patterns))]
        if not wanted:
            sys.exit("error: no matching files on this disc")
        os.makedirs(out_root, exist_ok=True)
        unplain = 0
        for p, ext, size in wanted:
            dst = os.path.abspath(os.path.join(out_root, *p.split("/")))
            if not dst.startswith(out_root + os.sep):
                sys.exit(f"error: unsafe path {p!r}")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst) and os.path.getsize(dst) == size:
                state = "already there"
            else:
                iso.copy(ext, dst)
                state = "copied"
            with open(dst, "rb") as c:
                kind = kind_of(c.read(16))
            if kind == "unknown" and (p.upper() in ("PS3_GAME/PARAM.SFO", "PS3_DISC.SFB") or
                                      os.path.basename(p).upper() == "EBOOT.BIN"):
                kind = "NOT PLAIN: the image looks encrypted here (this tool decrypts nothing)"
                unplain += 1
            print(f"[ps3iso_get] {state} {p} ({human(size)}): {kind}")
        sfo = os.path.join(out_root, "PS3_GAME", "PARAM.SFO")
        if os.path.exists(sfo):
            info = read_sfo(sfo)
            shown = ", ".join(f"{k} {info[k]}" for k in ("TITLE", "TITLE_ID", "APP_VER", "VERSION", "PS3_SYSTEM_VER")
                              if k in info)
            if shown:
                print(f"[ps3iso_get] PARAM.SFO: {shown}")
    print(f"[ps3iso_get] done -> {out_root}")
    if unplain:
        sys.exit(1)


if __name__ == "__main__":
    main()
