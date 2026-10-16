#!/usr/bin/env python3
"""
name_pc.py - name functions and globals in World at War's PC executables.

Reads the tables Ghidra exported (WawExport.java: functions.tsv, calls.tsv,
strings.tsv) and the executable itself, and writes names.tsv:

    addr  name  kind  pass  evidence

Passes, strongest first (a name or an address is taken by the first pass that
claims it):

  public    names from public, open-source projects that target the same
            build (tools/waw_decomp/public_symbols.tsv, verified=yes rows)
  selfname  a function's own name in its error/log strings
            ("BG_ParseCommands: expected animation")
  cmd       console commands: Cmd_AddCommand("name", func, &cmd) -> Cmd_<name>_f
  dvar      dvar registrations: g = Dvar_RegisterX("name", ...) -> dvar_<name>
            (only functions that reach the dvar system's core: material and sound
            alias lookups are called the same way)
  kisak     (optional, --kisak) the same unique strings in the reconstructed
            CoD4 source (KisakCOD): names only, never code
  table     { "name", handler } arrays (script builtins, parsers, ...)
  reader    the function that looks names up in such a table, named after KisakCOD's
            reader of the same array (--kisak-readers)
  sp / mp   (optional, --other) the other PC executable: identical code, then the
            only users of the same strings
  sp-graph / mp-graph  the other executable's names through matched functions
            (match_builds.py: strings and named globals, then the call graph and
            pointer tables)
  kisak-graph  KisakCOD names from the call graph

Usage:
    python3 tools/waw_decomp/name_pc.py --exe CoDWaW.exe --which sp \
        --raw <ghidra export dir> --public tools/waw_decomp/public_symbols.tsv \
        --out names.tsv [--kisak kisak_strings.tsv] [--other EXE RAW NAMES [--matches sp_mp.tsv]]

Needs: capstone, pefile (pip).
"""
import argparse
import bisect
import collections
import os
import re
import struct
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from match_builds import pass_graph  # noqa: E402
from naming_common import (DVAR_NAME_RE, Namer, Tables, kisak_table_index, pass_kisak,  # noqa: E402
                           pass_kisak_graph, pass_methods, pass_readers, pass_selfname, pass_tables, pass_transfer, read_tsv,
                           register_core_filter)


class Exe:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode(errors="replace")
            va = self.base + s.VirtualAddress
            self.secs.append((name, va, va + max(s.Misc_VirtualSize, s.SizeOfRawData)))
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = False

    def section_of(self, addr):
        for n, a, b in self.secs:
            if a <= addr < b:
                return n
        return None

    def bytes_at(self, addr, n):
        o = addr - self.base
        return self.img[o:o + n]

    def disasm(self, start, end):
        """Linear decode [start, end); yields (addr, mnemonic, op_str)."""
        code = self.bytes_at(start, end - start)
        for i in self.md.disasm_lite(code, start):
            yield i[0], i[2], i[3]


def code_shapes(exe, funcs, min_size=24):
    """A fingerprint of each function's code that ignores where things are: absolute addresses
    and call/jump targets become placeholders, so the same source compiled into both executables
    gives the same fingerprint. Returns {fingerprint: [entry, ...]}."""
    import hashlib
    lo_hi = [(a, b) for n, a, b in exe.secs]

    def is_addr(v):
        return any(a <= v < b for a, b in lo_hi)

    out = collections.defaultdict(list)
    for entry, r in funcs.items():
        size = int(r["size"])
        if size < min_size or size > 1 << 16:
            continue
        parts = []
        for a, mn, op in exe.disasm(entry, entry + size):
            if mn in ("call", "jmp") or mn.startswith("j"):
                op = "T"
            else:
                op = re.sub(r"0x[0-9a-f]+", lambda m: "A" if is_addr(int(m.group(0), 16)) else m.group(0), op)
            parts.append(mn + " " + op)
        if parts:
            out[hashlib.sha1("\n".join(parts).encode()).hexdigest()].append(entry)
    return out


