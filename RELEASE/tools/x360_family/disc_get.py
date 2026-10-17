#!/usr/bin/env python3
"""
disc_get.py - list a game disc and copy files off it, read only.

Reads the disc formats Call of Duty 3 shipped on:
  * PlayStation 2 / PlayStation 3 / plain ISO9660 images (.iso, 2048-byte
    sectors, or raw 2352-byte CD images),
  * original Xbox and Xbox 360 images (XDVDFS: redump XGD1/XGD2/XGD3 or
    trimmed XISO),
  * a folder (an extracted disc or a decrypted PS3 "JB folder"),
  * a .7z archive of such a folder (needs the 7z / 7zz command, or the
    py7zr Python package).
GameCube and Wii images are recognised and refused (not supported yet).

Usage:
  disc_get.py list   SOURCE [--out files.tsv]
  disc_get.py get    SOURCE PATTERN... --to DIR [--max-mb N]
  disc_get.py glyphs SOURCE --to DIR [--max-mb N] [--total-mb N]

  list     print every file (size, path) and what system the disc is for.
  get      copy the files whose path matches any PATTERN (shell-style,
           case-insensitive, e.g. '*.fnt' 'global*' 'USRDIR/*font*').
  glyphs   the button-picture preset: writes DIR/files.tsv (the whole list)
           and DIR/disc.txt, then copies the program file and every file
           whose name suggests fonts, buttons, icons or front-end/UI data
           into DIR/files/.

Nothing is ever written to the source.  Re-running skips files already
copied with the right size.
"""
import argparse
import fnmatch
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 2048
CHUNK = 8 * 1024 * 1024

# --------------------------------------------------------------------------
# small helpers


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def clean_rel(path):
    """Normalise an in-disc path to a/b/c with no '.', '..' or empty parts."""
    parts = []
    for p in re.split(r"[\\/]+", path):
        if p in ("", "."):
            continue
        if p == "..":
            raise ValueError(f"refusing unsafe path {path!r}")
        parts.append(p)
    if not parts:
        raise ValueError(f"refusing empty path {path!r}")
    return "/".join(parts)


def safe_dest(root, rel):
    root = os.path.abspath(root)
    dst = os.path.abspath(os.path.join(root, *clean_rel(rel).split("/")))
    if not dst.startswith(root + os.sep):
        raise ValueError(f"refusing unsafe path {rel!r}")
    return dst


class Entry:
    __slots__ = ("path", "size", "extents", "src")

    def __init__(self, path, size, extents=None, src=None):
        self.path = path          # a/b/c (disc-relative, '/' separators)
        self.size = size
        self.extents = extents    # [(byte offset, length)] inside an image
        self.src = src            # a real file (folder source) or archive member name


# --------------------------------------------------------------------------
# ISO9660 (PS2, PS3, PC discs)


class SectorReader:
    """Reads 2048-byte user-data sectors from a cooked or raw CD/DVD image."""

    def __init__(self, f, raw_size=SECTOR, data_off=0):
        self.f, self.raw, self.off = f, raw_size, data_off

    def read(self, lba, nbytes):
        if self.raw == SECTOR:
            self.f.seek(lba * SECTOR)
            return self.f.read(nbytes)
        out = bytearray()
        while len(out) < nbytes:
            self.f.seek(lba * self.raw + self.off)
            blk = self.f.read(SECTOR)
            if not blk:
                break
            out += blk
            lba += 1
        return bytes(out[:nbytes])

    def byte_offset(self, lba):
        return lba * self.raw + self.off


def iso9660_probe(f):
    # cooked 2048, raw 2352 mode 1 (16-byte header), raw 2352 mode 2 form 1 (24)
    for raw, off in ((SECTOR, 0), (2352, 16), (2352, 24)):
        f.seek(16 * raw + off)
        pvd = f.read(SECTOR)
        if len(pvd) == SECTOR and pvd[0] == 1 and pvd[1:6] == b"CD001":
            return SectorReader(f, raw, off), pvd
    return None, None


