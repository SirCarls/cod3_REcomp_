#!/usr/bin/env python3
"""
zone_loader.py - read World at War PC's own fastfile (zone) loader and write down what it loads.

The game's zone loader is generated code: one function per structure, all working through
"var" globals (varGfxWorld points at the GfxWorld being loaded) and a few stream helpers
(Load_Stream, DB_PushStreamPos, Load_XStringCustom...). Starting at the asset type switch, this
tool follows which var each function works on and which var it sets before calling the next one,
and takes the structure layouts from a C header with "//OFS: 0x.. SIZE: 0x.." member comments
(t4sp_types.py's output; private use only). That gives every loader function its structure
(Load_GfxWorld, Load_GfxCellArray, Mark_XModelPtr...) and every var its type, and, for each
structure, the order in which the stream fills it: which pointer members are followed, how many
elements (which count member, times which element size), at what alignment, in which block.
Custom maps are fastfiles from the same mod tools, so a reader that follows these programs reads
them too.

Usage:
    python3 tools/waw_decomp/zone_loader.py --exe CoDWaW.exe --raw <export dir> --header <types.h> \\
        [--names <names.tsv>] [--kisak-db <KisakCOD src/database>] [--names-out names.tsv] [--schema-out schema.txt]

--names: the build's names.tsv; functions it already names otherwise (DB_FindXAssetHeader) are not
loaders. --names-out: rows (addr, kind, name, type, evidence) for name_pc.py --extra (function names) and
WawApplyTypes.java (var globals with their pointer types). --schema-out: one block per loader
function, its structure and its load program. --kisak-db: KisakCOD's database folder; where it
has a function of the same name shape (Load_MaterialHandle rather than Load_MaterialPtr), that
name is used.

The addresses below are World at War PC 1.7 singleplayer's (CoDWaW.exe, timestamp 2009-10-29),
found by hand in its code; the tool checks the bytes at the entry points first and stops on any
other build. Static reading only: nothing is run.

Needs: capstone, pefile (pip).
"""
import argparse
import collections
import csv
import os
import re
import sys

import capstone
import pefile
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

# --- World at War PC 1.7 singleplayer --------------------------------------------------------
BUILD_CHECK = {  # address: first bytes, to make sure this is the build the addresses are for
    0x48C960: bytes.fromhex("51a164759500"),        # Load_XAssetHeader: push ecx; mov eax, [varXAsset]
    0x490210: bytes.fromhex("8b0ddc50ad00"),        # DB_PushStreamPos: mov ecx, [g_streamPosStackIndex]
}
POS = 0xB548BC              # g_streamPos
LOADDATA = 0x47B0E0         # DB_LoadXFileData(eax = destination, ecx = size)
PUSH, POP = 0x490210, 0x490280
LOADSTREAM = 0x490310       # Load_Stream(eax = destination, esi = size), when not inlined
XSTRING = 0x4903B0
INSERTPTR, ADDASSET, GETASSET = 0x4902E0, 0x48E300, 0x48E370
LOAD_ROOT, MARK_ROOT = 0x48C960, 0x48CC10
LOADER_RANGE = (0x478000, 0x492000)
CLIPMAP_ROOT = (0x957518, 0x483C90)  # types 11 and 12 jump to one block (0x48CBF5)
ASSET_STRUCT = ['XModelPieces', 'PhysPreset', 'PhysConstraints', 'DestructibleDef', 'XAnimParts', 'XModel',
                'Material', 'MaterialTechniqueSet', 'GfxImage', 'snd_alias_list_t', 'LoadedSound', 'clipMap_t',
                'clipMap_t', 'ComWorld', 'GameWorldSp', 'GameWorldMp', 'MapEnts', 'GfxWorld', 'GfxLightDef', None,
                'Font_s', 'MenuList', 'menuDef_t', 'LocalizeEntry', 'WeaponDef', 'SndDriverGlobals', 'FxEffectDef',
                'FxImpactTable', None, None, None, None, 'RawFile', 'StringTable', 'PackIndex']
