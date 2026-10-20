#!/usr/bin/env python3
"""
asset_fields.py - the text keys of World at War's asset files (weapons, vehicles, flame tables,
physics presets and constraints, destructibles, vision sets) and where each one lands in the
loaded structure.

The PC game can still read its asset files as text (`weapons/sp/thompson`: `fireTime\\0.1\\...`).
Each kind of file has a table of entries `{ "key", offset, type }` (12 bytes each, Call of Duty 4's
`cspField_t`), and one parser turns a key's text into the value at that offset of the structure:

    type  0  string (kept as a pointer)       type  6  float
    type  1  char[1024] in place              type  7  seconds in the file, int milliseconds stored
    type  2  char[64] in place                type  8  effect (FX handle)
    type  3  char[256] in place               type  9  model (XModel handle)
    type  4  int                              type 10  material (handle)
    type  5  bool (0 or 1, 4 bytes)           type 11  sound alias (handle)
    type 12 and up: the kind's own parser (for weapons: enumerations such as weaponType, the hide
    tags list, the notetrack-to-sound map; see the weapon enumerations output)

(read from the generic parser: 0x005F75D0 in the PC 1.7 campaign program, the 1000.0 factor of type
7 at its fmul). The same structures are what the game's fastfiles store, so the key names label the
fields of a ripped weapon. The Xbox 360 programs have no weapon parser (consoles only load
fastfiles), so the weapon table is in the PC programs only; the others are in both.

Usage:
    python3 tools/waw_decomp/asset_fields.py --exe CoDWaW.exe --out asset_fields.tsv \\
        [--decomp <export folder> --enums weapon_enums.tsv]
    python3 tools/waw_decomp/asset_fields.py --image image.pe --load 0x82000000 --out asset_fields.tsv

--decomp: the build's Ghidra export folder (decomp/*.c), for the weapon enumerations: the weapon
parser's own switch gives, per type, the list of names it accepts (read from the executable), the
offset it stores the index at and what its error message calls it.

Output: asset_fields.tsv: table address, kind (guessed from its keys), entry address, key, offset,
type, type meaning. weapon_enums.tsv: type, what, stored at, index, name.

Reads files only; needs pefile for --exe.
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from field_tables import PcImage, X360Image, cstr, u32  # noqa: E402

KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")
BASE_TYPES = {0: "string", 1: "char[1024]", 2: "char[64]", 3: "char[256]", 4: "int", 5: "bool", 6: "float",
              7: "milliseconds (seconds in the file)", 8: "effect", 9: "model", 10: "material", 11: "sound alias"}
WEAPON_SPECIAL = {0x14: "player anim type (a name from the player animation script)",
                  0x17: "bounce sound (alias prefix; one alias per surface type)",
                  0x22: "hide tags (up to 8 bone names)",
                  0x23: "notetrack-to-sound map (up to 20 notetrack, sound alias pairs)"}
MIN_ENTRIES = 6


def kind_of(keys):
    """What a table describes, from the keys it has."""
    k = set(keys)
    if {"weaponType", "fireTime"} <= k:
        return "weapon"
    if any(x.startswith("flameVar_") for x in k):
        return "flame table"
    if any(x.startswith("nitrous_") for x in k) or "steerWheels" in k:
        return "vehicle"
    if {"mass", "gravityScale"} <= k:
        return "physics preset"
    if "c1_type" in k:
        return "physics constraints"
    if "spawnModel2D4" in k or ("model" in k and any(x.startswith("spawnModel") for x in k)):
        return "destructible piece"
    if "r_glow" in k:
        return "vision set"
    if all(x.startswith("CS_") or x.isupper() for x in k):
        return "config string ranges"
    return "other"


def find_tables(im):
    """Every run of 6+ 12-byte entries {key*, offset < 0x20000, type < 0x40} in the data sections."""
    tables = []
    for lo, hi, ch in im.secs:
        if ch & 0x20000000:  # code
            continue
        a = lo
        while a + 12 <= hi:
            run, b = [], a
            while b + 12 <= hi:
                p, off, ty = u32(im, b), u32(im, b + 4), u32(im, b + 8)
                key = cstr(im, p, 64) if p is not None else None
                if key and KEY_RE.match(key) and off is not None and off < 0x20000 and ty is not None and ty < 0x40:
                    run.append((b, key, off, ty))
                    b += 12
                else:
                    break
            if len(run) >= MIN_ENTRIES:
                tables.append(run)
                a = b
            else:
                a += 4
    return tables


def read_c(folder):
    text = []
    for p in sorted(glob.glob(os.path.join(folder, "decomp", "*.c")) + glob.glob(os.path.join(folder, "*.c"))):
        with open(p, encoding="utf-8", errors="replace") as f:
            text.append(f.read())
    return "\n".join(text)


def weapon_parser_cases(c_text):
    """The weapon parser's switch: {type: (array address or None, count, store offset, what)}."""
    m = re.search(r"Unknown weapon type", c_text)
    if not m:
        return {}
    # the function holding that message: from its "// ADDRESS NAME (N bytes)" header to the next one
    start = c_text.rfind("\n// ", 0, m.start())
    end = c_text.find("\n// ", m.start())
    body = c_text[start if start >= 0 else 0:end if end > 0 else len(c_text)]
    cases = {}
    parts = re.split(r"case \(void \*\)(0x[0-9a-f]+|\d+):", body)
    for i in range(1, len(parts) - 1, 2):
        t = int(parts[i], 0)
        blk = parts[i + 1]
        arr = re.search(r"\(&\w*?([0-9a-f]{8})\)\[iVar\d+\]", blk)
        cnt = re.search(r"while \(iVar\d+ < (0x[0-9a-f]+|\d+)\)", blk)
        store = re.findall(r"\(param_1 \+ (0x[0-9a-f]+|\d+)\)", blk)
        what = re.search(r'Unknown ([^"\\]+?) \\"%s\\"', blk) or re.search(r'Unknown ([^"]+?) "%s"', blk)
        cases[t] = (int(arr.group(1), 16) if arr else None, int(cnt.group(1), 0) if cnt else None,
                    int(store[-1], 0) if store else None, what.group(1) if what else "")
    return cases


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--exe")
    src.add_argument("--image")
    ap.add_argument("--load", type=lambda s: int(s, 0), default=0x82000000)
    ap.add_argument("--decomp")
    ap.add_argument("--enums")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    im = PcImage(a.exe) if a.exe else X360Image(a.image, a.load)
    tables = find_tables(im)
    cases = weapon_parser_cases(read_c(a.decomp)) if a.decomp else {}

    rows = []
    for run in tables:
        kind = kind_of([k for _, k, _, _ in run])
        for addr, key, off, ty in run:
            if ty in BASE_TYPES:
                meaning = BASE_TYPES[ty]
            elif kind == "weapon" and ty in WEAPON_SPECIAL:
                meaning = WEAPON_SPECIAL[ty]
            elif kind == "weapon" and ty in cases and cases[ty][3]:
                meaning = f"enumeration: {cases[ty][3]}"
            else:
                meaning = f"{kind}'s own type {ty}"
            rows.append((run[0][0], kind, addr, key, off, ty, meaning))
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("table\tkind\tentry\tkey\toffset\ttype\tmeaning\n")
        for t, kind, addr, key, off, ty, meaning in rows:
            f.write(f"{t:08X}\t{kind}\t{addr:08X}\t{key}\t0x{off:X}\t{ty}\t{meaning}\n")
    for run in tables:
        kind = kind_of([k for _, k, _, _ in run])
        print(f"[asset_fields] {run[0][0]:08X} {kind}: {len(run)} keys ({run[0][1]} .. {run[-1][1]})")

    if a.enums:
        with open(a.enums, "w", encoding="utf-8", newline="\n") as f:
            f.write("type\twhat\tstored_at\tindex\tname\n")
            n = 0
            for ty in sorted(cases):
                arr, cnt, store, what = cases[ty]
                if arr is None or cnt is None:
                    continue
                for i in range(cnt):
                    name = cstr(im, u32(im, arr + 4 * i), 64)
                    f.write(f"{ty}\t{what}\t0x{store:X}\t{i}\t{name}\n" if store is not None else
                            f"{ty}\t{what}\t\t{i}\t{name}\n")
                    n += 1
        print(f"[asset_fields] weapon enumerations: {len(cases)} types in the parser, {n} names -> {a.enums}")
    print(f"[asset_fields] {len(rows)} keys in {len(tables)} tables -> {a.out}")


if __name__ == "__main__":
    main()
