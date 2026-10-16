#!/usr/bin/env python3
"""Builds a symbol map for Call of Duty 3's default.xex: a name, a confidence
and a line of evidence for every function (and key global) that can be
identified, toward a readable rewrite of the game.

    python3 tools/name_functions.py [image.pe] [--q3-src <Quake-III-Arena checkout>]
                                    [--config default.toml] [--imports image.imports]
                                    [-o symbols.toml] [--ppcx-names image.pe.names]

The image is the decrypted PE from xex_dump.py; everything is read from it
directly (code, strings, data), so no separate strings dump is needed. Function
bounds come from .pdata, the codegen config's [functions] table and every
call/branch target, code pointer and leftover stretch of code between them.
A text listing (--asm, default <image>.asm) is only checked for a match.

Evidence (a lower confidence never replaces a higher one):
  known        names the recompilation project already uses (src/game/addresses.h,
               the hooks, config/default.toml)
  curated      names checked by hand in the disassembly (CURATED_* below), most of
               them Quake III functions recognised by structure
  imports      kernel/XAM import thunks as __imp__<name> like the codegen (xex_imports.py
               listing), BaseSetLastNTError and the one-call XAPI wrappers (CloseHandle...)
  helpers      MSVC's __savegprlr_N / __restgprlr_N / __savefpr_N ... entry points
  self-naming  a string such as "CG_ParseServerinfo: ..." or "CM_LoadMap( %s )"
               used by exactly one function
  q3-strings   strings shared with exactly one Quake III Arena function (--q3-src)
  rtti         MSVC RTTI: vtables named by class, scalar deleting destructors,
               destructors and constructors from vtable stores, every other slot
               as Class::vfunc_N (the class that declares it)
  commands     Cmd_AddCommand-style calls and static {name, handler} command tables;
               Quake III's handler name when it has one for the command
  assets       load / unload callbacks passed to AssetType_Register, by type code
  dvars        cvar_t* globals (Cvar_Get's result stored to a fixed address) and the
               vmCvar_t globals of the cgame / game cvar tables
  trivial      the folded empty function, lone-branch thunks, single-import wrappers

Output: TOML, [[function]] then [[global]] entries sorted by address, each with
address, name, cname (a C identifier, for Class::method names), confidence
(known | high | medium) and evidence.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import re
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# The image
# ---------------------------------------------------------------------------


class Image:
    """A PE laid out by virtual address (xex_dump.py output): offset = address - base."""

    def __init__(self, path: Path):
        self.data = open(path, "rb").read()
        d = self.data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        n = struct.unpack_from("<H", d, e + 6)[0]
        opt = struct.unpack_from("<H", d, e + 20)[0]
        self.base = struct.unpack_from("<I", d, e + 24 + 28)[0]
        self.sections = []  # (name, start, end, characteristics)
        s = e + 24 + opt
        for _ in range(n):
            name = d[s:s + 8].rstrip(b"\0").decode("latin-1")
            vsize, va = struct.unpack_from("<II", d, s + 8)
            ch = struct.unpack_from("<I", d, s + 36)[0]
            self.sections.append((name, self.base + va, self.base + va + vsize, ch))
            s += 40
        self.code = [(a, b) for n, a, b, ch in self.sections if ch & 0x20000000]
        self.end = self.base + len(d)

    def u32(self, a: int) -> int:
        o = a - self.base
        if 0 <= o <= len(self.data) - 4:
            return struct.unpack_from(">I", self.data, o)[0]
        return 0

    def in_code(self, a: int) -> bool:
        return any(lo <= a < hi for lo, hi in self.code)

    def section_of(self, a: int):
        for n, lo, hi, ch in self.sections:
            if lo <= a < hi:
                return n
        return None

    def cstr(self, a: int, limit: int = 512):
        """The printable NUL-terminated string at a (None if a is not one)."""
        o = a - self.base
        if not (0 <= o < len(self.data)):
            return None
        e = self.data.find(b"\0", o, o + limit)
        if e <= o:
            return None
        s = self.data[o:e]
        # 0x14 / 0x15 open CoD's error and warning messages (Com_Error's class marker)
        if all(32 <= c < 127 or c in (9, 10, 13, 0x14, 0x15) for c in s):
            return s.decode("ascii")
        return None

    def pdata(self):
        funcs = []
        for n, lo, hi, ch in self.sections:
            if n != ".pdata":
                continue
            for a in range(lo, hi - 7, 8):
                beg, info = self.u32(a), self.u32(a + 4)
                if beg:
                    funcs.append((beg, beg + ((info >> 8) & 0x3FFFFF) * 4))
        return sorted(set(funcs))


# ---------------------------------------------------------------------------
# A small PowerPC scanner: constants, references and call arguments
# ---------------------------------------------------------------------------

# X-form (primary opcode 31) instructions by what they write.
_X_WRITES_RA = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 954, 922, 986, 26, 58, 27, 539,
                794, 826, 827}
_X_NO_GPR = {0, 32, 4, 68, 144, 146, 150, 151, 149, 183, 181, 215, 247, 407, 439, 662, 918, 663, 695,
             727, 759, 983, 467, 54, 86, 246, 278, 470, 1014, 598, 854, 210, 242, 18, 82, 178, 231, 199,
             135, 167, 487, 647, 711, 455, 391, 903, 935, 966, 518, 646, 774, 838}
_X_STORE_UPDATE = {183, 181, 247, 439, 695, 759}
_X_LOAD_UPDATE = {55, 53, 119, 311, 375, 567, 631, 373}
_VOLATILE = (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)


class Call:
    __slots__ = ("pc", "target", "args", "certain")

    def __init__(self, pc, target, args, certain):
        self.pc, self.target, self.args, self.certain = pc, target, args, certain


class FuncInfo:
    """What one scan of a function found."""

    def __init__(self, start, end):
        self.start, self.end = start, end
        self.calls = []        # Call (bl with a known target)
        self.refs = []         # (pc, address, kind) kind: 'addr' | 'load' | 'store'
        self.ret_stores = []   # (pc, called function, address, call pc) - "stw r3, global" after a call
        self.const_stores = [] # (pc, value, base register, displacement) - "stw <address constant>, d(rA)"
        self.global_const_stores = []  # (pc, value, address) - "stw <constant>, <fixed address>"
        self.indirect = 0      # bctrl count
        self.ninsn = (end - start) // 4
        self.tail = None       # target of a final unconditional b outside the function


def _simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def branch_target(pc, w):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return ((li if w & 2 else pc + li) & 0xFFFFFFFF), bool(w & 1)
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return ((bd if w & 2 else pc + bd) & 0xFFFFFFFF), bool(w & 1)
    return None, False


def is_save_restore_entry(img, a):
    """True if a is an entry of a register save/restore helper (__savegprlr_N and friends)."""
    w = img.u32(a)
    op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if op in (62, 58) and ra == 1 and 14 <= rd <= 31 and w & 3 == 0:     # std / ld rN, d(r1)
        return True
    if op in (54, 50) and ra == 12 and 14 <= rd <= 31:                     # stfd / lfd fN, d(r12)
        return True
    if w >> 16 == 0x3960 and _simm(w) < 0 and _simm(w) % 16 == 0:          # li r11,-16k ; stvx/lvx v,r11,r12
        x = img.u32(a + 4)
        return x >> 26 in (4, 31) and (x >> 16) & 31 == 11 and (x >> 11) & 31 == 12
    return False


def scan_function(img: Image, start: int, end: int) -> FuncInfo:
    """Linear scan of [start, end) tracking constant register values.

    A register's value is "certain" at a call when no branch target lies between
    the instruction that set it and the call: every path to the call then went
    through that instruction."""
    fi = FuncInfo(start, end)
    words = [img.u32(pc) for pc in range(start, end, 4)]
    labels = set()
    for i, w in enumerate(words):
        t, link = branch_target(start + 4 * i, w)
        if t is not None and not link and start <= t < end:
            labels.add(t)
    regs = {}       # reg -> int constant, or ("ret", called function, call pc) for r3 after a call
    setpc = {}      # reg -> pc of the instruction that set it
    last_label = start
    pc = start

    def put(r, v):
        if v is None:
            regs.pop(r, None)
        else:
            regs[r] = v
            setpc[r] = pc

    for i, w in enumerate(words):
        pc = start + 4 * i
        if pc in labels:
            last_label = pc
        op = w >> 26
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if op == 15:                                    # addis / lis
            if ra == 0:
                put(rd, ((w & 0xFFFF) << 16) & 0xFFFFFFFF)
            elif isinstance(regs.get(ra), int):
                put(rd, (regs[ra] + ((w & 0xFFFF) << 16)) & 0xFFFFFFFF)
            else:
                put(rd, None)
        elif op == 14:                                  # addi / li
            if ra == 0:
                put(rd, _simm(w) & 0xFFFFFFFF)
            elif isinstance(regs.get(ra), int):
                v = (regs[ra] + _simm(w)) & 0xFFFFFFFF
                if regs[ra] & 0xFFFF0000 and regs[ra] >= 0x80000000:
                    fi.refs.append((pc, v, "addr"))
                put(rd, v)
            else:
                put(rd, None)
        elif op == 24:                                  # ori (nop when all zero)
            if w == 0x60000000:
                pass
            elif isinstance(regs.get(rd), int):
                v = regs[rd] | (w & 0xFFFF)
                if v >= 0x80000000:
                    fi.refs.append((pc, v, "addr"))
                put(ra, v)
            else:
                put(ra, None)
        elif 32 <= op <= 55 or op in (58, 62):          # D-form loads and stores
            disp = _simm(w) if op not in (58, 62) else _simm(w & ~3)
            base = regs.get(ra) if ra else None
            store = op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 62)
            if isinstance(base, int) and base >= 0x80000000:
                addr = (base + disp) & 0xFFFFFFFF
                fi.refs.append((pc, addr, "store" if store else "load"))
                if op == 36 and isinstance(regs.get(rd), tuple):
                    fi.ret_stores.append((pc, regs[rd][1], addr, regs[rd][2]))
                elif op == 36 and isinstance(regs.get(rd), int):
                    fi.global_const_stores.append((pc, regs[rd], addr))
            elif op == 36 and isinstance(regs.get(rd), int) and regs[rd] >= 0x80000000:
                fi.const_stores.append((pc, regs[rd], ra, disp))
            if op == 46:                                # lmw
                for r in range(rd, 32):
                    put(r, None)
            elif op in (32, 33, 34, 35, 40, 41, 42, 43, 58):
                put(rd, None)
            if op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55) or (op in (58, 62) and w & 3 == 1):
                put(ra, None)
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo == 444 and rd == rb:                  # mr rA, rS
                v = regs.get(rd)
                put(ra, v)
                if v is not None:
                    setpc[ra] = setpc.get(rd, pc)
            elif xo in _X_WRITES_RA:
                put(ra, None)
            elif xo in _X_STORE_UPDATE:
                put(ra, None)
            elif xo in _X_LOAD_UPDATE:
                put(rd, None)
                put(ra, None)
            elif xo in _X_NO_GPR:
                pass
            else:
                put(rd, None)
        elif op in (20, 21, 23, 25, 26, 27, 28, 29, 30):  # rotates, logical immediates: write rA
            put(ra, None)
        elif op in (7, 8, 12, 13):                      # mulli, subfic, addic(.)
            put(rd, None)
        elif op == 18:                                  # b / bl
            t, link = branch_target(pc, w)
            if link and is_save_restore_entry(img, t):  # prologue/epilogue helper: not a real call
                regs.pop(11, None)
                regs.pop(12, None)
            elif link:
                args = {r: regs[r] for r in range(3, 11) if r in regs}
                certain = {r for r in args if setpc.get(r, start) >= last_label}
                fi.calls.append(Call(pc, t, args, certain))
                for r in _VOLATILE:
                    regs.pop(r, None)
                regs[3] = ("ret", t, pc)
                setpc[3] = pc
            elif not (start <= t < end) and pc == end - 4:
                fi.tail = t
        elif op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 528 and w & 1:                     # bctrl
                fi.indirect += 1
                for r in _VOLATILE:
                    regs.pop(r, None)
        # anything else (floating point, VMX, compares, branches) leaves the GPRs alone
    return fi


# ---------------------------------------------------------------------------
# Function boundaries
# ---------------------------------------------------------------------------

BLR, BCTR = 0x4E800020, 0x4E800420


def _is_terminator(w):
    return w in (BLR, BCTR, 0) or (w >> 26 == 18 and not w & 1)


def read_extra_functions(toml_path):
    """Function starts from a codegen config's [functions] table ("0xADDR = { ... }")."""
    out = {}
    if not toml_path or not Path(toml_path).is_file():
        return out
    section = None
    for line in Path(toml_path).read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.split("#", 1)[0].strip()
        if s.startswith("["):
            section = s.strip("[]").strip()
            continue
        m = re.match(r"(0x[0-9A-Fa-f]{8})\s*=\s*\{(.*)\}", s)
        if section == "functions" and m:
            size = re.search(r"size\s*=\s*(\d+)", m.group(2))
            out[int(m.group(1), 16)] = int(size.group(1)) if size else None
    return out


def find_functions(img: Image, extra: dict):
    """All function extents: .pdata, the config's extra functions, then every
    branch target, code pointer (in data or loaded by lis/addi) and leftover
    stretch of code between known functions. Returns sorted [(start, end)]."""
    pd = img.pdata()
    pstart = [s for s, _ in pd]
    pend = dict(pd)

    def inside_pdata(v):
        i = bisect.bisect_right(pstart, v) - 1
        return i >= 0 and pd[i][0] < v < pd[i][1]

    starts = set(pstart) | set(extra)

    def boundary(v):
        return v % 4 == 0 and img.in_code(v) and not inside_pdata(v) and not img.in_code(img.u32(v)) and (
            v in starts or _is_terminator(img.u32(v - 4)))

    for lo, hi in img.code:
        hi_regs = {}
        for pc in range(lo, hi, 4):
            w = img.u32(pc)
            op = w >> 26
            if op == 18:
                t, link = branch_target(pc, w)
                if img.in_code(t) and not inside_pdata(t):
                    if link:
                        starts.add(t)
                    else:
                        i = bisect.bisect_right(pstart, pc) - 1
                        if i >= 0 and pd[i][0] <= pc < pd[i][1] and not pd[i][0] <= t < pd[i][1]:
                            starts.add(t)            # tail call out of a .pdata function
                hi_regs.clear()
                continue
            rd, ra = (w >> 21) & 31, (w >> 16) & 31
            if op == 15 and ra == 0:
                hi_regs[rd] = (w & 0xFFFF) << 16
            elif op in (14, 24) and ra in hi_regs:
                v = (hi_regs[ra] + (_simm(w) if op == 14 else w & 0xFFFF)) & 0xFFFFFFFF
                if boundary(v):
                    starts.add(v)
            elif op == 19:
                hi_regs.clear()
    for n, lo, hi, ch in img.sections:
        if ch & 0x20000000 or n in (".pdata", ".reloc"):
            continue
        for a in range(lo, hi - 3, 4):
            v = img.u32(a)
            if boundary(v):
                starts.add(v)

    ordered = sorted(starts)

    def extent(v, limit):
        """End of a function without .pdata: the first terminator past every
        forward branch (and switch jump table) seen so far."""
        far, pc = v, v
        while pc < limit:
            w = img.u32(pc)
            if w == 0:
                break
            if w == BCTR:                               # a switch: its jump table may follow inline
                n = 0
                while v <= img.u32(pc + 4 + 4 * n) < limit and img.u32(pc + 4 + 4 * n) % 4 == 0:
                    far = max(far, img.u32(pc + 4 + 4 * n))
                    n += 1
                if n:
                    pc += 4 + 4 * n
                    continue
            t, link = branch_target(pc, w)
            if t is not None and not link and v <= t < limit:
                far = max(far, t)
            if _is_terminator(w) and pc >= far:
                if (w == BCTR or w >> 26 == 18) and img.u32(pc + 4) == BLR:
                    pc += 4                             # the compiler's stray blr after a tail call
                return pc + 4
            pc += 4
        return pc

    funcs = []
    for lo, hi in img.code:
        p = lo
        while p < hi:
            if p in pend:
                funcs.append((p, pend[p]))
                p = pend[p]
                continue
            w = img.u32(p)
            if w == 0:
                p += 4
                continue
            i = bisect.bisect_right(ordered, p)
            limit = min(ordered[i] if i < len(ordered) else hi, hi)
            size = extra.get(p)
            e = p + size if size else extent(p, limit)
            funcs.append((p, max(e, p + 4)))
            p = max(e, p + 4)
    return funcs


# ---------------------------------------------------------------------------
# The program: functions, their scans and cross references
# ---------------------------------------------------------------------------


