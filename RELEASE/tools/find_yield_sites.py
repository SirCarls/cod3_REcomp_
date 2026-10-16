#!/usr/bin/env python3
"""Find the script-thread yield calls in a CoD3 level DLL.

A level calls the engine's suspend functions through its interface table:

    lis   r11, table@ha
    addi  r11, r11, table@l
    lwz   r11, 76(r11)        ; or 80(r11)
    mtctr r11
    bctrl

On the console these calls never return (the engine longjmps away and later
resumes the thread elsewhere), so code after them is not a continuation. With
script threads on host fibers (src/game/script_threads.cpp) they do return,
and a mid-asm `return` is needed right after each one.

The table base is checked: it must be the address the level uses most often
in `lis/addi r11` + `lwz r11,N(r11)` + `mtctr r11` + `bctrl` sequences, which
is the level's interface table.

    find_yield_sites.py <level.pe> [--toml]
"""
import collections
import struct
import sys

YIELD_SLOTS = (76, 80)


def sections(img):
    e = struct.unpack_from("<I", img, 0x3C)[0]
    n = struct.unpack_from("<H", img, e + 6)[0]
    opt = struct.unpack_from("<H", img, e + 20)[0]
    base = struct.unpack_from("<I", img, e + 24 + 28)[0]
    s = e + 24 + opt
    out = []
    for _ in range(n):
        vsize, va = struct.unpack_from("<II", img, s + 8)
        ch = struct.unpack_from("<I", img, s + 36)[0]
        out.append((base + va, vsize, ch))
        s += 40
    return base, out


def main():
    img = open(sys.argv[1], "rb").read()
    base, secs = sections(img)

    def u32(addr):
        return struct.unpack_from(">I", img, addr - base)[0]

    calls = []  # (bctrl address, table base, slot offset)
    for va, size, ch in secs:
        if not ch & 0x20000000:  # executable sections only
            continue
        for pc in range(va + 16, va + size - 3, 4):
            if u32(pc) != 0x4E800421:  # bctrl
                continue
            if u32(pc - 4) != 0x7D6903A6:  # mtctr r11
                continue
            lwz = u32(pc - 8)
            if lwz >> 16 != 0x816B:  # lwz r11, d(r11)
                continue
            slot = lwz & 0xFFFF
            # Walk back to the lis/addi pair that set r11 (within a few instructions).
            hi = lo = None
            for back in range(12, 48, 4):
                insn = u32(pc - back)
                if lo is None and insn >> 16 == 0x396B:  # addi r11, r11, lo
                    lo = insn & 0xFFFF
                    lo = lo - 0x10000 if lo & 0x8000 else lo
                elif lo is not None and insn >> 16 == 0x3D60:  # lis r11, hi
                    hi = insn & 0xFFFF
                    break
            if hi is None:
                continue
            table = ((hi << 16) + lo) & 0xFFFFFFFF
            calls.append((pc, table, slot))

    if not calls:
        print("no table calls found", file=sys.stderr)
        return 1
    table = collections.Counter(t for _, t, _ in calls).most_common(1)[0][0]
    sites = sorted(pc for pc, t, slot in calls if t == table and slot in YIELD_SLOTS)

    if "--toml" in sys.argv:
        print(f"# Script-thread yields: calls through the interface table at 0x{table:08X}")
        print("# slots +76 / +80 (tools/find_yield_sites.py). See src/game/level_hooks.cpp.")
        for pc in sites:
            print("[[midasm_hook]]")
            print(f"address = 0x{pc:08X}")
            print('name = "cod3_script_yield_returned"')
            print("after_instruction = true")
            print("return = true")
            print()
    else:
        print(f"interface table 0x{table:08X} ({len(calls)} table calls), {len(sites)} yield sites:")
        for pc in sites:
            print(f"  0x{pc:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
