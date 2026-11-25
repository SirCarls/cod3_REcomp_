"""[waw-decomp] 2026-11-16 discmap: where each USA address is in another build of the same program.

Three ways, all made of the project's existing tools (run or imported as they are):

- the same image (the German and Fr/Es/It campaigns are the USA one): every address is the same;
- a regional rebuild (the same .pdata functions in the same order, some moved or changed: the German
  and Fr/Es/It multiplayer programs): tools/x360_family/build_diff.py carries every address
  (functions side by side, changed functions lined up instruction by instruction, .data / .rdata by
  their pointers); the rows the project's stored map has (dev/x360-family/cod3_regions/<build>/) are
  compared with it;
- a later build (functions added, removed, changed: the Japanese programs and level DLLs):
  tools/x360_family/rebuild_diff.py pairs the functions (the pair table), the pair table carries the
  addresses in unchanged functions (rc-discs' rule), tools/waw_decomp/carry_addresses.py finds the
  globals (aligned uses vote) and code with no .pdata entry, and tools/waw_decomp/match_builds.py,
  which pairs functions from strings and the call graph instead of code, is the second witness.

Every address gets one status:
  same        the same address in this build
  moved       found automatically, at another address
  changed     inside a function whose code changed (or whose start is all that carries): a person
              decides (the suggestion, when there is one, is in build_addr)
  gone        its function is not in this build
  unresolved  nothing found, or the witnesses disagree: a person decides
"""
from __future__ import annotations

import bisect
import collections
import contextlib
import io
import sys
from dataclasses import dataclass
from pathlib import Path

from common import IMAGE_BASE, hex8, parse_hex, read_tsv, run_tool, write_tsv

NEEDS_PERSON = ("changed", "gone", "unresolved")


@dataclass
class Result:
    status: str
    addr: int | None      # the address in the other build (for "changed": the suggestion, if any)
    method: str           # identical | build_diff | pair | carry_addresses | levels | image-base
    how: str
    witness: str = ""


def found(a: int, b: int, method: str, how: str) -> Result:
    return Result("same" if a == b else "moved", b, method, how)


def pdata_functions(image) -> dict[int, int]:
    """start -> size of every .pdata function of a build_diff.Image."""
    return {a: size for a, size in image.functions()}


def export_functions(folder: Path | None) -> dict[int, int]:
    """start -> size from a Ghidra export's functions.tsv (WawExport.java), or nothing."""
    if folder is None or not (folder / "functions.tsv").exists():
        return {}
    out = {}
    for r in read_tsv(folder / "functions.tsv"):
        try:
            out[int(r["entry"], 16)] = int(r["size"] or 0)
        except ValueError:
            pass
    return out


def pdata_export(image, folder: Path) -> Path:
    """A stand-in for a Ghidra export when a build has none: functions.tsv from its .pdata (what
    carry_addresses.py reads from --a-raw / --b-raw: entry and size)."""
    folder.mkdir(parents=True, exist_ok=True)
    write_tsv(folder / "functions.tsv", ["entry", "size", "name", "source"],
              [[f"{a:08X}", size, f"sub_{a:08X}", "DEFAULT"] for a, size in sorted(pdata_functions(image).items())])
    return folder


# ---------------------------------------------------------------- the same image
def match_identical(entries) -> dict:
    return {(e.program, e.usa): Result("same", e.usa, "identical", "the same image (same decrypted SHA-1)")
            for e in entries}


# ---------------------------------------------------------------- regional rebuilds: build_diff.py
def status_from_build_diff(a: int, b: int | None, how: str) -> Result:
    """build_diff.py's carry() says how it found each address; that decides the status."""
    if how.startswith("changed function"):
        return Result("changed", b, "build_diff", how)
    if how.startswith("inside changed function"):
        return Result("changed", None, "build_diff", how)
    if b is not None and (how.startswith(("function ", "code after function", ".rdata run", "between .rdata runs"))
                          or how == "same .data layout"):
        return found(a, b, "build_diff", how)
    if a == IMAGE_BASE:
        return Result("same", a, "image-base", "the image base")
    return Result("unresolved", b, "build_diff", how)