class Program:
    def __init__(self, img: Image, extra_functions: dict):
        self.img = img
        self.funcs = find_functions(img, extra_functions)
        self.starts = [s for s, _ in self.funcs]
        self.start_set = set(self.starts)
        self.end_of = dict(self.funcs)
        self.info = {s: scan_function(img, s, e) for s, e in self.funcs}
        self.callers = collections.defaultdict(list)    # target -> [(caller function, Call)]
        self.refby = collections.defaultdict(list)      # address -> [(function, pc, kind)]
        for s, fi in self.info.items():
            for c in fi.calls:
                self.callers[c.target].append((s, c))
            for pc, a, k in fi.refs:
                self.refby[a].append((s, pc, k))
        self._data_ptrs = None

    def strings_of(self, f):
        """[(pc, string address, text)] for the strings function f takes the address of."""
        out = []
        for pc, a, k in self.info[f].refs:
            if k == "addr":
                s = self.img.cstr(a)
                if s is not None:
                    out.append((pc, a, s))
        return out

    def data_pointers(self):
        """address value -> [locations in data sections holding it] (aligned big-endian words)."""
        if self._data_ptrs is None:
            d = collections.defaultdict(list)
            img = self.img
            for n, lo, hi, ch in img.sections:
                if ch & 0x20000000 or n in (".pdata", ".reloc"):
                    continue
                o0 = lo - img.base
                words = struct.unpack_from(">%dI" % ((hi - lo) // 4), img.data, o0)
                for i, v in enumerate(words):
                    if img.base <= v < img.end:
                        d[v].append(lo + 4 * i)
            self._data_ptrs = d
        return self._data_ptrs


# ---------------------------------------------------------------------------
# The symbol table
# ---------------------------------------------------------------------------

CONFIDENCE_RANK = {"known": 3, "high": 2, "medium": 1}
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CPP_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)+$")


class Symbol:
    __slots__ = ("address", "name", "confidence", "evidence", "kind", "source")

    def __init__(self, address, name, confidence, evidence, kind, source):
        self.address, self.name, self.confidence = address, name, confidence
        self.evidence, self.kind, self.source = evidence, kind, source

    @property
    def cname(self):
        return self.name.replace("::", "__")


class SymbolTable:
    """Names by address for functions and globals.

    The first claim on an address wins unless a later one has a higher
    confidence (passes run roughly strongest evidence first). A name already
    held by another address is not given twice: the weaker claim is dropped,
    or, when both are as strong, the later one gets a _2, _3... suffix."""

    def __init__(self):
        self.by_addr = {}       # (kind, address) -> Symbol
        self.by_name = {}       # cname -> Symbol
        self.rejected = []      # (Symbol, reason)

    def get(self, address, kind="function"):
        return self.by_addr.get((kind, address))

    def name_of(self, address):
        s = self.by_addr.get(("function", address)) or self.by_addr.get(("global", address))
        return s.name if s else None

    def add(self, address, name, confidence, evidence, source, kind="function"):
        if not (IDENT.match(name) or CPP_NAME.match(name)):
            raise ValueError(f"not a valid name: {name!r}")
        sym = Symbol(address, name, confidence, evidence, kind, source)
        old = self.by_addr.get((kind, address))
        if old is not None:
            if old.name == name:                     # agreement: keep the stronger evidence
                if CONFIDENCE_RANK[confidence] > CONFIDENCE_RANK[old.confidence]:
                    old.confidence, old.source = confidence, source
                    old.evidence = f"{evidence}; also {old.evidence}"
                return old
            if CONFIDENCE_RANK[confidence] <= CONFIDENCE_RANK[old.confidence]:
                self.rejected.append((sym, f"address already named {old.name}"))
                return None
            self._remove(old)
            self.rejected.append((old, f"replaced by {name} ({source})"))
        holder = self.by_name.get(sym.cname)
        if holder is not None:
            if CONFIDENCE_RANK[confidence] > CONFIDENCE_RANK[holder.confidence] and holder.source != "known":
                self._remove(holder)
                self.rejected.append((holder, f"name taken by 0x{address:08X} ({source})"))
            elif CONFIDENCE_RANK[confidence] < CONFIDENCE_RANK[holder.confidence]:
                self.rejected.append((sym, f"name held by 0x{holder.address:08X}"))
                return None
            else:
                n = 2
                while f"{sym.cname}_{n}" in self.by_name:
                    n += 1
                sym.name = f"{name}_{n}"
        self.by_addr[(kind, address)] = sym
        self.by_name[sym.cname] = sym
        return sym

    def _remove(self, sym):
        self.by_addr.pop((sym.kind, sym.address), None)
        if self.by_name.get(sym.cname) is sym:
            del self.by_name[sym.cname]

    def functions(self):
        return sorted((s for (k, a), s in self.by_addr.items() if k == "function"), key=lambda s: s.address)

    def globals(self):
        return sorted((s for (k, a), s in self.by_addr.items() if k == "global"), key=lambda s: s.address)


# ---------------------------------------------------------------------------
# Pass: names the project already relies on
# ---------------------------------------------------------------------------
# From src/game/addresses.h, the hooks in src/game/*.cpp and config/default.toml
# (the recompilation project). These win over everything found automatically.

KNOWN_FUNCTIONS = [
    # qcommon
    (0x8252BBE0, "Cvar_Get", "addresses.h kCvarGet"),
    (0x8252B850, "Cvar_Set2", "addresses.h kCvarSet2"),
    (0x82529790, "Cvar_FindVar", "addresses.h kCvarFindVar"),
    (0x824644D0, "Cbuf_AddText", "addresses.h kCbufAddText"),
    (0x82464408, "Cbuf_InsertText", "addresses.h kCbufInsertText"),
    (0x8247E380, "Cbuf_Execute", "addresses.h kCbufExecute"),
    (0x8247E498, "Cbuf_ExecuteText", "addresses.h kCbufExecuteText"),
    (0x8247E228, "Cmd_ExecuteString", "addresses.h kCmdExecuteString"),
    (0x8246CBA8, "Cmd_AddCommand", "addresses.h kCmdAddCommand"),
    (0x82523A58, "Com_Printf", "addresses.h kComPrintf (body compiled out)"),
    (0x82523A60, "Com_DPrintf", "addresses.h kComDPrintf (body compiled out)"),
    (0x825239A0, "Com_Error", "addresses.h kComError"),
    (0x82536DD0, "Com_Frame", "addresses.h / frame_rate.cpp"),
    (0x82535E80, "Com_EventLoop", "frame_rate notes (Com_Frame's wait loop)"),
    # cgame / client
    (0x8243A198, "CG_GetViewFov", "addresses.h kCgGetViewFov"),
    (0x8250B460, "CL_AdjustAngles", "addresses.h kClAdjustAngles"),
    (0x82510748, "CL_CreateCmd", "addresses.h (calls CL_AdjustAngles)"),
    (0x824447C8, "CG_DrawCrosshair", "addresses.h / config hooks"),
    (0x82435588, "CG_CalcCrosshairColor", "addresses.h crosshair notes"),
    (0x82439530, "CG_DrawMountedCrosshair", "addresses.h: the mounted gun / vehicle crosshair"),
    (0x82439250, "CG_ShouldDrawCrosshair", "project notes: decides whether a crosshair is shown"),
    # game: cheats (client command table 0x825F49F0)
    (0x825432C8, "CheatsOk", "addresses.h kCheatsOk"),
    (0x825452E0, "Cmd_God_f", "addresses.h kGod"),
    (0x82545248, "Cmd_Notarget_f", "addresses.h kNoTarget"),
    (0x825451B0, "Cmd_Noclip_f", "addresses.h kNoclip"),
    (0x82545128, "Cmd_Ufo_f", "addresses.h kUfo"),
    (0x82560988, "Cmd_Give_f", "addresses.h kGive"),
    (0x82543048, "Cmd_SetViewpos_f", "addresses.h kSetViewPos"),
    (0x82542F00, "Cmd_JumpToNode_f", "addresses.h kJumpToNode"),
    (0x8253FE98, "Cmd_Invinc_f", "addresses.h ('invinc', an empty function)"),
    # server
    (0x82517A00, "SV_SpawnServer", "addresses.h kSpawnServer"),
    (0x825148D0, "SV_MapRestart_f", "addresses.h kMapRestart ('map_restart')"),
    (0x825149A8, "SV_MapRestartInternal_f", "addresses.h kMapRestartInternal ('map_restart_internal')"),
    # front end: Options > Gameplay page (vtable 0x82071FAC)
    (0x824DBBF0, "OptionsGameplayMenu::ctor", "addresses.h kGameplayCtor"),
    (0x824DBB38, "OptionsGameplayMenu::dtor", "menu_fov.cpp (destructor body)"),
    (0x824E4D78, "OptionsGameplayMenu::Init", "addresses.h kGameplayInit vtbl[0]"),
    (0x824CD2D0, "OptionsGameplayMenu::Update", "addresses.h kGameplayUpdate vtbl[16]"),
    (0x824DB960, "OptionsGameplayMenu::Refresh", "addresses.h kGameplayRefresh vtbl[21]"),
    (0x824C3FE0, "OptionsGameplayMenu::MoveUp", "addresses.h kGameplayMoveA vtbl[25]"),
    (0x824C3F08, "OptionsGameplayMenu::MoveDown", "addresses.h kGameplayMoveB vtbl[26]"),
    (0x824BD160, "OptionsGameplayMenu::Toggle", "addresses.h kGameplayToggle (this, dir)"),
    (0x824C3E48, "OptionsGameplayMenu::Apply", "addresses.h kGameplayApply vtbl[30] (GameplayPage_Apply)"),
    (0x824D07D0, "Panel::FindElement", "addresses.h kPanelFindElement"),
    (0x8252CB98, "Localize", "addresses.h kLocalize (StringTable*, key)"),
    (0x823FBC58, "Profile_LoadSettings", "addresses.h kProfileLoad"),
    (0x824E5DF8, "ControlsPage_Apply", "addresses.h: the Controls page copies its rows into the settings"),
    # level scripts (src/game/script_threads.cpp)
    (0x824A6150, "ScriptThread_Run", "script_threads.cpp: runs (starts or resumes) one script thread "
     "under setjmp"),
    (0x823C0B98, "ScriptThread_CallEntry", "script_threads.cpp: calls a new thread's entry functor"),
    (0x824A3040, "ScriptThread_Destroy", "script_threads.cpp: the thread object's destructor"),
    # renderer / video
    (0x8212FDB8, "R_SelectVideoMode", "addresses.h kSelectMode"),
    (0x82153F50, "R_SetMultisample", "addresses.h kSetMultisample"),
    # C runtime
    (0x82351CC0, "setjmp", "config/default.toml setjmp_address"),
    (0x8234EEC0, "longjmp", "config/default.toml / script_threads.cpp"),
]

KNOWN_GLOBALS = [
    (0x829BAB14, "cvar_vars", "addresses.h kCvarVars"),
    (0x829BB178, "sv_cheats", "addresses.h kSvCheatsPtr (cvar_t*)"),
    (0x829EBA10, "cmd_functions", "addresses.h kCmdFunctions"),
    (0x82A11358, "cg_fov", "addresses.h kCgFovVmCvar (vmCvar_t)"),
    (0x82A0E630, "cg_crosshairAlpha", "addresses.h crosshair::kAlphaVmCvar (vmCvar_t)"),
    (0x829B6110, "g_cheats", "addresses.h cheats::kCheatsVmCvar (vmCvar_t 'sv_cheats' read by CheatsOk)"),
    (0x82A2A1C8, "clientActiveIndex", "addresses.h kActiveLocalClient"),
    (0x82A57900, "cl", "addresses.h kClientActive (clientActive_t[], stride 7280)"),
    (0x82A8E610, "controlSettings", "addresses.h controls::kSettings"),
    (0x82A2A150, "languageMode", "addresses.h fe::kLanguageMode"),
    (0x825A2168, "videoModeTable", "addresses.h video::kModeTable"),
    (0x8290F2BC, "videoMode", "addresses.h video::kVideoMode"),
    (0x829C2514, "com_maxfps", "frame_rate notes (cvar_t*)"),
    (0x825F49F0, "clientCommandTable", "addresses.h ({name, handler} pairs)"),
]


def pass_known(prog: Program, syms: SymbolTable, warnings: list):
    for a, name, ev in KNOWN_FUNCTIONS:
        if a not in prog.start_set:
            warnings.append(f"known function {name} 0x{a:08X} is not a function start here")
        syms.add(a, name, "known", ev, "known")
    for a, name, ev in KNOWN_GLOBALS:
        syms.add(a, name, "known", ev, "known", kind="global")


# ---------------------------------------------------------------------------
# Pass: names checked by hand
# ---------------------------------------------------------------------------
# Each was read in the disassembly; the evidence says what identifies it.
# Q3 = the Quake III Arena function of that name has the same structure.

CURATED_FUNCTIONS = [
    # qcommon
    (0x825372E8, "Com_Init", "high", "'--- Common Initialization Complete ---'; registers developer and "
     "com_maxfps, adds error/crash/freeze/quit/writeconfig, execs autoexec.cfg; calls Cmd_Init and SV_Init "
     "(Q3)"),
    (0x82523880, "Com_AddStartupCommands", "high", "Cbuf_AddText of each startup line and '\\n', skipping "
     "Q_stricmpn(line, 'set', 3) ones (Q3)"),
    (0x82451568, "Com_sprintf", "high", "(dest, size, fmt, ...): Cvar_SetValue formats '%i' / '%f' into a "
     "32-byte buffer with it (Q3)"),
    (0x82488370, "Cmd_Init", "high", "adds cmdlist/exec/vstr/echo/wait, exactly Q3 Cmd_Init"),
    (0x82468630, "Cmd_ArgvBuffer", "high", "(arg, buffer, length): strncpy of cmd_argv[arg]; "
     "CG_ConsoleCommand and ClientCommand fetch argv 0 with it (Q3)"),
    (0x8246CAA0, "Cmd_AddButtonCommand", "medium", "Cmd_AddCommand's body storing type 2; used only for the "
     "+/- button commands"),
    (0x82526570, "CopyString", "high", "allocates and copies the name in Cvar_Get, Cvar_Set2 and "
     "Cmd_AddCommand (Q3 CopyString)"),
    (0x82523AC8, "Z_Malloc", "medium", "operator new + memset 0, fatal 'out of memory' on failure; "
     "CopyString's allocator"),
    (0x824534B0, "va", "high", "string 'overrun string in call to va()'; formats into a static buffer"),
    (0x820CDE10, "Q_stricmpn", "high", "(s1, s2, n) called with n = 99999 by CG_ConsoleCommand "
     "(Q3 Q_stricmp = Q_stricmpn(s1, s2, 99999))"),
    (0x82468510, "Sys_Milliseconds", "high", "CL_ScaledMilliseconds = this * com_timescale->value (Q3)"),
    # cvars
    (0x8243C7C8, "CG_RegisterCvars", "high", "loops over the cgame cvarTable (147 x {vmCvar*, name, default, "
     "flags}): Cvar_Get, handle, Cvar_Update (Q3)"),
    (0x82434EE0, "CG_UpdateCvars", "high", "Cvar_Update on every entry of the cgame cvarTable (Q3)"),
    (0x8252DBF8, "Cvar_Register", "high", "Cvar_Get then vmCvar handle and Cvar_Update (Q3)"),
    (0x82530C20, "Cvar_SetValue", "high", "Com_sprintf '%i' or '%f' then Cvar_Set2(name, buf, 1) (Q3)"),
    (0x8252B718, "Cvar_VariableStringBuffer", "high", "Cvar_FindVar then strncpy of ->string into the "
     "buffer (Q3)"),
    (0x8252B780, "Cvar_VariableString", "high", "Cvar_FindVar, returns ->string (+4) or \"\" (Q3)"),
    (0x8252B7C8, "Cvar_VariableIntegerValue", "high", "Cvar_FindVar, returns ->integer (+32) or 0 (Q3)"),
    (0x8252B808, "Cvar_VariableValue", "high", "Cvar_FindVar, returns ->value (+28) or 0.0 (Q3)"),
    (0x82549B00, "G_RegisterCvars", "high", "loops over the game cvarTable (85 x 24 bytes) with Cvar_Get "
     "(Q3)"),
    (0x8253F510, "G_UpdateCvars", "high", "Cvar_Update on every entry of the game cvarTable, "
     "modificationCount check (Q3)"),
    # client
    (0x8250EDB8, "CL_InitInput", "high", "adds centerview and the +/- button commands, cl_nodelta, "
     "cl_debugMove (Q3)"),
    (0x8250A110, "IN_KeyUp", "high", "called with a kbutton_t by the '-' button handlers (IN_ProneUp, "
     "IN_DownUp...) that did not inline it; IN_KeyDown's partner (Q3)"),
    (0x8250FF48, "CL_MouseMove", "high", "prints '%f : %f' under cl_showmouserate, reads m_pitch, m_yaw, "
     "cl_mouseAccel; called by CL_CreateCmd after CL_AdjustAngles (Q3); turns the right stick here"),
    (0x8250AB10, "CL_ScaledMilliseconds", "high", "Sys_Milliseconds() * com_timescale->value; refimport "
     "Milliseconds slot (Q3)"),
    (0x8250AB68, "CL_RefPrintf", "high", "refimport Printf slot: vsprintf into a buffer (Q3)"),
    (0x824309F0, "GetRefAPI", "high", "CL_InitRef copies its result (a 160-byte refexport_t, slot 0 "
     "RE_Shutdown) into re; 'COULDNT_INIT_REFRESH' when NULL (Q3)"),
    # server
    (0x82519240, "SV_Init", "high", "calls SV_AddOperatorCommands, registers sv_serverid, sv_paks, "
     "sv_pakNames, sv_referencedPaks, nextmap, sv_killserver (Q3); called by Com_Init"),
    (0x82519090, "SV_AddOperatorCommands", "high", "adds map, devmap, spmap, map_restart... (Q3)"),
    # cgame / game
    (0x824398F8, "CG_InitConsoleCommands", "high", "called by CG_Init; adds the cgame command names and the "
     "server's 'levelshot', 'stats' without handlers (Q3)"),
    (0x82435680, "CG_ConsoleCommand", "high", "called by cg_vmMain; Cmd_ArgvBuffer(0) compared with the "
     "cgame command table 0x820637F0 (Q3)"),
    (0x82542CB0, "ClientCommand", "high", "(clientNum): Cmd_ArgvBuffer(0) dispatched through the client "
     "command tables 0x825F49F0 / 0x825F4A70 (Q3 g_cmds.c)"),
    # assets (.cod archives; see docs/formats/cod3.md)
    (0x82128500, "AssetType_Register", "high", "(fourcc, version, load, unload, 0) for TEX, MESH, MAT, SKEL, "
     "FONT, AEPS, FX; links a 24-byte record into the list at 0x8290E810 (project notes)"),
    (0x82128CF8, "AssetChunk_WalkDirectory", "high", "steps through a chunk's type records and items "
     "(project notes: the directory walker)"),
    # Treyarch library (c:/cod/code/tl)
    (0x82127938, "tlFatalError", "medium", "prints 'TL Fatal Error: ' and the message, or calls the "
     "installed handler"),
    (0x821279C0, "tlFatalErrorf", "medium", "vsprintf then tlFatalError; 'out of memory' callers"),
    (0x82127A20, "tlMemAlloc", "medium", "(size, align, flags) under a lock; 'Memory allocation failed. %d "
     "bytes, %d align'"),
    (0x821276D0, "tlMemFree", "medium", "frees under tlMemAlloc's lock (0x8290E6A8)"),
    # C runtime
    (0x8234EBA0, "memset", "high", "byte/word fill loop (dst, c, n); 724 callers"),
    (0x8234F460, "memcpy", "high", "(dst, src, n): copies CL_InitRef's refimport_t and the refexport_t"),
    (0x8234C7F0, "strncpy", "high", "copies up to n bytes and pads with zeros"),
    (0x823536D0, "_stricmp", "high", "byte compare folding A-Z to lower case"),
    (0x82351858, "abs", "high", "cmpwi r3,0; bgelr; neg r3,r3"),
    (0x8234BBC8, "atoi", "high", "strtol(s, NULL, 10) as a tail call"),
    (0x82350EA0, "strtol", "medium", "atoi's target (s, NULL, 10)"),
    (0x8234B400, "vsprintf", "medium", "(buf, fmt, args); used by tlFatalErrorf and CL_RefPrintf"),
    (0x8234F0D0, "_vsnprintf", "medium", "va formats with (buf, 2048, fmt, args)"),
    (0x82350390, "__iob_func", "medium", "returns the stdio FILE array; +64 (stderr) is printed to"),
    (0x8234B250, "sprintf", "high", "(buf, fmt, ...) with '%d' / '%f' formats; CRT formatter underneath"),
    (0x8234BFC0, "tolower", "high", "c - 'A' <= 25 ? c + 32 : c"),
    (0x8234BFD8, "isalpha", "high", "ctype table lookup masked with 0x103 (_ALPHA)"),
    (0x8234B538, "sin", "high", "range reduction by pi (constants pi/2, 1/pi, pi split) with an odd sign"),
    (0x8234B610, "cos", "high", "the same reduction applied to |x| + pi/2"),
    (0x8234C240, "atof", "medium", "tail call of the string-to-double converter with a NULL second argument"),
    # name hashes (event names, tags)
    (0x820A0118, "NameHash_Init", "medium", "fills a 32-byte {djb2 hash of the lower-cased text, text[28]} "
     "record from a C string"),
    (0x820CEBB8, "NameHash", "medium", "NameHash_Init on the stack and returns the hash; 706 callers pass "
     "names like 'tag_flash', 'end'"),
    # --- structural names (second pass) ---
    # Found by walking the call graph from the names above: position in the frame loop and the
    # init / shutdown sequences, the vmMain case numbers and the VM_Call numbers that reach them,
    # refexport slots, Q3 (and RTCW) function bodies, strings and cvars. 'Broc_' names are the level
    # scripts' built-ins, by their byte offset ('slot') in the level interface table that
    # G_InitLevelInterface fills; CoD3Recomp's builtins table (docs/SOURCES.md) was the lead for
    # their IW-style names and each one kept here was re-checked in the code (strings, fields).
    # qcommon: frame loop, events, command line
    (0x82127800, "Sys_PerformanceCounter", "medium", "returns QueryPerformanceCounter's 64-bit value; "
     "Sys_Milliseconds and the per-subsystem frame timers in Com_Frame / CL_Frame use it"),
    (0x82345740, "QueryPerformanceCounter", "high", "XAPI: reads the time base (mftb) into *arg and returns "
     "TRUE"),
    (0x823FACA0, "Sys_GetEvent", "high", "returns the next queued event (64-entry ring, 24-byte events) or "
     "an empty one stamped with Sys_Milliseconds; Com_GetRealEvent's source (Q3 win_main.c)"),
    (0x823FAD70, "Sys_QueEvent", "medium", "writes the next slot of Sys_GetEvent's 64-entry ring (frees the "
     "overwritten evPtr, Q3) with Sys_Milliseconds time; the type is fixed to 1 (key) in this build"),
    (0x823FBA30, "Sys_DirectoryHasContents", "medium", "'%s\\*' search skipping '.', '..' and 'CVS'; "
     "FS_AddGameDirectory checks a folder with it"),
    (0x823FBE78, "Sys_ListFilteredFiles", "high", "'%s\\%s\\*' / '%s\\*' searches, skips '.', '..' and "
     "'CVS', recurses into subdirectories and filters each path (Q3 win_main.c)"),
    (0x823FC6E8, "Sys_ListFiles", "high", "'%s\\*%s' search with the extension, CopyString of each name "
     "into a Z_Malloc'd list (Q3 win_main.c)"),
    (0x823FD210, "Sys_Init", "high", "Cvar_Set 'arch' 'ps2', Cvar_SetValue 'sys_cpuid', then IN_Init (Q3 "
     "order; Com_Init has an inlined copy)"),
    (0x82400098, "Sys_Main", "high", "the image's main(): picks the video mode (R_SelectVideoMode, "
     "R_SetMultisample), calls Com_Init once then Com_Frame forever"),
    (0x824536D8, "Swap_Init", "high", "short {1,0} byte-order test choosing the Big/Little Short/Long/Float "
     "function pointers (Q3 q_shared.c)"),
    (0x82453CD8, "Com_EndParseSession", "high", "'Com_EndParseSession: session underflow' (the other user "
     "inlines it)"),
    (0x82523920, "Com_ParseCommandLine", "high", "Com_Init: stores the command line and splits it at '+' / "
     "newline into com_consoleLines (max 32), count at 0x829BB17C (Q3)"),
    (0x82523B38, "Com_StringContains", "high", "substring search with optional case folding by toupper; "
     "Com_Filter's helper (Q3)"),
    (0x82526328, "Com_PushEvent", "high", "Com_Milliseconds pushes each non-empty event through it (Q3)"),
    (0x825263F0, "Com_SafeMode", "high", "scans the console lines for 'safe' and 'cvar_restart' (Q3); "
     "Com_Init execs safemode.cfg when true"),
    (0x825265E0, "Com_Filter", "high", "wildcard match ('*', '?', '[...]') through Com_StringContains and "
     "toupper (Q3)"),
    (0x82529820, "Com_Shutdown", "high", "Com_Quit_f's step after SV_Shutdown / CL_Shutdown: closes the log "
     "and journal files with FS_FCloseFile (Q3)"),
    (0x825298D8, "Com_ModifyMsec", "high", "Com_Frame passes the frame msec: fixedtime override, scaled by "
     "com_timescale, at least 1 when timescale is set, clamped to 100 (Q3)"),
    (0x82529988, "Com_GetRealEvent", "high", "Com_Milliseconds' event source (Q3)"),
    (0x82529D18, "Com_FilterPath", "high", "Sys_ListFilteredFiles' filter: path separators folded, then "
     "Com_Filter (Q3)"),
    (0x8252BF88, "Com_Milliseconds", "high", "pulls 24-byte events with Com_GetRealEvent and Com_PushEvent "
     "until an empty one, returns its time (Q3)"),
    (0x8252BFF8, "Com_GetEvent", "high", "Com_EventLoop's event source; falls back to Com_GetRealEvent when "
     "nothing is pushed (Q3)"),
    (0x8252DC88, "Com_LoadCvarsFromBuffer", "medium", "parses 'name value' pairs with Com_ParseExt and "
     "Cvar_Set2, \"WARNING: unknown cvar '%s'\" and the 'ERROR: the following ...' report; the shellshock "
     ".shock loader"),
    (0x8252E1C0, "Com_RunAndTimeServerPacket", "high", "SV_PacketEvent bracketed by com_speeds timing, "
     "prints 'SV_PacketEvent time: %i' at com_speeds 3 (Q3)"),
    (0x8252E248, "Com_StartupVariable", "high", "Com_Init calls it with NULL and 'developer': tokenizes "
     "each console line, 'set' -> Cvar_Set2 / Cvar_Get (Q3)"),
    (0x82530D00, "Com_InitJournaling", "high", "'journal' cvar, journal.dat / journaldata.dat, 'Journaling "
     "events' (Q3)"),
    # qcommon: commands and cvars
    (0x82463F38, "Cmd_TokenizeString", "high", "fills cmd_argc / cmd_argv; Cmd_ExecuteString, "
     "Com_StartupVariable and Com_SafeMode tokenize with it (Q3)"),
    (0x82464150, "Cmd_Args", "high", "joins cmd_argv[1..cmd_argc) with ' ' into a static buffer; "
     "CL_ForwardToServer_f sends it (Q3)"),
    (0x82464208, "Cmd_Argv", "high", "returns cmd_argv[arg] or \"\" when arg >= cmd_argc (Q3)"),
    (0x82464238, "Cmd_Argc", "high", "returns cmd_argc (Q3)"),
    (0x824799C0, "Cmd_RemoveCommand", "high", "unlinks and frees the named entry of cmd_functions; "
     "CL_Shutdown and CL_ShutdownInput remove their commands with it (Q3)"),
    (0x825235D0, "Cvar_ValidateString", "high", "strchr for backslash, quote and semicolon; Cvar_Get and "
     "Cvar_Set2 reject names with them (Q3)"),
    (0x82526148, "generateHashValue", "high", "string 'null name in generateHashValue'; tolower hash used "
     "by Cvar_FindVar and Cvar_Get (Q3 cvar.c)"),
    (0x82529690, "Cvar_InfoString_Big", "high", "Info_SetValueForKey_Big of every cvar with the flag bit; "
     "SV_SetConfigstring(1, Cvar_InfoString_Big(8)) (Q3)"),
    (0x82529710, "Cvar_InfoString", "high", "Info_SetValueForKey of every cvar with the flag bit; "
     "SV_SetConfigstring(0, Cvar_InfoString(4)) (Q3)"),
    (0x8252DC70, "Cvar_Reset", "high", "Cvar_Set2(name, NULL, 0) (Q3)"),
    (0x8252DC80, "Cvar_Set", "high", "Cvar_Set2(name, value, 1); the refimport's Cvar_Set slot after "
     "Cvar_FindVar in CL_InitRef (Q3)"),
    (0x82530B88, "Cvar_SetCheatState", "high", "every cvar with the cheat flag (0x200) whose string differs "
     "from its reset string is set back with Cvar_Set2 (Q3)"),
    # qcommon: files
    (0x82524E50, "COM_GetExtension", "medium", "returns the text after the last '.' of the last path "
     "component, or \"\"; FS_FOpenFileRead's extension test"),
    (0x82524EB0, "FS_FilenameCompare", "high", "case-folding compare treating backslash and ':' as '/', -1 "
     "on a difference (Q3)"),
    (0x82525038, "FS_HashFileName", "high", "tolower, stops at '.', backslash as '/', hash += c * (i + "
     "119), folded with >> 10 / >> 20 and masked (Q3)"),
    (0x82527118, "FS_FreeFileList", "high", "frees each name then the list; refimport slot after "
     "FS_ListFiles (Q3)"),
    (0x82527180, "FS_AddFileToList", "high", "Q_stricmp against the names already listed, else CopyString "
     "into the list (Q3)"),
    (0x825274D8, "FS_FCloseFile", "high", "SV_SetExpectedHunkUsage closes hunkusage.dat with it after "
     "FS_Read; FS_ReadFile calls it twice (Q3)"),
    (0x82527780, "FS_filelength", "high", "ftell, fseek to the end, ftell, fseek back (or the zip entry's "
     "length); FS_FOpenFileRead's result (Q3)"),
    (0x8252C0B8, "FS_AddGameDirectory", "high", "adds a search path: 'localized' / 'non-localized', "
     "'WARNING: game folder %s/%s added as both localized & non-localized' (Q3 role)"),
    (0x8252CA38, "FS_FileExists", "high", "FS_BuildOSPath(fs_homepath, ...) then fopen 'rb' / fclose (Q3)"),
    (0x8252EC20, "FS_Shutdown", "high", "removes 'path', 'fullpath', 'dir', 'fdir', 'touchFile' and frees "
     "the search paths; Com_Quit_f, FS_Restart, SV_SpawnServer (Q3)"),
    (0x8252ED38, "FS_AddLocalizedGameDirectory", "medium", "FS_Startup's per-directory call: "
     "FS_AddGameDirectory for the localized and the plain folder"),
    (0x8252ED90, "FS_ListFilteredFiles", "high", "FS_BuildOSPath per search path, Sys_ListFiles, "
     "FS_AddFileToList into a Z_Malloc'd list (Q3)"),
    (0x82532340, "FS_ListFiles", "high", "FS_ListFilteredFiles(path, extension, NULL, numfiles); refimport "
     "slot between FS_FreeFile and FS_FreeFileList (Q3)"),
    (0x82532D28, "FS_InitFilesystem", "high", "Com_StartupVariable for fs_cdpath, fs_basepath, fs_homepath, "
     "fs_game..., FS_Startup, then FS_ReadFile of default.cfg with the 'Couldn't load %s' error (Q3)"),
    # qcommon: strings, info strings, parsing
    (0x824515D8, "Q_strlwr", "high", "tolower over the string in place, returns it (Q3)"),
    (0x82451900, "COM_DefaultExtension", "high", "Cmd_Exec_f adds '.cfg' with it; Com_sprintf '%s%s' when "
     "the name has no extension (Q3)"),
    (0x82453580, "Q_stricmp", "high", "Q_stricmpn with n = 99999 inlined: case-folding compare (Q3)"),
    (0x824571F8, "Info_SetValueForKey_Big", "high", "calls Info_RemoveKey_Big (Q3's _Big variant shares the "
     "'Info_SetValueForKey: oversize infostring' text); Cvar_InfoString_Big's setter"),
    (0x82457408, "Info_SetValueForKey", "high", "the backslash / semicolon / quote key checks, "
     "Info_RemoveKey, Com_sprintf '\\%s\\%s' (Q3); Cvar_InfoString's setter"),
    # qcommon: network channel, messages, vm
    (0x82451C68, "NET_SendLoopPacket", "high", "copies a packet into the next loopback slot (3076-byte "
     "messages, send counter +12308) (Q3)"),
    (0x82451D00, "NET_GetLoopPacket", "high", "loopbacks[sock] (12312 bytes: 4 x 3076-byte messages), "
     "get/send counters, copies one message and sets the address type to 2 (NA_LOOPBACK) (Q3)"),
    (0x82451DB0, "NET_AdrToString", "high", "'loopback', '%i.%i.%i.%i:%i' and the IPX format (Q3); "
     "Netchan_Process and CL_PacketEvent print with it"),
    (0x82451F48, "MSG_ReadString", "high", "reads a NUL-terminated string at msg->readcount into a static "
     "buffer (empty on overflow); CL_ParseCommandString and SV_ExecuteClientMessage (Q3)"),
    (0x82453DC8, "NET_SendPacket", "high", "'send packet %4i' under showpackets, then NET_SendLoopPacket "
     "(Q3 net_chan.c)"),
    (0x824577C0, "NET_OutOfBandPrint", "high", "vsprintf after the -1 header, 'send packet' under "
     "showpackets, NET_SendLoopPacket; SV_PacketEvent replies with it (Q3)"),
    (0x8245BB30, "Netchan_Init", "high", "registers showpackets and showdrop (Q3 Netchan_Init; qport not "
     "used)"),
    (0x82497450, "VM_Call", "high", "(vm, callnum, ...): saves currentVM (0x82A2A2BC), copies 16 varargs "
     "and calls vm->entryPoint (+136) (Q3 dll path)"),
    (0x82497540, "VM_DllSyscall", "high", "VM_Create stores it as the game's and cgame's syscall pointer; "
     "forwards &arg to currentVM->systemCall (Q3)"),
    # math (q_math.c)
    (0x820A0520, "AnglesToForward", "medium", "level slot 0x1AC (anglestoforward) tail-calls it with "
     "(angles, out): the forward vector only"),
    (0x82526B88, "RadiusFromBounds", "high", "VectorLength of the larger |mins[i]| / |maxs[i]| per axis; "
     "SV_LinkEntity uses it (Q3)"),
    (0x82526BF8, "AngleNormalize180", "high", "(360/65536) * ((int)(a * 65536/360) & 65535), minus 360 "
     "above 180 (Q3)"),
    (0x82526C58, "AngleSubtract", "high", "fmod(a1 - a2, 360) folded into -180..180 (Q3)"),
    (0x82526E08, "VectorNormalize2", "high", "(v, out): out = v / |v| or cleared when the length is 0; "
     "returns the length (Q3)"),
    (0x82526F38, "VectorNormalize", "high", "length = sqrt(x*x + y*y + z*z); scales the vector by 1/length "
     "when non-zero and returns the length (Q3 q_math.c)"),
    (0x8252A008, "AngleDelta", "high", "AngleNormalize180(a1 - a2) inlined (Q3)"),
    (0x8252A068, "AnglesSubtract", "high", "AngleSubtract on each of the three components into v3 (Q3)"),
    (0x8252E838, "AngleVectors", "high", "level slot 0x1B0 (anglevectors) is a lone branch to it: forward / "
     "right / up from angles; CG_OffsetThirdPersonView uses it (Q3)"),
    (0x82531240, "vectoangles", "high", "level slot 0x1A0 (vectortoangles) swaps its arguments and "
     "tail-calls it: direction vector -> pitch / yaw angles (Q3 q_math.c)"),
    # client
    (0x82509558, "CL_ParseCommandString", "high", "CL_ParseServerMessage's svc_serverCommand (5) case: "
     "MSG_ReadString into the server command ring (Q3)"),
    (0x82509680, "CL_ShutdownRef", "high", "calls re.Shutdown through the refexport then zeroes re (Q3)"),
    (0x825097C0, "CL_AddReliableCommand", "high", "copies the text into the next reliable-command slot; "
     "CL_ForwardToServer_f / CL_ForwardCommandToServer / CL_KeyEvent queue commands with it (Q3)"),
    (0x82509A90, "CL_FinishMove", "high", "cmd serverTime / weapon and ANGLE2SHORT of cl.viewangles (x "
     "65536/360 + 0.5); CL_CreateCmd's last step (Q3)"),
    (0x82509BB8, "CL_CmdButtons", "high", "sets cmd->buttons bit i for each in_buttons[i] active or "
     "wasPressed and clears wasPressed, then the extra flag bits (Q3)"),
    (0x8250A070, "CL_KeyState", "high", "kbutton_t fraction of the frame held; CL_AdjustAngles calls it 4 "
     "times and CL_KeyMove 9 times (Q3)"),
    (0x8250A318, "CL_GameCommand", "high", "when cgvm is set: VM_Call(cgvm, 2 = CG_CONSOLE_COMMAND), the "
     "cgame case that runs CG_ConsoleCommand (Q3)"),
    (0x8250ACB8, "Key_StringToKeynum", "high", "Key_Bind_f and Key_Unbind_f look up argv 1 with it, '\"%s\" "
     "isn't a valid key' on failure (Q3)"),
    (0x8250AE88, "CL_WritePacket", "high", "prints '%i ' under cl_showSend, MSG_WriteString of the reliable "
     "commands, Netchan_Transmit (Q3)"),
    (0x8250B108, "CL_KeyMove", "high", "CL_CreateCmd's movement step after CL_CmdButtons: CL_KeyState of "
     "the forward / back / side / up buttons (Q3)"),
    (0x8250BF60, "CL_ShutdownCGame", "high", "VM_Call(cgvm, 1 = CG_SHUTDOWN) then clears the vm (Q3); "
     "CL_Shutdown and SV_SpawnServer call it"),
    (0x8250CC88, "Key_SetBinding", "high", "the last call of Key_Bind_f and Key_Unbind_f; frees the old "
     "binding and CopyString's the new one (Q3)"),
    (0x8250CF10, "CL_ParseSnapshot", "high", "CL_ParseServerMessage's svc_snapshot (7) case (Q3 svc_ops "
     "order: nop 1, serverCommand 5, snapshot 7, EOF 8)"),
    (0x8250D590, "CL_ShutdownInput", "high", "removes every command CL_InitInput added ('centerview', "
     "'+moveup' / '-moveup'...) with Cmd_RemoveCommand"),
    (0x8250DCF8, "CL_InitRenderer", "high", "registers the 'nglWhite' and 'console' shaders through the "
     "refexport (Q3 'white' / 'console')"),
    (0x8250DE88, "CL_CharEvent", "high", "Com_EventLoop's SE_CHAR (type 2) case, between CL_KeyEvent (1) "
     "and the empty mouse case (Q3)"),
    (0x8250E518, "CL_SystemInfoChanged", "high", "Info_ValueForKey of 'sv_serverid', 'sv_cheats' "
     "(Cvar_SetCheatState when 0), 'sv_paks', 'sv_pakNames', 'sv_referencedPaks' (Q3)"),
    (0x8250EA10, "CL_ConfigstringModified", "high", "atoi(Cmd_Argv(1)) with 'configstring > "
     "MAX_CONFIGSTRINGS'; CL_GetServerCommand's 'cs' case (Q3)"),
    (0x8250EC08, "CL_ClearState", "medium", "zeroes the client state (memset of cl); CL_Disconnect and "
     "CL_MapLoading call it (Q3)"),
    (0x8250F5F0, "CL_Disconnect", "high", "under cl_running: Cvar_Set 'r_uiFullScreen' '1' (Q3's first "
     "step); CL_Shutdown and SV_Shutdown call it"),
    (0x8250FDF8, "CL_AddKeyUpCommands", "medium", "Cbuf_AddText of '-%s %i %i' for the released key's '+' "
     "binding (Q3)"),
    (0x82510980, "CL_CreateNewCommands", "high", "stores CL_CreateCmd's result in the next cl.cmds slot "
     "(Q3); called by CL_Frame and CL_SendCmd"),
    (0x82510B20, "CL_ShutdownUI", "medium", "the front-end teardown called by CL_Shutdown and "
     "CL_ShutdownAll (Q3 order)"),
    (0x82510DF8, "CL_StartHunkUsers", "high", "under cl_running: CL_InitRenderer, then the front end's "
     "'FrontEnd' load (Q3); Com_Init / SV_SpawnServer"),
    (0x82510E98, "CL_ShutdownAll", "high", "CL_ShutdownCGame, the UI shutdown and the renderer's shutdown "
     "(Q3)"),
    (0x82510EF0, "CL_SendCmd", "high", "when cls.state is connected: CL_CreateNewCommands then "
     "CL_WritePacket (Q3)"),
    (0x82511168, "SCR_UpdateScreen", "high", "CL_Updatescreen_f's call; draws through SCR_DrawScreenField "
     "between the renderer's begin / end (Q3)"),
    (0x82511FE0, "CL_MapLoading", "high", "SV_SpawnServer's client step: Cvar_Set 'nextmap', CL_ClearState, "
     "servername 'localhost', 'r_uiFullScreen', SCR_UpdateScreen (Q3)"),
    # renderer exports
    (0x824178A0, "RE_EndFrame", "high", "refexport slot 25: SCR_UpdateScreen calls it after drawing with "
     "&time_frontend (0x829BB210) (Q3 EndFrame)"),
    (0x824179A0, "RE_StretchPic", "high", "refexport slot 18: CG_FillRect's draw call with x, y, w, h, s1, "
     "t1, s2, t2 and a shader (Q3)"),
    (0x82417A58, "RE_SetColor", "high", "refexport slot 17: CG_FillRect calls it with the color and again "
     "with NULL around the stretch pic (Q3)"),
    (0x8241A0A0, "RE_BeginFrame", "high", "refexport slot 24: SCR_UpdateScreen calls it before "
     "SCR_DrawScreenField (Q3 BeginFrame)"),
    (0x8242BA20, "RE_BeginRegistration", "high", "refexport slot 1: calls R_Init and copies the glconfig "
     "out (Q3)"),
    # server
    (0x82512290, "SV_WriteSnapshotToClient", "high", "writes svc_snapshot (7), the server time from "
     "VM_Call(gvm, 19) and the snapshot flags (Q3)"),
    (0x82512338, "SV_ClientThink", "high", "VM_Call(gvm, 6, the active client's last usercmd), the game "
     "case that runs ClientThink (Q3 sv_client.c); CL_Frame calls it directly in single player"),
    (0x82512660, "SV_GameCommand", "high", "Cmd_ExecuteString's fallback: VM_Call(gvm, 12) whose vmMain "
     "case is the 'entitylist' console command handler (Q3)"),
    (0x825127F0, "SV_UpdateServerCommandsToClient", "high", "writes svc_serverCommand (5), the sequence and "
     "MSG_WriteString for each unsent reliable command, then marks them sent (Q3)"),
    (0x82512C40, "SV_ShutdownGameProgs", "high", "VM_Call(gvm, 1 = GAME_SHUTDOWN) then clears the vm (Q3); "
     "SV_Shutdown and SV_SpawnServer call it"),
    (0x82512E08, "SV_ExecuteClientCommand", "high", "VM_Call(gvm, 5) whose vmMain case is ClientCommand "
     "(Q3)"),
    (0x82512F28, "SV_ClearServer", "high", "frees the configstrings and zeroes sv; SV_Shutdown and "
     "SV_SpawnServer (Q3)"),
    (0x825130C0, "SV_ClientCommand", "high", "MSG_ReadString of the command, 'clientCommand: %i : %s' under "
     "sv_showCommands, then SV_ExecuteClientCommand (Q3)"),
    (0x82513198, "SV_LinkEntity", "high", "the game's trap_LinkEntity: last call of FinishSpawningItem and "
     "of Cmd_SetViewpos_f's teleport (Q3); links through the collision world (95 callers)"),
    (0x82513628, "SV_SendServerCommand", "high", "(client, fmt, ...): vsprintf then SV_AddServerCommand; "
     "SV_SetConfigstring broadcasts 'cs' with it (Q3)"),
    (0x825136F8, "SV_GameSendServerCommand", "high", "the game's trap: SV_SendServerCommand of '%s' to one "
     "client or all; CheatsOk / Cmd_Where_f print through it (Q3)"),
    (0x82513B80, "SV_PacketEvent", "high", "Com_RunAndTimeServerPacket's call; Netchan_Process then "
     "SV_ExecuteClientMessage, 'disconnect' (Q3)"),
    (0x82513E48, "SV_InitGameVM", "high", "VM_Call(gvm, 0 = GAME_INIT, ..., Com_Milliseconds()) (Q3); "
     "SV_InitGameProgs' last call"),
    (0x82513EF0, "SV_SetBrushModel", "high", "the inline model number from the '*N' name (atoi), its "
     "bounds, then SV_LinkEntity; InitTrigger's call (Q3)"),
    (0x82514198, "SV_RunGameFrame", "high", "SV_Frame's game step: refreshes CS_SERVERINFO / CS_SYSTEMINFO "
     "from Cvar_InfoString(4) / Cvar_InfoString_Big(8) when cvar_modifiedFlags has the bit, then "
     "VM_Call(gvm, 10, msec) (Q3 SV_Frame body)"),
    (0x82514DD8, "SV_SendClientSnapshot", "high", "SV_BuildClientSnapshot, message init, "
     "SV_UpdateServerCommandsToClient, SV_WriteSnapshotToClient, Netchan_Transmit (Q3)"),
    (0x82514ED0, "SV_SetExpectedHunkUsage", "high", "reads hunkusage.dat, finds the map with Com_ParseExt "
     "and sets com_expectedhunkusage, else '-1' (RTCW sv_init.c)"),
    (0x825161D0, "SV_SendClientMessages", "high", "SV_Frame's last step: VM_Call(gvm, 21) then "
     "SV_SendClientSnapshot for the active client (Q3)"),
    (0x82516E28, "SV_Trace", "high", "the game's trap_Trace (64 callers): FinishSpawningItem's drop trace "
     "and G_RunItem's move trace go through it; collision-model box traces underneath (Q3)"),
    (0x82518E40, "SV_Frame", "high", "Com_Frame calls it with msec before the client half; sv_killserver -> "
     "shutdown and Cvar_Set 'sv_killserver' '0', 'vstr nextmap' on time wrap (Q3)"),
    # cgame
    (0x82433460, "CG_Error", "high", "vsprintf then Com_Error; cg_vmMain reports 'cg_vmMain: unknown "
     "command %i' with it (Q3)"),
    (0x824334C0, "CG_Printf", "high", "vsprintf into a 1 KB buffer then the (empty) print; CG_Viewpos_f's "
     "'(%i %i %i) : %i' and the shellshock commands' usage lines (Q3)"),
    (0x82433520, "CG_FreeLocalEntity", "high", "string 'CG_FreeLocalEntity: not active' "
     "(CG_AddLocalEntities has an inlined copy) (Q3)"),
    (0x82433810, "CG_FillRect", "high", "re.SetColor(color), screen-scaled re.StretchPic with the fixed "
     "white shader, re.SetColor(NULL); the crosshair's line rectangles (Q3)"),
    (0x82434FC8, "CG_AllocLocalEntity", "high", "frees the oldest active local entity with "
     "CG_FreeLocalEntity when none is free, then zeroes the new one (Q3)"),
    (0x8243A638, "CG_ParseServerinfo", "high", "Info_ValueForKey 'mapname' into 'maps/%s.bsp' (Q3)"),
    (0x8243A9D0, "CG_Shutdown", "high", "cgame vmMain case 1 (CG_SHUTDOWN), reached by CL_ShutdownCGame's "
     "VM_Call(cgvm, 1) (Q3)"),
    (0x8243AA38, "CG_NewClientInfo", "medium", "Info_ValueForKey 'n' (name) and 't' (atoi team) of the "
     "client configstring (Q3)"),
    (0x8243C390, "CG_TransitionPlayerState", "high", "called by CG_TransitionSnapshot and at the end of "
     "CG_PredictPlayerState (Q3)"),
    (0x8243CD70, "CG_CalcEntityLerpPositions", "high", "BG_EvaluateTrajectory of pos and apos at cg.time "
     "(or interpolation), called right before CG_AddCEntity (Q3)"),
    (0x8243D528, "CG_DrawCenterString", "high", "reads cg_centertime, splits the text at newlines (Q3)"),
    (0x8243F280, "CG_CalcVrect", "high", "clamps cg_viewsize to 30..100 with Cvar_Set (Q3), cg_letterbox"),
    (0x8243F490, "CG_SetInitialSnapshot", "medium", "CG_ProcessSnapshots' first-snapshot setup: 'fade' "
     "command, cg_norender back to 0"),
    (0x82443A98, "CG_CalcFov", "high", "reads cg_fov through CG_GetViewFov, the widescreen and zoom "
     "adjustments (Q3)"),
    (0x82444CD0, "CG_CenterPrint", "medium", "'Center Print' text into the center-print buffer"),
    (0x82444E88, "CG_CalcMuzzlePoint", "high", "string 'No %s in CG_CalcMuzzlePoint on ...' "
     "(tag_gunner_flash), the bg_viewheight cvars (Q3)"),
    (0x82446238, "CG_Draw2D", "high", "profile label 'CG_Draw2D'; reads cg_draw2D and cg_drawStatus (Q3)"),
    (0x824483C8, "CG_OffsetThirdPersonView", "high", "reads cg_thirdPersonAngle, cg_thirdPersonRange, "
     "cg_thirdPersonLock (Q3)"),
    (0x824487F8, "CG_DrawActive", "high", "CG_DrawActiveFrame's last call: renders the scene through the "
     "refexport (cg_skybox) then CG_Draw2D (Q3)"),
    (0x8244AAE0, "CG_PredictPlayerState", "high", "reads cg_nopredict, cg_errordecay, pmove_fixed, "
     "pmove_msec; CL_GetUserCmd and Pmove per command (Q3)"),
    (0x8244B0A8, "CG_RegisterGraphics", "high", "registers 'gfx/2d/backtile', 'gfx/misc/tracer', "
     "'hit_direction', 'overlay_low_health'... for CG_Init (Q3)"),
    (0x8244CFB0, "CG_AddCEntity", "high", "'Bad entity type: %i' default case of the entity type switch, "
     "CG_Item for items (Q3)"),
    (0x8244DF60, "CG_ServerCommand", "high", "Cmd_ArgvBuffer(0) dispatch of the server commands ('server "
     "print', 'game message'...) (Q3)"),
    (0x8244E678, "CG_AddPacketEntities", "high", "loops over the snapshot entities: "
     "CG_CalcEntityLerpPositions then CG_AddCEntity for each (Q3)"),
    (0x8244E9F8, "CG_ExecuteNewServerCommands", "high", "CL_GetServerCommand for each new sequence, then "
     "CG_ServerCommand (Q3)"),
    (0x8244EB48, "CG_SetNextSnap", "high", "CG_ProcessSnapshots stores the newly read snapshot with it "
     "before the 'Server time went backwards' check; runs CG_ExecuteNewServerCommands (Q3)"),
    (0x8244ECF8, "CG_TransitionSnapshot", "high", "CG_ProcessSnapshots' loop step when cg.time passed the "
     "next snapshot; cg_nopredict test before CG_TransitionPlayerState (Q3)"),
    (0x8244FEA8, "CG_AddViewWeapon", "high", "cg_drawGun check, CG_RegisterWeapon and "
     "CG_WeaponRunXModelAnims for the view weapon (Q3)"),
    (0x82450C10, "CG_CalcViewValues", "high", "view offsets, then CG_CalcVrect and CG_CalcFov (Q3 order "
     "ends with return CG_CalcFov())"),
    (0x82450C90, "CG_DrawActiveFrame", "high", "cgame vmMain case 3 (CG_DRAW_ACTIVE_FRAME); CG_UpdateCvars "
     "then CG_ProcessSnapshots, the 'Cg_view' render pass (Q3)"),
    # level script interface (Broc builtins: level interface table slots)
    (0x824BB360, "G_LoadLevelDll", "high", "sprintf 'game:\\%s.dll', loads the level DLL, calls its first "
     "export with the interface pointer, then G_InitLevelInterface; called by G_InitGame"),
    (0x824BB028, "G_InitLevelInterface", "high", "fills the level interface table (*0x82A2A2A0) from the "
     "(offset, function) lists at 0x825F5638 / 0x825F5E80 and the per-slot setters"),
    (0x824A5500, "Broc_Thread", "high", "level slot 0x3C (12,523 level calls): allocates a script thread "
     "object for a level function and starts it (sub_824A2B18)"),
    (0x824A55E8, "Broc_Wait", "high", "level slot 0x4C: an AeThreadWaitState of f1 seconds then yields with "
     "longjmp"),
    (0x824A56B8, "Broc_WaitFrames", "high", "level slot 0x50: an AeThreadWaitFramesState then yields with "
     "longjmp"),
    (0x824AB478, "Broc_Endon", "high", "level slot 0x58: (entity, event hash) adds an "
     "AeThreadEntityNotifyState of type 3 to the running thread without yielding"),
    (0x824A5268, "Broc_Notify", "high", "level slot 0x74 (7,081 level calls): (entity, event hash) queues a "
     "20-byte event for waiting threads and runs the entity's handlers"),
    (0x824A77E8, "Broc_GetEnt", "high", "level slot 0x94: entity lookup by value and key 'classname' / "
     "'targetname' / 'target' / 'groupname'"),
    (0x824A08B0, "Broc_IsAlive", "high", "level slot 0x118: entity reference -> actor (+532) alive flag "
     "(+784) or health (+776) > 0"),
    (0x824A07E8, "Broc_IsVehicle", "high", "level slot 0x120: entity reference -> vehicle pointer (+528) "
     "set"),
    (0x824A0790, "Broc_IsAI", "high", "level slot 0x124: entity reference -> actor pointer (+532) set"),
    (0x824A0738, "Broc_IsPlayer", "high", "level slot 0x128: entity reference -> client pointer (+536) set"),
    (0x824A06E0, "Broc_IsTurret", "high", "level slot 0x12C: entity reference -> turret pointer (+540) set"),
    (0x820B2508, "Broc_RandomInt", "high", "level slot 0x140: own LCG (x 214013 + 2531011) scaled to [0, n)"),
    (0x820B2538, "Broc_RandomFloat", "high", "level slot 0x144: the same LCG scaled to a float range"),
    (0x820B2588, "Broc_RandomIntRange", "high", "level slot 0x148: the LCG scaled to [min, max)"),
    (0x820B25E0, "Broc_RandomFloatRange", "high", "level slot 0x14C: the LCG scaled to a float [min, max)"),
    (0x820A00C8, "Broc_Sin", "high", "level slot 0x154: float wrapper of sin"),
    (0x820A00A0, "Broc_Cos", "high", "level slot 0x158: float wrapper of cos"),
    (0x820B21B8, "Broc_Distance", "high", "level slot 0x184: sqrt of the squared difference of two vectors"),
    (0x820B21F0, "Broc_DistanceSquared", "high", "level slot 0x188: squared difference of two vectors"),
    (0x820B2150, "Broc_Length", "high", "level slot 0x18C: sqrt(x*x + y*y + z*z)"),
    (0x820B2170, "Broc_LengthSquared", "high", "level slot 0x190: x*x + y*y + z*z"),
    (0x820B2190, "Broc_VectorDot", "high", "level slot 0x198: dot product of two vectors"),
    (0x82497E48, "Broc_GetTime", "high", "level slot 0x42C: returns level.time (0x82A4E790 + 156)"),
    (0x824ABA90, "Broc_ObjectiveDelete", "high", "level slot 0x1E8: 'index %i is an illegal objective "
     "index', clears the objective configstring"),
    (0x824B5910, "Broc_ObjectiveState", "high", "level slot 0x1EC: Info_SetValueForKey 'state' in the "
     "objective configstring"),
    (0x824A9760, "Broc_ObjectivePosition", "high", "level slot 0x1FC: Info_SetValueForKey 'org' as '%i %i "
     "%i'"),
    (0x824B53F8, "Broc_ObjectiveCurrent", "high", "level slot 0x204: reads every objective's 'state' and "
     "makes one current"),
    (0x824A95F0, "Broc_ObjectiveRing", "high", "level slot 0x208: toggles the objective's 'ring' key"),
    (0x824AB430, "Broc_GetDvar", "high", "level slot 0x24C: Cvar_VariableStringBuffer into a script string"),
    (0x824A8530, "Broc_GetDvarInt", "high", "level slot 0x250: Cvar_FindVar(name)->integer (+32)"),
    (0x824A84E8, "Broc_GetDvarFloat", "high", "level slot 0x254: Cvar_FindVar(name)->value (+28)"),
    (0x824B4510, "Broc_Spawn", "high", "level slot 0x264: 'unable to spawn \"%s\" entity'"),
    (0x824AAE48, "Broc_GetSpawnerTeamArray", "high", "level slot 0x444: 'getspawnerteamarray'"),
    (0x824A4F38, "Broc_MissionSuccess", "high", "level slot 0x4A4: next level name, 'creditsdone' / "
     "'gamefinished', g_changelevel_time"),
    (0x824AAD00, "Broc_MissionFailed", "high", "level slot 0x4A8: 'INGAME_PLAYER_DIED' message through "
     "Localize, g_reloading"),
    (0x824A7C28, "Broc_Earthquake", "high", "level slot 0x4B4: (scale, duration, origin, radius) through "
     "the cgame camera shake (cg_camerashake)"),
    (0x824B9428, "Broc_MagicBullet", "high", "level slot 0x4D4: 'MagicBullet called with unknown weapon "
     "name'"),
    (0x824B0148, "Broc_BadPlace_Cylinder", "high", "level slot 0x508: 'badplace_cylinder'"),
    (0x824AFFC8, "Broc_BadPlace_Arc", "high", "level slot 0x50C: 'badplace_arc'"),
    (0x8249FDE8, "Broc_SetNormalHealth", "high", "level slot 0x5F8: 'setNormalHealth must be greater than "
     "0'"),
    (0x8249FD00, "Broc_Show", "high", "level slot 0x608: clears flag 0x400 of the entity's +632"),
    (0x8249FCB0, "Broc_Hide", "high", "level slot 0x60C: sets flag 0x400 of the entity's +632"),
    (0x824AD700, "Broc_DisconnectPaths", "high", "level slot 0x614: \"entity of type '%s' cannot disconnect "
     "paths\""),
    (0x824A70C0, "Broc_ConnectPaths", "high", "level slot 0x618: \"entity of type '%s' cannot connect "
     "paths\""),
    (0x8249F470, "Broc_SetTurretTeam", "high", "level slot 0x644: 'axis' / 'allies', \"entity type '%s' is "
     "not a turret\""),
    (0x8249EE80, "Broc_SetCursorHint", "high", "level slot 0x660: 'HINT_INHERIT', 'List of valid hint type "
     "strings'"),
    (0x824AA2F0, "Broc_SetHintString", "high", "level slot 0x668: 'Too many different hintstring values'"),
    (0x824B29E8, "Broc_UseAnimTree", "high", "level slot 0x6A0: 'cannot change the animtree of classname'"),
    (0x824AC338, "Broc_ShellShock", "high", "level slot 0x714: shock types 'death' / 'pain' / 'default' "
     "registered with G_FindConfigstringIndex"),
    (0x8249E8D0, "Broc_ViewKick", "high", "level slot 0x71C: 'viewkick: damage %g < 0'"),
    (0x8249E508, "Broc_SetTeam", "high", "level slot 0x750: \"unknown team '%s', should be axis, allies, or "
     "neutral\""),
    (0x824B4A60, "Broc_DropWeapon", "high", "level slot 0x81C: \"unknown weapon '%s' in dropWeapon\", "
     "tag_weapon_left / right"),
    (0x824B7C70, "Broc_Teleport", "high", "level slot 0x824: 'Teleport failed because player...', 'Player "
     "can see goal pos'"),
    (0x824A3FB8, "Broc_IsStanceAllowed", "high", "level slot 0x840: 'invalid stance in isStanceAllowed()'"),
    (0x824A6718, "Broc_Noclip", "high", "level slot 0x9BC: toggles noclip, prints GAME_NOCLIPON / "
     "GAME_NOCLIPOFF"),
    (0x824A66B8, "Broc_Reverb", "high", "level slot 0x9C0: client command 'reverb \"%s\" %g %g'"),
    (0x824ABB60, "Broc_PlayLocalSound", "high", "level slot 0x9C8: \"unknown sound alias '%s'\", client "
     "command 'ls %i'"),
    (0x8249ABC8, "Broc_StartPath", "high", "level slot 0x9F0: \"Can't start path on a vehicle that hasn't "
     "been attached\""),
    (0x824A92A8, "Broc_GetWheelSurface", "high", "level slot 0xA14: 'Vehicle type [%s] has no wheels'"),
    (0x824B8948, "Broc_FireWeapon", "high", "level slot 0xA68: 'No weapon specified for [%s]', 'Vehicles "
     "only support bullet and projectile'"),
    (0x824AA9C0, "Broc_SetShader", "high", "level slot 0xA8C: HUD element 'width %i < 0' / 'height %i < 0'"),
    (0x824A4E00, "Broc_SetTimer", "high", "level slot 0xA90: 'setTimer'"),
    (0x824A4DE8, "Broc_SetTimerUp", "high", "level slot 0xA94: 'setTimerUp'"),
    (0x824A4DD0, "Broc_SetTenthsTimer", "high", "level slot 0xA98: 'setTenthsTimer'"),
    (0x824A4DB8, "Broc_SetTenthsTimerUp", "high", "level slot 0xA9C: 'setTenthsTimerUp'"),
    (0x824ACC58, "Broc_SetClock", "high", "level slot 0xAA0: 'setClock'"),
    (0x824ACC20, "Broc_SetClockUp", "high", "level slot 0xAA4: 'setClockUp'"),
    (0x824A1670, "Broc_FadeOverTime", "high", "level slot 0xAAC: 'fade time %g <= 0' / '> 60'"),
    (0x824991F8, "Broc_ScaleOverTime", "high", "level slot 0xAB0: 'scale time %g <= 0' / '> 60'"),
    (0x82499120, "Broc_MoveOverTime", "high", "level slot 0xAB4: 'move time %g <= 0' / '> 60'"),
    (0x820B2290, "Broc_VectorNormalize", "high", "level slot 0x19C: copies the vector and scales it by "
     "1/length when non-zero"),
    (0x820B2228, "Broc_Closer", "high", "level slot 0x194: compares the squared distances from a to b and "
     "to c"),
    (0x820A0098, "Broc_Abs", "high", "level slot 0x174: fabs"),
    (0x824995C0, "Broc_Sqrt", "high", "level slot 0x178: fsqrts"),
    # game and bg
    (0x8246B1B0, "PM_AddEvent", "high", "pm->ps->events[eventSequence & 3] = event, eventParms 0, "
     "eventSequence++ (BG_AddPredictableEventToPlayerstate inlined); PM_CrashLand's landing events (Q3)"),
    (0x8246B398, "BG_PlayerStateToEntityState", "high", "copies origin, viewangles, movementDir, eFlags and "
     "the pending events from the playerState to the entityState; ClientSpawn (Q3)"),
    (0x8246B500, "BG_EvaluateTrajectoryDelta", "high", "the velocity variant: cos for TR_SINE, g_gravity "
     "for the gravity types; G_BounceItem (Q3)"),
    (0x824714A8, "PM_CrashLand", "high", "'landing vel: %.1f fall height', 'falling damage: %i', the "
     "bg_fallDamage cvars; called from PM_GroundTrace (Q3)"),
    (0x82471C78, "PM_AddTouchEnt", "high", "appends an entity number to pm->touchents (max 32) unless "
     "already there; PM_GroundTrace (Q3)"),
    (0x82478430, "BG_EvaluateTrajectory", "high", "switch over 11 trajectory types with sin for TR_SINE and "
     "the 0.001 time scale; Reached_BinaryMover, G_RunItem, G_RunMissile (Q3)"),
    (0x82486908, "BG_SetupWeaponInfo", "high", "prints 'Game: BG_SetupWeaponInfo', 'Could not allocate "
     "WeaponInfo'; loads the weapon files (CG_Init / G_InitGame)"),
    (0x82538B08, "G_SpawnStringNoDefault", "medium", "(key hash, out): the same spawnVars lookup without a "
     "default; SP_trigger_multiple's 'wait' / 'random' reads"),
    (0x82538B60, "G_SpawnString", "high", "(key hash, default, out): looks the key up in level.spawnVars "
     "(count at +176) or returns the default (Q3)"),
    (0x82539518, "G_Error", "high", "vsprintf then the error call; G_RunThink's 'NULL ent->think' and "
     "SelectRandomFurthestSpawnPoint's \"Couldn't find a spawn point\" (Q3)"),
    (0x82539570, "G_LogPrintf", "high", "empty (compiled-out) log print; G_ShutdownGame logs "
     "'ShutdownGame:' and the dashed line with it (Q3)"),
    (0x82539578, "G_Printf", "medium", "empty (compiled-out) print called with the Q3 G_Printf texts: "
     "FinishSpawningItem 'startsolid', Svcmd_EntityList_f, SP_trigger_multiple"),
    (0x82539D58, "vtos", "high", "'(%i %i %i)' into one of 8 rotating 32-byte buffers; Cmd_Where_f and "
     "FinishSpawningItem's 'startsolid at %s' (Q3)"),
    (0x8253B0F8, "G_SpawnVector", "high", "spawn var parsed with sscanf '%f %f %f' (Q3)"),
    (0x8253B170, "G_SpawnInt", "high", "spawn var through G_SpawnStringNoDefault then atoi, else the "
     "default (Q3)"),
    (0x8253B1D0, "G_SpawnFloat", "high", "spawn var through G_SpawnStringNoDefault then atof, else the "
     "default (Q3)"),
    (0x8253CD70, "ConcatArgs", "high", "joins cmd_argv[start..] into a static buffer; Cmd_Give_f / "
     "Cmd_Take_f read the item name with it (Q3 g_cmds.c)"),
    (0x8253D1A0, "G_FreeEntity", "high", "unlinks the entity and frees its memory; FinishSpawningItem "
     "(startsolid) and ClientDisconnect call it (Q3)"),
    (0x8253DC18, "Svcmd_EntityList_f", "high", "ConsoleCommand's 'entitylist' handler (Q3)"),
    (0x8253EB70, "Svcmd_ListVehicles_f", "high", "ConsoleCommand's 'listvehicles' handler"),
    (0x8253FDC8, "G_ParseHitLocDmgTable", "medium", "'Error parsing hitloc damage table'"),
    (0x82540168, "SetClientViewAngle", "medium", "Cmd_SetViewpos_f and Cmd_JumpToNode_f set the view angles "
     "with it after moving the player (Q3 TeleportPlayer's step)"),
    (0x82541150, "ConsoleCommand", "high", "game vmMain case 12: Cmd_ArgvBuffer(0) compared with "
     "'entitylist' and 'listvehicles' (Q3 g_svcmds.c)"),
    (0x82543568, "G_Spawn", "high", "allocates a 1040-byte game entity (pool or heap), constructs it with "
     "its number (-1 = next free) and counts it in level; LaunchItem and Broc_Spawn call it (Q3 role)"),
    (0x82543C70, "G_FindConfigstringIndex", "high", "string '>>> G_FindConfigstringIndex...' overflow "
     "report listing the sound alias / shellshock configstrings; SV_SetConfigstring of a new name (Q3)"),
    (0x82545820, "SpotWouldTelefrag", "medium", "CM_AreaEntities over the spot's bounds; used by the spawn "
     "point selection (Q3)"),
    (0x82547140, "InitTrigger", "high", "called by SP_trigger_multiple and the other trigger spawners: "
     "movedir and SV_SetBrushModel (Q3)"),
    (0x82549450, "InitMover", "high", "reads 'noise' (default '100'), 'light' and 'color' spawn vars; "
     "called by the func_bobbing / func_pendulum spawners (Q3 g_mover.c)"),
    (0x8254A060, "ClientConnect", "high", "game vmMain case 2 (GAME_CLIENT_CONNECT, returns a value); 'No "
     "sentient for player.'; SV_SpawnServer connects the local player with it (Q3)"),
    (0x8254AC60, "SP_trigger_damage", "high", "'wait' / 'random' / 'accumulate' / 'threshold' spawn vars; "
     "'trigger_damage has random >= wait'"),
    (0x8254B128, "SP_trigger_friendlychain", "high", "'trigger_friendlychain must target...' error, "
     "InitTrigger, SV_LinkEntity"),
    (0x8254C088, "SP_func_pendulum", "high", "'speed', 'dmg', 'phase' spawn vars and g_gravity for the "
     "swing frequency, then InitMover (Q3)"),
    (0x8254C2A0, "SP_func_bobbing", "high", "'speed', 'height', 'phase', 'dmg' spawn vars, then InitMover "
     "(Q3)"),
    (0x8254E090, "SelectSpawnPoint", "high", "ClientBegin's spawn choice; calls "
     "SelectRandomFurthestSpawnPoint (Q3)"),
    (0x82557B10, "G_BounceItem", "medium", "G_RunItem's impact step: reflects the trajectory (g_gravity "
     "evaluation), relinks the item (Q3)"),
    (0x82557F68, "G_SpawnItem", "high", "G_CallSpawn's item path: 'random' / 'wait' spawn vars, then links "
     "the item (Q3)"),
    (0x82558308, "LaunchItem", "high", "Drop_Item's call: spawns the item entity and links it with "
     "SV_LinkEntity (Q3)"),
    (0x82559F10, "G_RunItem", "high", "G_RunFrameForEntity's eType 2 (ET_ITEM) / physicsObject case: trace, "
     "SV_LinkEntity, G_RunThink, removal or G_BounceItem (Q3)"),
    (0x8255A158, "Drop_Item", "high", "random spread (rand) then LaunchItem; TossClientItems and the "
     "drop-weapon paths (Q3)"),
    (0x8255A270, "TossClientItems", "high", "BG_FindItemForWeapon of the held weapon then Drop_Item; called "
     "by player_die and ClientDisconnect (Q3)"),
    (0x8255A370, "ClientDisconnect", "medium", "game vmMain case 4: TossClientItems then frees the entity "
     "(Q3)"),
    (0x8255EF30, "player_die", "high", "the death handler: TossClientItems, 'INGAME_PLAYER_DIED', "
     "cg_redFlashTime; reached from Cmd_Kill_f (Q3)"),
    (0x8256A460, "G_RunMover", "high", "G_RunFrameForEntity's eType 4 (ET_MOVER) case: moves the team then "
     "G_RunThink (Q3)"),
    (0x8256A660, "G_MissileImpact", "medium", "G_RunMissile's impact step: 'Warning Missle Weapon == 0', "
     "the explosion / damage calls (Q3)"),
    (0x8256B7E8, "G_RunMissile", "high", "G_RunFrameForEntity's eType 3 (ET_MISSILE) case: trajectory "
     "trace, impact handling, G_RunThink (Q3)"),
    (0x8256C010, "G_RunFrameForEntity", "high", "G_RunFrame's per-entity step: eType 3 (ET_MISSILE) -> "
     "G_RunMissile, eType 2 (ET_ITEM) or physicsObject -> G_RunItem, eType 4 (ET_MOVER) -> G_RunMover, else "
     "G_RunThink (Q3 loop body)"),
    (0x8256C260, "ClientThink_real", "high", "clamps pmove_msec to 8..33 with Cvar_Set (the Q3 strings), "
     "reads g_gravity, g_speed, pmove_fixed (Q3 g_active.c)"),
    (0x8256CFB0, "ClientEndFrame", "high", "called by G_RunFrame after the entity loop and at the end of "
     "ClientSpawn (Q3)"),
    (0x8256D2B8, "ClientThink", "high", "game vmMain case 6: copies the user command and runs "
     "ClientThink_real (Q3)"),
    (0x8256D3E0, "G_RunFrame", "high", "the game's frame: G_UpdateCvars first, then the entity loop "
     "reaching G_RunThink; SV_SpawnServer runs it with 50 ms settle frames (Q3); 'unmatching "
     "beginPrediction and endPrediction calls in script'"),
    (0x8256D978, "G_InitGame", "high", "game vmMain case 0 (GAME_INIT, 4 arguments); sets up the level: "
     "timescale, checkpoints, the interaction / damage tables (Q3)"),
    (0x8256E370, "ClientSpawn", "high", "ClientBegin's call: SV_GetUsercmd, SetClientViewAngle, the "
     "bg_viewheight cvars, ClientThink then ClientEndFrame (Q3 g_client.c)"),
    (0x8256E878, "ClientBegin", "high", "game vmMain case 3 (GAME_CLIENT_BEGIN): SelectSpawnPoint then "
     "ClientSpawn (Q3)"),
    (0x8256E920, "vmMain", "high", "game entry point VM_Create stores for 'game': switch over 22 GAME_* "
     "commands (GAME_SHUTDOWN 1 -> G_ShutdownGame, ClientCommand...) (Q3 g_main.c vmMain)"),
    # C runtime
    (0x8234B4D8, "sscanf", "high", "G_SpawnVector's '%f %f %f' parse; CRT input formatter underneath"),
    (0x8234B6F0, "strncmp", "high", "counted byte compare (ctr = n) stopping at a difference or NUL; "
     "G_CallSpawn compares class names with it"),
    (0x8234BB48, "strstr", "high", "substring search loop; FS_CreatePath checks '..' and '::' with it (Q3)"),
    (0x8234C078, "srand", "high", "stores the seed where rand reads it; SV_SpawnServer seeds it with the "
     "time (Q3 srand(Com_Milliseconds()))"),
    (0x8234C0A8, "rand", "high", "per-thread seed * 214013 + 2531011, returns (seed >> 16) & 0x7FFF (MSVC "
     "CRT)"),
    (0x8234C2A0, "strchr", "high", "byte search loop returning NULL at the terminator; Info_* key checks "
     "(Q3)"),
    (0x8234C5D8, "fflush", "high", "FS_Write flushes after fwrite (Q3 handleSync); uses __iob_func"),
    (0x8234C850, "fmod", "high", "double remainder with the NaN / infinity checks; AngleSubtract calls it "
     "with 360.0"),
    (0x8234C9E0, "_fsopen", "high", "fopen's tail call with the share flag"),
    (0x8234CB70, "fopen", "high", "li r5,0x40 (_SH_DENYNO) then b _fsopen; FS_CopyFile's call order fopen, "
     "fseek, ftell, fseek, malloc, fread, fclose matches Q3"),
    (0x8234CD80, "fread_s", "high", "fread's tail call target with buffer size -1"),
    (0x8234CF00, "fread", "high", "fread_s(buf, SIZE_MAX, size, count, stream) tail call; FS_Read / "
     "FS_CopyFile (Q3 fread)"),
    (0x8234D2F0, "fwrite", "high", "locked wrapper; FS_Write and FS_CopyFile write with it (Q3)"),
    (0x8234D410, "toupper", "high", "c - 'a' <= 25 ? c - 32 : c"),
    (0x8234D660, "ftell", "high", "locked wrapper; FS_CopyFile's length query between the two fseeks (Q3)"),
    (0x8234D838, "fseek", "high", "locked wrapper; FS_CopyFile seeks to the end and back around ftell (Q3)"),
    (0x8234DA08, "fclose", "high", "locked wrapper; FS_CopyFile closes both files with it (Q3)"),
    (0x8234E220, "memmove", "high", "Cbuf_Execute shifts the remaining command text down with it (Q3)"),
    (0x8234E2E8, "printf", "high", "stdout (__iob_func) formatted output; CL_Shutdown's 'recursive "
     "shutdown' print (Q3)"),
    (0x8234EB30, "strcpy", "high", "byte copy loop up to and including the NUL; level interface slot 0x20"),
    # --- end structural names ---
    # --- Rust decomp (docs/DECOMP.md): named while porting; each port matches the original on
    # thousands of inputs run through tools/decomp/decomp.py check ---
    (0x82526E78, "VectorNormalizeFast", "high", "Q3's fast inverse square root on a vec3 in place: "
     "0x5F3759DF - (bits >> 1), one Newton step with 1.5 and 0.5 (Rust port checked)"),
    (0x82526EF0, "Vec2Normalize", "high", "VectorNormalize on x and y only: returns the 2D length and "
     "scales by 1/length when non-zero (Rust port checked)"),
    (0x82526F90, "Vec2DistanceSq", "high", "(p1, p2): (p2.x - p1.x)^2 + (p2.y - p1.y)^2 (Rust port checked)"),
    (0x82526FB8, "Vec2Distance", "high", "fsqrts of the same sum as Vec2DistanceSq (Rust port checked)"),
    (0x82526FE0, "Distance", "high", "(p1, p2): |p2 - p1| in 3D (Q3 Distance; Rust port checked)"),
]

CURATED_GLOBALS = [
    (0x829F00E0, "cmd_argv", "high", "Cmd_Vstr_f / Cvar_Set_f read argv[1] at +4"),
    (0x829F123C, "cmd_argc", "high", "Cmd_Vstr_f / Cvar_Set_f compare it with 2 / 3"),
    (0x829BB828, "cvar_indexes", "high", "CG_RegisterCvars turns cvar_t* into a handle as (cv - this) / 44"),
    (0x829BAB0C, "com_timescale", "high", "cvar_t* scaled into CL_ScaledMilliseconds (Q3)"),
    (0x82A11B70, "ri", "high", "refimport_t CL_InitRef fills (Cvar_Get at +32, Com_Error at +4)"),
    (0x829C3AF0, "re", "high", "refexport_t CL_InitRef copies from GetRefAPI"),
    # --- structural names (second pass) ---
    (0x82A2A1B8, "gvm", "high", "VM_Create('game') result stored by SV_InitGameProgs / SV_SpawnServer; "
     "every game VM_Call loads it (Q3)"),
    (0x829C91E4, "cgvm", "high", "VM_Create('cgame') result stored by CL_InitCGame; every cgame VM_Call "
     "loads it (Q3)"),
    (0x82A2A2BC, "currentVM", "high", "VM_Call saves, sets and restores it around the call; VM_DllSyscall "
     "forwards through it (Q3)"),
    (0x829BB208, "cvar_modifiedFlags", "high", "SV_RunGameFrame tests the 4 (serverinfo) and 8 (systeminfo) "
     "bits and clears them (Q3)"),
    (0x82A2A2A0, "levelInterface", "high", "pointer to the level DLL's interface table; "
     "G_InitLevelInterface fills its slots"),
    (0x82A4E790, "level", "high", "level_locals_t: framenum +152, time +156, previousTime +160 (advanced by "
     "game vmMain case 10), spawn vars +176 (Q3)"),
    (0x829C3F08, "cls", "high", "clientStatic_t; state at +0 (CL_SendCmd, SCR_DrawScreenField's 'bad "
     "cls.state') (Q3)"),
    (0x829C39E0, "svs", "high", "serverStatic_t: clients at +8 (4832-byte client_t), snapshot entity "
     "counters at +12 / +16 checked for wrapping in SV_Frame (Q3)"),
    (0x829BAB20, "com_consoleLines", "high", "Com_ParseCommandLine stores the command line pieces here (max "
     "32) (Q3)"),
    (0x829BB17C, "com_numConsoleLines", "high", "Com_ParseCommandLine's line count (Q3)"),
    (0x829FCFA8, "loopbacks", "high", "NET_GetLoopPacket / NET_SendLoopPacket: per-socket 12312-byte rings "
     "of 4 x 3076-byte messages (Q3)"),
    (0x829BB210, "time_frontend", "high", "re.EndFrame(&time_frontend, ...) in SCR_UpdateScreen; "
     "Com_Frame's com_speeds report (Q3)"),
    (0x829BB20C, "time_backend", "high", "Com_Frame's com_speeds report next to time_frontend (Q3)"),
    (0x829F1040, "pm", "high", "the current pmove_t*: PM_AddEvent and PM_AddTouchEnt read pm->ps and "
     "pm->numtouch (Q3)"),
    (0x82A1B190, "eventQue", "high", "Sys_GetEvent / Sys_QueEvent's 64 x 24-byte event ring (Q3 win_main.c)"),
    (0x82A158F4, "eventHead", "high", "Sys_QueEvent's write counter (Q3)"),
    (0x82A1B8CC, "eventTail", "high", "Sys_GetEvent's read counter (Q3)"),
    (0x829BB218, "com_pushedEvents", "high", "Com_PushEvent's 64 x 24-byte ring; Com_Init clears its 0x600 "
     "bytes (Q3)"),
    (0x82A2A104, "com_pushedEventsHead", "high", "Com_PushEvent's write counter (Q3)"),
    (0x82A2A108, "com_pushedEventsTail", "high", "Com_PushEvent's overflow counter / Com_GetEvent's read "
     "side (Q3)"),
    # --- end structural names ---
]

# Xbox kernel calls that XAPI (kernel32) wraps one-for-one.
XAPI_WRAPPERS = {
    "NtClose": "CloseHandle", "NtResumeThread": "ResumeThread", "NtSuspendThread": "SuspendThread",
    "NtReleaseMutant": "ReleaseMutex", "NtSetEvent": "SetEvent", "NtClearEvent": "ResetEvent",
    "NtPulseEvent": "PulseEvent", "NtReleaseSemaphore": "ReleaseSemaphore",
    "NtDuplicateObject": "DuplicateHandle", "NtQueueApcThread": "QueueUserAPC",
    "KeQueryPerformanceFrequency": "QueryPerformanceFrequency",
}


def pass_curated(prog: Program, syms: SymbolTable, warnings: list):
    for a, name, conf, ev in CURATED_FUNCTIONS:
        if a not in prog.start_set:
            warnings.append(f"curated function {name} 0x{a:08X} is not a function start here")
            continue
        syms.add(a, name, conf, ev, "curated")
    for a, name, conf, ev in CURATED_GLOBALS:
        syms.add(a, name, conf, ev, "curated", kind="global")


def pass_xapi(prog: Program, syms: SymbolTable, thunks: dict, report: dict):
    """BaseSetLastNTError (maps an NTSTATUS with RtlNtStatusToDosError) and the
    small XAPI functions that only call one kernel import (and it)."""
    by_name = {n: a for a, n in thunks.items()}
    base_err = None
    rtl = by_name.get("RtlNtStatusToDosError")
    if rtl:
        cands = [f for f, c in prog.callers.get(rtl, []) if prog.info[f].ninsn <= 16 and
                 len(prog.info[f].calls) == 1]
        if len(set(cands)) == 1:
            base_err = cands[0]
            syms.add(base_err, "BaseSetLastNTError", "high", "only calls RtlNtStatusToDosError and stores "
                     "the result as the thread's last error; called by the XAPI wrappers", "imports")
    named = 0
    for imp, api in XAPI_WRAPPERS.items():
        t = by_name.get(imp)
        if not t:
            continue
        cands = []
        for f, c in prog.callers.get(t, []):
            fi = prog.info[f]
            others = {x.target for x in fi.calls} - {t, base_err}
            if fi.ninsn <= 24 and not others and not fi.indirect:
                cands.append(f)
        if len(set(cands)) == 1:
            if syms.add(cands[0], api, "medium", f"XAPI wrapper: only calls {imp}" +
                        (" and BaseSetLastNTError" if base_err else ""), "imports"):
                named += 1
    report["xapi"] = named


# ---------------------------------------------------------------------------
# Pass: kernel / XAM imports
# ---------------------------------------------------------------------------


def read_imports(path):
    """xex_imports.py output: 'ADDRESS func|var library!name' per line."""
    out = []
    if not path or not Path(path).is_file():
        return out
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split()
        if len(parts) == 3 and "!" in parts[2]:
            lib, name = parts[2].split("!", 1)
            out.append((int(parts[0], 16), parts[1], lib, name))
    return out


def pass_imports(prog: Program, syms: SymbolTable, imports):
    thunks = {}
    funcs = {name for a, k, lib, name in imports if k == "func"}
    for a, kind, lib, name in imports:
        if not IDENT.match(name) or name.startswith("ord_"):
            continue
        if kind == "func":
            thunks[a] = name
            syms.add(a, f"__imp__{name}", "known", f"import thunk {lib}!{name} (codegen's name)", "imports")
        elif name not in funcs:
            syms.add(a, f"__imp__{name}", "known", f"imported variable {lib}!{name}", "imports", kind="global")
    return thunks


# ---------------------------------------------------------------------------
# Pass: compiler register save/restore helpers
# ---------------------------------------------------------------------------


def pass_save_restore_helpers(prog: Program, syms: SymbolTable):
    """MSVC's out-of-line prologue/epilogue helpers. Each register has its own
    entry point into one run of stores (or loads) that ends in blr:
        std r14,-152(r1) ... std r31,-16(r1); stw r12,-8(r1); blr   __savegprlr_14..31
        ld  r14,-152(r1) ... lwz r12,-8(r1); mtlr r12; blr           __restgprlr_14..31
        stfd f14,-144(r12) ... blr / lfd ...                        __savefpr_14..31 / __restfpr_
        li r11,-288; stvx v14,r11,r12 ... blr / lvx ...              __savevmx_14..31 / __restvmx_
    (and the VMX128 runs for v64..v127, same shape)."""
    img = prog.img
    found = 0

    def name(addr, label, reg):
        nonlocal found
        if syms.add(addr, f"{label}_{reg}", "high", f"register save/restore helper ({label} r{reg})"
                    if "gpr" in label else f"register save/restore helper ({label} {reg})",
                    "helpers"):
            found += 1

    for lo, hi in img.code:
        pc = lo
        while pc < hi:
            w = img.u32(pc)
            op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            # GPR runs: std/ld rN, d(r1) for N = 14..31
            if op in (62, 58) and ra == 1 and rd == 14 and w & 3 == 0:
                regs = []
                p = pc
                while True:
                    x = img.u32(p)
                    if x >> 26 == op and (x >> 16) & 31 == 1 and x & 3 == 0 and \
                            (x >> 21) & 31 == 14 + len(regs):
                        regs.append(p)
                        p += 4
                    else:
                        break
                tail = [img.u32(p + 4 * i) for i in range(3)]
                if len(regs) == 18 and op == 62 and tail[0] == 0x9181FFF8 and tail[1] == BLR:
                    for i, a in enumerate(regs):
                        name(a, "__savegprlr", 14 + i)
                elif len(regs) == 18 and op == 58 and tail[0] == 0x8181FFF8 and tail[1] == 0x7D8803A6 \
                        and tail[2] == BLR:
                    for i, a in enumerate(regs):
                        name(a, "__restgprlr", 14 + i)
                pc = p
                continue
            # FPR runs: stfd/lfd fN, d(r12) for N = 14..31
            if op in (54, 50) and ra == 12 and rd == 14:
                regs = []
                p = pc
                while img.u32(p) >> 26 == op and (img.u32(p) >> 16) & 31 == 12 and \
                        (img.u32(p) >> 21) & 31 == 14 + len(regs):
                    regs.append(p)
                    p += 4
                if len(regs) == 18 and img.u32(p) == BLR:
                    for i, a in enumerate(regs):
                        name(a, "__savefpr" if op == 54 else "__restfpr", 14 + i)
                pc = p
                continue
            # VMX runs: li r11,-16*k ; stvx/lvx vN,r11,r12 (or the VMX128 forms)
            if op == 14 and rd == 11 and ra == 0 and _simm(w) < 0 and _simm(w) % 16 == 0:
                pairs = []
                p = pc
                while True:
                    li, mem = img.u32(p), img.u32(p + 4)
                    if li >> 16 != 0x3960 or _simm(li) >= 0 or _simm(li) % 16:
                        break
                    if mem >> 26 not in (4, 31) or (mem >> 16) & 31 != 11 or (mem >> 11) & 31 != 12:
                        break
                    pairs.append((p, _simm(li), mem))
                    p += 8
                if len(pairs) >= 8 and img.u32(p) == BLR and all(
                        b[1] - a[1] == 16 for a, b in zip(pairs, pairs[1:])) and pairs[-1][1] == -16:
                    mem = pairs[0][2]
                    if mem >> 26 == 31:
                        store = (mem >> 1) & 0x3FF == 231            # stvx (lvx is 103)
                        top = 32
                    else:
                        store = (mem & 0x7F0) == 0x1C0 or (mem & 0x3F0) == 0x1C0  # stvx128 vs lvx128
                        top = 128
                    for a, off, m in pairs:
                        name(a, "__savevmx" if store else "__restvmx", top + off // 16)
                    pc = p
                    continue
            pc += 4
    return found


# ---------------------------------------------------------------------------
# Pass: strings that name the function using them
# ---------------------------------------------------------------------------
# "CG_ParseServerinfo: bad ...", "CM_LoadMap( %s, %i )", "SetCamera() requires ...":
# a function name followed by ':' or '('. A prefix only counts when exactly one
# function uses it, and that function uses no other such prefix (a second one
# would mean an inlined callee).

SELF_NAME = re.compile(r"^[\x14\x15\s]*(?:\^\d)?\s*(?:(?:ERROR|WARNING|Warning|Error)\s*:\s*)?"
                       r"((?:[A-Za-z_][A-Za-z0-9_]*::)*~?[A-Za-z_][A-Za-z0-9_]*)\s*(\(|:(?!:))")


def self_name_prefix(text):
    """(name, confidence) when text starts with something that reads as a
    function name, else None. Q3-style names (CG_Foo), C++ names and calls
    written with '(' are high; a bare CamelCase word before ':' (it may be a
    tag rather than the function's name) is medium."""
    m = SELF_NAME.match(text)
    if not m:
        return None
    p = m.group(1)
    if p.startswith("~") or "::~" in p:
        return None
    last = p.split("::")[-1]
    if not re.search(r"[a-z]", last):
        return None                           # ALL_CAPS: a constant, not a function
    if "_" in last.strip("_") or "::" in p:
        return p, "high"
    if re.match(r"^[A-Z][a-z0-9]+[A-Z]", last):
        return p, ("high" if m.group(2) == "(" else "medium")
    return None


def pass_self_naming(prog: Program, syms: SymbolTable, report: dict, q3_owners=None):
    """q3_owners (string -> Q3 functions using it) vetoes prefixes that Quake III
    itself prints from a function of another name ("ShutdownGame:" is printed
    by G_ShutdownGame)."""
    users = collections.defaultdict(set)
    texts = collections.defaultdict(list)
    conf = {}
    for f, fi in prog.info.items():
        for pc, a, s in prog.strings_of(f):
            hit = self_name_prefix(s)
            if not hit:
                continue
            p, c = hit
            if q3_owners and s.lstrip("\x14\x15") in q3_owners and p not in q3_owners[s.lstrip("\x14\x15")]:
                continue
            users[p].add(f)
            texts[(p, f)].append(s)
            if conf.get(p) != "high":
                conf[p] = c
    per_func = collections.defaultdict(set)
    for p, fs in users.items():
        for f in fs:
            per_func[f].add(p)
    named = 0
    for p, fs in sorted(users.items()):
        if len(fs) != 1:
            continue
        f = next(iter(fs))
        if len(per_func[f]) != 1 or f not in prog.start_set:
            continue
        sample = texts[(p, f)][0].strip("\x14\x15 \n").replace("\n", " ")
        if len(sample) > 60:
            sample = sample[:57] + "..."
        if syms.add(f, p, conf[p], f"string '{sample}' (only user)", "self-naming"):
            named += 1
    report["self_named"] = named


# ---------------------------------------------------------------------------
# Pass: Quake III string fingerprints
# ---------------------------------------------------------------------------


def read_q3_source(root):
    """{function name: (set of string literals, set of called names)} from the
    Quake III Arena GPL source (engine, game, cgame and the win32 platform
    layer; tools, UI, bot library and the other platforms left out)."""
    root = Path(root)
    skip = ("bspc", "q3map", "q3radiant", "q3asm", "q3cpp", "lcc", "tools", "macosx", "splines", "jpeg",
            "ui", "q3_ui", "botlib", "unix", "null")
    funcs = {}
    for path in sorted(root.glob("code/**/*.c")):
        rel = path.relative_to(root / "code").parts
        if rel[0] in skip:
            continue
        text = path.read_text(encoding="latin-1")
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text)
        for m in re.finditer(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*\{", text, re.M):
            name = m.group(1)
            if name in ("if", "while", "for", "switch", "return", "sizeof"):
                continue
            depth, i = 0, m.end() - 1
            for j in range(i, len(text)):
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
            body = text[i:j + 1]
            lits = {bytes(x, "latin-1").decode("unicode_escape", "replace")
                    for x in re.findall(r'"((?:[^"\\]|\\.)*)"', body)}
            calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body))
            old = funcs.get(name)
            funcs[name] = (lits | old[0], calls | old[1]) if old else (lits, calls)
    return funcs


def _message_like(s):
    """A string unlikely to occur by chance: a sentence or format, not a key."""
    return len(s) >= 8 and (" " in s.strip() or "%" in s or "\n" in s or re.search(r"[^\w./\\-]", s))


def q3_string_owners(q3):
    owners = collections.defaultdict(set)
    for name, (lits, calls) in q3.items():
        for s in lits:
            owners[s].add(name)
    return owners


def pass_q3_strings(prog: Program, syms: SymbolTable, q3, report: dict):
    """Name a function after the Quake III function whose strings it uses.

    Strong votes come from message-like strings found in exactly one Q3
    function; when a function has none, identifier-like strings (cvar names,
    keys) vote instead but need four of them. Several voted Q3 functions are
    resolved only when one of them calls the others in Q3 (they were inlined
    into it); a Q3 name chosen by two CoD3 functions is given to neither."""
    owners = q3_string_owners(q3)

    def reaches(a, b, depth=3):
        frontier, seen = {a}, {a}
        for _ in range(depth):
            frontier = {c for x in frontier for c in q3.get(x, ((), ()))[1] if c in q3} - seen
            if b in frontier:
                return True
            seen |= frontier
        return False

    picks = {}
    for f in prog.info:
        strong = collections.defaultdict(set)
        weak = collections.defaultdict(set)
        for pc, a, s in prog.strings_of(f):
            s = s.lstrip("\x14\x15")           # CoD's error class marker; Q3 passes ERR_* instead
            o = owners.get(s)
            if not o or len(o) != 1:
                continue
            q = next(iter(o))
            (strong if _message_like(s) else weak)[q].add(s)
        votes = strong or {q: ss for q, ss in weak.items() if len(ss) >= 4}
        if not votes:
            continue
        cands = list(votes)
        if len(cands) > 1:
            tops = [c for c in cands if all(c == d or reaches(c, d) for d in cands)]
            if len(tops) != 1:
                continue
            q = tops[0]
        else:
            q = cands[0]
        n = sum(len(v) for v in votes.values())
        if strong:
            longest = max(len(s) for s in votes[q])
            conf = "high" if len(votes[q]) >= 2 else ("medium" if longest >= 12 else None)
        else:
            conf = "medium"
        if conf is None:
            continue
        sample = sorted(votes[q], key=len)[-1].strip().replace("\n", " ")
        if len(sample) > 50:
            sample = sample[:47] + "..."
        kind = "strings" if strong else "cvar/key names"
        picks.setdefault(q, []).append((f, conf, f"Q3 {q}: {n} {kind} in common, e.g. '{sample}'"))
    named = 0
    for q, lst in sorted(picks.items()):
        if len(lst) != 1:
            continue
        f, conf, ev = lst[0]
        if syms.add(f, q, conf, ev, "q3-strings"):
            named += 1
    report["q3_named"] = named


# ---------------------------------------------------------------------------
# MSVC RTTI
# ---------------------------------------------------------------------------
# Type descriptor:          {vtable of type_info, spare, ".?AVName@@\0"}
# Complete object locator:  {signature 0, offset, cdOffset, type descriptor*, hierarchy descriptor*}
# Hierarchy descriptor:     {signature, attributes, numBaseClasses, base class array*}
# Base class descriptor:    {type descriptor*, numContainedBases, mdisp, pdisp, vdisp, attributes}
# A vtable is preceded by a pointer to its locator.

_MS_BASIC = {"C": "signed char", "D": "char", "E": "unsigned char", "F": "short", "G": "unsigned short",
             "H": "int", "I": "unsigned int", "J": "long", "K": "unsigned long", "M": "float",
             "N": "double", "O": "long double", "X": "void", "_N": "bool", "_J": "__int64",
             "_K": "unsigned __int64"}


def demangle_type_name(mangled: str) -> str:
    """'.?AV?$Foo@VBar@ns@@M@@' -> 'Foo<ns::Bar,float>' (the subset MSVC uses for RTTI names)."""
    s = mangled[4:] if mangled.startswith((".?AV", ".?AU")) else mangled

    def name(pos, backrefs):
        """A qualified name ending in '@' (the outer '@' of the list): returns (text, pos)."""
        parts = []
        while pos < len(s) and s[pos] != "@":
            if s[pos].isdigit():
                parts.append(backrefs[int(s[pos])] if int(s[pos]) < len(backrefs) else "?")
                pos += 1
            elif s.startswith("?$", pos):
                inner_refs = []
                end = s.index("@", pos + 2)
                tname = s[pos + 2:end]
                inner_refs.append(tname)
                args, pos = template_args(end + 1, inner_refs)
                text = f"{tname}<{','.join(args)}>"
                parts.append(text)
                backrefs.append(text)
            else:
                end = s.index("@", pos)
                frag = s[pos:end]
                parts.append(frag)
                backrefs.append(frag)
                pos = end + 1
        return "::".join(reversed(parts)), pos + 1

    def template_args(pos, backrefs):
        args = []
        while pos < len(s) and s[pos] != "@":
            t, pos = type_(pos, backrefs)
            args.append(t)
        return args, pos + 1

    def type_(pos, backrefs):
        c = s[pos]
        if c in "VUW":
            n, p = name(pos + 1, backrefs)
            return n, p
        if c == "P" and pos + 1 < len(s) and s[pos + 1] == "A":
            t, p = type_(pos + 2, backrefs)
            return t + "*", p
        if c == "_" and s[pos:pos + 2] in _MS_BASIC:
            return _MS_BASIC[s[pos:pos + 2]], pos + 2
        if c == "$" and s[pos:pos + 2] == "$0":            # integer template argument
            return "N", pos + 3
        if c in _MS_BASIC:
            return _MS_BASIC[c], pos + 1
        return "?", pos + 1

    try:
        text, _ = name(0, [])
        return text
    except (ValueError, IndexError):
        return s.rstrip("@")


def c_ident(cpp_name: str) -> str:
    """A C identifier for a demangled class name: Foo<ns::Bar,float> -> Foo_ns_Bar_float."""
    s = cpp_name.replace("::", "_").replace("unsigned ", "u").replace("*", "Ptr")
    s = re.sub(r"[<>,\s]+", "_", s)
    return re.sub(r"_+", "_", s).strip("_")


class RttiClass:
    def __init__(self, td, mangled):
        self.td = td
        self.mangled = mangled
        self.cpp = demangle_type_name(mangled)
        self.ident = c_ident(self.cpp)
        self.bases = []          # ident of every class in the hierarchy, self first
        self.vtables = []        # (vtable address, locator offset, number of slots)


def read_rtti(prog: Program):
    """Classes (by type descriptor) and vtables (address -> (RttiClass, offset, slots))."""
    img = prog.img
    ptrs = prog.data_pointers()
    classes = {}
    for m in re.finditer(rb"\.\?A[VU][\x20-\x7e]{1,400}?@@\x00", img.data):
        td = img.base + m.start() - 8
        classes[td] = RttiClass(td, m.group()[:-1].decode("ascii"))
    locators = {}
    for td, cls in classes.items():
        for loc in ptrs.get(td, []):
            col = loc - 12
            if img.u32(col) != 0 or img.section_of(col) not in (".rdata", ".data"):
                continue
            locators[col] = cls
            chd = img.u32(col + 16)
            nbase, arr = img.u32(chd + 8), img.u32(chd + 12)
            if not cls.bases and 0 < nbase < 64:
                for i in range(nbase):
                    btd = img.u32(img.u32(arr + 4 * i))
                    cls.bases.append(classes[btd].ident if btd in classes else "?")
    vtables = {}
    for col, cls in locators.items():
        for ref in ptrs.get(col, []):
            vt = ref + 4
            n = 0            # slots run until the next word that is not a code address
            while img.in_code(img.u32(vt + 4 * n)):
                n += 1
            vtables[vt] = (cls, img.u32(col + 4), n)
            cls.vtables.append((vt, img.u32(col + 4), n))
    return classes, vtables


def _deleting_flag_test(img, f, end):
    """pc of 'clrlwi rX,r4,31' (flags & 1) near the start of f, or None."""
    for pc in range(f, min(f + 4 * 32, end), 4):
        w = img.u32(pc)
        if w >> 26 == 21 and (w >> 21) & 31 == 4 and (w & 0xFFFE) == (31 << 6 | 31 << 1):
            return pc
    return None


def _writes_r3(w):
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in (14, 15, 7, 8, 12, 13, 32, 33, 34, 35, 40, 41, 42, 43, 58) and rd == 3:
        return True
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30) and ra == 3:
        return True
    if op == 31:
        xo = (w >> 1) & 0x3FF
        return (ra == 3) if xo in _X_WRITES_RA else (rd == 3 and xo not in _X_NO_GPR)
    return False


