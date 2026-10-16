#!/usr/bin/env python3
"""Tiny Xbox 360 PPC analysis helper (xrefs, callers, string lookup, function dumps)."""
import bisect, os, pickle, re, struct, sys

BASE = 0x82000000
HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.environ.get("PPCX_IMG", os.path.join(HERE, "..", "xex", "default.pe"))
ASM = os.environ.get("PPCX_ASM", IMG.replace(".pe", ".asm"))
CACHE = IMG + ".xref.pickle"

img = open(IMG, "rb").read()


def u32(a):
    o = a - BASE
    return struct.unpack(">I", img[o:o + 4])[0]


def sections():
    e = struct.unpack("<I", img[0x3C:0x40])[0]
    n = struct.unpack("<H", img[e + 6:e + 8])[0]
    opt = struct.unpack("<H", img[e + 20:e + 22])[0]
    s = e + 24 + opt
    out = []
    for i in range(n):
        name = img[s:s + 8].rstrip(b"\0").decode()
        vs, va = struct.unpack("<II", img[s + 8:s + 16])
        ch = struct.unpack("<I", img[s + 36:s + 40])[0]
        out.append((name, BASE + va, vs, ch))
        s += 40
    return out


SECS = sections()
CODE = [(a, a + s) for n, a, s, ch in SECS if ch & 0x20000000]


def pdata():
    funcs = []
    for n, a, s, ch in SECS:
        if n == ".pdata":
            for i in range(0, s - 7, 8):
                beg, info = struct.unpack(">II", img[a - BASE + i:a - BASE + i + 8])
                if beg == 0:
                    continue
                flen = ((info >> 8) & 0x3FFFFF) * 4
                funcs.append((beg, beg + flen))
    funcs.sort()
    return funcs


FUNCS = pdata()
FSTARTS = [f[0] for f in FUNCS]


def func_of(pc):
    i = bisect.bisect_right(FSTARTS, pc) - 1
    if i >= 0 and FUNCS[i][0] <= pc < FUNCS[i][1]:
        return FUNCS[i]
    return None


def build():
    refs, calls = {}, {}
    fstart_set = set(FSTARTS)
    for lo, hi in CODE:
        hi_reg = {}
        for pc in range(lo, hi, 4):
            if pc in fstart_set:
                hi_reg = {}
            w = u32(pc)
            op = w >> 26
            rd, ra = (w >> 21) & 31, (w >> 16) & 31
            simm = w & 0xFFFF
            if simm & 0x8000:
                simm -= 0x10000
            if op == 15 and ra == 0:  # lis
                hi_reg[rd] = ((w & 0xFFFF) << 16) & 0xFFFFFFFF
                continue
            if op == 18:  # b / bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                tgt = (li if (w & 2) else pc + li) & 0xFFFFFFFF
                if w & 1:
                    calls.setdefault(tgt, []).append(pc)
                else:
                    refs.setdefault(tgt, []).append(("b", pc))
                continue
            if op == 24 and rd in hi_reg:  # ori rA, rS, imm
                refs.setdefault(hi_reg[rd] | (w & 0xFFFF), []).append((op, pc))
            elif ra in hi_reg and op in (14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                                         48, 49, 50, 51, 52, 53, 54, 55):
                refs.setdefault((hi_reg[ra] + simm) & 0xFFFFFFFF, []).append((op, pc))
            # drop tracking for whatever register this instruction overwrites
            if op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
                hi_reg.pop(ra, None)
            elif op == 31:
                xo = (w >> 1) & 0x3FF
                if xo in (444, 28, 60, 316, 124, 24, 536, 792, 824, 954, 922, 26, 476, 412, 284):
                    hi_reg.pop(ra, None)
                else:
                    hi_reg.pop(rd, None)
            elif op in (7, 8, 12, 13, 14, 32, 33, 34, 35, 40, 41, 42, 43, 46, 58):
                hi_reg.pop(rd, None)
    return refs, calls


if os.path.exists(CACHE) and os.path.getmtime(CACHE) > os.path.getmtime(__file__):
    REFS, CALLS = pickle.load(open(CACHE, "rb"))