def same_section_bytes(ref, other, a: int) -> str | None:
    """For an address build_diff.py leaves unmapped (a section it doesn't read, e.g. .reloc): the
    same address when its section has the same place and size in both builds and 16 bytes there
    are equal. Returns the reason, or None."""
    name = ref.section_of(a)
    if not name or name not in other.secs or ref.secs[name][:2] != other.secs[name][:2]:
        return None
    wa, wb = ref.u32(a & ~3), other.u32(a & ~3)
    if wa is None or wb is None:
        return None
    if ref.words(a & ~3, 16) != other.words(a & ~3, 16):
        return None
    return f"section {name} at the same place and size in both builds; 16 bytes there equal"


def boundary_rule(ref, other, a: int) -> Result | None:
    """Numbers the code uses that are not the address of anything: the image's size (a campaign_map.tsv
    row), a number outside the image (kept as it is), and the end of a section (the first byte after
    .text, for range checks), which is that section's end in the other build."""
    if a == len(ref.img):
        return found(a, len(other.img), "boundary", "the image size")
    if not ref.base <= a < ref.base + len(ref.img):
        return Result("same", a, "boundary", "not an address in the image: kept as it is")
    for name, (lo, size, _) in ref.secs.items():
        if a == lo + size and ref.section_of(a) is None and name in other.secs:
            olo, osize, _ = other.secs[name]
            return found(a, olo + osize, "boundary", f"the end of section {name} (the first byte after it)")
    return None


def match_regional(project, folder: Path, ref_pe: Path, other_pe: Path, ref_img, other_img, entries, log: Path):
    want = folder / "want_build_diff.txt"
    want.write_text("".join(f"0x{e.usa:08X}\n" for e in entries), encoding="utf-8")
    out_dir = folder / "build_diff"
    rc = run_tool(project.tool("x360_family", "build_diff.py"),
                  ["--a", ref_pe, "--b", other_pe, "--out-dir", out_dir, "--addresses", want], log)
    if rc != 0:
        sys.exit(f"error: build_diff.py failed (exit {rc}); see {log}")
    carried = {}
    for r in read_tsv(out_dir / "addresses.tsv"):
        carried[int(r["a"], 16)] = (parse_hex(r["b"]), r["how"])
    results = {}
    for e in entries:
        b, how = carried.get(e.usa, (None, "not in build_diff.py's output"))
        res = status_from_build_diff(e.usa, b, how)
        if res.status == "unresolved" and b is None:
            res = boundary_rule(ref_img, other_img, e.usa) or res
        if res.status == "unresolved" and b is None:
            why = same_section_bytes(ref_img, other_img, e.usa)
            if why:
                res = Result("same", e.usa, "build_diff", how + "; " + why)
        results[(e.program, e.usa)] = res
    return results


def compare_stored_map(results: dict, program: str, stored: Path) -> str:
    """How the result agrees with the project's stored map for this build (build_diff.py's
    addresses.tsv of an earlier run), when there is one."""
    if not stored.exists():
        return f"no stored map at {stored}"
    same = differ = 0
    examples = []
    for r in read_tsv(stored):
        a, b = parse_hex(r["a"]), parse_hex(r["b"])
        res = results.get((program, a))
        if a is None or b is None or res is None or res.addr is None:
            continue
        if res.addr == b:
            same += 1
        else:
            differ += 1
            examples.append(f"{a:08X}: stored {b:08X}, now {res.addr:08X}")
    return f"the stored map ({stored.parent.name}) agrees on {same} of {same + differ} shared rows" + (
        "; differs: " + ", ".join(examples[:5]) if examples else "")