def returns_this(prog, f):
    """Whether f hands its first argument back in r3 (what an MSVC constructor does)."""
    img, fi = prog.img, prog.info[f]
    words = [(pc, img.u32(pc)) for pc in range(f, fi.end, 4)]
    if not fi.calls and not fi.indirect:
        return not any(_writes_r3(w) for pc, w in words)
    saved = None
    for pc, w in words[:14]:
        if w >> 26 == 31 and (w >> 1) & 0x3FF == 444 and (w >> 21) & 31 == 3 and (w >> 11) & 31 == 3:
            saved = (w >> 16) & 31
            break
    if saved is None:
        return False
    for pc, w in words[-12:]:
        if w >> 26 == 31 and (w >> 1) & 0x3FF == 444 and (w >> 16) & 31 == 3 and \
                (w >> 21) & 31 == saved and (w >> 11) & 31 == saved:
            return True
    return False


# Virtual methods whose meaning we know from one class, by the class that
# declares the slot. Slot numbers are vtable indices (byte offset / 4).
KNOWN_SLOTS = {
    # Front-end menus (FEMenu): from the Options > Gameplay page hooks (src/game/menu_fov.cpp).
    ("FEMenu", 0): ("Init", "Options > Gameplay vtbl[0] Init(this, panel)"),
    ("FEMenu", 16): ("Update", "Options > Gameplay vtbl[16] Update(this, dt)"),
    ("FEMenu", 21): ("Refresh", "Options > Gameplay vtbl[21] Refresh(this)"),
    ("FEMenu", 25): ("MoveUp", "Options > Gameplay vtbl[25] move selection up"),
    ("FEMenu", 26): ("MoveDown", "Options > Gameplay vtbl[26] move selection down"),
    ("FEMenu", 27): ("OnLeft", "Options > Gameplay vtbl[27] = Toggle(this, 0)"),
    ("FEMenu", 28): ("OnRight", "Options > Gameplay vtbl[28] = Toggle(this, 1)"),
}


