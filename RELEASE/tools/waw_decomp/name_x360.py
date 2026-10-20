#!/usr/bin/env python3
"""
name_x360.py - name functions and globals in World at War's Xbox 360 executable.

Reads the Ghidra export (WawExport.java: functions.tsv, calls.tsv, strings.tsv)
and the plain image (xex2pe.py), and writes names.tsv (addr, name, kind, pass,
evidence). Passes, strongest first:

  runtime   the compiler's register save/restore routines (__savegprlr_14 ...), by their code
  extra     (--extra, repeatable) names found by hand, with their evidence (addr, name, kind,
            pass, evidence; a row of kind "note" is a comment only; a row of kind "inlined"
            keeps its name off every function)
  method    C++ methods named in their own strings (Demonware's "bdCore::init" log lines,
            compiler signatures in assertions) when exactly one function has the name; names
            two functions share come last
  pc        the PC build's name for the function that uses the same strings
            (--pc-names/--pc-raw: the PC names.tsv and its Ghidra export)
  selfname  a function's own name in its error/log strings
  cmd       console commands: Cmd_AddCommand("name", handler, &cmd) -> Cmd_<name>_f
  dvar      g = Dvar_RegisterX("name", ...) -> dvar_<name>
  kisak     KisakCOD (reconstructed CoD4 source) names by shared strings (names only)
  table     { "name", handler } arrays (script builtins, parsers, ...); a table with the same
            entries in the PC build gives its names, so both builds agree
  pc-graph  the PC build's names through matched functions (match_builds.py: shared strings
            and named globals, then the call graph and pointer tables); matched functions that stay unnamed
            get a note with their PC address
  kisak-graph  KisakCOD names from the call graph
  method    the rest of the methods (a name two functions share: name, name_2)

Usage:
    python3 tools/waw_decomp/name_x360.py --raw <export dir> --image image.pe --load 0x82000000 \
        --out names.tsv [--pc-names names.tsv --pc-raw <pc export dir> [--pc-exe CoDWaW.exe]] \
        [--kisak ... --kisak-tables ...] [--matches pc_match.tsv] [--extra hand.tsv ...]
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from match_builds import pass_graph  # noqa: E402
from naming_common import (DVAR_NAME_RE, Namer, Tables, kisak_table_index, other_build_tables,  # noqa: E402
                           pass_kisak, pass_kisak_graph, pass_methods, pass_selfname, pass_tables, pass_transfer, read_tsv,
                           register_core_filter)


class Image:
    """The flat image of an XEX (PE layout at its load address); big-endian code and data."""

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
            name = self.img[s:s + 8].rstrip(b"\0").decode("latin-1")
            vs, va = struct.unpack_from("<II", self.img, s + 8)
            ch, = struct.unpack_from("<I", self.img, s + 36)
            self.secs.append((name, load + va, load + va + vs, ch))
            s += 40

    def section_of(self, a):
        for n, lo, hi, ch in self.secs:
            if lo <= a < hi:
                return n
        return None

    def is_code(self, a):
        return any(lo <= a < hi and ch & 0x20000000 for n, lo, hi, ch in self.secs)

    def is_data(self, a):
        return any(lo <= a < hi and not ch & 0x20000000 for n, lo, hi, ch in self.secs)

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from(">I", self.img, o)[0] if 0 <= o <= len(self.img) - 4 else None

    def cstr(self, a, maxlen=64):
        o = a - self.base
        if not (0 <= o < len(self.img)) or not self.is_data(a):
            return None
        b = self.img[o:o + maxlen]
        z = b.find(b"\0")
        return b[:z].decode("latin-1") if z > 0 else None


def runtime_stubs(img, t):
    """The compiler's register save/restore routines: runs of 'std rN, d(r1)' (__savegprlr_N),
    'ld rN, d(r1)' (__restgprlr_N), 'stfd fN, d(r12)' (__savefpr_N) and 'lfd fN, d(r12)'
    (__restfpr_N), one entry point per register, ending in blr. Every function that saves
    registers calls one, so they have hundreds of callers and look alike: they are named by their
    instructions, never matched."""
    kinds = {62: ("__savegprlr_", 1), 58: ("__restgprlr_", 1), 54: ("__savefpr_", 12), 50: ("__restfpr_", 12)}
    out = {}
    for n, lo, hi, ch in img.secs:
        if not ch & 0x20000000:
            continue
        a = lo
        while a < hi - 8:
            w = img.u32(a)
            if w is None:
                break
            op, r, base = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if op in kinds and base == kinds[op][1] and r == 14:
                run = []
                b = a
                while b < hi:
                    v = img.u32(b)
                    if v is None or v >> 26 != op or (v >> 16) & 31 != kinds[op][1] or (v >> 21) & 31 != 14 + len(run):
                        break
                    run.append(b)
                    b += 4
                if len(run) == 18:  # registers 14 to 31
                    for k, e in enumerate(run):
                        if e in t.funcs:
                            out[e] = (f"{kinds[op][0]}{14 + k}", f"register {'save' if op in (62, 54) else 'restore'}"
                                      f" routine: {len(run)} entry points from {run[0]:08X}, this one starts at register {14 + k}")
                    a = b
                    continue
            a += 4
    return out


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def call_contexts(img, t):
    """Walk each function's code (contiguous on Xbox 360: .pdata gives the bounds) tracking
    constants built with lis/addi/ori, and record at every `bl`: the target, the constants in
    r3-r6, and a global the result (r3) is stored to right after the call."""
    out = []
    for entry in t.entries:
        size = int(t.funcs[entry]["size"])
        if size <= 0 or size > 1 << 20:
            continue
        regs = {}
        pending = None  # (site, target, args) waiting for a 'stw r3, d(rX)'
        seen_after = 0
        for a in range(entry, entry + size, 4):
            w = img.u32(a)
            if w is None:
                break
            op = w >> 26
            rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if pending is not None:
                # the result (r3) is usually stored a few instructions later, with the next call's
                # argument setup scheduled in between; stop once r3 is overwritten
                seen_after += 1
                writes_r3 = rd == 3 and op in (7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 58) or \
                    (op in (24, 25, 28, 29, 21) and ra == 3) or (op == 31 and ((w >> 1) & 0x3FF) == 444 and ra == 3)
                if op == 36 and rd == 3 and ra in regs:  # stw r3, d(rA)
                    out.append(pending + ((regs[ra] + s16(imm)) & 0xFFFFFFFF,))
                    pending = None
                elif seen_after >= 16 or op == 18 or writes_r3:
                    out.append(pending + (None,))
                    pending = None
            if op == 15:  # addis / lis
                base = 0 if ra == 0 else regs.get(ra)
                if base is None:
                    regs.pop(rd, None)
                else:
                    regs[rd] = (base + (s16(imm) << 16)) & 0xFFFFFFFF
            elif op == 14:  # addi / li
                base = 0 if ra == 0 else regs.get(ra)
                if base is None:
                    regs.pop(rd, None)
                else:
                    regs[rd] = (base + s16(imm)) & 0xFFFFFFFF
            elif op == 24:  # ori rA, rS, imm
                if rd in regs:
                    regs[ra] = regs[rd] | imm
                else:
                    regs.pop(ra, None)
            elif op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == ((w >> 11) & 31):  # mr rA, rS
                if rd in regs:
                    regs[ra] = regs[rd]
                else:
                    regs.pop(ra, None)
            elif op == 18 and w & 1:  # bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                target = (a + li) & 0xFFFFFFFF if not w & 2 else li & 0xFFFFFFFF
                if pending is not None:
                    out.append(pending + (None,))
                pending = (a, target, tuple(regs.get(r) for r in (3, 4, 5, 6)))
                seen_after = 0
                for r in [0] + list(range(3, 13)):
                    regs.pop(r, None)
            elif op in (32, 33, 34, 35, 40, 41, 42, 43, 48, 49, 50, 51, 58):  # loads overwrite rD
                regs.pop(rd, None)
            elif op == 31 or op == 21 or op == 20 or op == 28 or op == 29 or op == 30 or op == 7 or op == 8:
                # arithmetic/logic: the destination is rA for logic forms, rD for arithmetic
                regs.pop(rd, None)
                regs.pop(ra, None)
        if pending is not None:
            out.append(pending + (None,))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--load", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--pc-names")
    ap.add_argument("--pc-raw")
    ap.add_argument("--pc-exe", help="the PC executable: its tables of function pointers help the matching")
    ap.add_argument("--pc-script-api", help="the PC build's script_api.tsv (default: script_api.tsv next to"
                                            " --pc-names, when there): every entry of its tables")
    ap.add_argument("--kisak")
    ap.add_argument("--kisak-tables")
    ap.add_argument("--extra", action="append", default=[],
                    help="TSV: addr, name, kind, pass, evidence; names found by hand, taken right after the runtime routines (may be given more than once)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--matches", help="write every matched (Xbox 360, PC) function pair here")
    args = ap.parse_args()

    img = Image(args.image, args.load)
    t = Tables(args.raw)
    namer = Namer()
    stats = collections.Counter()

    # --- runtime: the compiler's register save/restore routines
    stubs = runtime_stubs(img, t)
    for a, (name, ev) in sorted(stubs.items()):
        if namer.add(a, name, "function", "runtime", ev):
            stats["runtime"] += 1

    # --- extra: names found by hand (a row of kind "note" is a comment only)
    for extra in args.extra:
        for r in read_tsv(extra):
            addr = int(r["addr"], 16)
            if r["kind"] == "inlined":
                namer.reserve(r["name"])
                stats["extra_inlined"] += 1
            elif r["kind"] == "note":
                namer.note(addr, r.get("pass") or "extra", r.get("evidence", ""), keep=True)
                stats["extra_notes"] += 1
            elif (r["kind"] != "function" or addr in t.funcs) and namer.add(
                    addr, r["name"], r["kind"], r.get("pass") or "extra", r.get("evidence", "")):
                stats["extra"] += 1

    # --- C++ methods that name exactly one function: before the PC names, since the same
    # Demonware message can sit in another class's method on PC (the rest at the end)
    pass_methods(namer, t, stats, unique_only=True)

    # --- pc: names from the PC build by shared strings
    if args.pc_names and args.pc_raw:
        other = Tables(args.pc_raw)
        names = {}
        for a, r in other.funcs.items():
            if not other.default_name(a) and r["source"] != "DEFAULT":
                names[a] = r["name"]
        for r in read_tsv(args.pc_names):
            if r["kind"] == "function":
                names[int(r["addr"], 16)] = r["name"]
        pass_transfer(namer, t, other, names, "pc", stats)

    # --- selfname
    pass_selfname(namer, t, stats)

    # --- call sites: console commands and dvars
    ctx = call_contexts(img, t)
    str_ok = lambda v: v is not None and img.cstr(v) is not None  # noqa: E731
    cmd_votes = collections.Counter()
    dvar_votes = collections.Counter()
    for site, target, (r3, r4, r5, r6), store in ctx:
        if str_ok(r3) and r4 is not None and r4 in t.funcs and r5 is not None and img.is_data(r5):
            cmd_votes[target] += 1
        if str_ok(r3) and DVAR_NAME_RE.match(img.cstr(r3) or "") and store is not None and img.is_data(store):
            dvar_votes[target] += 1
    cmd_callees = [c for c, n in cmd_votes.most_common(2) if n >= 20]
    for i, c in enumerate(cmd_callees):
        namer.add(c, "Cmd_AddCommand" if i == 0 else f"Cmd_AddCommand_{i}", "function", "cmd",
                  f"{cmd_votes[c]} calls with (name, handler, cmd_function_s)")
    # material and sound alias lookups are called the same way: keep the real dvar functions
    dvar_callees, dropped = register_core_filter({c for c, n in dvar_votes.items() if n >= 15}, dvar_votes,
                                                 t.callees, t.callers)
    stats["not_dvar_register_functions"] = len(dropped)
    for site, target, (r3, r4, r5, r6), store in ctx:
        if target in cmd_callees and str_ok(r3) and r4 in t.funcs:
            cname = img.cstr(r3)
            if re.fullmatch(r"[A-Za-z0-9_+\-]{1,40}", cname):
                clean = cname.replace("+", "plus_").replace("-", "minus_")
                if namer.add(r4, f"Cmd_{clean}_f", "function", "cmd", f'console command "{cname}" (registered at {site:08X})'):
                    stats["cmd"] += 1
                if r5 is not None:
                    namer.add(r5, f"cmd_{clean}_VAR", "global", "cmd", f'cmd_function_s of "{cname}"')
        if target in dvar_callees and store is not None and img.is_data(store) and str_ok(r3):
            dname = img.cstr(r3)
            if DVAR_NAME_RE.match(dname) and namer.add(store, f"dvar_{dname}", "global", "dvar",
                                                       f'dvar "{dname}" registered at {site:08X}'):
                stats["dvar"] += 1
    # name each registration function after the PC function that registers the same dvars
    pc_reg = {}  # dvar name -> PC register function name
    if args.pc_names and args.pc_raw:
        for r in read_tsv(args.pc_names):
            m = re.match(r'dvar "([^"]+)" registered at ([0-9A-F]{8})', r["evidence"])
            if m and r["pass"] == "dvar":
                pc_reg[m.group(1)] = int(m.group(2), 16)
        pc_calls = {int(r["site"], 16): r["callee"] for r in read_tsv(f"{args.pc_raw}/calls.tsv")}
        pc_fn = {int(r["addr"], 16): r["name"] for r in read_tsv(args.pc_names) if r["kind"] == "function"}
        for d, site in list(pc_reg.items()):
            callee = pc_calls.get(site, "")
            pc_reg[d] = pc_fn.get(int(callee, 16)) if re.fullmatch(r"[0-9A-F]{8}", callee) else None
    reg_votes = collections.defaultdict(collections.Counter)
    for site, target, (r3, r4, r5, r6), store in ctx:
        if target in dvar_callees and str_ok(r3):
            n = pc_reg.get(img.cstr(r3))
            if n:
                reg_votes[target][n] += 1
    for c in sorted(dvar_callees):
        best = reg_votes[c].most_common(1)
        if best and best[0][1] >= 5 and best[0][1] >= sum(reg_votes[c].values()) * 0.8 and \
                namer.add(c, best[0][0], "function", "dvar",
                          f"{dvar_votes[c]} dvar registrations; {best[0][1]} of the same dvars go through {best[0][0]} on PC"):
            continue
        namer.add(c, f"Dvar_Register_{c:08X}", "function", "dvar", f"{dvar_votes[c]} dvar registrations")
    stats["dvar_register_functions"] = len(dvar_callees)

    # --- kisak
    if args.kisak:
        pass_kisak(namer, t, args.kisak, stats)

    # --- tables of { "name", handler, ... } (big-endian words)
    words = []
    for n, lo, hi, ch in img.secs:
        if not ch & 0x20000000 and n not in (".pdata", ".reloc"):
            o = lo - img.base
            cnt = (hi - lo) // 4
            if 0 <= o and o + cnt * 4 <= len(img.img):
                words.append((lo, struct.unpack_from(">%dI" % cnt, img.img, o)))

    def ident(v):
        s = t.str_at.get(v) if v in t.str_at else img.cstr(v)
        return s if s and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", s) else None

    pc_api = args.pc_script_api
    if not pc_api and args.pc_names:
        guess = os.path.join(os.path.dirname(os.path.abspath(args.pc_names)), "script_api.tsv")
        pc_api = guess if os.path.exists(guess) else None
    # a handler is any word-aligned code address: Ghidra may not have made a function there yet
    pass_tables(namer, words, ident, lambda v: v % 4 == 0 and img.is_code(v), kisak_table_index(args.kisak_tables),
                stats, other_tables=other_build_tables(args.pc_names, pc_api) if args.pc_names else None,
                other_label="the PC build")

    # --- pc-graph: functions matched with the PC build's
    if args.pc_names and args.pc_raw:
        m = pass_graph(namer, args.raw, args.pc_raw, args.pc_names, "pc-graph", "PC", stats, notes=True,
                       image_here=args.image, image_there=args.pc_exe, cross_cpu=True)
        if args.matches:
            with open(args.matches, "w", encoding="utf-8") as o:
                o.write("x360\tpc\thow\tvotes\n")
                for a in sorted(m.ab):
                    o.write(f"{a:08X}\t{m.ab[a]:08X}\t{m.how[a][0]}\t{m.how[a][1]}\n")

    # --- kisak-graph
    # (import thunks and the register routines are no game functions)
    if args.kisak:
        pass_kisak_graph(namer, t, args.kisak, stats,
                         is_library=lambda a: a in stubs or t.funcs.get(a, {}).get("source") == "IMPORTED")

    # --- C++ methods that name themselves (Demonware, compiler function signatures): last, so they
    # only name what the passes above left
    pass_methods(namer, t, stats)

    namer.write(args.out)
    nf = sum(1 for v in namer.by_addr.values() if v[1] == "function")
    print(f"x360: {len(t.funcs)} functions, named {nf} functions and {len(namer.by_addr) - nf} globals")
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}")


if __name__ == "__main__":
    sys.exit(main())