def compare_stored_pairs(fresh: Path, stored: Path) -> str:
    """How today's rebuild_diff.py table agrees with one the project keeps for this build."""
    if not stored.exists() or not fresh.exists():
        return f"no stored pair table at {stored.parent.name}/{stored.name}"
    a, b = read_pairs(fresh), read_pairs(stored)
    shared = set(a) & set(b)
    same = sum(1 for k in shared if a[k][:2] == b[k][:2])
    return (f"rebuild_diff.py today and the stored {stored.parent.name}/{stored.name} agree on {same} of "
            f"{len(shared)} reference functions (pair and status)")


# ---------------------------------------------------------------- later builds
def run_rebuild_diff(project, ref_pe: Path, other_pe: Path, out: Path, log: Path, base: int = IMAGE_BASE) -> None:
    """tools/x360_family/rebuild_diff.py on two images. For an image at the usual base it runs as a
    program; a level DLL (base 0x89000000) needs its images read at that base, which rebuild_diff.py
    has no option for, so its main() is called here with build_diff.Image given the DLL's base."""
    if out.exists():
        return
    if base == IMAGE_BASE:
        rc = run_tool(project.tool("x360_family", "rebuild_diff.py"),
                      ["--a", ref_pe, "--b", other_pe, "--out", out], log)
        if rc != 0:
            sys.exit(f"error: rebuild_diff.py failed (exit {rc}); see {log}")
        return
    import build_diff
    import rebuild_diff
    saved_image, saved_argv = rebuild_diff.Image, sys.argv
    rebuild_diff.Image = lambda path: build_diff.Image(path, base)
    sys.argv = ["rebuild_diff.py", "--a", str(ref_pe), "--b", str(other_pe), "--out", str(out)]
    text = io.StringIO()
    try:
        with contextlib.redirect_stdout(text):
            rebuild_diff.main()
    finally:
        rebuild_diff.Image, sys.argv = saved_image, saved_argv
        with open(log, "a", encoding="utf-8") as w:
            w.write(f"rebuild_diff.py (base {base:#x}) {ref_pe.name} {other_pe.name}: {text.getvalue()}")


def read_pairs(path: Path) -> dict[int, tuple]:
    """rebuild_diff.py's table: a -> (b or None, status, ratio, a_size, b_size)."""
    pair = {}
    for r in read_tsv(path):
        if r["a"]:
            pair[int(r["a"], 16)] = (parse_hex(r["b"]), r["status"], r["ratio"], int(r["a_size"] or 0),
                                     int(r["b_size"] or 0))
    return pair


def neighbors_rule(pair: dict, keys: list[int], a: int) -> Result | None:
    """Code with no .pdata entry of its own: carried with the functions on both sides when both are
    unchanged and moved by the same amount (rc-discs' and eu_mp_patch.py's rule)."""
    k = bisect.bisect_right(keys, a) - 1
    if 0 <= k < len(keys) - 1:
        pa, na = keys[k], keys[k + 1]
        pb, ps = pair[pa][0], pair[pa][1]
        nb, ns = pair[na][0], pair[na][1]
        if ps == ns == "same" and pb is not None and nb is not None and pb - pa == nb - na:
            return found(a, a + (pb - pa), "pair",
                         f"no .pdata entry; between {pa:08X} and {na:08X}, both {pb - pa:+#x}")
    return None