def iso9660_entries(f):
    rd, pvd = iso9660_probe(f)
    block = struct.unpack_from("<H", pvd, 128)[0] or SECTOR
    if block != SECTOR:
        raise SystemExit(f"error: ISO9660 block size {block} is not supported")
    root = pvd[156:156 + 34]
    out = []
    seen = set()

    def walk(lba, size, prefix, depth):
        if depth > 32 or (lba, size) in seen:
            return
        seen.add((lba, size))
        data = rd.read(lba, size)
        pos = 0
        pending = None  # multi-extent file being assembled
        while pos < len(data):
            rlen = data[pos]
            if rlen == 0:  # records never cross a sector: skip to the next one
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + rlen]
            pos += rlen
            if len(rec) < 34:
                break
            ext = struct.unpack_from("<I", rec, 2)[0]
            dlen = struct.unpack_from("<I", rec, 10)[0]
            flags = rec[25]
            nlen = rec[32]
            name = rec[33:33 + nlen]
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("latin-1")
            if ";" in name:
                name = name.split(";", 1)[0]
            name = name.rstrip(".") or name
            rel = prefix + name
            if flags & 0x02:
                walk(ext, dlen, rel + "/", depth + 1)
                continue
            piece = (rd.byte_offset(ext), dlen)
            if pending and pending.path == rel:
                pending.extents.append(piece)
                pending.size += dlen
            else:
                pending = Entry(rel, dlen, [piece])
                out.append(pending)
            if not flags & 0x80:  # last (or only) extent
                pending = None

    walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "", 0)
    if rd.raw != SECTOR:
        # raw images: a file's bytes are not contiguous; read sector by sector
        for e in out:
            e.extents = [("raw", rd, piece) for piece in e.extents]
    return out


# --------------------------------------------------------------------------
# XDVDFS (original Xbox, Xbox 360)

XDVD_MAGIC = b"MICROSOFT*XBOX*MEDIA"
XDVD_PARTITIONS = [
    ("XISO", 0x00000000),
    ("XGD2", 0x0FD90000),
    ("XGD3", 0x02080000),
    ("XGD1", 0x18300000),
]


def xdvdfs_probe(f):
    for kind, off in XDVD_PARTITIONS:
        f.seek(off + 32 * SECTOR)
        hdr = f.read(SECTOR)
        if len(hdr) == SECTOR and hdr[:20] == XDVD_MAGIC and hdr[0x7EC:0x7EC + 20] == XDVD_MAGIC:
            root_sector, root_size = struct.unpack_from("<II", hdr, 20)
            return kind, off, root_sector, root_size
    return None


def xdvdfs_entries(f, probe):
    kind, base, root_sector, root_size = probe
    out = []
    seen = set()

    def read_dir(sector, size):
        if size == 0:
            return []
        f.seek(base + sector * SECTOR)
        data = f.read(size)
        stack, visited, items = [0], set(), []
        while stack:
            off = stack.pop()
            if off in visited or off + 14 > len(data):
                continue
            visited.add(off)
            left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue
            name = data[off + 14:off + 14 + nlen].decode("latin-1")
            items.append((name, attrs, start, fsize))
            for child in (left, right):
                if child and child != 0xFFFF and child * 4 < len(data):
                    stack.append(child * 4)
        return items

    def walk(sector, size, prefix, depth):
        if depth > 32 or (sector, size) in seen:
            return
        seen.add((sector, size))
        for name, attrs, start, fsize in sorted(read_dir(sector, size), key=lambda e: e[0].lower()):
            if name in ("", ".", "..") or "/" in name or "\\" in name:
                continue
            rel = prefix + name
            if attrs & 0x10:
                walk(start, fsize, rel + "/", depth + 1)
            else:
                out.append(Entry(rel, fsize, [(base + start * SECTOR, fsize)]))

    walk(root_sector, root_size, "", 0)
    return kind, out


# --------------------------------------------------------------------------
# sources


class Source:
    kind = "?"
    layout = ""

    def entries(self):
        raise NotImplementedError

    def copy(self, entries, out_root, log):
        raise NotImplementedError

    def close(self):
        pass