HELPERS = {
    PUSH: ('DB_PushStreamPos', 'pushes the stream position stack and switches to block eax (KisakCOD DB_PushStreamPos)'),
    POP: ('DB_PopStreamPos', 'pops the stream position stack (KisakCOD DB_PopStreamPos)'),
    LOADSTREAM: ('Load_Stream', 'reads esi bytes to eax unless the current block is the runtime one (zeroed) or a'
                                ' delayed one (KisakCOD Load_Stream)'),
    XSTRING: ('Load_XStringCustom', 'reads a NUL-terminated string at the stream position (KisakCOD Load_XStringCustom)'),
    INSERTPTR: ('DB_InsertPointer', 'reserves an aligned pointer slot in block 4 (KisakCOD DB_InsertPointer)'),
    ADDASSET: ('DB_AddXAsset', 'registers a loaded asset (type in eax) through DB_LinkXAssetEntry (KisakCOD DB_AddXAsset)'),
    GETASSET: ('DB_GetXAsset', 'finds the entry of (type esi, header) in db_hashTable and sets its inuse byte'
                               ' (KisakCOD DB_GetXAsset)'),
    LOAD_ROOT: ('Load_XAssetHeader', 'the asset type switch of the zone loader: var<T>Ptr = varXAssetHeader, then'
                                     ' the loader of each type'),
    MARK_ROOT: ('Mark_XAssetHeader', 'the same switch for the mark pass'),
    0x47B1F0: ('Load_XAssetListCustom', 'reads the 16-byte XAssetList and its script strings (KisakCOD)'),
    0x47B260: ('Load_XAssetArrayCustom', 'reads count * 8 bytes of XAsset entries and loads each (KisakCOD)'),
}
GLOBALS = {
    POS: ('g_streamPos', 'unsigned char*'), 0xBF008C: ('g_streamPosIndex', 'unsigned int'),
    0xB25AB4: ('g_streamBlocks', 'XBlock*'), 0xB54BE4: ('g_streamPosArray', ''), 0xD85680: ('g_streamDelayArray', ''),
    0xAEA264: ('g_streamDelayIndex', 'unsigned int'), 0xD8D678: ('g_streamPosStack', ''),
    0xAD50DC: ('g_streamPosStackIndex', 'unsigned int'), 0x957564: ('varXAsset', 'XAsset*'),
    0x9575E8: ('varXAssetHeader', 'XAssetHeader*'), 0x9575FC: ('varXAssetList', 'XAssetList*'),
    0x9573F0: ('g_varXAssetList', 'XAssetList'),
}
PRIM = {'unsigned __int16': 'ushort', 'unsigned __int8': 'byte', 'char': 'char', 'int': 'int', 'unsigned int': 'uint',
        'float': 'float', '__int16': 'short', 'unsigned short': 'ushort', 'short': 'short', 'unsigned char': 'byte',
        'bool': 'bool'}
CTYPE = {'ushort': 'unsigned short', 'byte': 'unsigned char', 'char': 'char', 'int': 'int', 'uint': 'unsigned int',
         'float': 'float', 'short': 'short', 'bool': 'bool'}
FULL = {'al': 'eax', 'ax': 'eax', 'bl': 'ebx', 'bx': 'ebx', 'cl': 'ecx', 'cx': 'ecx', 'dl': 'edx', 'dx': 'edx',
        'si': 'esi', 'di': 'edi', 'bp': 'ebp'}


# --- the header ------------------------------------------------------------------------------
MEMBER_RE = re.compile(r"^\s*(?:const\s+)?(?:struct\s+|union\s+|enum\s+)?([A-Za-z_][\w ]*?)\s*((?:\*\s*(?:const\s*)?)*)"
                       r"\s*([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)\s*;\s*//OFS: 0x([0-9A-Fa-f]+) SIZE: 0x([0-9A-Fa-f]+)")