def pass_rtti(prog: Program, syms: SymbolTable, report: dict):
    img = prog.img
    classes, vtables = read_rtti(prog)
    report["rtti_classes"] = len(classes)
    report["rtti_vtables"] = len(vtables)
    by_ident = {c.ident: c for c in classes.values()}

    # The vtables themselves are globals.
    for vt, (cls, off, n) in sorted(vtables.items()):
        label = f"{cls.ident}::vftable" + (f"_{off}" if off else "")
        syms.add(vt, label, "high", f"rtti '{cls.cpp}' vtable, {n} slots"
                 + (f", offset {off}" if off else ""), "rtti", kind="global")

    # Where each function sits: [(class, slot, vtable)] (primary vtables only).
    occ = collections.defaultdict(list)
    for vt, (cls, off, n) in vtables.items():
        if off:
            continue
        for i in range(n):
            occ[img.u32(vt + 4 * i)].append((cls, i, vt))

    def owner(classes_):
        """The class among these that every other one derives from."""
        idents = {c.ident for c in classes_}
        cands = [c for c in classes_ if all(c.ident in by_ident[d].bases for d in idents)]
        return cands[0] if cands else None

    def stored_vtable(f, first=True):
        """Class of the first (or last) vtable f stores at offset 0 of an object."""
        st = [v for pc, v, ra, d in prog.info[f].const_stores
              if v in vtables and d == 0 and not vtables[v][1]]
        if not st:
            return None
        return vtables[st[0] if first else st[-1]][0]

    named = collections.Counter()
    delete_calls = collections.Counter()
    dtors = {}          # function -> (class, evidence)
    # 1. scalar deleting destructors: (this, flags) ... if (flags & 1) operator delete(this)
    for f, places in occ.items():
        fi = prog.info.get(f)
        if not fi or fi.ninsn > 80:
            continue
        test = _deleting_flag_test(img, f, fi.end)
        if test is None:
            continue
        # The class whose vtable holds it; when identical destructors were folded
        # into one, the class whose vtable it stores (or their common base).
        holders = {c.ident: c for c, i, vt in places}
        stored = stored_vtable(f)
        if len(holders) == 1:
            cls = next(iter(holders.values()))
        elif stored is not None and stored.ident in holders:
            cls = stored
        else:
            cls = owner(list(holders.values()))
        if cls is None:
            continue
        slots = sorted({i for c, i, vt in places})
        if syms.add(f, f"{cls.ident}::deleting_dtor", "high",
                    f"rtti {cls.cpp} vtable slot {slots[0]}: (this, flags) with the flags & 1 delete test",
                    "rtti"):
            named["deleting_dtor"] += 1
        before = [c for c in fi.calls if c.pc < test]
        after = [c for c in fi.calls if c.pc > test]
        if after:
            delete_calls[after[0].target] += 1
        if before:
            d = before[0].target
            if d in prog.info:
                dcls = stored_vtable(d)
                if dcls is None:
                    if stored is None:
                        dcls = cls                            # the destructor itself was not inlined
                    elif len(stored.bases) > 1:
                        dcls = by_ident.get(stored.bases[1])  # the inlined body calls its base's destructor
                if dcls is not None:
                    dtors.setdefault(d, (dcls, f"called first by {cls.ident}::deleting_dtor (0x{f:08X})"))
    for d, (dcls, ev) in dtors.items():
        if syms.add(d, f"{dcls.ident}::dtor", "high", f"rtti: {ev}", "rtti"):
            named["dtor"] += 1
    # operator delete: the call under the flags test, when most deleting destructors agree.
    if delete_calls:
        target, n = delete_calls.most_common(1)[0]
        if n >= 10:
            syms.add(target, "operator_delete", "high",
                     f"called under the (flags & 1) test by {n} scalar deleting destructors", "rtti")

    # 2. constructors and destructors from the order of their vtable stores.
    # A constructor runs its bases' constructors first, so it stores the base
    # vtables and then its own (base -> derived); a destructor stores its own
    # vtable first and its bases' after (derived -> base). An object fresh from
    # operator new is always handed to a constructor.
    def this_regs(f):
        """r3 and the register the prologue copies it to."""
        out = {3}
        for pc in range(f, min(f + 56, prog.end_of[f]), 4):
            w = img.u32(pc)
            if w >> 26 == 31 and (w >> 1) & 0x3FF == 444 and (w >> 21) & 31 == 3 and (w >> 11) & 31 == 3:
                out.add((w >> 16) & 31)
                break
        return out

    def vseq(f):
        seq = []
        mine = this_regs(f)
        for pc, v, ra, d in prog.info[f].const_stores:
            if v in vtables and d == 0 and not vtables[v][1] and ra in mine:
                c = vtables[v][0]
                if not seq or seq[-1] is not c:
                    seq.append(c)
        return seq

    def chain(seq, derived_last):
        pairs = zip(seq, seq[1:])
        if derived_last:
            return all(a.ident in b.bases for a, b in pairs)
        return all(b.ident in a.bases for a, b in pairs)

    news = collections.Counter()
    for f, fi in prog.info.items():
        for c in fi.calls:
            r3 = c.args.get(3)
            if isinstance(r3, tuple) and c.target in prog.info and vseq(c.target):
                news[r3[1]] += 1
    allocators = {a for a, n in news.items() if n >= 3}
    if news:
        a, n = news.most_common(1)[0]
        if n >= 10:
            syms.add(a, "operator_new", "high", f"its result goes straight to {n} constructors "
                     "(functions that store a vtable into it)", "rtti")
    fresh = set()
    for f, fi in prog.info.items():
        for c in fi.calls:
            r3 = c.args.get(3)
            if isinstance(r3, tuple) and r3[1] in allocators:
                fresh.add(c.target)
    ctors = collections.defaultdict(list)
    for f in prog.info:
        if syms.get(f) or f in occ:
            continue
        seq = vseq(f)
        if not seq:
            continue
        if f in fresh and chain(seq, True):
            ctors[seq[-1]].append((f, "high", "called on the result of operator new"))
        elif len(seq) >= 2 and chain(seq, True) and prog.info[f].ninsn <= 80 and returns_this(prog, f):
            ctors[seq[-1]].append((f, "medium", "stores base then own vtables and returns this"))
        elif len(seq) >= 2 and chain(seq, False) and f not in fresh:
            if syms.add(f, f"{seq[0].ident}::dtor", "high",
                        f"rtti: stores the {seq[0].cpp} vtable, then its base's ({seq[1].cpp})", "rtti"):
                named["dtor"] += 1
    for cls, lst in ctors.items():
        for k, (f, conf, why) in enumerate(sorted(lst)):
            name = f"{cls.ident}::ctor" + (f"_{k + 1}" if k else "")
            if syms.add(f, name, conf, f"rtti: stores the {cls.cpp} vtable at this+0; {why}", "rtti"):
                named["ctor"] += 1

    # 3. every other virtual method: the class that declares the slot
    for f, places in sorted(occ.items()):
        if syms.get(f) or f not in prog.start_set:
            continue
        slots = {i for c, i, vt in places}
        if len(slots) != 1:
            continue
        slot = slots.pop()
        cls = owner([c for c, i, vt in places])
        if cls is None:
            continue
        sem = None
        for b in cls.bases:
            if (b, slot) in KNOWN_SLOTS:
                sem = KNOWN_SLOTS[(b, slot)]
                break
        if sem:
            if syms.add(f, f"{cls.ident}::{sem[0]}", "medium",
                        f"rtti {cls.cpp} vtable slot {slot}; meaning from {sem[1]}", "rtti"):
                named["semantic"] += 1
        elif syms.add(f, f"{cls.ident}::vfunc_{slot}", "high",
                      f"rtti {cls.cpp} vtable 0x{places[0][2]:08X} slot {slot}"
                      + (f" (also in {len(places) - 1} derived vtables)" if len(places) > 1 else ""), "rtti"):
            named["vfunc"] += 1

    # 4. global objects: a vtable stored into a fixed address
    objs = collections.defaultdict(set)
    for f, fi in prog.info.items():
        for pc, v, a in fi.global_const_stores:
            if v in vtables and not vtables[v][1]:
                objs[vtables[v][0]].add(a)
    # an address can be stored several vtables (a constructor chain): keep the most derived
    at = collections.defaultdict(set)
    for cls, addrs in objs.items():
        for a in addrs:
            at[a].add(cls)
    per_class = collections.defaultdict(list)
    for a, clss in sorted(at.items()):
        per_class[max(clss, key=lambda c: len(c.bases))].append(a)
    for cls, addrs in per_class.items():
        if len(addrs) != 1:
            continue                         # arrays of objects: not worth a name each
        a = addrs[0]
        if syms.add(a, f"g_{cls.ident}", "high", f"rtti: the {cls.cpp} vtable is stored at this address "
                    "(a global object)", "rtti", kind="global"):
            named["global_object"] += 1
    report["rtti_named"] = dict(named)
    return classes, vtables


