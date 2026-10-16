#!/usr/bin/env python3
"""
name_pc.py - name the functions and globals of a PC Call of Duty 2 or United Offensive
executable (or game DLL) from the tables Ghidra exported (PcExport.java tables).

Writes names.tsv:  addr  name  kind  pass  evidence

Passes, strongest first. An address or a name is taken by the first pass that claims it; a
later pass that disagrees is recorded in the evidence ("also mac: X") so mismatches show.

  cod2x     --cod2x <symbols_*.h>: CoD2x's table of addresses, lengths and names for this exact
            build (names and addresses only; "sub_*" placeholders skipped)
  selfname  the function's own name in its messages: "SV_DirectConnect: ...",
            "SV_DirectConnect()\\n", or "%s: ..." with the name passed as an argument
  cmd       console commands: Cmd_AddCommand("name", handler) -> Cmd_<name>_f
  dvar      Dvar_Register*("name", ...) / Cvar_Get("name", ...) stored into a global ->
            dvar_<name> / cvar_<name>; game modules: Cvar_Register(&vmCvar, "name", ...) and
            tables of { &vmCvar, "name", "default", flags }
  q3        --q3 <tsv>: the same distinctive strings in exactly one Quake III Arena function
  match     --match <dir>...: code shape (instructions, strings, callees) identical or nearly so
            to a named function of another build (the CoD2 MP run)
  mac       --mac <tsv>: the same distinctive strings in exactly one function of the Mac 1.3
            build's decompile (which has real symbol names); then its call graph, only where
            the callees of both functions agree (evidence says "call graph")
  table     --tables <tsv>...: arrays of { "name", handler } that match a reference table
            (script builtins, menu parsers): the handler gets the reference's name

The <tsv> references are made by tools/waw_decomp/kisak_strings.py (name, file, strings,
calls; and *_tables.tsv). Only names are taken from them, never code.

Usage:
    python3 tools/pc_decomp/name_pc.py --exe <exe> --raw <tables dir> --out names.tsv
        [--var-prefix dvar|cvar] [--cod2x symbols.h] [--q3 q3.tsv] [--mac mac.tsv]
        [--match <reference work dir>...] [--tables a_tables.tsv b_tables.tsv]

Needs: capstone, pefile (pip).
"""
import argparse
import collections
import difflib
import hashlib
import os
import re
import struct
import sys

import capstone
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "waw_decomp"))
from naming_common import Namer, read_tsv  # noqa: E402  (shared with the World at War tools)

FUNC_NAME = r"[A-Z][A-Za-z0-9]{0,11}_[A-Za-z0-9_]*[a-z][A-Za-z0-9_]*|[a-z]{1,8}_[A-Z][A-Za-z0-9_]+"
# "Name: ...", "Name(): ...", "^1Name: ..." (color codes), "ERROR: Name: ..."
SELF_COLON = re.compile(r"^(?:\^\d)?(?:(?:ERROR|WARNING|Error|Warning)\s*:\s*)?(" + FUNC_NAME + r")\s*(?:\(\s*\))?\s*:")
# "Name()" alone, "Name()\n" (trace messages)
SELF_CALL = re.compile(r"^(?:\^\d)?(" + FUNC_NAME + r")\s*\(\s*\)\s*\n?$")
FUNC_NAME_RE = re.compile(r"^(?:" + FUNC_NAME + r")$")
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CMD_RE = re.compile(r"^[A-Za-z0-9_+\-]{1,40}$")
VAR_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{1,63}$")
VAR_SHAPE = re.compile(r"^[a-z]{1,6}_[A-Za-z0-9_]+$|^[a-z]+[A-Z][A-Za-z0-9_]*$")


