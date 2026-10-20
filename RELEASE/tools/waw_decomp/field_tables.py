#!/usr/bin/env python3
"""
field_tables.py - the script field tables of a World at War build, and names for their handlers.

Scripts read and write engine fields directly (`self.origin`, `ai.goalradius`, `hudelem.alpha`). The
Call of Duty 4 engine keeps one table per kind of object for that, an array of entries
`{ "name", offset, type, ..., setter, getter }` ending with a null name; World at War has the same
tables with more fields and some new ones (players, vehicles, client entities). This tool finds them
in a build's data by their field names, decodes every entry, pairs each table with the KisakCOD
(reconstructed CoD4 source) array that shares the most field names, and names a setter or getter
when KisakCOD's entry for the same field has a named function in the same slot.

Usage:
    python3 tools/waw_decomp/field_tables.py --exe CoDWaW.exe --kisak-src <KisakCOD>/src \\
        --out fields.tsv [--names names.tsv] [--prefer game]
    python3 tools/waw_decomp/field_tables.py --image image.pe --load 0x82000000 --kisak-src ... \\
        --out fields.tsv

--exe: a PC executable (little-endian). --image/--load: an Xbox 360 image made by xex2pe.py, loaded
at --load (big-endian). --kisak-src: KisakCOD's src folder (its game/ and game_mp/ field arrays are
read from the source text). --prefer: the KisakCOD folder to prefer when the same table exists in
both (`game` for singleplayer builds, `game_mp` for multiplayer). --names: an existing names table
of the build; addresses it already names are left out of the output. --tables: also write every
decoded entry (table address, entry, field, offset, type, slots) to this file.

Output: a table for name_pc.py / name_x360.py --extra (addr, kind, name, pass "fields", evidence).
An address that would get two different names, or a name two addresses would get, is left out.

Reads files only; needs pefile for --exe.
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from naming_common import read_tsv  # noqa: E402

NAME_RE = re.compile(r"^[a-z_][a-z0-9_]{0,40}$")
STRIDES = (12, 16, 20, 24, 28, 32)


class PcImage:
    """A PC executable as mapped in memory (little-endian)."""

    def __init__(self, path):
        import pefile
        pe = pefile.PE(path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.img = pe.get_memory_mapped_image()
        self.secs = [(self.base + s.VirtualAddress, self.base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData),
                      s.Characteristics) for s in pe.sections]
        self.fmt = "<I"


class X360Image:
    """An Xbox 360 image from xex2pe.py: the PE layout at its load address, big-endian."""

    def __init__(self, path, load):
        with open(path, "rb") as f:
            self.img = f.read()
        self.base = load
        pe, = struct.unpack_from("<I", self.img, 0x3C)
        nsec, = struct.unpack_from("<H", self.img, pe + 6)
        optsz, = struct.unpack_from("<H", self.img, pe + 20)
        self.secs = []
        s = pe + 24 + optsz
        for _ in range(nsec):
            vs, va = struct.unpack_from("<II", self.img, s + 8)
            ch, = struct.unpack_from("<I", self.img, s + 36)
            self.secs.append((load + va, load + va + vs, ch))
            s += 40
        self.fmt = ">I"


def u32(im, a):
    o = a - im.base
    return struct.unpack_from(im.fmt, im.img, o)[0] if 0 <= o <= len(im.img) - 4 else None


def is_code(im, a):
    return any(lo <= a < hi and ch & 0x20000000 for lo, hi, ch in im.secs)


def is_data(im, a):
    return any(lo <= a < hi and not ch & 0x20000000 for lo, hi, ch in im.secs)


def cstr(im, a, maxlen=48):
    if a is None or not is_data(im, a):
        return None
    o = a - im.base
    b = im.img[o:o + maxlen]
    z = b.find(b"\0")
    return b[:z].decode("latin-1") if z > 0 else None


def split_top(s):
    """Split an initializer's items on the commas outside parentheses."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