def load_public(path, which):
    out = []
    for r in read_tsv(path):
        if r["exe"] != which or r["verified"] != "yes":
            continue
        out.append((int(r["addr"], 16), r["name"], r["kind"], r["source_url"]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--which", required=True, choices=["sp", "mp"])
    ap.add_argument("--raw", required=True)
    ap.add_argument("--public")
    ap.add_argument("--kisak", help="TSV: name, file, strings (\\x1f separated) from KisakCOD")
    ap.add_argument("--kisak-tables", help="TSV: string, function, file (kisak_strings.py *_tables.tsv)")
    ap.add_argument("--kisak-readers", help="TSV: table, file, function (kisak_strings.py *_readers.tsv);"
                                             " default: next to --kisak-tables")
    ap.add_argument("--other", nargs=3, metavar=("EXE", "RAW", "NAMES"),
                    help="the other PC executable (SP for MP, MP for SP): its exe, Ghidra export and names.tsv;"
                         " identical code and shared strings carry its names over")
    ap.add_argument("--out", required=True)
    ap.add_argument("--matches", help="with --other: write every matched (this, other) function pair here")
    args = ap.parse_args()

    exe = Exe(args.exe)
    t = Tables(args.raw)
    funcs, entries, calls, str_at = t.funcs, t.entries, t.calls, t.str_at
    namer = Namer()
    stats = collections.Counter()

    # --- public
    pub_names = {}
    if args.public:
        for addr, name, kind, src in load_public(args.public, args.which):
            k = "function" if kind == "function" else "global"
            if k == "function" and addr not in funcs:
                stats["public_not_function_start"] += 1
                continue
            if namer.add(addr, name, k, "public", src):
                stats["public"] += 1
                pub_names[name] = addr

    # --- selfname
    pass_selfname(namer, t, stats)

    # --- call sites with pushed immediates (commands and dvars)
    by_func_sites = collections.defaultdict(list)
    for r in calls:
        if r["callee"].startswith(("ext:", "ptr:")):
            continue
        by_func_sites[int(r["caller"], 16)].append((int(r["site"], 16), int(r["callee"], 16)))

    def site_context(func, sites):
        """For each call site in func: (pushed immediates before it, first 'mov [mem], eax' after it)."""
        f = funcs.get(func)
        if not f:
            return {}
        end = max(s for s, _ in sites) + 32
        insns = list(exe.disasm(func, end))
        idx = {a: i for i, (a, _, _) in enumerate(insns)}
        out = {}
        for s, callee in sites:
            i = idx.get(s)
            if i is None:
                continue
            pushes = []
            j = i - 1
            while j >= 0 and len(pushes) < 8 and i - j <= 16:
                a, mn, op = insns[j]
                if mn in ("call", "ret", "jmp") or mn.startswith("j"):
                    break
                if mn == "push":
                    pushes.append(op)
                elif mn == "mov" and re.match(r"e[a-ds][xip], 0x[0-9a-f]+$", op):
                    pushes.append(op.split(", ")[1])  # register arguments (MSVC __usercall)
                j -= 1
            store = None
            # the result (eax) may be stored a few instructions later, after the next call's
            # arguments are pushed; stop once eax is overwritten
            for a, mn, op in insns[i + 1:i + 13]:
                mm = re.match(r"dword ptr \[(0x[0-9a-f]+)\], eax$", op) if mn == "mov" else None
                if mm:
                    store = int(mm.group(1), 16)
                    break
                if mn in ("call", "ret", "jmp") or re.match(r"(e?ax|al|ah)\b", op) and mn not in ("push", "test", "cmp"):
                    break
            out[s] = (callee, pushes, store)
        return out

    def imm(op):
        try:
            return int(op, 16) if op.startswith("0x") else int(op)
        except ValueError:
            return None

    contexts = {}
    for func, sites in by_func_sites.items():
        contexts.update(site_context(func, sites))

    # find Cmd_AddCommand: the callee most often given (string, .text function pointer, data pointer)
    cmd_votes = collections.Counter()
    for s, (callee, pushes, store) in contexts.items():
        vals = [imm(p) for p in pushes[:3]]
        if len(vals) == 3 and all(v is not None for v in vals):
            name_s, fn, buf = vals[0], vals[1], vals[2]
            if name_s in str_at and fn in funcs and exe.section_of(buf) == ".data":
                cmd_votes[callee] += 1
    cmd_callees = [c for c, n in cmd_votes.most_common(3) if n >= 20]
    stats["cmd_add_candidates"] = len(cmd_callees)
    for i, c in enumerate(cmd_callees):
        namer.add(c, "Cmd_AddCommand" if i == 0 else f"Cmd_AddCommand_{i}", "function", "cmd",
                  f"{cmd_votes[c]} calls with (name, handler, cmd_function_s)")
    for s, (callee, pushes, store) in contexts.items():
        if callee not in cmd_callees:
            continue
        vals = [imm(p) for p in pushes[:3]]
        if len(vals) < 3 or any(v is None for v in vals):
            continue
        cname = str_at.get(vals[0], "")
        if not re.fullmatch(r"[A-Za-z0-9_+\-]{1,40}", cname):
            continue
        clean = cname.replace("+", "plus_").replace("-", "minus_")
        if namer.add(vals[1], f"Cmd_{clean}_f", "function", "cmd", f'console command "{cname}" (call at {s:08X})'):
            stats["cmd"] += 1
        namer.add(vals[2], f"cmd_{clean}_VAR", "global", "cmd", f'cmd_function_s of "{cname}"')

    # inlined Cmd_AddCommand (World at War PC): the cmd_function_s fields are stored directly,
    #   mov [cmd+4], <name>; mov [cmd+0x10], <handler>; mov [cmd], <old head>; mov [cmd_functions], cmd
    def inlined_cmds():
        found = []
        head_votes = collections.Counter()
        for func in entries:
            f = funcs[func]
            size = int(f["size"])
            if size < 24 or size > 200000:
                continue
            insns = list(exe.disasm(func, func + size))
            regs = {}
            stores = []  # (index, dest, value_or_None, src_text)
            for k, (a, mn, op) in enumerate(insns):
                m = re.match(r"(e[a-ds][xip]), (0x[0-9a-f]+)$", op) if mn == "mov" else None
                if m:
                    regs[m.group(1)] = int(m.group(2), 16)
                    continue
                m = re.match(r"dword ptr \[(0x[0-9a-f]+)\], (\S+)$", op) if mn == "mov" else None
                if m:
                    dest = int(m.group(1), 16)
                    src = m.group(2)
                    val = int(src, 16) if src.startswith("0x") else regs.get(src)
                    stores.append((k, dest, val, src))
                    continue
                if mn in ("call",):
                    regs.pop("eax", None); regs.pop("ecx", None); regs.pop("edx", None)
            for n, (k, dest, val, src) in enumerate(stores):
                if val is None or exe.section_of(val) not in (".data",) or not src.startswith("0x"):
                    continue
                cmd = val
                window = [s for s in stores[max(0, n - 6):n] if cmd <= s[1] < cmd + 0x20]
                handler = next((s[2] for s in window if s[2] in funcs), None)
                name = next((s[2] for s in window if s[2] in str_at), None)
                if handler and name:
                    head_votes[dest] += 1
                    found.append((dest, cmd, handler, name, insns[k][0]))
        if not head_votes:
            return None, []
        head = head_votes.most_common(1)[0][0]
        return head, [x for x in found if x[0] == head]

    head, cmds = inlined_cmds()
    if head is not None:
        namer.add(head, "cmd_functions", "global", "cmd", f"list head of {len(cmds)} inlined Cmd_AddCommand calls")
    for _, cmd, handler, nm_addr, site in cmds:
        cname = str_at.get(nm_addr, "")
        if not re.fullmatch(r"[A-Za-z0-9_+\-]{1,40}", cname):
            continue
        clean = cname.replace("+", "plus_").replace("-", "minus_")
        if namer.add(handler, f"Cmd_{clean}_f", "function", "cmd", f'console command "{cname}" (registered at {site:08X})'):
            stats["cmd"] += 1
        namer.add(cmd, f"cmd_{clean}_VAR", "global", "cmd", f'cmd_function_s of "{cname}"')

    # dvars: callers that pass a dvar-name string and store eax into a global right after
    reg_targets = set()
    for nm, a in pub_names.items():
        if nm.startswith("Dvar_Register"):
            reg_targets.add(a)
    # wrappers: any function whose call sites mostly look like (name string ... ) -> store
    dvar_votes = collections.Counter()
    for s, (callee, pushes, store) in contexts.items():
        if store is None or exe.section_of(store) != ".data":
            continue
        for p in pushes:
            v = imm(p)
            if v in str_at and DVAR_NAME_RE.match(str_at[v]):
                dvar_votes[callee] += 1
                break
    # material and sound alias lookups are called the same way: keep the real dvar functions
    dvar_callees, dropped = register_core_filter({c for c, n in dvar_votes.items() if n >= 15} | reg_targets,
                                                 dvar_votes, t.callees, t.callers, refs=reg_targets)
    stats["dvar_register_functions"] = len(dvar_callees)
    stats["not_dvar_register_functions"] = len(dropped)
    for s, (callee, pushes, store) in sorted(contexts.items()):
        if callee not in dvar_callees or store is None:
            continue
        dname = None
        for p in reversed(pushes):  # first argument = last push before the call
            pass
        for p in pushes:  # nearest push first
            v = imm(p)
            if v in str_at and DVAR_NAME_RE.match(str_at[v]):
                dname = str_at[v]
                break
        if not dname:
            continue
        if namer.add(store, f"dvar_{dname}", "global", "dvar", f'dvar "{dname}" registered at {s:08X}'):
            stats["dvar"] += 1
    for c in sorted(dvar_callees):
        if c not in namer.by_addr:
            namer.add(c, f"Dvar_Register_{c:08X}", "function", "dvar", f"{dvar_votes[c]} dvar registrations")

    # --- kisak (names from the reconstructed CoD4 source, by unique shared strings)
    if args.kisak:
        pass_kisak(namer, t, args.kisak, stats)

    # --- tables of { "name", handler, ... }
    def cstr(a):
        """A plain identifier string stored at a (read from the image; Ghidra may not have defined it)."""
        if a in str_at:
            return str_at[a] if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", str_at[a] or "") else None
        if exe.section_of(a) not in (".rdata", ".data"):
            return None
        b = exe.bytes_at(a, 64)
        z = b.find(b"\0")
        if z <= 0:
            return None
        s = b[:z].decode("latin-1")
        return s if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", s) else None

    text_lo, text_hi = next((a, b) for n, a, b in exe.secs if n == ".text")
    words = []
    for sname, s0, s1 in exe.secs:
        if sname in (".data", ".rdata"):
            raw = exe.bytes_at(s0, s1 - s0)  # stops at the image's end (zero-filled .bss isn't stored)
            n = len(raw) // 4
            words.append((s0, struct.unpack("<%dI" % n, raw[:n * 4])))
    text = exe.bytes_at(text_lo, text_hi - text_lo)

    def table_refs(tbl):
        """The functions whose code holds the table's address (or its second column's)."""
        out = set()
        for target in (tbl, tbl + 4):
            pat = struct.pack("<I", target)
            i = text.find(pat)
            while i != -1:
                k = bisect.bisect_right(entries, text_lo + i) - 1
                if k >= 0:
                    out.add(entries[k])
                i = text.find(pat, i + 1)
        return out

    def loop_bounds(f, size):
        """Entry counts a lookup loop in f stops at: cmp reg, (count * size) - 1 or count * size."""
        out = set()
        n = int(funcs[f]["size"]) if f in funcs else 0
        if not 0 < n < 4096:
            return out
        for _, mn, op in exe.disasm(f, f + n):
            mm = re.match(r"(?:e[a-d]x|e[sd]i|ebp), (0x[0-9a-f]+|\d+)$", op) if mn == "cmp" else None
            if mm:
                v = int(mm.group(1), 0)
                for c in (v + 1, v):
                    if c >= 2 * size and c % size == 0:
                        out.add(c // size)
        return out

    def table_bounds(tbl, size):
        # a cut only where the rest of the table has a reader of its own (a loop over 72 entries
        # of 8 bytes, cmp with 0x48, would otherwise read as 9 entries)
        return {c for f in table_refs(tbl) for c in loop_bounds(f, size) if table_refs(tbl + c * size)}

    def table_users(tbl):
        """Names of the functions whose code holds the table's address."""
        out, pat = set(), struct.pack("<I", tbl)
        i = text.find(pat)
        while i != -1:
            a = text_lo + i
            k = bisect.bisect_right(entries, a) - 1
            if k >= 0:
                out.add(namer.func_name(entries[k]) or "")
            i = text.find(pat, i + 1)
        return out

    tables = pass_tables(namer, words, cstr, lambda v: text_lo <= v < text_hi, kisak_table_index(args.kisak_tables),
                         stats, users=table_users, bounds=table_bounds)
    readers_tsv = args.kisak_readers
    if not readers_tsv and args.kisak_tables:
        guess = args.kisak_tables.replace("_tables.tsv", "_readers.tsv")
        readers_tsv = guess if guess != args.kisak_tables and os.path.exists(guess) else None
    if readers_tsv:
        pass_readers(namer, tables, readers_tsv, table_refs, loop_bounds, stats)

    # --- other: names from the other PC executable (identical code, then shared strings)
    other_names = {}
    if args.other:
        o_exe, o_raw, o_names = args.other
        ot = Tables(o_raw)
        for r in read_tsv(o_names):
            if r["kind"] == "function":
                other_names[int(r["addr"], 16)] = r["name"]
        for a, r in ot.funcs.items():
            if a not in other_names and not ot.default_name(a) and r["source"] != "DEFAULT":
                other_names[a] = r["name"]
        mine = code_shapes(exe, funcs)
        theirs = code_shapes(Exe(o_exe), ot.funcs)
        label = "mp" if args.which == "sp" else "sp"
        for h, here in mine.items():
            there = theirs.get(h)
            if len(here) == 1 and there and len(there) == 1 and there[0] in other_names:
                if namer.add(here[0], other_names[there[0]], "function", label,
                             f"same code as {other_names[there[0]]} at {there[0]:08X} in the {label.upper()} executable"):
                    stats[label + "-code"] += 1
        pass_transfer(namer, t, ot, other_names, label, stats)
        # the rest of the other executable's names through matched functions (call graph)
        m = pass_graph(namer, args.raw, o_raw, o_names, label + "-graph", label.upper(), stats,
                       image_here=args.exe, image_there=o_exe)
        if args.matches:
            with open(args.matches, "w", encoding="utf-8") as o:
                o.write(f"{args.which}\t{label}\thow\tvotes\n")
                for a in sorted(m.ab):
                    o.write(f"{a:08X}\t{m.ab[a]:08X}\t{m.how[a][0]}\t{m.how[a][1]}\n")

    # --- kisak-graph (game functions are 16-byte aligned; unaligned ones are CRT/DirectX code)
    if args.kisak:
        pass_kisak_graph(namer, t, args.kisak, stats, is_library=lambda a: a & 0xF != 0)

    # --- C++ methods that name themselves (Demonware, compiler function signatures): last, so they
    # only name what the passes above left
    pass_methods(namer, t, stats)

    namer.write(args.out)
    nf = sum(1 for v in namer.by_addr.values() if v[1] == "function")
    print(f"{args.which}: {len(funcs)} functions, named {nf} functions and {len(namer.by_addr) - nf} globals")
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}")


if __name__ == "__main__":
    sys.exit(main())