class Header:
    def __init__(self, path):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        self.structs = {}
        for m in re.finditer(r"\n\s*(struct|union) (?:__declspec\(align\(\d+\)\) )?(\w+)\s*\n\s*\{(.*?)\n\s*\};",
                             text, re.S):
            kind, name, body = m.groups()
            depth, members = 0, []
            for line in body.split("\n"):
                if depth == 0:
                    mm = MEMBER_RE.match(line)
                    if mm:
                        t, stars, n, dims, o, s = mm.groups()
                        count = 1
                        for d in re.findall(r"\[([^\]]*)\]", dims):
                            try:
                                count *= int(d, 0)
                            except ValueError:
                                count = None
                                break
                        members.append({"off": 0 if kind == "union" else int(o, 16), "size": int(s, 16),
                                        "type": t.strip(), "ptr": stars.count("*"), "name": n, "count": count})
                depth += line.count("{") - line.count("}")
            self.structs[name] = members
        self.typedefs = dict(re.findall(r"typedef\s+(?:struct|union)\s+(\w+)\s+(\w+)\s*;", text))
        self.size = {n: max(m["off"] + m["size"] for m in ms) for n, ms in self.structs.items() if ms}

    def base(self, t):
        t = t.replace("const ", "").strip()
        return t if t in self.structs else self.typedefs.get(t, t)

    def member_at(self, t, off):
        best = None
        for m in self.structs.get(t, []):
            if m["off"] <= off < m["off"] + max(m["size"], 1):
                best = m
        return best

    def path(self, t, off, depth=0):
        """'dpvsPlanes.cellCount' for an offset inside inline members."""
        m = self.member_at(t, off) if t else None
        if not m:
            return f"+0x{off:X}"
        inner, ty = off - m["off"], self.base(m["type"])
        if m["ptr"] == 0 and ty in self.structs and depth < 4 and inner:
            es = self.size.get(ty) or 1
            if m["count"] and m["count"] > 1:
                idx, inner = divmod(inner, es)
                return f"{m['name']}[{idx}].{self.path(ty, inner, depth + 1)}"
            return f"{m['name']}.{self.path(ty, inner, depth + 1)}"
        if inner and m["count"] and m["count"] > 1:
            return f"{m['name']}[{inner // max(m['size'] // m['count'], 1)}]"
        return m["name"]


