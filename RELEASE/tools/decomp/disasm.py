"""Readable disassembly of one guest function, for reading the code before porting it.

Uses Capstone (BSD) when it's installed (`pip install capstone`); Xenon's
VMX128 instructions, which Capstone doesn't know, show as `.long`.

Annotations:
- calls and branches name their target from the symbol map (`bl Com_Printf`);
- an address built with `lis` + `addi`/`ori`/a load or store shows what it
  points at: a name, a string, or the float/int stored there.
"""
from __future__ import annotations

import struct

from image import GuestImage

try:
    import capstone
except ImportError:  # pragma: no cover - depends on the machine
    capstone = None


def _signed16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def _describe_data(img: GuestImage, address: int, size: int = 4) -> str:
    """What a computed address points at: a name, a string, or the number stored there."""
    if address in img.names:
        return img.names[address]
    text = img.cstr(address, 120)
    if text is not None and len(text) >= 4 and all(32 <= c < 127 or c in (9, 10, 0x14, 0x15) for c in text):
        # 0x14 / 0x15 start the game's localized and colored strings.
        shown = text.decode("ascii").replace("\n", "\\n").replace("\x15", "\\x15").replace("\x14", "\\x14")
        return '"' + shown + '"'
    if img.contains(address) and not img.is_code(address):
        if size == 8:
            value = struct.unpack(">d", img.read(address, 8))[0]
            return f"0x{address:08X} (= {value:g} double)"
        word = img.u32(address)
        as_float = struct.unpack(">f", struct.pack(">I", word))[0]
        if word and 1e-6 < abs(as_float) < 1e9:
            return f"0x{address:08X} (= {as_float:g}f / 0x{word:08X})"
        return f"0x{address:08X} (= 0x{word:08X})"
    return f"0x{address:08X}"


def disassemble(img: GuestImage, start: int, end: int | None = None) -> list[str]:
    if capstone is None:
        raise SystemExit("Disassembly needs Capstone: pip install capstone")
    if end is None:
        bounds = img.function_at(start)
        end = bounds[1] if bounds and bounds[0] == start else start + 64 * 4
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_64 | capstone.CS_MODE_BIG_ENDIAN)
    upper: dict[int, int] = {}  # register -> value loaded by lis
    lines = []
    for pc in range(start, end, 4):
        word = img.u32(pc)
        insn = next(md.disasm(img.read(pc, 4), pc), None)
        text = f"{insn.mnemonic} {insn.op_str}" if insn else f".long 0x{word:08X}"
        note = ""
        op, rd, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        writes = None  # the register this instruction overwrites, when it's a simple form
        if op == 15 and ra == 0:  # lis rD, imm: remember the upper half
            upper[rd] = (word & 0xFFFF) << 16
        elif op == 14 and ra in upper:  # addi rD, rA, simm: a full address
            value = (upper[ra] + _signed16(word & 0xFFFF)) & 0xFFFFFFFF
            note = _describe_data(img, value)
            upper[rd] = value
        elif op == 24 and rd in upper:  # ori rA, rS, uimm
            value = upper[rd] | (word & 0xFFFF)
            note = _describe_data(img, value)
            upper[ra] = value
        elif 32 <= op <= 55 and ra in upper:  # loads and stores at an offset from a known address
            note = _describe_data(img, (upper[ra] + _signed16(word & 0xFFFF)) & 0xFFFFFFFF,
                                  8 if op in (50, 51, 54, 55) else 4)
            if op in (32, 33, 34, 35, 40, 41, 42, 43):  # integer loads overwrite rD
                writes = rd
        elif op in (16, 18):  # bc / b / bl
            if insn and insn.op_str:
                try:
                    target = int(insn.op_str.split(",")[-1].strip(), 16)
                    note = img.names.get(target) or img.imports.get(target, "")
                except ValueError:
                    pass
        elif op in (7, 8, 12, 13, 14, 32, 33, 34, 35, 40, 41, 42, 43, 46, 58):
            writes = rd
        elif op in (21, 23, 28, 29, 24, 25, 26, 27, 31):
            writes = ra if op != 31 else None
        if writes is not None and not (op == 15):
            upper.pop(writes, None)
        if op == 31:  # most X-form ops write rD; keep it simple and forget both
            upper.pop(rd, None)
            upper.pop(ra, None)
        if pc in img.names and pc != start:
            lines.append(f"            {img.names[pc]}:")
        lines.append(f"  {pc:08X}  {word:08X}  {text:<34}" + (f" ; {note}" if note else ""))
    return lines