class ImageSource(Source):
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        head = self.f.read(0x20)
        if len(head) >= 0x20 and head[0x18:0x1C] == b"\x5D\x1C\x9E\xA3":
            raise SystemExit("error: this is a Wii disc image; Wii discs are not supported yet")
        if len(head) >= 0x20 and head[0x1C:0x20] == b"\xC2\x33\x9F\x3D":
            raise SystemExit("error: this is a GameCube disc image; GameCube discs are not supported "
                             "(and Call of Duty 3 was never released on GameCube)")
        probe = xdvdfs_probe(self.f)
        if probe:
            self.kind = "xdvdfs"
            self.layout, self._entries = xdvdfs_entries(self.f, probe)
            return
        rd, _ = iso9660_probe(self.f)
        if rd:
            self.kind = "iso9660"
            self.layout = "cooked 2048" if rd.raw == SECTOR else f"raw {rd.raw}"
            self._entries = iso9660_entries(self.f)
            return
        raise SystemExit("error: no ISO9660 or XDVDFS volume found in this image")

    def entries(self):
        return self._entries

    def copy(self, entries, out_root, log):
        for e in entries:
            dst = safe_dest(out_root, e.path)
            if os.path.exists(dst) and os.path.getsize(dst) == e.size:
                log(f"have   {e.path}")
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            tmp = dst + ".part"
            with open(tmp, "wb") as o:
                for ext in e.extents:
                    if ext[0] == "raw":
                        _, rd, (off, length) = ext
                        lba = (off - rd.off) // rd.raw
                        while length:
                            n = min(length, CHUNK)
                            buf = rd.read(lba, n)
                            if len(buf) != n:
                                raise SystemExit(f"error: image truncated while reading {e.path}")
                            o.write(buf)
                            length -= n
                            lba += n // SECTOR
                        continue
                    off, length = ext
                    self.f.seek(off)
                    while length:
                        buf = self.f.read(min(CHUNK, length))
                        if not buf:
                            raise SystemExit(f"error: image truncated while reading {e.path}")
                        o.write(buf)
                        length -= len(buf)
            os.replace(tmp, dst)
            log(f"copied {e.path} ({human(e.size)})")

    def close(self):
        self.f.close()


class FolderSource(Source):
    kind = "folder"

    def __init__(self, path):
        self.root = os.path.abspath(path)

    def entries(self):
        out = []
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames.sort(key=str.lower)
            for fn in sorted(filenames, key=str.lower):
                full = os.path.join(dirpath, fn)
                if os.path.islink(full) or not os.path.isfile(full):
                    continue
                rel = os.path.relpath(full, self.root).replace(os.sep, "/")
                out.append(Entry(rel, os.path.getsize(full), src=full))
        return out

    def copy(self, entries, out_root, log):
        for e in entries:
            dst = safe_dest(out_root, e.path)
            if os.path.exists(dst) and os.path.getsize(dst) == e.size:
                log(f"have   {e.path}")
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(e.src, dst + ".part")
            os.replace(dst + ".part", dst)
            log(f"copied {e.path} ({human(e.size)})")


def find_7z_cli():
    for name in ("7z", "7zz", "7za", "7zr"):
        p = shutil.which(name)
        if p:
            return p
    return None