# ---------------------------------------------------------------------------
# Pass: console commands
# ---------------------------------------------------------------------------
# Quake III's handler for each command name CoD3 also has, by module (e engine,
# cg cgame, g game), from Cmd_AddCommand calls, cgame's command table and
# g_cmds.c's ClientCommand in the Quake III Arena GPL source.
Q3_COMMANDS = {
    ("+attack", "e"): "IN_Button0Down", ("+back", "e"): "IN_BackDown", ("+forward", "e"): "IN_ForwardDown",
    ("+left", "e"): "IN_LeftDown", ("+lookdown", "e"): "IN_LookdownDown", ("+lookup", "e"): "IN_LookupDown",
    ("+mlook", "e"): "IN_MLookDown", ("+movedown", "e"): "IN_DownDown",
    ("+moveleft", "e"): "IN_MoveleftDown", ("+moveright", "e"): "IN_MoverightDown",
    ("+moveup", "e"): "IN_UpDown", ("+right", "e"): "IN_RightDown", ("+scores", "cg"): "CG_ScoresDown_f",
    ("+speed", "e"): "IN_SpeedDown", ("+strafe", "e"): "IN_StrafeDown", ("-attack", "e"): "IN_Button0Up",
    ("-back", "e"): "IN_BackUp", ("-forward", "e"): "IN_ForwardUp", ("-left", "e"): "IN_LeftUp",
    ("-lookdown", "e"): "IN_LookdownUp", ("-lookup", "e"): "IN_LookupUp", ("-mlook", "e"): "IN_MLookUp",
    ("-movedown", "e"): "IN_DownUp", ("-moveleft", "e"): "IN_MoveleftUp",
    ("-moveright", "e"): "IN_MoverightUp", ("-moveup", "e"): "IN_UpUp", ("-right", "e"): "IN_RightUp",
    ("-scores", "cg"): "CG_ScoresUp_f", ("-speed", "e"): "IN_SpeedUp", ("-strafe", "e"): "IN_StrafeUp",
    ("bind", "e"): "Key_Bind_f", ("bindlist", "e"): "Key_Bindlist_f", ("camera", "cg"): "CG_Camera_f",
    ("centerview", "e"): "IN_CenterView", ("cinematic", "e"): "CL_PlayCinematic_f",
    ("clear", "e"): "Con_Clear_f", ("cmd", "e"): "CL_ForwardToServer_f", ("cmdlist", "e"): "Cmd_List_f",
    ("crash", "e"): "Com_Crash_f", ("cvar_restart", "e"): "Cvar_Restart_f", ("devmap", "e"): "SV_Map_f",
    ("dir", "e"): "FS_Dir_f", ("disconnect", "e"): "CL_Disconnect_f", ("echo", "e"): "Cmd_Echo_f",
    ("error", "e"): "Com_Error_f", ("exec", "e"): "Cmd_Exec_f", ("fdir", "e"): "FS_NewDir_f",
    ("freeze", "e"): "Com_Freeze_f", ("gfxinfo", "e"): "GfxInfo_f", ("give", "g"): "Cmd_Give_f",
    ("god", "g"): "Cmd_God_f", ("imagelist", "e"): "R_ImageList_f", ("kill", "g"): "Cmd_Kill_f",
    ("levelshot", "g"): "Cmd_LevelShot_f", ("map", "e"): "SV_Map_f", ("map_restart", "e"): "SV_MapRestart_f",
    ("model", "e"): "CL_SetModel_f", ("modelist", "e"): "R_ModeList_f", ("modellist", "e"): "R_Modellist_f",
    ("music", "e"): "S_Music_f", ("noclip", "g"): "Cmd_Noclip_f", ("notarget", "g"): "Cmd_Notarget_f",
    ("path", "e"): "FS_Path_f", ("quit", "e"): "Com_Quit_f", ("reset", "e"): "Cvar_Reset_f",
    ("screenshot", "e"): "R_ScreenShot_f", ("set", "e"): "Cvar_Set_f", ("seta", "e"): "Cvar_SetA_f",
    ("setenv", "e"): "CL_Setenv_f", ("sets", "e"): "Cvar_SetS_f", ("setviewpos", "g"): "Cmd_SetViewpos_f",
    ("shaderlist", "e"): "R_ShaderList_f", ("sizedown", "cg"): "CG_SizeDown_f",
    ("sizeup", "cg"): "CG_SizeUp_f", ("skinlist", "e"): "R_SkinList_f",
    ("snd_restart", "e"): "CL_Snd_Restart_f", ("spmap", "e"): "SV_Map_f", ("stats", "g"): "Cmd_Stats_f",
    ("toggle", "e"): "Cvar_Toggle_f", ("unbind", "e"): "Key_Unbind_f", ("unbindall", "e"): "Key_Unbindall_f",
    ("vid_restart", "e"): "CL_Vid_Restart_f", ("viewpos", "cg"): "CG_Viewpos_f", ("vstr", "e"): "Cmd_Vstr_f",
    ("wait", "e"): "Cmd_Wait_f", ("weapnext", "cg"): "CG_NextWeapon_f", ("weapon", "cg"): "CG_Weapon_f",
    ("weapprev", "cg"): "CG_PrevWeapon_f", ("where", "g"): "Cmd_Where_f",
    ("writeconfig", "e"): "Com_WriteConfig_f",
}


