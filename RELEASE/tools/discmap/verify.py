"""[waw-decomp] 2026-11-16 discmap: check a build's map against the two programs themselves.

acct2-decomp's wip/eu_mp/eu_mp_verify.py, for any build (it was written for the Fr/Es/It program, its
SHA-1s and its map): for every address the map carries,
  - code: the 8 instructions there in the reference build and in this build, with the addresses
    taken out (tools/x360_family/same_code.py's masked()), must be the same;
  - data: 16 bytes must be the same, a word that points into a function that kept its code and
    points to that function's new place counting as the same.
Rows a person decided (overrides) are checked too, and so are the suggestions of the rows still
waiting for a decision (shown, not counted as failures). Read only.
"""
from __future__ import annotations

import bisect
import collections

from common import parse_hex


class MovedFunctions:
    """The functions that kept their code (same / moved): a pointer into one of them in the reference
    build must point to the same place in it in the other build."""

    def __init__(self, pairs: dict[int, tuple[int, int]]):
        self.items = sorted((a, b, size) for a, (b, size) in pairs.items())
        self.starts = [x[0] for x in self.items]

    def same_pointer(self, wa: int, wb: int) -> bool:
        k = bisect.bisect_right(self.starts, wa) - 1
        if k < 0:
            return False
        a, b, size = self.items[k]
        return wa < a + max(size, 4) and wb == b + (wa - a)


def words(img, a: int, n: int):
    o = a - img.base
    if o < 0 or o + 4 * n > len(img.img):
        return None
    return img.words(a, 4 * n)


def is_code(img, a: int) -> bool:
    name = img.section_of(a)
    return bool(name) and bool(img.secs[name][2] & 0x20000000)


def same_target(ref_img, other_img, x: int, y: int) -> bool:
    """Two pointers, one in each build, point at the same thing: the same code (4 instructions,
    addresses masked) or the same 16 bytes of data. For a pointer into code or .rdata that moved
    (a vtable slot pointing at a thunk with no .pdata entry, a table of constants that moved)."""
    from same_code import masked  # tools/x360_family
    if not (ref_img.base <= x < ref_img.base + len(ref_img.img) and
            other_img.base <= y < other_img.base + len(other_img.img)):
        return False
    wa, wb = words(ref_img, x & ~3, 4), words(other_img, y & ~3, 4)
    if wa is None or wb is None or x & 3 != y & 3:
        return False
    if is_code(ref_img, x):
        return is_code(other_img, y) and masked(list(wa)) == masked(list(wb))
    return list(wa) == list(wb)


def check(ref_img, other_img, a: int, b: int, moved: MovedFunctions) -> tuple[bool, str]:
    """(same, what was compared) for one address."""
    from same_code import masked  # tools/x360_family
    if ref_img.section_of(a) is None:
        return True, "boundary (not inside a section: nothing to compare)"
    if is_code(ref_img, a):
        wa, wb = words(ref_img, a & ~3, 8), words(other_img, b & ~3, 8)
        if wa is None or wb is None:
            return False, "code: outside the image"
        return masked(list(wa)) == masked(list(wb)), "code"
    wa, wb = words(ref_img, a & ~3, 4), words(other_img, b & ~3, 4)
    if wa is None or wb is None:
        return False, "data: outside the image"
    if list(wa) == list(wb):
        return True, "data"
    if all(x == y or moved.same_pointer(x, y) for x, y in zip(wa, wb)):
        return True, "data (pointers to moved functions)"
    if all(x == y or moved.same_pointer(x, y) or same_target(ref_img, other_img, x, y) for x, y in zip(wa, wb)):
        return True, "data (pointers to the same code or data, moved)"
    return False, "data: " + " ".join(f"{x:08X}/{y:08X}" for x, y in zip(wa, wb))


def verify(images: dict, moved: dict, map_rows: list[dict], table: dict, pending: dict) -> tuple[list[str], dict]:
    """images: program -> (reference image, other image) (build_diff.Image), for the main program and
    each level whose image differs; moved: program -> MovedFunctions; table: overrides.final_table();
    pending: (program, usa) -> suggestion for rows still waiting for a person.
    Returns the report lines and the counts."""
    counts = collections.Counter()
    lines = []
    for r in map_rows:
        program = r["program"]
        if program not in images:
            continue
        ref_img, other_img = images[program]
        a = int(r["usa_addr"], 16)
        key = (program, a)
        if key in table:
            b, source = table[key]
            if b is None:
                continue        # absent in this build: nothing to compare
            kind = "override" if source.startswith("override") else "automatic"
        elif key in pending and pending[key] is not None:
            b, kind = pending[key], "suggestion"
        else:
            continue
        ok, what = check(ref_img, other_img, a, b, moved[program])
        if what.startswith("boundary"):
            counts[(kind, "boundary", True)] += 1
            continue
        family = "code" if what.startswith("code") else "data"
        counts[(kind, family, ok)] += 1
        if not ok or kind != "automatic":
            lines.append(f"{'same     ' if ok else 'DIFFERS  '} {kind:10s} {program:16s} {a:08X} -> {b:08X}  "
                         f"{what[:90]}  [{r['symbols'] or r['kinds']}; {r['status']}]")
    return lines, counts


def summary(counts) -> list[str]:
    out = []
    for kind in ("automatic", "override", "suggestion"):
        for family in ("code", "data"):
            same, differ = counts[(kind, family, True)], counts[(kind, family, False)]
            if same or differ:
                out.append(f"{kind} {family}: {same} the same, {differ} differ")
        if counts[(kind, "boundary", True)]:
            out.append(f"{kind}: {counts[(kind, 'boundary', True)]} boundaries (image size, section end, not an "
                       "address): nothing to compare")
    return out


def pairs_from_map_folder(rows: list[dict]) -> dict[int, tuple[int, int]]:
    """function pairs (a -> (b, size)) for the functions that kept their code, from build_diff.py's
    functions.tsv or rebuild_diff.py's table."""
    out = {}
    for r in rows:
        a, b = parse_hex(r.get("a", "")), parse_hex(r.get("b", ""))
        if a is None or b is None or r.get("status") not in ("same", "moved"):
            continue
        out[a] = (b, int(r.get("a_size") or 0))
    return out