class SevenZipSource(Source):
    """A .7z of an extracted disc folder (e.g. a decrypted PS3 JB folder)."""
    kind = "7z"

    def __init__(self, path, prefer_cli=True):
        self.path = os.path.abspath(path)
        self.cli = find_7z_cli() if prefer_cli else None
        self.py7zr = None
        if not self.cli:
            try:
                import py7zr  # noqa: F401
                self.py7zr = py7zr
            except ImportError:
                raise SystemExit("error: reading .7z needs the 7z command (Debian/Ubuntu/Mint: "
                                 "sudo apt install p7zip-full) or the py7zr package (pip install py7zr)")
        self.layout = "7z command " + os.path.basename(self.cli) if self.cli else "py7zr"
        self._entries = self._list()
        isos = [e for e in self._entries if e.path.lower().endswith((".iso", ".bin", ".img"))]
        if isos and len(isos) == len(self._entries):
            raise SystemExit(f"error: this archive only holds a disc image ({isos[0].path}). For a PS3 "
                             "disc that image is still encrypted; use the *_JB_Folder.7z (decrypted "
                             "folder) instead. For other systems, unpack the image and point at the .iso.")

    def _list(self):
        out = []
        if self.cli:
            r = subprocess.run([self.cli, "l", "-slt", "-ba", "-scsUTF-8", self.path],
                               capture_output=True)
            text = r.stdout.decode("utf-8", "replace")
            if r.returncode != 0 or "Path = " not in text:
                # older p7zip has no -ba; retry without it
                r = subprocess.run([self.cli, "l", "-slt", self.path], capture_output=True)
                text = r.stdout.decode("utf-8", "replace")
                if r.returncode != 0:
                    raise SystemExit("error: 7z could not list the archive:\n" + r.stderr.decode("utf-8", "replace"))
                # skip the archive's own header block (it ends at the '----------' line)
                cut = text.find("\n----------\n")
                if cut >= 0:
                    text = text[cut + 12:]
            for block in re.split(r"\n\s*\n", text):
                kv = {}
                for line in block.splitlines():
                    if " = " in line:
                        k, v = line.split(" = ", 1)
                        kv[k.strip()] = v
                if "Path" not in kv:
                    continue
                if kv.get("Folder") == "+" or "D" in kv.get("Attributes", "")[:1]:
                    continue
                try:
                    rel = clean_rel(kv["Path"])
                except ValueError:
                    continue
                out.append(Entry(rel, int(kv.get("Size") or 0), src=kv["Path"]))
        else:
            with self.py7zr.SevenZipFile(self.path, "r") as z:
                for info in z.list():
                    if info.is_directory:
                        continue
                    try:
                        rel = clean_rel(info.filename)
                    except ValueError:
                        continue
                    out.append(Entry(rel, int(info.uncompressed or 0), src=info.filename))
        return out

    def entries(self):
        return self._entries

    def copy(self, entries, out_root, log):
        todo = []
        for e in entries:
            dst = safe_dest(out_root, e.path)
            if os.path.exists(dst) and os.path.getsize(dst) == e.size:
                log(f"have   {e.path}")
            else:
                todo.append(e)
        if not todo:
            return
        log(f"unpacking {len(todo)} file(s) from the archive (one pass; a big solid archive takes a while)")
        os.makedirs(out_root, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".disc_get_", dir=out_root) as tmp:
            if self.cli:
                lst = os.path.join(tmp, "list.txt")
                with open(lst, "w", encoding="utf-8") as fh:
                    fh.write("\n".join(e.src for e in todo) + "\n")
                stage = os.path.join(tmp, "x")
                r = subprocess.run([self.cli, "x", "-y", "-spd", "-scsUTF-8", f"-o{stage}",
                                    self.path, f"@{lst}"], capture_output=True)
                if r.returncode not in (0, 1):  # 1 = warnings
                    raise SystemExit("error: 7z failed:\n" + r.stderr.decode("utf-8", "replace"))
            else:
                stage = os.path.join(tmp, "x")
                with self.py7zr.SevenZipFile(self.path, "r") as z:
                    z.extract(path=stage, targets=[e.src for e in todo])
            for e in todo:
                got = safe_dest(stage, e.src)
                if not os.path.isfile(got):
                    log(f"MISSING {e.path} (not unpacked)")
                    continue
                dst = safe_dest(out_root, e.path)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.move(got, dst)
                log(f"copied {e.path} ({human(e.size)})")


def open_source(path):
    if os.path.isdir(path):
        return FolderSource(path)
    if not os.path.isfile(path):
        raise SystemExit(f"error: {path} not found")
    with open(path, "rb") as f:
        magic = f.read(6)
    if magic == b"7z\xBC\xAF\x27\x1C":
        return SevenZipSource(path)
    return ImageSource(path)


# --------------------------------------------------------------------------
# what system is this disc for


def detect_system(src, entries):
    by_lower = {e.path.lower(): e for e in entries}

    def find(suffix):
        for k, e in by_lower.items():
            if k == suffix or k.endswith("/" + suffix):
                return e
        return None

    info = {"system": "unknown", "program": None, "title": None}
    sfo = find("ps3_game/param.sfo")
    if sfo or find("ps3_disc.sfb"):
        info["system"] = "PlayStation 3"
        ebo = find("ps3_game/usrdir/eboot.bin")
        info["program"] = ebo.path if ebo else None
        if sfo and sfo.size < 65536 and not isinstance(src, SevenZipSource):
            info["title"] = read_param_sfo(read_small(src, sfo))
        return info
    cnf = find("system.cnf")
    if cnf and cnf.size < 4096:
        text = read_small(src, cnf).decode("latin-1", "replace")
        m = re.search(r"BOOT2\s*=\s*cdrom0:\\?([^;\s]+)", text, re.I)
        if m:
            info["system"] = "PlayStation 2"
            want = m.group(1).replace("\\", "/").lower()
            e = by_lower.get(want) or find(want.split("/")[-1])
            info["program"] = e.path if e else m.group(1)
            info["title"] = m.group(1)
            return info
        if re.search(r"BOOT\s*=", text, re.I):
            info["system"] = "PlayStation"
            return info
    if find("default.xex"):
        info["system"], info["program"] = "Xbox 360", find("default.xex").path
        return info
    if find("default.xbe"):
        info["system"], info["program"] = "Xbox", find("default.xbe").path
        xbe = read_small(src, find("default.xbe"), 0x1000)
        info["title"] = xbe_title(xbe)
        return info
    return info