# --- events: a linear register-value walk of one function ------------------------------------
class Code:
    def __init__(self, exe, raw):
        pe = pefile.PE(exe, fast_load=True)
        self.base, self.img = pe.OPTIONAL_HEADER.ImageBase, pe.get_memory_mapped_image()
        for a, b in BUILD_CHECK.items():
            if self.img[a - self.base:a - self.base + len(b)] != b:
                sys.exit(f"{exe}: not World at War PC 1.7 singleplayer (bytes at {a:08X} differ)")
        with open(os.path.join(raw, "functions.tsv"), encoding="utf-8", newline="") as f:
            self.funcs = {int(r["entry"], 16): int(r["size"])
                          for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE)}
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.cache = {}

    def disasm(self, a, n):
        return self.md.disasm(self.img[a - self.base:a - self.base + n], a)

    def events(self, fa):
        """Value kinds: ('const', n), ('pos',), ('glob', a) = contents of [a], ('fieldval', v, off),
        ('add', v, n), ('and', v, mask), ('mul', v, k). Events: call, setglob, setfield, setpos, push."""
        if fa in self.cache:
            return self.cache[fa]
        regs, ev = {}, []

        def reg(i, r):
            n = i.reg_name(r)
            return FULL.get(n, n)

        for i in self.disasm(fa, self.funcs.get(fa, 0)):
            ops, mn = i.operands, i.mnemonic

            def val(op):
                if op.type == X86_OP_IMM:
                    return ("const", op.imm & 0xFFFFFFFF)
                if op.type == X86_OP_REG:
                    return regs.get(reg(i, op.reg))
                if op.type == X86_OP_MEM and op.mem.index == 0:
                    d = op.mem.disp & 0xFFFFFFFF
                    if op.mem.base == 0:
                        return ("pos",) if d == POS else ("glob", d)
                    bv = regs.get(reg(i, op.mem.base))
                    return ("fieldval", bv, op.mem.disp) if bv else None
                return None

            def call_event(target):
                ev.append(("call", target, i.address, {r: regs.get(r) for r in ("eax", "ecx", "edx", "esi", "edi")}))

            if mn == "call":
                call_event(ops[0].imm & 0xFFFFFFFF if ops[0].type == X86_OP_IMM else None)
                for r in ("eax", "ecx", "edx"):
                    regs.pop(r, None)
                continue
            if mn == "jmp" and ops and ops[0].type == X86_OP_IMM:   # tail call
                t = ops[0].imm & 0xFFFFFFFF
                if t in self.funcs and t != fa:
                    call_event(t)
                continue
            if mn in ("mov", "movzx", "movsx") and len(ops) == 2:
                d, s = ops
                v = val(s)
                if d.type == X86_OP_REG:
                    regs[reg(i, d.reg)] = v
                elif d.type == X86_OP_MEM and d.mem.index == 0:
                    if d.mem.base == 0:
                        a = d.mem.disp & 0xFFFFFFFF
                        ev.append(("setpos", v, i.address) if a == POS else ("setglob", a, v, i.address))
                    else:
                        ev.append(("setfield", regs.get(reg(i, d.mem.base)), d.mem.disp, v, i.address))
                continue
            if mn == "lea" and ops[0].type == X86_OP_REG:
                m, r = ops[1].mem, reg(i, ops[0].reg)
                if m.index and m.base and m.index == m.base and m.disp == 0:
                    regs[r] = ("mul", regs.get(reg(i, m.base)), m.scale + 1)
                elif m.index and not m.base and m.disp == 0:
                    regs[r] = ("mul", regs.get(reg(i, m.index)), m.scale)
                elif not m.index and m.base:
                    bv = regs.get(reg(i, m.base))
                    regs[r] = ("add", bv, m.disp) if bv else None
                else:
                    regs[r] = None
                continue
            if mn in ("add", "and", "shl", "imul") and ops and ops[0].type == X86_OP_REG:
                r = reg(i, ops[0].reg)
                v = regs.get(r)
                if mn == "add" and ops[1].type == X86_OP_IMM:
                    regs[r] = ("add", v, ops[1].imm)
                    continue
                if mn == "and" and ops[1].type == X86_OP_IMM:
                    regs[r] = ("and", v, ops[1].imm & 0xFFFFFFFF)
                    continue
                k = 2 if (mn == "add" and ops[1].type == X86_OP_REG and ops[1].reg == ops[0].reg) else \
                    (1 << ops[1].imm) if (mn == "shl" and ops[1].type == X86_OP_IMM) else None
                if k:
                    regs[r] = ("mul", v[1], v[2] * k) if v and v[0] == "mul" else ("mul", v, k)
                    continue
                if mn == "imul" and len(ops) == 3 and ops[2].type == X86_OP_IMM:
                    regs[r] = ("mul", val(ops[1]), ops[2].imm)
                    continue
            if mn == "push":
                ev.append(("push", val(ops[0]) if ops else None, i.address))
                continue
            if mn in ("cmp", "test", "jmp", "ret", "pop", "nop") or mn.startswith(("j", "set")):
                continue
            try:
                for r in i.regs_access()[1]:
                    regs.pop(reg(i, r), None)
            except capstone.CsError:
                regs.clear()
        self.cache[fa] = ev
        return ev

    def root(self):
        """Load_XAssetHeader's if-chain: type -> (var<T>Ptr, Load_<T>Ptr)."""
        out, cur, w = {}, None, None
        for i in self.disasm(LOAD_ROOT, 0x2A0):
            m = re.match(r"eax, (0x[0-9a-f]+|\d+)$", i.op_str)
            if i.mnemonic == "cmp" and m:
                cur = int(m.group(1), 0)
            if i.mnemonic == "mov":
                mm = re.match(r"dword ptr \[(0x[0-9a-f]+)\], e[a-z]x$", i.op_str)
                if mm:
                    w = int(mm.group(1), 16)
            if i.mnemonic == "call" and cur is not None:
                out[cur] = (w, int(i.op_str, 16))
            if i.mnemonic == "ret" and i.address > LOAD_ROOT + 0x290:
                break
        out[11] = out[12] = CLIPMAP_ROOT
        return out


# --- following the vars ----------------------------------------------------------------------
def in_loader(a):
    return LOADER_RANGE[0] <= a < LOADER_RANGE[1]


def propagate(code, hdr, skip=frozenset()):
    """skip: functions that are not structure loaders (named otherwise, such as DB_FindXAssetHeader)."""
    vartype, functype, queue = {}, {}, []
    skip = set(skip) | {LOADDATA}
    for typ, (w, f) in code.root().items():
        st = ASSET_STRUCT[typ] if typ < len(ASSET_STRUCT) else None
        if st:
            vartype[w] = (st, 2)
            functype[f] = (st, 2)
            queue.append(f)
    queue.append(MARK_ROOT)
    functype[MARK_ROOT] = ("XAsset", 0)
    done = set()
    while queue:
        f = queue.pop(0)
        if f in done:
            continue
        done.add(f)
        t, d = functype[f]
        lastglob, lastfield = {}, {}
        for e in code.events(f):
            if e[0] == "setglob":
                lastglob[e[1]] = e[2]
            if e[0] == "setfield" and e[1] and e[1][0] == "glob":
                lastfield[e[1][1]] = (e[2], e[3])
            if e[0] != "call" or e[1] in HELPERS or e[1] in skip or e[1] is None or not in_loader(e[1]) or not lastglob:
                continue
            g = e[1]
            w, v = list(lastglob.items())[-1]
            lastglob = {}
            nt = vartype.get(w) or child_type(hdr, vartype, w, v, lastfield, (t, d))
            if nt:
                vartype.setdefault(w, nt)
                if g not in functype:
                    functype[g] = nt
                    queue.append(g)
    return vartype, functype