def camel(text):
    return "".join(p[:1].upper() + p[1:] for p in re.split(r"[^A-Za-z0-9]+", text) if p)


def command_handler_name(cmd, module, prefix, free=lambda name: True):
    """Q3's name for the command's handler (unless another handler already has
    it: CoD3 split some of Q3's shared handlers), else a Q3-style one."""
    key = cmd.lower()
    for mod in (module, "e", "cg", "g"):
        if (key, mod) in Q3_COMMANDS and free(Q3_COMMANDS[(key, mod)]):
            return Q3_COMMANDS[(key, mod)], True
    if cmd[:1] in "+-" and len(cmd) > 1:
        return f"IN_{camel(cmd[1:])}{'Down' if cmd[0] == '+' else 'Up'}", False
    if key.startswith(prefix.lower() + "_"):
        cmd = cmd[len(prefix) + 1:]            # cg_shellshock -> CG_Shellshock_f
    return f"{prefix}_{camel(cmd)}_f", False


def find_name_tables(prog: Program):
    """Static {const char* name, function} arrays (8-byte entries, at least 4).
    Returns [(table address, [(name, function)], name_first)]."""
    img = prog.img
    ident = re.compile(r"^[A-Za-z_+\-][\w+\-. ]*$")
    out = []
    for n, lo, hi, ch in img.sections:
        if ch & 0x20000000 or n in (".pdata", ".reloc"):
            continue
        a = lo
        while a < hi - 8:
            entries = []
            b = a
            while b + 8 <= hi:
                s, f = img.cstr(img.u32(b)), img.u32(b + 4)
                if s and ident.match(s) and f in prog.start_set:
                    entries.append((s, f))
                    b += 8
                else:
                    break
            if len(entries) >= 4:
                if img.u32(a - 4) in prog.start_set:
                    # the pairs are really {function, name}: shift by one word
                    shifted = [(img.cstr(img.u32(x + 4)), img.u32(x)) for x in range(a - 4, b - 4, 8)]
                    if all(s for s, f in shifted):
                        out.append((a - 4, shifted, False))
                else:
                    out.append((a, entries, True))
                a = b
            else:
                a += 4
    return out