def read_small(src, e, limit=None):
    n = e.size if limit is None else min(e.size, limit)
    if isinstance(src, ImageSource):
        out = bytearray()
        for ext in e.extents:
            if len(out) >= n:
                break
            if ext[0] == "raw":
                _, rd, (off, length) = ext
                out += rd.read((off - rd.off) // rd.raw, min(length, n - len(out)))
            else:
                off, length = ext
                src.f.seek(off)
                out += src.f.read(min(length, n - len(out)))
        return bytes(out[:n])
    if isinstance(src, FolderSource):
        with open(e.src, "rb") as fh:
            return fh.read(n)
    with tempfile.TemporaryDirectory(prefix="disc_get_") as tmp:
        src.copy([e], tmp, lambda m: None)
        with open(safe_dest(tmp, e.path), "rb") as fh:
            return fh.read(n)


def read_param_sfo(data):
    try:
        if data[:4] != b"\x00PSF":
            return None
        key_tab, data_tab, count = struct.unpack_from("<III", data, 8)
        vals = {}
        for i in range(count):
            ko, fmt, ln, mx, do = struct.unpack_from("<HHIII", data, 20 + i * 16)
            key = data[key_tab + ko:data.index(b"\x00", key_tab + ko)].decode()
            raw = data[data_tab + do:data_tab + do + ln]
            vals[key] = raw.rstrip(b"\x00").decode("utf-8", "replace") if fmt != 0x0404 else struct.unpack("<I", raw)[0]
        return f"{vals.get('TITLE', '?')} [{vals.get('TITLE_ID', '?')}]"
    except Exception:
        return None


def xbe_title(data):
    try:
        if data[:4] != b"XBEH":
            return None
        base, = struct.unpack_from("<I", data, 0x104)
        cert, = struct.unpack_from("<I", data, 0x118)
        c = cert - base
        tid, = struct.unpack_from("<I", data, c + 8)
        name = data[c + 0xC:c + 0xC + 80].decode("utf-16-le", "replace").split("\x00", 1)[0]
        return f"{name} [{tid:08X}]"
    except Exception:
        return None


# --------------------------------------------------------------------------
# the glyph preset

GLYPH_NAME = re.compile(
    r"font|button|btn|icon|glyph|symbol|controller|gamepad|joypad|"
    r"frontend|front_end|(^|[/_.])(fe|ui|hud|menus?|pad)([/_.]|$)|"
    r"global|common|shared|(^|[/_.])loc(al)?([/_.]|$)|lang|english|"
    r"(^|[/_.])(text|strings?)([/_.]|$)|"
    r"\.(fnt|font|ttf|otf|bmf)$",
    re.I)
# never worth copying for buttons: movies, audio, level geometry
GLYPH_SKIP = re.compile(r"\.(pss|bik|wmv|vp6|mpg|m2v|pam|xmv|sfd|vag|vgs|ads|wav|xwb|xsb|at3|"
                        r"msf|mib|mih|snd|sob|bnk|pak\.d|ipu)$|(^|/)(movies?|video|audio|sound|music)/", re.I)


def glyph_candidates(entries, program, max_bytes):
    picks, skipped = [], []
    for e in entries:
        why = None
        if program and e.path.lower() == program.lower():
            why = "program"
        elif e.path.lower().endswith(("system.cnf", "param.sfo", "default.xbe", "default.xex")):
            why = "disc info"
        elif GLYPH_SKIP.search(e.path):
            continue
        elif GLYPH_NAME.search(e.path):
            why = "name"
        if not why:
            continue
        if e.size > max_bytes and why not in ("program",):
            skipped.append((e, f"bigger than {human(max_bytes)}"))
            continue
        picks.append((e, why))
    return picks, skipped


# --------------------------------------------------------------------------
# command line


def write_list(entries, path):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("size\tpath\n")
        for e in entries:
            fh.write(f"{e.size}\t{e.path}\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="list every file on the disc")
    p.add_argument("source")
    p.add_argument("--out", help="also write the list as a TSV file")
    p = sub.add_parser("get", help="copy files matching patterns")
    p.add_argument("source")
    p.add_argument("patterns", nargs="+")
    p.add_argument("--to", required=True)
    p.add_argument("--max-mb", type=float, default=0, help="skip files bigger than this (0 = no limit)")
    p = sub.add_parser("glyphs", help="copy what is needed to find the button pictures")
    p.add_argument("source")
    p.add_argument("--to", required=True)
    p.add_argument("--max-mb", type=float, default=380, help="skip single files bigger than this (default 380)")
    p.add_argument("--total-mb", type=float, default=2000, help="stop before copying more than this (default 2000)")
    a = ap.parse_args(argv)

    src = open_source(a.source)
    try:
        entries = src.entries()
        info = detect_system(src, entries)
        total = sum(e.size for e in entries)
        print(f"[disc] {a.source}")
        print(f"[disc] read as {src.kind}{' (' + src.layout + ')' if src.layout else ''}: "
              f"{len(entries)} files, {human(total)}")
        print(f"[disc] system: {info['system']}"
              + (f", program {info['program']}" if info["program"] else "")
              + (f", title {info['title']}" if info["title"] else ""))

        if a.cmd == "list":
            for e in entries:
                print(f"{e.size:>12} {e.path}")
            if a.out:
                write_list(entries, a.out)
                print(f"[disc] list written to {a.out}")
            return 0

        out_root = os.path.abspath(a.to)
        os.makedirs(out_root, exist_ok=True)
        log = lambda m: print(f"[disc] {m}", flush=True)  # noqa: E731

        if a.cmd == "get":
            pats = [x.lower() for x in a.patterns]
            picks = [e for e in entries
                     if any(fnmatch.fnmatchcase(e.path.lower(), x) or fnmatch.fnmatchcase(e.path.lower().split("/")[-1], x)
                            for x in pats)]
            if a.max_mb:
                big = [e for e in picks if e.size > a.max_mb * 1048576]
                for e in big:
                    log(f"skipped {e.path} ({human(e.size)}, over --max-mb)")
                picks = [e for e in picks if e not in big]
            log(f"{len(picks)} file(s) match, {human(sum(e.size for e in picks))}")
            src.copy(picks, out_root, log)
            return 0

        # glyphs preset
        write_list(entries, os.path.join(out_root, "files.tsv"))
        picks, skipped = glyph_candidates(entries, info["program"], int(a.max_mb * 1048576))
        budget = int(a.total_mb * 1048576)
        chosen, used = [], 0
        # small files first so a tight budget still gets the most files
        for e, why in sorted(picks, key=lambda p: (p[1] != "program", p[0].size)):
            if used + e.size > budget and why != "program":
                skipped.append((e, "over the --total-mb budget"))
                continue
            chosen.append((e, why))
            used += e.size
        with open(os.path.join(out_root, "disc.txt"), "w", encoding="utf-8") as fh:
            fh.write(f"source\t{os.path.abspath(a.source)}\n")
            fh.write(f"read_as\t{src.kind} {src.layout}\n")
            for k in ("system", "program", "title"):
                fh.write(f"{k}\t{info[k] or ''}\n")
            fh.write(f"files\t{len(entries)}\n")
            fh.write(f"bytes\t{total}\n")
        with open(os.path.join(out_root, "copied.tsv"), "w", encoding="utf-8") as fh:
            fh.write("size\twhy\tpath\n")
            for e, why in chosen:
                fh.write(f"{e.size}\t{why}\t{e.path}\n")
            for e, why in skipped:
                fh.write(f"{e.size}\tSKIPPED: {why}\t{e.path}\n")
        log(f"copying {len(chosen)} file(s), {human(used)}; {len(skipped)} skipped (see copied.tsv)")
        src.copy([e for e, _ in chosen], os.path.join(out_root, "files"), log)
        log(f"done -> {out_root}")
        return 0
    finally:
        src.close()


if __name__ == "__main__":
    sys.exit(main())