def unesc(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x" and i + 4 <= len(s):
                out.append(chr(int(s[i + 2:i + 4], 16)))
                i += 4
                continue
            out.append({"t": "\t", "n": "\n", "r": "\r", "\\": "\\"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def printable(s):
    return s is not None and all(32 <= ord(c) < 127 for c in s)


def kis_unesc(s):
    """kisak_strings.py escapes only backslash, tab, newline, carriage return."""
    return s.replace("\\t", "\t").replace("\\n", "\n").replace("\\r", "\r").replace("\\\\", "\\")


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode(errors="replace")
            va = self.base + s.VirtualAddress
            self.secs.append((name, va, va + max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics))
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def section_of(self, addr):
        for n, a, b, _ in self.secs:
            if a <= addr < b:
                return n
        return None

    def is_code(self, addr):
        for n, a, b, ch in self.secs:
            if a <= addr < b:
                return bool(ch & 0x20000000) and not n.startswith("stxt")
        return False

    def is_data(self, addr):
        for n, a, b, ch in self.secs:
            if a <= addr < b:
                return not (ch & 0x20000000) and n not in (".rsrc", ".reloc")
        return False

    def bytes_at(self, addr, n):
        o = addr - self.base
        return self.img[o:o + n]

    def u32(self, addr):
        b = self.bytes_at(addr, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else None

    def cstr(self, addr, maxlen=256):
        b = self.bytes_at(addr, maxlen)
        z = b.find(b"\0")
        return b[:z].decode("latin-1") if z > 0 else None

    def insns(self, ranges):
        """[(addr, size, mnemonic, op_str)] over a function's body ranges, in address order."""
        out = []
        for lo, hi in ranges:
            for i in self.md.disasm_lite(self.bytes_at(lo, hi - lo + 1), lo):
                out.append(i)
        return out


class Tables:
    """One program's export (PcExport.java tables)."""

    def __init__(self, raw):
        self.funcs = {int(r["entry"], 16): r for r in read_tsv(f"{raw}/functions.tsv")}
        self.entries = sorted(self.funcs)
        self.ranges = {}
        for e, r in self.funcs.items():
            rs = []
            for part in r["ranges"].split(","):
                if part:
                    lo, hi = part.split("-")
                    rs.append((int(lo, 16), int(hi, 16)))
            self.ranges[e] = rs
        self.calls = read_tsv(f"{raw}/calls.tsv")
        self.str_at = {}
        self.str_funcs = collections.defaultdict(set)
        self.func_strs = collections.defaultdict(set)
        for r in read_tsv(f"{raw}/strings.tsv"):
            a, text = int(r["addr"], 16), unesc(r["text"])
            self.str_at[a] = text
            for f in r["funcs"].split(","):
                if f:
                    self.str_funcs[text].add(int(f, 16))
                    self.func_strs[int(f, 16)].add(text)
        self.callees = collections.defaultdict(set)
        self.callers = collections.defaultdict(set)
        self.call_seq = collections.defaultdict(list)  # caller -> [(site, callee key)] in site order
        for r in self.calls:
            site, c = int(r["site"], 16), r["callee"]
            caller = int(r["caller"], 16)
            if not c.startswith(("ext:", "ptr:")):
                c = int(c, 16)
                if r["kind"] in ("UNCONDITIONAL_CALL", "TAILJUMP") and c in self.funcs and c != caller:
                    self.callees[caller].add(c)
                    self.callers[c].add(caller)
            self.call_seq[caller].append((site, c))
        for k in self.call_seq:
            seen, seq = set(), []
            for s, c in sorted(self.call_seq[k], key=lambda x: (x[0], str(x[1]))):
                if s not in seen:
                    seen.add(s)
                    seq.append((s, c))
            self.call_seq[k] = seq


class Names(Namer):
    """Namer that also remembers what later passes would have said (for the evidence)."""

    def __init__(self):
        super().__init__()
        self.also = collections.defaultdict(list)
        self.agree = collections.Counter()

    def add(self, addr, name, kind, pas, evidence):
        cur = self.by_addr.get(addr)
        if cur is not None:
            if cur[0] == name:
                self.agree[(cur[2], pas, "same")] += 1
            elif cur[2] != pas:
                self.agree[(cur[2], pas, "different")] += 1
                self.also[addr].append(f"{pas}: {name}")
            return False
        return super().add(addr, name, kind, pas, evidence)

    def add_global(self, addr, name, pas, evidence):
        """A global; a second global for the same name gets _2, _3, ..."""
        if addr in self.by_addr:
            return False
        n, k = name, 1
        while n in self.by_name:
            k += 1
            n = f"{name}_{k}"
        return super().add(addr, n, "global", pas, evidence)

    def write(self, path):
        with open(path, "w", encoding="utf-8") as o:
            o.write("addr\tname\tkind\tpass\tevidence\n")
            for a in sorted(self.by_addr):
                name, kind, pas, ev = self.by_addr[a]
                if self.also.get(a):
                    ev += "; also " + ", ".join(self.also[a][:4])
                o.write("\t".join(x.replace("\t", " ").replace("\n", " ") for x in (f"{a:08X}", name, kind, pas, ev)) + "\n")


# ---------------------------------------------------------------- cod2x

def pass_cod2x(names, t, img, header, stats):
    text = open(header, encoding="utf-8", errors="replace").read()
    rows = re.findall(r'\{\s*0x([0-9a-fA-F]+)u?\s*,\s*0x([0-9a-fA-F]+)u?\s*,\s*"([^"]+)"', text)
    seen = collections.Counter()
    src = os.path.basename(header)
    for a, ln, n in rows:
        if "sub_" in n or not IDENT_RE.match(n):
            stats["cod2x_skipped_placeholder"] += 1
            continue
        a = int(a, 16)
        if not img.is_code(a):
            stats["cod2x_not_code"] += 1
            continue
        seen[n] += 1
        name = n if seen[n] == 1 else f"{n}_{seen[n]}"
        ev = f"CoD2x {src}: {n}, length 0x{int(ln, 16):X}"
        if a not in t.funcs:
            ev += "; not a function start after analysis (created by the name step)"
            stats["cod2x_new_function"] += 1
        if names.add(a, name, "function", "cod2x", ev):
            stats["cod2x"] += 1


# ---------------------------------------------------------------- selfname

def pass_selfname(names, t, stats):
    votes = collections.defaultdict(collections.Counter)
    how = {}
    for text, fs in t.str_funcs.items():
        m = SELF_COLON.match(text) or SELF_CALL.match(text)
        if not m:
            continue
        name = m.group(1)
        if len(name) < 5:
            continue
        for f in fs:
            votes[f][name] += 1
            how[(f, name)] = text
    # "%s: ..." with the function's name passed as an argument: the name string is used by this
    # function only, and the function also uses a "%s: " / "%s(): " format
    for f, strs in t.func_strs.items():
        if not any(re.match(r"^(?:\^\d)?%s\s*(?:\(\))?\s*:", s) for s in strs):
            continue
        for s in strs:
            if FUNC_NAME_RE.match(s) and len(t.str_funcs[s]) == 1:
                votes[f][s] += 1
                how.setdefault((f, s), f'"%s: ..." with "{s}"')
    cand = collections.defaultdict(list)
    for f, c in votes.items():
        total = sum(c.values())
        name, n = c.most_common(1)[0]
        if len(c) > 1 and c.most_common(2)[1][1] == n:
            stats["selfname_tie"] += 1
            continue
        size = int(t.funcs[f]["size"]) if f in t.funcs else 1 << 30
        cand[name].append((n, n / total, -size, f))
    for name, lst in sorted(cand.items()):
        lst.sort(reverse=True)
        n, share, _, f = lst[0]
        if len(lst) > 1 and lst[1][0] == n:
            stats["selfname_ambiguous"] += 1
            continue
        other = f"; {len(lst) - 1} other function(s) use it less" if len(lst) > 1 else ""
        ex = how[(f, name)].replace("\n", "\\n")[:60]
        if names.add(f, name, "function", "selfname", f'{n} own-name string(s) ({share:.0%}), e.g. "{ex}"{other}'):
            stats["selfname"] += 1


# ---------------------------------------------------------------- call sites

WRITES_FIRST = {"mov", "lea", "add", "sub", "and", "or", "xor", "inc", "dec", "pop", "movzx", "movsx",
                "shl", "shr", "sar", "imul", "neg", "not", "sbb", "adc", "setne", "sete", "cdq", "xchg"}
REGS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp")


def parse_imm(op):
    try:
        v = int(op, 16) if op.startswith(("0x", "-0x")) else int(op)
        return v & 0xFFFFFFFF
    except ValueError:
        return None


def site_contexts(t, img):
    """site -> (callee key, args (first argument first; None when unknown), regs at the call,
    global the result is stored into right after the call or None)."""
    out = {}
    for f, seq in t.call_seq.items():
        if f not in t.ranges:
            continue
        ins = img.insns(t.ranges[f])
        idx = {a: k for k, (a, _, _, _) in enumerate(ins)}
        for site, callee in seq:
            k = idx.get(site)
            if k is None:
                continue
            j = k - 1
            while j >= 0 and k - j <= 24:
                mn = ins[j][2]
                if mn in ("call", "ret", "jmp") or mn.startswith("j") or mn.startswith("rep"):
                    break
                j -= 1
            regs, stack = {}, []
            for a, size, mn, op in ins[j + 1:k]:
                ops = [x.strip() for x in op.split(",")] if op else []
                if mn == "push":
                    if ops and ops[0] in REGS:
                        stack.append(regs.get(ops[0]))
                    elif ops:
                        stack.append(parse_imm(ops[0]))
                    continue
                if mn == "pop" and stack:
                    stack.pop()
                if mn == "sub" and ops[:1] == ["esp"]:
                    n = parse_imm(ops[1]) or 0
                    stack.extend([None] * min(n // 4, 16))
                    continue
                if mn == "mov" and len(ops) == 2 and ops[0] in REGS:
                    v = parse_imm(ops[1])
                    regs[ops[0]] = v if v is not None else (regs.get(ops[1]) if ops[1] in REGS else None)
                    continue
                if mn == "xor" and len(ops) == 2 and ops[0] == ops[1] and ops[0] in REGS:
                    regs[ops[0]] = 0
                    continue
                if ops and ops[0] in REGS and mn in WRITES_FIRST:
                    regs.pop(ops[0], None)
            # the result: eax, maybe copied to another register first, stored into a global
            store, held = None, {"eax"}
            for a, size, mn, op in ins[k + 1:k + 14]:
                if mn in ("call", "ret", "jmp") or mn.startswith("j"):
                    break
                ops = [x.strip() for x in op.split(",")] if op else []
                if mn == "mov" and len(ops) == 2:
                    m = re.match(r"dword ptr \[(0x[0-9a-f]+)\]$", ops[0])
                    if m and ops[1] in held:
                        store = int(m.group(1), 16)
                        break
                    if ops[0] in REGS:
                        if ops[1] in held:
                            held.add(ops[0])
                        else:
                            held.discard(ops[0])
                        continue
                if ops and ops[0] in held and mn in WRITES_FIRST:
                    held.discard(ops[0])
                if not held:
                    break
            out[site] = (callee, list(reversed(stack)), {r: regs.get(r) for r in ("eax", "ecx", "edx")}, store)
    return out


def callee_name(names, t, c):
    if isinstance(c, int):
        n = names.func_name(c)
        if n:
            return n
        r = t.funcs.get(c)
        if r and r["source"] != "DEFAULT" and not r["name"].startswith(("FUN_", "thunk_FUN_")):
            return r["name"]
        return None
    return c  # "ext:..." / "ptr:..."


# ---------------------------------------------------------------- cmd

def pass_cmd(names, t, img, ctx, stats):
    known = {a for a, v in names.by_addr.items() if v[1] == "function" and v[0] in ("Cmd_AddCommand",)}
    votes, sites = collections.Counter(), collections.Counter()
    for s, (callee, args, regs, store) in ctx.items():
        sites[callee] += 1
        if len(args) >= 2 and args[0] in t.str_at and CMD_RE.match(t.str_at[args[0]]) and args[1] is not None \
                and (args[1] in t.funcs or img.is_code(args[1])):
            votes[callee] += 1
    regs_ = set(known) | {c for c, n in votes.items() if n >= 15 and n >= 0.6 * sites[c]}
    stats["cmd_register_functions"] = len(regs_)
    for c in sorted(regs_, key=str):
        if isinstance(c, int) and c not in names.by_addr:
            names.add(c, "Cmd_AddCommand", "function", "cmd", f"{votes[c]} calls with (command name, handler)")
    for s, (callee, args, regs, store) in sorted(ctx.items()):
        if callee not in regs_ or len(args) < 2 or args[0] not in t.str_at or not args[1]:
            continue
        cname = t.str_at[args[0]]
        if not CMD_RE.match(cname) or not img.is_code(args[1]):
            continue
        clean = cname.replace("+", "plus_").replace("-", "minus_")
        if names.add(args[1], f"Cmd_{clean}_f", "function", "cmd", f'handler of console command "{cname}" (registered at {s:08X})'):
            stats["cmd"] += 1


# ---------------------------------------------------------------- dvar / cvar

def pass_vars(names, t, img, ctx, prefix, stats):
    known = {a for a, v in names.by_addr.items() if v[1] == "function"
             and re.match(r"^(Dvar_Register|Cvar_Get$|Cvar_Register$)", v[0])}
    # g = Register("name", ...): the name is the first argument, the result goes into a global
    votes, shaped, nargs, sites = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    for s, (callee, args, regs, store) in ctx.items():
        sites[callee] += 1
        if store is None or not img.is_data(store) or not args or args[0] not in t.str_at:
            continue
        v = t.str_at[args[0]]
        if VAR_RE.match(v):
            votes[callee] += 1
            nargs[callee] += len(args)
            if VAR_SHAPE.match(v):
                shaped[callee] += 1
    found = {c for c, n in votes.items() if n >= 20 and shaped[c] >= 0.6 * n and nargs[c] >= 2.5 * n}
    regs_ = known | found
    stats["var_register_functions"] = len(regs_)
    for c in sorted(found - known, key=str):
        if isinstance(c, int):
            names.add(c, f"{prefix.capitalize()}_Register_{c:08X}", "function", prefix,
                      f"{votes[c]} calls as g = f(\"name\", ...) with variable-shaped names")
    for s, (callee, args, regs, store) in sorted(ctx.items()):
        if callee not in regs_ or store is None or not args or args[0] not in t.str_at:
            continue
        v = t.str_at[args[0]]
        if VAR_RE.match(v) and names.add_global(store, f"{prefix}_{v}", prefix, f'{prefix} "{v}" registered at {s:08X}'):
            stats[prefix] += 1
    # game modules: Cvar_Register(&vmCvar, "name", "default", flags) through the engine
    v2, s2 = collections.Counter(), collections.Counter()
    for s, (callee, args, regs, store) in ctx.items():
        if len(args) >= 3 and args[0] is not None and img.is_data(args[0]) and args[1] in t.str_at \
                and VAR_RE.match(t.str_at[args[1]]) and args[2] in t.str_at:
            v2[callee] += 1
    reg2 = {c for c, n in v2.items() if n >= 8}
    for s, (callee, args, regs, store) in sorted(ctx.items()):
        if callee in reg2 and len(args) >= 3 and args[1] in t.str_at and args[0] is not None and img.is_data(args[0]):
            v = t.str_at[args[1]]
            if VAR_RE.match(v) and names.add_global(args[0], f"{prefix}_{v}", prefix, f'vmCvar_t of "{v}" registered at {s:08X}'):
                stats[prefix + "_vm"] += 1
    # tables of { &vmCvar, "name", "default", flags, ... }
    for sname, lo, hi, ch in img.secs:
        if ch & 0x20000000 or sname in (".rsrc", ".reloc") or not sname.startswith(".data"):
            continue
        raw = img.bytes_at(lo, hi - lo)
        n = len(raw) // 4
        words = struct.unpack("<%dI" % n, raw[:n * 4])
        best = None
        for stride in (4, 5, 6, 7, 8):
            i = 0
            while i + stride * 8 < n:
                run = 0
                while i + (run + 1) * stride <= n:
                    w = words[i + run * stride:i + (run + 1) * stride]
                    if not ((w[0] == 0 or img.is_data(w[0])) and w[1] in t.str_at and VAR_RE.match(t.str_at[w[1]] or "")
                            and img.is_data(w[2]) and (w[2] in t.str_at or printable(img.cstr(w[2]))) and w[3] < 0x10000):
                        break
                    run += 1
                if run >= 8 and sum(1 for r in range(run) if words[i + r * stride]) >= run / 2:
                    tbl = lo + i * 4
                    for r in range(run):
                        w = words[i + r * stride:i + (r + 1) * stride]
                        v = t.str_at[w[1]]
                        if w[0] and names.add_global(w[0], f"{prefix}_{v}", prefix, f'vmCvar_t of "{v}" in the table at {tbl:08X}'):
                            stats[prefix + "_table"] += 1
                    names.add_global(tbl, f"{prefix}Table_{tbl:08X}", prefix, f"{run} entries of (&vmCvar, name, default, flags), stride {stride * 4}")
                    i += run * stride
                    best = True
                else:
                    i += 1
            if best:
                break


# ---------------------------------------------------------------- strings shared with a reference source

def load_ref(path):
    rows = read_tsv(path)
    str_names = collections.defaultdict(set)
    name_strs = collections.defaultdict(set)
    calls = collections.defaultdict(set)
    for r in rows:
        nm = r["name"]
        for s in r["strings"].split("\x1f"):
            s = kis_unesc(s)
            if s:
                str_names[s].add(nm)
                name_strs[nm].add(s)
        calls[nm].update(c for c in r["calls"].split(",") if c)
    return str_names, name_strs, calls


def pass_ref_strings(names, t, path, label, stats, min_len=8):
    """A string used by exactly one function here and exactly one function of the reference
    names it; the reference function's other strings that exist here must mostly be used by
    this function too, and the name must not be claimed by two functions here."""
    str_names, name_strs, _ = load_ref(path)
    votes = collections.defaultdict(collections.Counter)
    for s, nms in str_names.items():
        if len(s) < min_len or len(nms) != 1:
            continue
        here = t.str_funcs.get(s)
        if not here or len(here) != 1:
            continue
        votes[next(iter(here))][next(iter(nms))] += 1
    cand = collections.defaultdict(list)  # reference name -> [(votes, function, used, present)]
    for f, c in votes.items():
        nm, n = c.most_common(1)[0]
        if len(c) > 1 and c.most_common(2)[1][1] == n:
            stats[f"{label}_tie"] += 1
            continue
        # both ways: the reference function's strings found here are (nearly) all this function's
        # (else this is a piece of it, inlined there), and most of this function's strings that
        # the reference has at all are that function's (else the name belongs to a caller)
        present = [s for s in name_strs[nm] if s in t.str_funcs]
        used = [s for s in present if f in t.str_funcs[s]]
        mine = [s for s in t.func_strs[f] if s in str_names]
        theirs = [s for s in mine if nm in str_names[s]]
        if len(used) < 0.8 * len(present) or len(theirs) < 0.5 * len(mine):
            stats[f"{label}_disagree"] += 1
            continue
        cand[nm].append((n, f, len(used), len(present)))
    for nm, lst in sorted(cand.items()):
        lst.sort(reverse=True)
        if len(lst) > 1 and lst[1][0] == lst[0][0]:
            stats[f"{label}_ambiguous"] += 1
            continue
        n, f, used, present = lst[0]
        if names.add(f, nm, "function", label, f"{n} unique string(s) shared with {label} {nm}; uses {used} of its {present} strings found here"):
            stats[label] += 1


def pass_ref_graph(names, t, path, label, stats, is_library):
    """Names from the reference's call graph, only where the callees agree: a named function
    whose every named callee is also called by its reference counterpart, with exactly one
    unnamed (game) callee here and exactly one callee left over there. The candidate's own named
    callees must all be callees of that reference function too. Repeats while it finds names."""
    _, _, raw_calls = load_ref(path)
    # decompiler casts and helper macros look like calls ("(int)(x)", "LOBYTE(x)"); C++ methods
    # ("Class__Method") are the reference port's own code, not this build's
    junk = {"int", "char", "float", "double", "unsigned", "signed", "short", "long", "void", "bool", "return"}

    def real(m):
        return m not in junk and "__" not in m and not m.isupper() and not m.startswith("_")
    ref_calls = {n: {m for m in c if real(m)} for n, c in raw_calls.items() if real(n)}
    ref_defined = set(ref_calls)
    # a function named here by an earlier pass that the reference's strings name differently
    # ("also mac: X") is X over there
    alias = {}
    for a, lst in names.also.items():
        for x in lst:
            if x.startswith(label + ": "):
                alias[a] = x.split(": ", 1)[1]

    def fname(a):
        return alias.get(a) or names.func_name(a)

    def fits(g, m):
        return ({fname(x) for x in t.callees[g]} - {None}) <= ref_calls.get(m, set())

    for _ in range(10):
        made = 0
        for f in list(names.by_addr):
            n = fname(f)
            if not n or n not in ref_calls:
                continue
            open_ = [g for g in t.callees[f] if g not in names.by_addr and not is_library(g)]
            if len(open_) != 1:
                continue
            named_here = {fname(g) for g in t.callees[f]} - {None}
            if not named_here <= ref_calls[n]:
                continue
            # the same number of (game) callees on both sides
            mine = {g for g in t.callees[f] if not is_library(g)}
            if len(mine) != len(ref_calls[n] & ref_defined):
                continue
            left = [m for m in ref_calls[n] - named_here if m in ref_defined]
            if len(left) == 1 and left[0] not in names.by_name and left[0] not in alias.values() and fits(open_[0], left[0]) and names.add(
                    open_[0], left[0], "function", label, f"call graph: the one unnamed callee of {n}, whose {label} counterpart calls {left[0]}"):
                made += 1
        for g in t.entries:
            if g in names.by_addr or is_library(g) or len(t.callers[g]) < 2:
                continue
            cands = None
            for f in t.callers[g]:
                n = fname(f)
                if not n or n not in ref_calls:
                    cands = None
                    break
                named_here = {fname(x) for x in t.callees[f]} - {None}
                mine = {x for x in t.callees[f] if not is_library(x)}
                if not named_here <= ref_calls[n] or len(mine) != len(ref_calls[n] & ref_defined):
                    cands = None
                    break
                c = {m for m in ref_calls[n] - named_here if m in ref_defined and m not in names.by_name and m not in alias.values()}
                cands = c if cands is None else cands & c
            if cands is not None and len(cands) == 1:
                m = next(iter(cands))
                if fits(g, m) and names.add(g, m, "function", label, f"call graph: the one {label} callee shared by its {len(t.callers[g])} named callers"):
                    made += 1
        stats[f"{label}-graph"] += made
        if made == 0:
            break


# ---------------------------------------------------------------- tables of { "name", handler }

def pass_tables(names, t, img, table_paths, stats):
    # (source, file) -> lower script name -> C names (a file can hold several tables that share
    # names, like the menu and item keyword tables: the table's own prefix picks between them)
    ref = collections.defaultdict(lambda: collections.defaultdict(list))
    for p in table_paths:
        src = os.path.basename(p).replace("_tables.tsv", "")
        for r in read_tsv(p):
            if IDENT_RE.match(r["function"]) and not r["function"].startswith(("qfalse", "qtrue", "NULL")):
                lst = ref[(src, r["file"])][r["string"].lower()]
                if r["function"] not in lst:
                    lst.append(r["function"])

    def ident(a):
        s = t.str_at.get(a)
        if s is None and img.is_data(a):
            s = img.cstr(a, 64)
        return s if s and IDENT_RE.match(s) else None

    for sname, lo, hi, ch in img.secs:
        if ch & 0x20000000 or sname in (".rsrc", ".reloc"):
            continue
        raw = img.bytes_at(lo, hi - lo)
        n = len(raw) // 4
        words = struct.unpack("<%dI" % n, raw[:n * 4])
        i = 0
        while i < n - 1:
            hit = None
            for stride in (2, 3, 4):
                run, j = 0, i
                while j + 1 < n and img.is_code(words[j + 1]) and ident(words[j]):
                    run += 1
                    j += stride
                if run >= 6:
                    hit = (stride, run)
                    break
            if not hit:
                i += 1
                continue
            stride, run = hit
            tbl = lo + i * 4
            entries = [(ident(words[i + k * stride]), words[i + k * stride + 1]) for k in range(run)]
            low = [e[0].lower() for e in entries]
            src, hits = None, 0
            for key, m in ref.items():
                h = sum(1 for s in low if s in m)
                if h > hits:
                    src, hits = key, h
            if src is None or hits < 3 or hits < 0.3 * run:
                stats["table_unmatched"] += 1
                i += run * stride
                continue
            m = ref[src]
            pre = collections.Counter(p for s in low if s in m for p in {c.split("_", 1)[0] + "_" for c in m[s] if "_" in c})
            prefix = pre.most_common(1)[0][0] if pre else None
            names.add_global(tbl, f"table_{tbl:08X}", "table", f"{run} (name, handler) entries, stride {stride * 4}; like {src[0]} {src[1]}")
            for sn, fn in entries:
                cands = m.get(sn.lower(), [])
                nm = next((c for c in cands if prefix and c.startswith(prefix)), cands[0] if cands else None)
                ev = f'handler of "{sn}" in the table at {tbl:08X} ({src[0]} {os.path.basename(src[1])}'
                if nm is None:
                    if not prefix:
                        continue
                    nm = prefix + sn
                    ev += ", name made from the table's prefix"
                if names.add(fn, nm, "function", "table", ev + ")"):
                    stats["table"] += 1
            i += run * stride


# ---------------------------------------------------------------- match (code shape against another build)

def normalize(ins, img):
    """Mnemonics and operand shapes with addresses and immediates masked."""
    toks = []
    for a, size, mn, op in ins:
        o = re.sub(r"0x[0-9a-f]{5,}", "A", op)
        o = re.sub(r"\b0x[0-9a-f]+\b|\b\d+\b", "N", o)
        toks.append(mn + " " + o)
    return toks


class Shapes:
    def __init__(self, t, img, names):
        self.t, self.img, self.names = t, img, names
        self.cache = {}

    def get(self, f):
        if f not in self.cache:
            ins = self.img.insns(self.t.ranges.get(f, []))
            toks = normalize(ins, self.img)
            mn = [x.split(" ", 1)[0] for x in toks]
            self.cache[f] = (hashlib.sha1("\n".join(toks).encode()).hexdigest(), hashlib.sha1(" ".join(mn).encode()).hexdigest(), toks, mn)
        return self.cache[f]


def pass_match(names, t, img, refdir, stats):
    """Name functions after the named functions of another build (refdir: its work dir with
    raw/ and names.tsv, and the executable path in exe.txt)."""
    rt = Tables(os.path.join(refdir, "raw"))
    rimg = Image(open(os.path.join(refdir, "exe.txt"), encoding="utf-8").read().strip())
    rnames = {int(r["addr"], 16): r["name"] for r in read_tsv(os.path.join(refdir, "names.tsv")) if r["kind"] == "function"}
    S, R = Shapes(t, img, names), Shapes(rt, rimg, None)
    lib = lambda f: f & 0xF != 0  # noqa: E731  game code is 16-byte aligned; the C runtime is not

    def strs(tt, f):
        return frozenset(s for s in tt.func_strs.get(f, ()) if len(s) >= 4)

    def cnames_here(f):
        return collections.Counter(callee_name(names, t, c) for _, c in t.call_seq.get(f, []) if isinstance(c, int) and callee_name(names, t, c))

    def cnames_ref(g):
        return collections.Counter(rnames.get(c) or rt.funcs.get(c, {}).get("name") for _, c in rt.call_seq.get(g, [])
                                   if isinstance(c, int) and (rnames.get(c) or (rt.funcs.get(c) and rt.funcs[c]["source"] != "DEFAULT")))

    def agree(f, g):
        """Strings and named callees must not contradict, and at least one of them must agree."""
        sf, sg = strs(t, f), strs(rt, g)
        cf, cg = cnames_here(f), cnames_ref(g)
        s_ok = (sf or sg) and len(sf & sg) >= 0.8 * max(len(sf), len(sg))
        s_bad = (sf or sg) and not s_ok
        common = set(cf) & set(cg)
        c_ok = bool(common) and all(cf[k] == cg[k] for k in common) and not (set(cf) - set(cg))
        c_bad = bool(set(cf) - set(cg))
        return (s_ok or c_ok) and not s_bad and not c_bad, ("strings" if s_ok else "") + ("+" if s_ok and c_ok else "") + ("callees" if c_ok else "")

    def add(f, g, how):
        nm = rnames[g]
        if names.add(f, nm, "function", "match", f"{how} {nm} at {g:08X} in {os.path.basename(refdir)}"):
            stats["match"] += 1
            return True
        return False

    for rnd in range(6):
        made = 0
        # 1. same shape (masked instruction text), unique on both sides
        by_h, by_hr = collections.defaultdict(list), collections.defaultdict(list)
        for f in t.entries:
            if f not in names.by_addr and not lib(f):
                by_h[S.get(f)[0]].append(f)
        for g in rnames:
            if g in rt.funcs and not lib(g):
                by_hr[R.get(g)[0]].append(g)
        for h, fs in by_h.items():
            gs = by_hr.get(h)
            if len(fs) == 1 and gs and len(gs) == 1 and len(S.get(fs[0])[2]) >= 6:
                ok, why = agree(fs[0], gs[0])
                if ok and add(fs[0], gs[0], f"same code ({len(S.get(fs[0])[2])} instructions; {why} agree) as"):
                    made += 1
        # 2. the same string set (unique on both sides) and similar code
        by_s, by_sr = collections.defaultdict(list), collections.defaultdict(list)
        for f in t.entries:
            s = strs(t, f)
            if s and f not in names.by_addr:
                by_s[s].append(f)
        for g in rnames:
            s = strs(rt, g)
            if s:
                by_sr[s].append(g)
        for s, fs in by_s.items():
            gs = by_sr.get(s)
            if len(fs) != 1 or not gs or len(gs) != 1 or fs[0] in names.by_addr:
                continue
            f, g = fs[0], gs[0]
            r = difflib.SequenceMatcher(None, S.get(f)[3], R.get(g)[3], autojunk=False).ratio()
            if r >= 0.6 and agree(f, g)[0] and add(f, g, f"same {len(s)} string(s), code {r:.0%} alike, as"):
                made += 1
        # 3. callees in step: in a pair of functions with the same instruction sequence, the n-th
        #    call goes to the same function on both sides
        raddr = {}
        for g, n2 in rnames.items():
            raddr.setdefault(n2, g)
        pairs = [(a, raddr[v[0]]) for a, v in list(names.by_addr.items()) if v[1] == "function" and v[0] in raddr]
        for f, g in pairs:
            if f not in t.ranges or g not in rt.ranges or S.get(f)[1] != R.get(g)[1]:
                continue
            cf = [c for _, c in t.call_seq.get(f, [])]
            cg = [c for _, c in rt.call_seq.get(g, [])]
            if len(cf) != len(cg):
                continue
            for a, b in zip(cf, cg):
                if not isinstance(a, int) or not isinstance(b, int) or a in names.by_addr or b not in rnames:
                    continue
                if a not in t.funcs or b not in rt.funcs or lib(a):
                    continue
                ra = difflib.SequenceMatcher(None, S.get(a)[3], R.get(b)[3], autojunk=False).ratio()
                if ra >= 0.75 and agree(a, b)[0] and add(a, b, f"called in step from {rnames[g]} (code {ra:.0%} alike) as"):
                    made += 1
        stats[f"match_round{rnd + 1}"] = made
        if made == 0:
            break


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--var-prefix", default="dvar", choices=["dvar", "cvar"])
    ap.add_argument("--cod2x")
    ap.add_argument("--q3")
    ap.add_argument("--match", nargs="*", default=[], help="work dirs of runs to match against, best first")
    ap.add_argument("--mac")
    ap.add_argument("--tables", nargs="*", default=[])
    args = ap.parse_args()

    img = Image(args.exe)
    t = Tables(args.raw)
    names = Names()
    stats = collections.Counter()
    if args.cod2x:
        pass_cod2x(names, t, img, args.cod2x, stats)
    pass_selfname(names, t, stats)
    ctx = site_contexts(t, img)
    pass_cmd(names, t, img, ctx, stats)
    pass_vars(names, t, img, ctx, args.var_prefix, stats)
    if args.q3:
        pass_ref_strings(names, t, args.q3, "q3", stats)
    for ref in args.match:
        pass_match(names, t, img, ref, stats)
    if args.mac:
        pass_ref_strings(names, t, args.mac, "mac", stats)
        pass_ref_graph(names, t, args.mac, "mac", stats, is_library=lambda a: a & 0xF != 0)
    if args.tables:
        pass_tables(names, t, img, args.tables, stats)
    names.write(args.out)
    nf = sum(1 for v in names.by_addr.values() if v[1] == "function")
    print(f"{os.path.basename(args.exe)}: {len(t.funcs)} functions; named {nf} functions and {len(names.by_addr) - nf} globals")
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}")
    for (a, b, how), v in sorted(names.agree.items()):
        print(f"  {b} on a {a} name: {how} {v}")


if __name__ == "__main__":
    sys.exit(main())
