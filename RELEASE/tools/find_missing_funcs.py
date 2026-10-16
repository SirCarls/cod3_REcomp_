#!/usr/bin/env python3
"""Find guest functions that rexglue codegen did not discover.

Codegen's vtable scanner found nothing in retail CoD3 (the image does keep
MSVC RTTI: 220 vtables, see tools/name_functions.py), and any code that is
only reachable through a data pointer (vtable slots, callback tables) and has
no .pdata entry is missing from the function table. At run
time the game then dies with "Call to invalid or unregistered function".
The usual culprits are compiler-generated this-adjustor thunks
("addi r3,r3,N ; b target") and tiny leaf functions.

This scans every data section for big-endian pointers into code, groups them
into runs of consecutive code pointers, and reports pointers that are not
known function starts when they sit in a vtable-like run (most neighbors are
known functions) or are a lone function pointer at a function boundary.

    find_missing_funcs.py <image.pe> <generated/<module>/<project>_init.cpp> [--toml]
"""
import re
import struct
import sys


def sections(img):
    e = struct.unpack_from("<I", img, 0x3C)[0]
    n = struct.unpack_from("<H", img, e + 6)[0]
    opt = struct.unpack_from("<H", img, e + 20)[0]
    base = struct.unpack_from("<I", img, e + 24 + 28)[0]
    s = e + 24 + opt
    out = []
    for _ in range(n):
        name = img[s:s + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", img, s + 8)
        ch = struct.unpack_from("<I", img, s + 36)[0]
        out.append((name, base + va, vsize, rawptr, rawsize, ch))
        s += 40
    return base, out


def main():
    img = open(sys.argv[1], "rb").read()
    init = open(sys.argv[2], encoding="utf-8", errors="replace").read()
    as_toml = "--toml" in sys.argv
    known = {int(x, 16) for x in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), \w+ \}", init)}
    base, secs = sections(img)

    # The image is already laid out by virtual address in default.pe (xex_dump output).
    def u32(addr):
        off = addr - base
        if off < 0 or off + 4 > len(img):
            return 0
        return struct.unpack_from(">I", img, off)[0]

    code = [(va, va + vs) for n, va, vs, rp, rs, ch in secs if ch & 0x20000000]

    def in_code(v):
        return any(a <= v < b for a, b in code)

    def is_terminator(insn):
        op = insn >> 26
        if insn == 0x4E800020:            # blr
            return True
        if insn == 0x4E800420:            # bctr
            return True
        if op == 18 and not (insn & 1):   # b / ba (no link)
            return True
        if insn == 0:                     # padding
            return True
        return False

    # .pdata: (begin, length) of every function the compiler described. A
    # pointer that lands inside one of these is a label (switch case, computed
    # goto), not a missing function.
    pdata = []
    for n, va, vs, rp, rs, ch in secs:
        if n == ".pdata":
            for off in range(0, vs - 7, 8):
                beg, data = struct.unpack_from(">II", img, va - base + off)
                if beg == 0:
                    continue
                pdata.append((beg, beg + ((data >> 8) & 0x3FFFFF) * 4))
    pdata.sort()
    pdata_begins = [p[0] for p in pdata]

    def inside_pdata(v):
        import bisect
        i = bisect.bisect_right(pdata_begins, v) - 1
        if i >= 0 and pdata[i][0] < v < pdata[i][1]:
            return True
        # Inline jump tables (right after a bctr) hold code addresses, not code.
        if in_code(u32(v)) and in_code(u32(v + 4)):
            return True
        return False

    known_sorted = sorted(known)

    def next_known(v):
        import bisect
        i = bisect.bisect_right(known_sorted, v)
        return known_sorted[i] if i < len(known_sorted) else v + 0x10000

    def branch_target(pc, insn):
        op = insn >> 26
        if op == 18:                                    # b / bl
            li = insn & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            return (li if insn & 2 else pc + li), bool(insn & 1)
        if op == 16:                                    # bc
            bd = insn & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            return (bd if insn & 2 else pc + bd), bool(insn & 1)
        return None, False

    def extent(v):
        """Function size: scan forward following internal forward branches
        until an exit with nothing pending, stopping at the next known
        function or at padding."""
        limit = min(next_known(v), v + 0x4000)
        far = v
        pc = v
        while pc < limit:
            insn = u32(pc)
            if insn == 0:
                break
            tgt, link = branch_target(pc, insn)
            if tgt is not None and not link and v <= tgt < limit:
                far = max(far, tgt)
            if is_terminator(insn) and pc >= far:
                # An unconditional b to a target before v or past the limit is a tail call.
                return pc + 4 - v
            pc += 4
        return (pc - v) if pc > v else None

    found = {}
    for n, va, vs, rp, rs, ch in secs:
        if ch & 0x20000000 or n in (".pdata", ".reloc", ".idata", ".XBLD"):
            continue
        words = []
        for off in range(0, vs - 3, 4):
            v = u32(va + off)
            words.append(v if (v % 4 == 0 and in_code(v)) else None)
        i = 0
        while i < len(words):
            if words[i] is None:
                i += 1
                continue
            j = i
            while j < len(words) and words[j] is not None:
                j += 1
            run = words[i:j]
            nknown = sum(1 for v in run if v in known)
            for k, v in enumerate(run):
                if v in known or v in found or inside_pdata(v):
                    continue
                prev = u32(v - 4)
                boundary = is_terminator(prev)
                vtable_like = len(run) >= 2 and nknown * 2 >= len(run)
                lone = len(run) == 1 and boundary
                if (vtable_like and boundary) or lone:
                    size = extent(v)
                    if size:
                        found[v] = (size, va + 4 * (i + k), "vtable" if vtable_like else "pointer")
            i = j

    # Code -> code address constants (lis rA,hi ; addi/ori rB,rA,lo): callbacks
    # handed to the engine (commands, threads, sort comparators, ...).
    for a, b in code:
        regs = {}
        for pc in range(a, b, 4):
            insn = u32(pc)
            op = insn >> 26
            rd, ra = (insn >> 21) & 31, (insn >> 16) & 31
            imm = insn & 0xFFFF
            if op == 15 and ra == 0:                      # lis rD, imm
                regs[rd] = (imm << 16, pc)
                continue
            if op in (14, 24) and ra in regs and pc - regs[ra][1] <= 32:   # addi / ori
                hi = regs[ra][0]
                v = (hi + (imm - 0x10000 if (op == 14 and imm & 0x8000) else imm)) & 0xFFFFFFFF
                if v % 4 == 0 and in_code(v) and v not in known and v not in found and not inside_pdata(v):
                    if is_terminator(u32(v - 4)):
                        size = extent(v)
                        if size:
                            found[v] = (size, pc, "code-ref")
            if op == 18 or (op == 19):                     # branch: forget tracked regs
                regs.clear()

    if as_toml:
        for v in sorted(found):
            size, ref, kind = found[v]
            print(f"0x{v:08X} = {{ size = {size} }}  # {kind} @0x{ref:08X}")
    else:
        print(f"{len(known)} known functions, {len(found)} missing candidates")
        for v in sorted(found):
            size, ref, kind = found[v]
            print(f"0x{v:08X} size {size:3d}  {kind:7s} referenced from 0x{ref:08X}")


if __name__ == "__main__":
    main()