def match_later(project, folder: Path, ref_pe: Path, other_pe: Path, ref_raw: Path | None, other_raw: Path | None,
                ref_img, other_img, funcs, entries, log: Path, witness: bool, witness_timeout: float):
    pairs_tsv = folder / "rebuild_pairs.tsv"
    run_rebuild_diff(project, ref_pe, other_pe, pairs_tsv, log)
    pair = read_pairs(pairs_tsv)
    keys = sorted(pair)

    # carry_addresses.py: the pair table (its a_addr/b_addr/how/votes form), the addresses wanted
    carry_pairs = folder / "carry_pairs.tsv"
    write_tsv(carry_pairs, ["a_addr", "b_addr", "how", "votes"],
              [[f"{a:08X}", f"{v[0]:08X}", v[1], v[2]] for a, v in sorted(pair.items()) if v[0] is not None])
    want = folder / "carry_want.tsv"
    write_tsv(want, ["addr", "name"], [[f"{e.usa:08X}", (e.syms or ["-"])[0]] for e in entries
                                       if e.required and e.cls not in ("level", "image-base")])
    a_raw = ref_raw or pdata_export(ref_img, folder / "ref_pdata")
    b_raw = other_raw or pdata_export(other_img, folder / "other_pdata")
    carry_tsv = folder / "carry.tsv"
    if not carry_tsv.exists():
        rc = run_tool(project.tool("waw_decomp", "carry_addresses.py"),
                      ["--a-image", ref_pe, "--a-raw", a_raw, "--b-image", other_pe, "--b-raw", b_raw,
                       "--pairs", carry_pairs, "--want", want, "--out", carry_tsv,
                       "--globals-out", folder / "carry_globals.tsv"], log)
        if rc != 0:
            sys.exit(f"error: carry_addresses.py failed (exit {rc}); see {log}")
    carry = {int(r["a_addr"], 16): r for r in read_tsv(carry_tsv)}

    # the second witness: match_builds.py needs both builds' Ghidra exports (calls and strings)
    mb_pairs, witness_note = {}, "match_builds.py not run"
    can_witness = all(raw is not None and (raw / "calls.tsv").exists() and (raw / "strings.tsv").exists()
                      for raw in (ref_raw, other_raw))
    if witness and can_witness:
        mb_tsv = folder / "match_builds.tsv"
        if not mb_tsv.exists():
            run_tool(project.tool("waw_decomp", "match_builds.py"),
                     ["--a", ref_raw, "--a-image", ref_pe, "--b", other_raw, "--b-image", other_pe, "--out", mb_tsv],
                     log, timeout=witness_timeout)
        if mb_tsv.exists():
            mb_pairs = {int(r["a_addr"], 16): int(r["b_addr"], 16) for r in read_tsv(mb_tsv)}
            witness_note = f"match_builds.py paired {len(mb_pairs)} functions"
        else:
            witness_note = "match_builds.py gave no table (stopped or failed; see the log)"
    elif witness:
        witness_note = "match_builds.py not run: a build has no Ghidra export with calls.tsv and strings.tsv"

    results = {}
    for e in entries:
        a = e.usa
        cr = carry.get(a)
        auto_b = parse_hex(cr["b_addr"]) if cr else None
        auto_how = f"carry_addresses.py: {cr['how']} ({cr['evidence'][:80]})" if cr and auto_b is not None else ""
        res = None
        pf = None
        if e.cls == "image-base":
            res = Result("same", a, "image-base", "the image base")
        elif e.cls in ("func", "code"):
            pf = funcs.pdata_of(a) if e.cls == "code" or a not in pair else a
            if pf is None or pf not in pair:
                res = neighbors_rule(pair, keys, a)
                if res is None and auto_b is not None:
                    res = found(a, auto_b, "carry_addresses", auto_how)
                if res is None:
                    res = Result("unresolved", None, "pair", "no .pdata entry; the neighbors moved differently or "
                                 "changed, and carry_addresses.py found nothing")
            else:
                b, st, ratio, _, _ = pair[pf]
                if st == "gone":
                    res = Result("gone", None, "pair", f"function {pf:08X} is gone in this build")
                elif st == "same":
                    res = found(a, b + (a - pf), "pair", f"function {pf:08X} has the same code ({ratio})")
                elif a == pf:
                    res = Result("changed", b, "pair", f"function {pf:08X} changed ({ratio}): its start carried, "
                                 "its body differs")
                else:
                    sugg = auto_b if cr and cr["how"] == "aligned" else None
                    res = Result("changed", sugg, "pair", f"inside changed function {pf:08X} ({ratio})"
                                 + (f"; {auto_how}" if sugg is not None else ""))
            # the witnesses: carry_addresses.py on the same rows, match_builds.py on the function
            notes = []
            if res.method == "pair" and res.status in ("same", "moved") and auto_b is not None:
                notes.append("carry_addresses agrees" if auto_b == res.addr else f"carry_addresses says {auto_b:08X}")
            if pf is not None and pf in pair and pf in mb_pairs:
                mb, pb = mb_pairs[pf], pair[pf][0]
                if mb == pb:
                    notes.append("match_builds agrees")
                elif pb is None:
                    notes.append(f"match_builds pairs {pf:08X} with {mb:08X} (the pair table: gone)")
                else:
                    notes.append(f"match_builds pairs {pf:08X} with {mb:08X}, the pair table with {pb:08X}")
                    if res.status in ("same", "moved"):
                        res = Result("unresolved", res.addr, res.method, res.how + "; the witnesses disagree")
            res.witness = "; ".join(notes)
        elif e.cls == "data" and boundary_rule(ref_img, other_img, a):
            res = boundary_rule(ref_img, other_img, a)
        elif e.cls == "data":
            if auto_b is not None:
                res = found(a, auto_b, "carry_addresses", auto_how)
            else:
                ev = cr["evidence"][:80] if cr else "not asked"
                res = Result("unresolved", None, "carry_addresses", f"a global no aligned use carries ({ev})")
        else:
            res = Result("unresolved", None, "-", "a level DLL address in the program's own code: tied to no level")
        results[(e.program, a)] = res
    return results, witness_note