def pass_commands(prog: Program, syms: SymbolTable, report: dict):
    """Handlers of Cmd_AddCommand-style calls and of static {name, handler} tables."""
    registrars = {}
    for name, kind in (("Cmd_AddCommand", "e"), ("Cmd_AddButtonCommand", "e"),
                       ("Cmd_AddServerCommand", "sv")):
        sym = syms.by_name.get(name)
        if sym:
            registrars[sym.address] = (name, kind)
    regs = []        # (command, handler, how it was registered, Q3 module, name prefix)
    for target, (rname, kind) in registrars.items():
        for f, c in prog.callers.get(target, []):
            name, func = c.args.get(3), c.args.get(4)
            if not (isinstance(name, int) and isinstance(func, int) and 3 in c.certain and 4 in c.certain):
                continue
            cmd = prog.img.cstr(name)
            if not cmd or func not in prog.start_set:
                continue
            owner = syms.name_of(f) or ""
            prefix = "SV" if kind == "sv" else (owner.split("_")[0] if "_" in owner else "Cmd")
            regs.append((cmd, func, f"{rname} from 0x{f:08X}", "e", prefix))
    tables = []
    for addr, entries, _ in find_name_tables(prog):
        names = {s.lower() for s, f in entries}
        known = [(k, m) for (k, m) in Q3_COMMANDS if k in names]
        if len({k for k, m in known}) < 2:
            continue                    # not a command table (shader toggles, XAM tasks...)
        hits = {m for k, m in known}
        module = "g" if "g" in hits else "cg" if "cg" in hits else "e"
        prefix = {"g": "Cmd", "cg": "CG", "e": "Cmd"}[module]
        tables.append(addr)
        for s, f in entries:
            regs.append((s, f, f"command table 0x{addr:08X}", module, prefix))
    by_handler = collections.defaultdict(list)
    for r in regs:
        by_handler[r[1]].append(r)
    named = 0
    for func, rs in sorted(by_handler.items()):
        if prog.img.u32(func) == BLR and len(prog.refby.get(func, [])) + len(
                prog.data_pointers().get(func, [])) > len(rs) + 1:
            continue                    # a shared empty function, not this command's own handler
        cmd, _, how, module, prefix = rs[0]
        name, from_q3 = command_handler_name(
            cmd, module, prefix, lambda n: n not in syms.by_name or syms.by_name[n].address == func)
        aliases = sorted({r[0] for r in rs} - {cmd})
        ev = f"cmd '{cmd}' ({how})" + (f", also '{', '.join(aliases)}'" if aliases else "")
        if from_q3:
            ev += "; Q3 handler name"
        if syms.add(func, name, "high", ev, "commands"):
            named += 1
    report["command_tables"] = [f"0x{t:08X}" for t in tables]
    report["commands_named"] = named


