#!/usr/bin/env python3
"""
stfs_get.py - copy files out of an Xbox 360 STFS package (an Xbox Live Arcade game, a title
update, DLC: the files that start with "LIVE", "PIRS" or "CON "), without unpacking the whole
package. Read only: the package is never changed.

Used to get Call of Duty Classic's (XBLA) default.xex for decompiling.

Usage (from the project folder, Linux or Windows):
    python3 tools/x360_family/stfs_get.py "<package file>" dev/decomp-input/codclassic_360
    python3 tools/x360_family/stfs_get.py "<package file>" --list

Options:
    --pattern GLOB   file name pattern to copy (repeatable; default *.xex and *.dll)

The layout follows the public descriptions of STFS (Free60 wiki "STFS"; the same rules as the
open-source Velocity and wxPirs tools): 4 KB blocks after the header, with hash-table blocks
interleaved every 170 data blocks (and every 170*170 at the next level); a file is a chain of
blocks, consecutive or linked through the hash tables.
"""
import argparse
import fnmatch
import os
import struct
import sys

BLOCK = 0x1000


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, f):
        self.f = f
        head = self.read(0, 0x971A)
        self.magic = head[:4]
        if self.magic not in (b"LIVE", b"PIRS", b"CON "):
            sys.exit(f"error: not an STFS package (starts with {self.magic!r})")
        self.header_size, = struct.unpack_from(">I", head, 0x340)
        self.content_type, = struct.unpack_from(">I", head, 0x344)
        self.title_id, = struct.unpack_from(">I", head, 0x360)
        desc_type, = struct.unpack_from(">I", head, 0x3A9)
        if desc_type != 0:
            sys.exit("error: this package is SVOD (a disc-sized package with a .data folder), not STFS")
        vd = head[0x379:0x379 + 0x24]
        self.block_sep = vd[2]
        self.ft_blocks, = struct.unpack_from("<H", vd, 3)
        self.ft_start = u24le(vd, 5)
        self.alloc_blocks, = struct.unpack_from(">I", vd, 0x1C)
        # "female" packages (read-only, signed) keep one copy of each hash table, "male" ones two
        self.shift = (~self.block_sep) & 1
        self.first = (self.header_size + 0xFFF) & 0xFFFFF000
        self.display = head[0x411:0x411 + 0x80].decode("utf-16-be", "replace").split("\0")[0]

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def backing(self, n):
        """Data block number -> block number in the file, counting the hash tables before it."""
        r = (((n + 0xAA) // 0xAA) << self.shift) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << self.shift)
        return (1 << self.shift) + r + (((n + 0x70E4) // 0x70E4) << self.shift)

    def addr(self, n):
        return (self.backing(n) << 12) + self.first

    def hash_entry(self, n):
        """The level-0 hash entry of data block n: (sha1, status, next block)."""
        step0 = 0xAB if self.shift == 0 else 0xAC
        num = (n // 0xAA) * step0
        if n >= 0xAA:
            num += ((n // 0x70E4) + 1) << self.shift
            if n >= 0x70E4:
                num += 1 << self.shift
        a = (num << 12) + self.first + (n % 0xAA) * 0x18
        if self.shift == 1 and self.block_sep & 2:
            a += 0x1000  # the second copy of the table is the current one
        e = self.read(a, 0x18)
        return e[:20], e[20], u24be(e, 21)

    def chain(self, start, count, consecutive):
        n, out = start, []
        for _ in range(count):
            out.append(n)
            if consecutive:
                n += 1
            else:
                n = self.hash_entry(n)[2]
                if n == 0xFFFFFF:
                    break
        return out

    def entries(self):
        raw = b"".join(self.read(self.addr(b), BLOCK)
                       for b in self.chain(self.ft_start, self.ft_blocks, False))
        ents = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            flags = e[0x28]
            nlen = flags & 0x3F
            if nlen == 0:
                continue
            name = e[:nlen].decode("latin-1")
            ents.append({
                "index": i // 0x40, "name": name, "dir": bool(flags & 0x80), "consecutive": bool(flags & 0x40),
                "blocks": u24le(e, 0x29), "start": u24le(e, 0x2F),
                "parent": struct.unpack_from(">h", e, 0x32)[0], "size": struct.unpack_from(">I", e, 0x34)[0]})
        by_index = {e["index"]: e for e in ents}

        def path(e):
            parts, seen = [e["name"]], set()
            p = e["parent"]
            while p != -1 and p in by_index and p not in seen:
                seen.add(p)
                parts.append(by_index[p]["name"])
                p = by_index[p]["parent"]
            return "/".join(reversed(parts))
        for e in ents:
            e["path"] = path(e)
        return ents

    def extract(self, e, dst):
        left = e["size"]
        with open(dst, "wb") as o:
            for b in self.chain(e["start"], e["blocks"], e["consecutive"]):
                if left <= 0:
                    break
                buf = self.read(self.addr(b), min(BLOCK, left))
                if len(buf) < min(BLOCK, left):
                    sys.exit(f"error: package truncated while reading {e['path']}")
                o.write(buf)
                left -= len(buf)
        if left > 0:
            sys.exit(f"error: the block chain of {e['path']} ended {left} bytes early")


def safe(rel):
    parts = [p for p in rel.replace("\\", "/").split("/") if p not in ("", ".", "..")]
    return os.path.join(*parts) if parts else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package")
    ap.add_argument("out_dir", nargs="?", default=os.path.join("dev", "decomp-input", "stfs"))
    ap.add_argument("--list", action="store_true", help="only list the files")
    ap.add_argument("--pattern", action="append", default=[], help="file name pattern to copy")
    args = ap.parse_args()
    patterns = args.pattern or ["*.xex", "*.dll"]

    with open(args.package, "rb") as f:
        s = Stfs(f)
        print(f"[stfs_get] {s.magic.decode()} package, title {s.title_id:08X}, content type {s.content_type:08X}, "
              f"\"{s.display}\"; {s.alloc_blocks} blocks, {'one' if s.shift == 0 else 'two'} copies of each hash table")
        ents = s.entries()
        for e in ents:
            print(("<DIR> " if e["dir"] else f"{e['size']:>12} ") + e["path"])
        if args.list:
            return
        out_root = os.path.abspath(args.out_dir)
        if out_root == os.path.dirname(os.path.abspath(args.package)):
            sys.exit("error: choose an output folder that is not the package's own folder")
        wanted = [e for e in ents if not e["dir"]
                  and any(fnmatch.fnmatch(e["name"].lower(), p.lower()) for p in patterns)]
        if not wanted:
            sys.exit("error: no matching files in this package")
        for e in wanted:
            rel = safe(e["path"])
            dst = os.path.abspath(os.path.join(out_root, rel))
            if not dst.startswith(out_root + os.sep):
                sys.exit(f"error: unsafe path {e['path']!r}")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            s.extract(e, dst)
            with open(dst, "rb") as chk:
                magic = chk.read(4)
            note = "" if not e["name"].lower().endswith(".xex") or magic == b"XEX2" else "  (warning: does not start with XEX2)"
            print(f"[stfs_get] copied {e['path']} ({e['size']} bytes){note}")
    print(f"[stfs_get] done -> {out_root}")


if __name__ == "__main__":
    main()