else:
    REFS, CALLS = build()
    pickle.dump((REFS, CALLS), open(CACHE, "wb"))


def find_str(s):
    """Return start addresses of all NUL-terminated strings containing s."""
    b = s.encode()
    out, i = [], img.find(b)
    while i >= 0:
        st = img.rfind(b"\0", 0, i) + 1
        if not out or out[-1] != BASE + st:
            out.append(BASE + st)
        i = img.find(b, i + 1)
    return out


def cstr(a, n=200):
    o = a - BASE
    if not (0 <= o < len(img)):
        return None
    e = img.find(b"\0", o, o + n)
    if e < 0:
        return None
    s = img[o:e]
    if len(s) >= 2 and all(32 <= c < 127 or c in (9, 10, 0x14, 0x15) for c in s):
        return s.decode().replace("\n", "\\n").replace("\x15", "\\x15")
    return None


def fname(a):
    f = func_of(a)
    return f"sub_{f[0]:08X}+0x{a - f[0]:X}" if f else f"?{a:08X}"


_asm_index = None


def asm_lines(lo, hi):
    global _asm_index
    if _asm_index is None:
        _asm_index = open(ASM).read().split("\n")
    # binary search by address (lines start with 8 hex chars)
    L = _asm_index
    lo_i, hi_i = 0, len(L) - 1
    while lo_i < hi_i:
        m = (lo_i + hi_i) // 2
        if L[m] and int(L[m][:8], 16) < lo:
            lo_i = m + 1
        else:
            hi_i = m
    out = []
    i = lo_i
    while i < len(L) and L[i] and int(L[i][:8], 16) < hi:
        out.append(L[i])
        i += 1
    return out


def annotate(lines):
    global _pc2ref
    if _pc2ref is None:
        _pc2ref = {}
        for t, lst in REFS.items():
            for kind, pc in lst:
                if kind != "b":
                    _pc2ref[pc] = t
    res = []
    for ln in lines:
        pc = int(ln[:8], 16)
        note = ""
        if pc in _pc2ref:
            t = _pc2ref[pc]
            s = cstr(t)
            note = f"  ; 0x{t:08X}" + (f' "{s}"' if s else "") + (f" <{NAMES[t]}>" if t in NAMES else "")
        m = re.search(r"\bbl?\s+0x([0-9a-f]+)", ln)
        if m:
            t = int(m.group(1), 16)
            if t in NAMES:
                note += f"  ; {NAMES[t]}"
        res.append(ln + note)
    return res


_pc2ref = None
NAMES = {}
_names_file = IMG + ".names"
if os.path.exists(_names_file):
    for l in open(_names_file):
        l = l.split("#")[0].strip()
        if l:
            a, n = l.split()[:2]
            NAMES[int(a, 16)] = n


def dis(a, n=None):
    f = func_of(a)
    lo, hi = (f if f and n is None else (a, a + 4 * (n or 64)))
    print(f"== sub_{lo:08X} [{lo:08X}-{hi:08X}] {NAMES.get(lo, '')}")
    for l in annotate(asm_lines(lo, hi)):
        print(l)


def main():
    cmd, *args = sys.argv[1:]
    if cmd == "str":
        for a in find_str(args[0]):
            print(f"string @ 0x{a:08X}")
            for kind, pc in REFS.get(a, []):
                print(f"   ref {kind} at {pc:08X} in {fname(pc)}")
    elif cmd == "refs":
        a = int(args[0], 16)
        for kind, pc in REFS.get(a, []):
            print(f"ref {kind} at {pc:08X} in {fname(pc)}")
    elif cmd == "callers":
        a = int(args[0], 16)
        for pc in CALLS.get(a, []):
            print(f"call at {pc:08X} in {fname(pc)}")
    elif cmd == "dis":
        dis(int(args[0], 16), int(args[1]) if len(args) > 1 else None)
    elif cmd == "func":
        print(fname(int(args[0], 16)))


if __name__ == "__main__":
    main()