def child_type(hdr, vartype, w, v, lastfield, own):
    """The type of var w, set to v before a call."""
    def member_type(base_var, off):
        if base_var in vartype and vartype[base_var][1] == 1:
            m = hdr.member_at(vartype[base_var][0], off)
            return m
        return None

    if v and v[0] == "fieldval" and v[1] and v[1][0] in ("glob", "add"):   # w = parent->pointer member
        base, off = v[1], v[2]
        if base[0] == "add" and base[1] and base[1][0] == "glob":
            off, base = base[2] + off, base[1]
        m = member_type(base[1], off) if base[0] == "glob" else None
        if m and m["ptr"] >= 1:
            return hdr.base(m["type"]), 1 if m["ptr"] == 1 else 2
    if v and v[0] == "add" and v[1] and v[1][0] == "glob" and v[1][1] in vartype:   # w = &parent->member
        m = member_type(v[1][1], v[2])
        if m:
            return hdr.base(m["type"]), 2 if m["ptr"] >= 1 else 1
    if v and (v == ("pos",) or v[0] == "and"):   # w = data just placed at the stream position
        cand = [(b, o) for b, (o, x) in lastfield.items() if b in vartype and x and (x == ("pos",) or x[0] == "and")]
        if cand:
            b, o = cand[-1]
            bt, bd = vartype[b]
            if bd == 1:
                m = hdr.member_at(bt, o)
                if m and m["ptr"] >= 1:
                    return hdr.base(m["type"]), 1 if m["ptr"] == 1 else 2
            elif bd == 2:
                return bt, 1
        elif own[1] == 2:
            return own[0], 1
    if v and v[0] == "glob" and v[1] in vartype:
        bt, bd = vartype[v[1]]
        if w != v[1] and bd == 1:   # another var at the start of the same object: its first member
            m = hdr.member_at(bt, 0)
            mt = hdr.base(m["type"]) if m else None
            if m and m["ptr"] == 0 and mt in hdr.structs and hdr.size.get(mt, 0) < hdr.size.get(bt, 0):
                return mt, 1
        return bt, bd
    if own[1] == 1 and (v is None or (v[0] == "glob" and v[1] == w)):
        return own   # the element loop of an array loader
    return None


# --- load programs ---------------------------------------------------------------------------
def field_off(v, subj):
    if v and v[0] == "fieldval":
        if v[1] == ("glob", subj):
            return v[2]
        if v[1] and v[1][0] == "add" and v[1][1] == ("glob", subj):
            return v[1][2] + v[2]
    return None


def count_expr(v, subj):
    if v is None:
        return None
    if v[0] == "const":
        return ("const", v[1])
    o = field_off(v, subj)
    if o is not None:
        return ("field", o, 1)
    if v[0] == "mul":
        o = field_off(v[1], subj)
        if o is not None:
            return ("field", o, v[2])
        if v[1] and v[1][0] == "const":
            return ("const", v[1][1] * v[2])
    return ("expr",)