# ---------------------------------------------------------------- level DLLs
def match_level(project, folder: Path, level: str, ref_pe: Path | None, other_pe: Path | None,
                same_image: bool, base: int, entries, log: Path) -> tuple[dict, str]:
    """Each level DLL of a campaign build against the reference's (rc-discs' rc_dlls.py rules)."""
    if same_image:
        return match_identical(entries), "the same image"
    if other_pe is None:
        return ({(e.program, e.usa): Result("unresolved", None, "levels", f"{level}.dll is not on this build's disc")
                 for e in entries}, "not on the disc")
    pairs_tsv = folder / f"level_{level}_pairs.tsv"
    run_rebuild_diff(project, ref_pe, other_pe, pairs_tsv, log, base)
    pair = read_pairs(pairs_tsv)
    starts = sorted(pair)
    results = {}
    for e in entries:
        a = e.usa
        k = bisect.bisect_right(starts, a) - 1
        res = None
        if k >= 0 and a < starts[k] + pair[starts[k]][3]:
            f = starts[k]
            fb, fs, ratio = pair[f][0], pair[f][1], pair[f][2]
            if fs == "same":
                res = found(a, fb + (a - f), "levels", f"in {f:08X}, the same code")
            elif fs == "changed":
                res = Result("changed", fb if a == f else None, "levels",
                             f"{f:08X} changed ({ratio})" + (": its start" if a == f else ""))
            else:
                res = Result("gone", None, "levels", f"in {f:08X}, gone in this build")
        if res is None:
            res = neighbors_rule(pair, starts, a)
        if res is None:
            res = Result("unresolved", None, "levels", "between functions that moved differently or changed")
        results[(e.program, a)] = res
    kinds = collections.Counter(v[1] for v in pair.values())
    return results, (f"{len(pair)} reference functions: {kinds['same']} the same code, {kinds['changed']} changed, "
                     f"{kinds['gone']} gone")


# ---------------------------------------------------------------- the map file
MAP_HEADER = ["program", "group", "usa_addr", "class", "kinds", "symbols", "required", "status", "build_addr",
              "method", "how", "witness", "loose_literal", "uses"]


def map_rows(entries, results) -> list[list]:
    rows = []
    for e in entries:
        r = results[(e.program, e.usa)]
        uses = " ".join(e.files[:4]) + (" ..." if len(e.files) > 4 else "")
        rows.append([e.program, e.group, f"{e.usa:08X}", e.cls, ",".join(e.kinds), ",".join(e.syms),
                     "yes" if e.required else "", r.status, hex8(r.addr), r.method, r.how, r.witness,
                     " ".join(e.loose_files) if e.loose else "", uses])
    return rows