ARRAY_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\[\s*\d*\s*\]\s*=\s*\{")


def groups(body):
    """The top-level { ... } groups of an array initializer's body (entries may span lines)."""
    out, depth, start = [], 0, None
    for i, ch in enumerate(body):
        if ch == "{":
            if depth == 0:
                start = i + 1
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                out.append(body[start:i])
    return out


def kisak_arrays(src):
    """{(folder, file, array): [(field, [function or None per slot])]} from KisakCOD's source text.
    An entry is { "name", offset, F_TYPE, ... }; a slot is an item after the type that is a function
    (&Name or Name) or NULL (numbers, such as the hud element masks, are not slots)."""
    arrays = collections.OrderedDict()
    for folder in ("game", "game_mp"):
        root = os.path.join(src, folder)
        if not os.path.isdir(root):
            continue
        for fn in sorted(os.listdir(root)):
            if not fn.endswith(".cpp"):
                continue
            with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                text = re.sub(r"//[^\n]*", "", f.read())
            for m in ARRAY_RE.finditer(text):
                depth, i = 1, m.end()
                while depth and i < len(text):
                    depth += {"{": 1, "}": -1}.get(text[i], 0)
                    i += 1
                entries = []
                for g in groups(text[m.end():i - 1]):
                    items = split_top(g)
                    if len(items) < 3 or not re.fullmatch(r'"[A-Za-z0-9_]+"', items[0]) or not items[2].startswith("F_"):
                        continue
                    slots = []
                    for it in items[3:]:
                        it = it.strip().lstrip("&")
                        if it == "NULL":
                            slots.append(None)
                        elif re.fullmatch(r"[A-Za-z_]\w*", it):
                            slots.append(it)
                    entries.append((items[0].strip('"'), slots))
                if len(entries) >= 3:
                    arrays[(folder, fn, m.group(1))] = entries
    return arrays


def find_tables(im, field_names):
    """Tables in the data: runs of entries of one stride whose first word points at a field-name
    string and whose second word is a small offset, ending at a null name. At least three of the
    names must be KisakCOD field names."""
    cands = set()
    for lo, hi, ch in im.secs:
        if ch & 0x20000000:
            continue
        o, end = lo - im.base, min(hi - im.base, len(im.img)) - 4
        while o < end:
            v = struct.unpack_from(im.fmt, im.img, o)[0]
            if is_data(im, v):
                s = cstr(im, v)
                if s in field_names:
                    cands.add(im.base + o)
            o += 4
    tables, used = [], set()
    for a in sorted(cands):
        if a in used:
            continue
        best = None
        for st in STRIDES:
            start = a
            while True:  # back to the first entry
                p = start - st
                s = cstr(im, u32(im, p))
                ofs = u32(im, p + 4)
                if s and NAME_RE.match(s) and ofs is not None and ofs < 0x40000:
                    start = p
                else:
                    break
            n, known, p = 0, 0, start
            while True:
                nm = u32(im, p)
                s = cstr(im, nm)
                ofs = u32(im, p + 4)
                if not (s and NAME_RE.match(s) and ofs is not None and ofs < 0x40000):
                    break
                n += 1
                known += s in field_names
                p += st
            if nm != 0:  # a table ends with a null name
                continue
            if known >= 3 and (best is None or (known, n) > (best[3], best[2])):
                best = (start, st, n, known)
        if best:
            start, st, n, known = best
            tables.append((start, st, n))
            used.update(start + k * st for k in range(n))
    # keep the best stride where two tables overlap
    out = []
    for t in sorted(tables, key=lambda t: -t[2]):
        if not any(t[0] < o[0] + o[1] * o[2] and o[0] < t[0] + t[1] * t[2] for o in out):
            out.append(t)
    return sorted(out)


def decode(im, table):
    start, st, n = table
    rows = []
    for k in range(n):
        p = start + k * st
        words = [u32(im, p + 4 * i) for i in range(st // 4)]
        rows.append((cstr(im, words[0]), words[1:]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe")
    ap.add_argument("--image")
    ap.add_argument("--load", type=lambda s: int(s, 0))
    ap.add_argument("--kisak-src", required=True)
    ap.add_argument("--prefer", default="game", choices=("game", "game_mp"))
    ap.add_argument("--names")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tables")
    a = ap.parse_args()
    if bool(a.exe) == bool(a.image):
        ap.error("give --exe or --image (with --load)")
    im = PcImage(a.exe) if a.exe else X360Image(a.image, a.load)
    arrays = kisak_arrays(a.kisak_src)
    field_names = {f for v in arrays.values() for f, _ in v}
    named = {}
    if a.names:
        for r in read_tsv(a.names):
            if r.get("name"):
                named[r["addr"].upper()] = r["name"]
    tables = find_tables(im, field_names)
    want = collections.defaultdict(set)   # address -> names wanted
    why = {}
    tab_rows = []
    for t in tables:
        rows = decode(im, t)
        names = {f for f, _ in rows}
        scored = sorted(arrays.items(), key=lambda kv: (len(names & {f for f, _ in kv[1]}), kv[0][0] == a.prefer), reverse=True)
        (folder, fn, arr), kentries = scored[0]
        overlap = len(names & {f for f, _ in kentries})
        kmap = {f: slots for f, slots in kentries}
        print(f"table 0x{t[0]:08X}: {t[2]} entries of {t[1]} bytes; KisakCOD {folder}/{fn} {arr} ({overlap} names in common)")
        for field, words in rows:
            tab_rows.append((f"{t[0]:08X}", field, words))
            slots = kmap.get(field)
            if not slots:
                continue
            tail = words[-len(slots):]
            if len(tail) != len(slots) or not all(w == 0 or is_code(im, w) for w in tail):
                continue
            for w, kname in zip(tail, slots):
                if w and kname:
                    addr = f"{w:08X}"
                    want[addr].add(kname)
                    why.setdefault((addr, kname), f"the function in the slot where KisakCOD's {arr} ({folder}/{fn}) has {kname}, for the field '{field}' (table 0x{t[0]:08X})")
    by_name = collections.defaultdict(set)
    for addr, ns in want.items():
        if len(ns) == 1:
            by_name[next(iter(ns))].add(addr)
    out = []
    for name, addrs in sorted(by_name.items()):
        if len(addrs) != 1:
            continue  # two different functions want the same name
        addr = next(iter(addrs))
        if addr in named:
            continue
        out.append((addr, name, why[(addr, name)]))
    with open(a.out, "w", encoding="utf-8", newline="\n") as o:
        o.write("addr\tkind\tname\tpass\tevidence\n")
        for addr, name, ev in sorted(out):
            o.write(f"{addr}\tfunction\t{name}\tfields\t{ev}\n")
    if a.tables:
        with open(a.tables, "w", encoding="utf-8", newline="\n") as o:
            o.write("table\tfield\twords\n")
            for t, field, words in tab_rows:
                o.write(f"{t}\t{field}\t{' '.join(f'{w:08X}' for w in words)}\n")
    conflicts = sum(1 for ns in want.values() if len(ns) > 1)
    print(f"{len(tables)} tables, {len(want)} handlers, {len(out)} new names "
          f"({conflicts} addresses with two names left out)")


if __name__ == "__main__":
    main()