def program(code, vartype, functype, f):
    t, d = functype[f]
    subj = next((w for w, v in vartype.items() if v == (t, d)), None)
    out, pending, lastglob = [], None, None
    for e in code.events(f):
        k = e[0]
        if k == "setfield" and e[1] == ("glob", subj) and e[3] and (e[3] == ("pos",) or e[3][0] == "and"):
            pending = (e[2], (~e[3][2] & 0xFFFFFFFF) + 1 if e[3][0] == "and" else 1)
            continue
        if k == "setglob":
            lastglob = (e[1], e[2])
            continue
        if k != "call":
            continue
        g, r = e[1], e[3]
        if g == PUSH and r["eax"] and r["eax"][0] == "const":
            out.append(("push", r["eax"][1]))
        elif g == POP:
            out.append(("pop",))
        elif g in (LOADDATA, LOADSTREAM):
            size = r["ecx"] if g == LOADDATA else r["esi"]
            if r["eax"] == ("glob", subj) and not out:
                out.append(("self", count_expr(size, subj)))
            elif pending:
                out.append(("data", pending[0], pending[1], count_expr(size, subj)))
                pending = None
        elif g == XSTRING:
            a = r["eax"]
            off = 0 if a == ("glob", subj) else a[2] if a and a[0] == "add" and a[1] == ("glob", subj) else None
            if off is not None:
                out.append(("string", off))
                pending = None
        elif g in functype and g != f:
            cnt = count_expr(r["eax"], subj)
            if pending:
                out.append(("ptr", pending[0], pending[1], functype[g], cnt, g))
                pending = None
            elif lastglob and lastglob[1] and lastglob[1][0] == "add" and lastglob[1][1] == ("glob", subj):
                out.append(("member", lastglob[1][2], functype[g], cnt, g))
            else:
                out.append(("call", functype[g], cnt, g))
            lastglob = None
    clean, depth = [], 0   # one pop per push (the other return paths pop too)
    for it in out:
        if it[0] == "push":
            depth += 1
        if it[0] == "pop":
            if depth == 0:
                continue
            depth -= 1
        clean.append(it)
    return t, d, clean


def describe(hdr, t, it, names):
    def c(x):
        if x is None:
            return "?"
        if x[0] == "const":
            return str(x[1])
        if x[0] == "field":
            return hdr.path(t, x[1]) + (f" * {x[2]}" if x[2] != 1 else "")
        return "(computed)"

    def ty(x):
        return x[0] + ("*" if x[1] == 2 else "")

    k = it[0]
    if k == "self":
        return f"the structure itself ({c(it[1])} bytes)"
    if k == "push":
        return f"push block {it[1]}"
    if k == "pop":
        return "pop block"
    if k == "string":
        return f"{hdr.path(t, it[1])}: string"
    if k == "data":
        return f"{hdr.path(t, it[1])}: {c(it[3])} bytes, aligned {it[2]}"
    if k == "ptr":
        n = f" x {c(it[4])}" if it[4] and it[4][0] == "field" else ""
        return f"{hdr.path(t, it[1])}: {ty(it[3])}{n}, aligned {it[2]} ({names.get(it[5], f'{it[5]:08X}')})"
    if k == "member":
        n = f" x {c(it[3])}" if it[3] and it[3][0] == "field" else ""
        return f"{hdr.path(t, it[1])} (inside): {ty(it[2])}{n} ({names.get(it[4], f'{it[4]:08X}')})"
    n = f" x {c(it[2])}" if it[2] and it[2][0] == "field" else ""
    return f"{ty(it[1])}{n} ({names.get(it[3], f'{it[3]:08X}')})"


# --- names -----------------------------------------------------------------------------------
def kisak_names(folder):
    out = set()
    if folder:
        for fn in sorted(os.listdir(folder)):
            if fn.endswith(".cpp"):
                with open(os.path.join(folder, fn), encoding="utf-8", errors="replace") as f:
                    out.update(re.findall(r"^\w[\w\s\*]*?\b((?:Load|Mark)_\w+)\s*\(", f.read(), re.M))
    return out