# ---------------------------------------------------------------------------
# Pass: asset type callbacks
# ---------------------------------------------------------------------------


def pass_asset_types(prog: Program, syms: SymbolTable, report: dict):
    """AssetType_Register(fourcc, version, load, unload, arg): name the load and
    unload callbacks after the type (the code is stored byte-reversed: 'HSEM'
    is MESH). The function registering the most types goes first, so a type
    registered twice gives the second pair a _2."""
    reg = syms.by_name.get("AssetType_Register")
    if not reg:
        return
    sites = collections.defaultdict(list)
    for f, c in prog.callers.get(reg.address, []):
        if not {3, 4, 5, 6} <= c.certain:
            continue
        code = c.args.get(3)
        raw = struct.pack(">I", code & 0xFFFFFFFF) if isinstance(code, int) else b""
        fourcc = raw.decode("latin-1")[::-1].rstrip("\0")
        if not re.match(r"^[A-Z0-9]{2,4}$", fourcc):
            continue
        sites[f].append((fourcc, c.args.get(4), c.args.get(5), c.args.get(6)))
    named = 0
    for f in sorted(sites, key=lambda f: (-len(sites[f]), f)):
        for fourcc, version, load, unload in sites[f]:
            for func, verb in ((load, "Load"), (unload, "Unload")):
                if isinstance(func, int) and func in prog.start_set and syms.add(
                        func, f"Asset{verb}_{fourcc}", "high",
                        f"{verb.lower()} callback of asset type {fourcc} v{version} "
                        f"(AssetType_Register from 0x{f:08X})", "assets"):
                    named += 1
    report["asset_callbacks"] = named


# ---------------------------------------------------------------------------
# Pass: dvar (cvar) globals
# ---------------------------------------------------------------------------


def find_vmcvar_tables(prog: Program):
    """Quake III style cvarTable_t arrays: {vmCvar_t*, name, default, flags, ...}
    with 16-byte (cgame) or 24-byte (game) entries. [(address, stride, entries)]."""
    img = prog.img
    dvar = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
    data = [(lo, hi) for n, lo, hi, ch in img.sections if n in (".data", ".rdata")]
    out = []
    for lo, hi in data:
        a = lo
        while a < hi - 32:
            best = None
            for stride in (16, 24):
                entries, b = [], a
                while b + stride <= hi:
                    var, name, default, flags = (img.u32(b + 4 * k) for k in range(4))
                    nm = img.cstr(name)
                    dflt = "" if img.base <= default < img.end and img.u32(default) >> 24 == 0 else \
                        img.cstr(default)
                    if nm and dvar.match(nm) and dflt is not None and flags < 0x10000 and \
                            (var == 0 or img.section_of(var) == ".data"):
                        entries.append((var, nm, dflt, flags))
                        b += stride
                    else:
                        break
                if len(entries) >= 10 and (best is None or len(entries) > len(best[2])):
                    best = (a, stride, entries)
            if best:
                out.append(best)
                a += best[1] * len(best[2])
            else:
                a += 4
    return out


C_KEYWORDS = {"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
              "enum", "extern", "float", "for", "goto", "if", "int", "long", "register", "return", "short",
              "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
              "volatile", "while", "name", "time", "index"}


def dvar_global_name(dvar):
    if not IDENT.match(dvar):
        return None
    return f"{dvar}_cvar" if dvar.lower() in C_KEYWORDS or "_" not in dvar else dvar


def pass_dvar_globals(prog: Program, syms: SymbolTable, report: dict):
    """cvar_t* globals (Cvar_Get's result stored to a fixed address) and the
    vmCvar_t globals of the cgame / game cvar tables, named Quake III style
    after their dvar (com_maxfps, cg_fov...; <dvar>_cvar when the name has no
    subsystem prefix; a module's vmCvar_t of a dvar that also has an engine
    cvar_t* gets _cg / _g)."""
    img = prog.img
    cvar_get = syms.by_name.get("Cvar_Get")
    stores = collections.defaultdict(set)      # address -> dvar names
    where = collections.defaultdict(set)
    if cvar_get:
        calls = {c.pc: c for f, c in prog.callers.get(cvar_get.address, [])}
        for f, fi in prog.info.items():
            for pc, target, addr, call_pc in fi.ret_stores:
                c = calls.get(call_pc)
                if target != cvar_get.address or c is None or 3 not in c.certain:
                    continue
                name = img.cstr(c.args.get(3)) if isinstance(c.args.get(3), int) else None
                if name:
                    stores[addr].add(name)
                    where[addr].add(f)
    n_ptr = 0
    for addr, names in sorted(stores.items()):
        if len({x.lower() for x in names}) != 1:
            continue                                # one global, several dvars: a scratch slot
        dvar = sorted(names)[0]
        gname = dvar_global_name(dvar)
        users = ", ".join(f"0x{f:08X}" for f in sorted(where[addr])[:2])
        if gname and syms.add(addr, gname, "high", f"cvar_t* of dvar '{dvar}' (Cvar_Get result stored by "
                              f"{users})", "dvars", kind="global"):
            n_ptr += 1
    n_vm = 0
    tables = []
    for base, stride, entries in find_vmcvar_tables(prog):
        module = "cgame" if stride == 16 else "game"
        tables.append(f"0x{base:08X} ({module}, {len(entries)} x {stride} bytes)")
        syms.add(base, "cvarTable" if module == "game" else "cg_cvarTable", "high",
                 f"{module} cvarTable_t[{len(entries)}] {{vmCvar_t*, name, default, flags"
                 + (", modificationCount, trackChange}" if stride == 24 else "}"), "dvars", kind="global")
        for var, dvar, default, flags in entries:
            if not var:
                continue
            gname = dvar_global_name(dvar)
            if not gname:
                continue
            if syms.get(var, "global"):
                continue
            if gname in syms.by_name:                # the engine's cvar_t* has the plain name
                gname = f"{dvar}_{'cg' if module == 'cgame' else 'g'}"
            if syms.add(var, gname, "high", f"vmCvar_t of dvar '{dvar}' ({module} cvarTable "
                        f"0x{base:08X})", "dvars", kind="global"):
                n_vm += 1
    report["cvar_pointers"] = n_ptr
    report["vmcvars"] = n_vm
    report["vmcvar_tables"] = tables


# ---------------------------------------------------------------------------
# Pass: small functions recognisable by their code alone
# ---------------------------------------------------------------------------


def pass_trivial(prog: Program, syms: SymbolTable, thunks: dict, report: dict):
    """The shared empty function, jumps to named functions and single-import wrappers."""
    img = prog.img
    named = collections.Counter()
    # The empty function the linker folded every '{}' into: the most referenced lone blr.
    empties = [f for f in prog.starts if img.u32(f) == BLR]
    if empties:
        refs = lambda f: len(prog.callers.get(f, [])) + len(prog.data_pointers().get(f, [])) + \
            len(prog.refby.get(f, []))
        top = max(empties, key=refs)
        if refs(top) >= 20:
            syms.add(top, "nullsub", "high", f"empty function (blr) shared by {refs(top)} calls and "
                     "pointers (identical-code folding)", "trivial")
            named["nullsub"] += 1
    # 'b X' as the whole function: a thunk for X (when X has only the one)
    thunk_of = collections.defaultdict(list)
    for f in prog.starts:
        if syms.get(f) or prog.end_of[f] - f > 8:
            continue
        w = img.u32(f)
        if w >> 26 == 18 and not w & 3:
            t, _ = branch_target(f, w)
            target = syms.get(t)
            if target and target.source != "trivial" and "::" not in target.name:
                thunk_of[target].append(f)
    for target, fs in thunk_of.items():
        base = thunks.get(target.address, target.cname)        # an import: its plain name
        if len(fs) == 1 and syms.add(fs[0], f"{base}_thunk", "medium", f"a lone 'b {target.name}'",
                                     "trivial"):
            named["thunk"] += 1
    # Thin wrappers: up to 12 instructions whose only call is one import (the only such wrapper of it).
    wrappers = collections.defaultdict(list)
    for f, fi in prog.info.items():
        if syms.get(f) or fi.ninsn > 12 or fi.indirect:
            continue
        targets = [c.target for c in fi.calls] + ([fi.tail] if fi.tail else [])
        if len(targets) == 1 and targets[0] in thunks:
            wrappers[thunks[targets[0]]].append(f)
    for imp, fs in wrappers.items():
        if len(fs) == 1 and syms.add(fs[0], f"{imp}_wrapper", "medium",
                                     f"{prog.info[fs[0]].ninsn}-instruction wrapper around the import {imp}",
                                     "imports"):
            named["import_wrapper"] += 1
    report["trivial"] = dict(named)


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def toml_str(s):
    out = []
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 32 or ord(ch) == 127:
            out.append(f"\\u{ord(ch):04X}")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def write_toml(path, syms: SymbolTable, header_lines):
    lines = ["# " + l if l else "#" for l in header_lines]
    for kind, items in (("function", syms.functions()), ("global", syms.globals())):
        for s in items:
            lines.append("")
            lines.append(f"[[{kind}]]")
            lines.append(f"address = 0x{s.address:08X}")
            lines.append(f"name = {toml_str(s.name)}")
            if s.cname != s.name:
                lines.append(f"cname = {toml_str(s.cname)}")
            lines.append(f"confidence = {toml_str(s.confidence)}")
            lines.append(f"evidence = {toml_str(s.evidence)}")
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_ppcx_names(path, syms: SymbolTable):
    """'ADDRESS NAME' lines, the format ppcx.py reads from <image>.names."""
    with open(path, "w", encoding="utf-8") as f:
        for s in syms.functions() + syms.globals():
            f.write(f"{s.address:08X} {s.cname}\n")


def check_listing(asm_path, prog: Program, limit=200000):
    """Spot-check that a text disassembly ('820a0004 bl      0x8234f298') is of
    this image: every direct call in its first lines must be one we decoded."""
    calls = {c.pc: c.target for fi in prog.info.values() for c in fi.calls}
    bad = n = 0
    with open(asm_path, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f):
            if i >= limit:
                break
            m = re.match(r"([0-9a-f]{8})\s+bl\s+0x([0-9a-f]+)", line)
            if not m:
                continue
            pc, t = int(m.group(1), 16), int(m.group(2), 16)
            if is_save_restore_entry(prog.img, t) or pc not in calls:
                continue
            n += 1
            bad += calls[pc] != t
    return n, bad


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pe", nargs="?", default=str(Path(__file__).parent / ".." / "xex" / "default.pe"),
                    help="decrypted image from xex_dump.py (default: ../xex/default.pe next to this tool)")
    ap.add_argument("--asm", help="text disassembly of the same image (default: the image with .asm); "
                    "only used to check it matches")
    ap.add_argument("--imports", help="xex_imports.py listing (default: the image with .imports)")
    ap.add_argument("--config", default=str(REPO / "COD_3_WAW_Setup-tool" / "recomp" / "config"
                                            / "default.toml"),
                    help="codegen config whose [functions] table adds function starts")
    ap.add_argument("--q3-src", help="Quake III Arena source checkout "
                    "(github.com/id-Software/Quake-III-Arena) "
                    "for the string fingerprints; skipped when not given")
    ap.add_argument("-o", "--out", help="symbol map to write (default: research/symbols.toml beside the "
                    "image's folder, as in <workspace>/xex and <workspace>/research)")
    ap.add_argument("--ppcx-names", help="also write 'ADDRESS NAME' lines for ppcx.py (<image>.names)")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    pe = Path(args.pe)
    if not pe.is_file():
        sys.exit(f"no image at {pe}")
    asm = Path(args.asm) if args.asm else pe.with_suffix(".asm")
    imports_path = Path(args.imports) if args.imports else pe.with_suffix(".imports")
    out = Path(args.out) if args.out else pe.resolve().parent.parent / "research" / "symbols.toml"

    img = Image(pe)
    extra = read_extra_functions(args.config)
    prog = Program(img, extra)
    print(f"{len(prog.funcs)} functions ({len(img.pdata())} from .pdata, {len(extra)} from {args.config})")
    warnings, report = [], {}
    if asm.is_file():
        n, bad = check_listing(asm, prog)
        print(f"listing {asm.name}: {n} calls checked, {bad} differ")
        if bad:
            warnings.append(f"{asm} does not match {pe} ({bad} of {n} calls differ)")

    syms = SymbolTable()
    pass_known(prog, syms, warnings)
    pass_curated(prog, syms, warnings)
    imports = read_imports(imports_path)
    thunks = pass_imports(prog, syms, imports)
    if not imports:
        warnings.append(f"no imports listing at {imports_path}: import thunks and wrappers left unnamed")
    report["helpers"] = pass_save_restore_helpers(prog, syms)
    q3 = read_q3_source(args.q3_src) if args.q3_src else None
    if args.q3_src and not q3:
        warnings.append(f"no Quake III source under {args.q3_src}")
    pass_self_naming(prog, syms, report, q3_string_owners(q3) if q3 else None)
    if q3:
        pass_q3_strings(prog, syms, q3, report)
    pass_rtti(prog, syms, report)
    pass_commands(prog, syms, report)
    pass_asset_types(prog, syms, report)
    pass_xapi(prog, syms, thunks, report)
    pass_dvar_globals(prog, syms, report)
    pass_trivial(prog, syms, thunks, report)

    funcs, globs = syms.functions(), syms.globals()
    by_conf = collections.Counter(s.confidence for s in funcs)
    by_src = collections.Counter(s.source for s in funcs)
    gby_src = collections.Counter(s.source for s in globs)
    header = [
        "Call of Duty 3 (Xbox 360) default.xex symbol map.",
        "Generated by tools/name_functions.py; edit the tool, not this file.",
        f"Image: {pe.name}, {len(prog.funcs)} functions, of which {len(funcs)} named here; "
        f"{len(globs)} globals.",
        "confidence: known = names the recompilation project already uses (addresses.h, hooks) and the",
        "  kernel/XAM import thunks;",
        "  high = direct evidence (RTTI, command registration, a string naming the function, Q3 structure);",
        "  medium = one weaker clue (see evidence).",
        "C++ names (Class::method) carry a C identifier in cname.",
        "",
        "functions by confidence: " + ", ".join(f"{k} {by_conf[k]}" for k in ("known", "high", "medium")),
        "functions by evidence: " + ", ".join(f"{k} {v}" for k, v in by_src.most_common()),
        "globals by evidence: " + ", ".join(f"{k} {v}" for k, v in gby_src.most_common()),
    ]
    write_toml(out, syms, header)
    if args.ppcx_names:
        write_ppcx_names(args.ppcx_names, syms)
    print(f"wrote {out}: {len(funcs)} functions, {len(globs)} globals")
    print("  functions by confidence: " + ", ".join(f"{k} {by_conf[k]}" for k in ("known", "high", "medium")))
    print("  functions by evidence:   " + ", ".join(f"{k} {v}" for k, v in by_src.most_common()))
    print("  globals by evidence:     " + ", ".join(f"{k} {v}" for k, v in gby_src.most_common()))
    print(f"  RTTI: {report.get('rtti_classes', 0)} type descriptors, "
          f"{report.get('rtti_vtables', 0)} vtables "
          f"({report.get('rtti_named')})")
    print(f"  command tables: {report.get('command_tables')}; cvar tables: {report.get('vmcvar_tables')}")
    if not q3:
        print("  (no --q3-src: Quake III string fingerprints skipped)")
    print(f"  {len(syms.rejected)} weaker or duplicate claims dropped")
    for w in warnings:
        print("warning: " + w)
    return 0


if __name__ == "__main__":
    sys.exit(main())