def make_names(code, hdr, vartype, functype, kisak):
    roots = {f for _, (w, f) in code.root().items()}
    counted = collections.Counter()
    progs = {f: program(code, vartype, functype, f) for f in functype if f != MARK_ROOT}
    for f, (t, d, out) in progs.items():
        for it in out:
            cnt = it[4] if it[0] == "ptr" else it[3] if it[0] == "member" else it[2] if it[0] == "call" else None
            if cnt and cnt[0] == "field":
                counted[it[-1]] += 1
    names, why = {}, {}
    for f, (t, d) in functype.items():
        if f == MARK_ROOT or f in HELPERS or t in ("void", None, "XAsset"):
            continue
        load = any(x[0] == "setpos" or (x[0] == "call" and x[1] in (LOADDATA, LOADSTREAM, PUSH, POP, XSTRING))
                   for x in code.events(f))
        role, T = ("Load" if load else "Mark"), PRIM.get(t, t)
        arr = counted[f] > 0 and f not in roots
        if d == 2:
            cands = [f"{role}_{T}{s}{'Array' if arr else ''}" for s in ("Ptr", "Handle", "_ptr")]
        else:
            cands = [f"{role}_{T}{'Array' if arr else ''}"]
        names[f] = next((c for c in cands if c in kisak), cands[0])
        why[f] = (f"{'loads' if load else 'marks'} {t}{'*' if d == 2 else ''}{' arrays (callers pass a count)' if arr else ''};"
                  f" typed by following the loader's var globals from the asset type switch (zone_loader.py)"
                  + ("; KisakCOD has the same name" if names[f] in kisak else ""))
    groups = collections.defaultdict(list)
    for f, n in names.items():
        groups[n].append(f)
    for n, fs in groups.items():   # a shared name: the one calling the other in a loop loads the array
        for a in fs:
            if len(fs) > 1 and not n.endswith("Array") and any(
                    x[0] == "call" and x[1] in fs and x[1] != a for x in code.events(a)) and n + "Array" not in names.values():
                names[a] = n + "Array"
                why[a] += "; calls the single loader of the same type"
    out, seen = {}, collections.Counter()
    for f in sorted(names):
        seen[names[f]] += 1
        out[f] = (names[f] if seen[names[f]] == 1 else f"{names[f]}_{seen[names[f]]}", why[f])
    out.update(HELPERS)
    var_names, seen = {}, collections.Counter()
    for w in sorted(vartype):
        t, d = vartype[w]
        if t in ("void", None):
            continue
        T = PRIM.get(t, t)
        n = f"var{T}" if d == 1 else f"var{T}Ptr"
        seen[n] += 1
        ct = CTYPE.get(T, t if t in hdr.structs else None)
        var_names[w] = (n if seen[n] == 1 else f"{n}_{seen[n]}", (ct + ("*" if d == 1 else "**")) if ct else "")
    var_names.update(GLOBALS)
    return out, var_names, progs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--kisak-db")
    ap.add_argument("--names", help="this build's names.tsv: functions it names (other than Load_/Mark_) are left out")
    ap.add_argument("--names-out")
    ap.add_argument("--schema-out")
    args = ap.parse_args()
    code, hdr = Code(args.exe, args.raw), Header(args.header)
    skip = set()
    if args.names:
        with open(args.names, encoding="utf-8", newline="") as f:
            for r in csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE):
                if r["kind"] == "function" and not r["name"].startswith(("Load_", "Mark_", "FUN_")):
                    skip.add(int(r["addr"], 16))
    vartype, functype = propagate(code, hdr, skip - set(HELPERS))
    names, var_names, progs = make_names(code, hdr, vartype, functype, kisak_names(args.kisak_db))
    if args.names_out:
        with open(args.names_out, "w", encoding="utf-8", newline="") as o:
            o.write("addr\tkind\tname\ttype\tevidence\tpass\n")
            for f, (n, w) in sorted(names.items()):
                o.write(f"{f:08X}\tfunction\t{n}\t\t{w}\tzone\n")
            for a, (n, ty) in sorted(var_names.items()):
                ev = ("the zone loader's current object (functions load through it)" if n.startswith("var")
                      else "zone stream state (KisakCOD db_stream.cpp name; same use in DB_PushStreamPos/Load_Stream)")
                o.write(f"{a:08X}\tglobal\t{n}\t{ty}\t{ev}\tzone\n")
    if args.schema_out:
        fnames = {f: n for f, (n, _) in names.items()}
        with open(args.schema_out, "w", encoding="utf-8") as o:
            o.write("# World at War PC 1.7 SP zone loader: what each loader function reads, in stream order.\n"
                    "# Made by tools/waw_decomp/zone_loader.py from the game's code and the T4SP header.\n\n")
            for f in sorted(progs, key=lambda a: fnames.get(a, "")):
                t, d, out = progs[f]
                o.write(f"{fnames.get(f, f'{f:08X}')} ({f:08X}): {t}{'*' if d == 2 else ''}"
                        f"{f', {hdr.size[t]} bytes' if d == 1 and t in hdr.size else ''}\n")
                for it in out:
                    o.write(f"    {describe(hdr, t, it, fnames)}\n")
                o.write("\n")
    k = collections.Counter(n.split("_")[0] for n, _ in names.values())
    print(f"{len(functype)} loader functions typed, {len(vartype)} vars; names: {dict(k)};"
          f" {sum(1 for n, _ in names.values() if n in kisak_names(args.kisak_db))} as in KisakCOD")
    return 0


if __name__ == "__main__":
    sys.exit(main())
