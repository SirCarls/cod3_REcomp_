"""Readable x86 disassembly of one function (Capstone), with calls, imports,
strings and float constants annotated."""
from __future__ import annotations

import struct

try:
    import capstone
    from capstone import x86_const as X
except ImportError:  # pragma: no cover - depends on the machine
    capstone = None

from pe import PEImage


def _cs():
    if capstone is None:
        raise SystemExit("disassembly needs Capstone: pip install capstone")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


def function_extent(image: PEImage, start: int, limit: int = 0x4000) -> int:
    """Where a function ends: the known length, else a walk that follows its
    branches until a return or jump with no branch target left beyond it."""
    if start in image.lengths:
        return start + image.lengths[start]
    md = _cs()
    code = image.read(start, limit)
    furthest = start
    end = start
    for ins in md.disasm(code, start):
        end = ins.address + ins.size
        if ins.mnemonic.startswith("j") and ins.operands and ins.operands[0].type == X.X86_OP_IMM:
            target = ins.operands[0].imm
            if start <= target < start + limit:
                furthest = max(furthest, target)
        if ins.mnemonic in ("ret", "retf", "int3") or (
                ins.mnemonic == "jmp" and not (ins.operands and ins.operands[0].type == X.X86_OP_IMM
                                               and start <= ins.operands[0].imm < ins.address)):
            if furthest < end:
                return end
    return end


def string_at(image: PEImage, address: int) -> bytes | None:
    """A printable NUL-terminated string at an address in the image, if there is one."""
    if not image.contains(address) or image.is_code(address):
        return None
    s = image.cstr(address, 200)
    if s is None or len(s) < 1:
        return None
    if all(32 <= c < 127 or c in (9, 10, 13, 0x14, 0x15) for c in s):
        return s
    return None


def annotate(image: PEImage, ins) -> str:
    notes = []
    for op in ins.operands:
        value = None
        if op.type == X.X86_OP_IMM:
            value = op.imm & 0xFFFFFFFF
        elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
            value = op.mem.disp & 0xFFFFFFFF
            if value in image.imports:
                notes.append(image.imports[value])
                continue
            if image.contains(value) and not image.is_code(value):
                if op.size == 4 and ins.mnemonic.startswith("f"):
                    notes.append(f"{image.f32(value)!r}f")
                elif op.size == 8 and ins.mnemonic.startswith("f"):
                    notes.append(f"{image.f64(value)!r}")
        if value is None:
            continue
        if value in image.names:
            notes.append(image.names[value])
        elif ins.mnemonic == "call" and image.is_code(value):
            notes.append(image.describe(value))
        s = string_at(image, value)
        if s is not None:
            notes.append(repr(s.decode("latin-1"))[:80])
    return ("    ; " + ", ".join(notes)) if notes else ""


def disassemble(image: PEImage, start: int, end: int | None = None) -> list[str]:
    md = _cs()
    if end is None:
        end = function_extent(image, start)
    lines = []
    for ins in md.disasm(image.read(start, end - start), start):
        label = f"{image.names[ins.address]}:" if ins.address in image.names and ins.address != start else ""
        if label:
            lines.append(label)
        raw = ins.bytes.hex()
        lines.append(f"  {ins.address:08X}  {raw:<16} {ins.mnemonic:<7} {ins.op_str}{annotate(image, ins)}")
    return lines


def references_to(image: PEImage, target: int, kinds=("call", "jmp", "push", "mov", "lea", "cmp")) -> list[int]:
    """Instructions anywhere in the code that name `target` (calls, pushes of a string ...).

    A byte search for the little-endian address, confirmed by disassembling the instruction there.
    """
    needle = struct.pack("<I", target)
    md = _cs()
    out = []
    for lo, hi in image.code_ranges():
        code = image.read(lo, hi - lo)
        i = code.find(needle)
        while i >= 0:
            for back in range(1, 8):
                at = lo + i - back
                ins = next(md.disasm(image.read(at, 16), at), None)
                if ins and ins.address + ins.size >= lo + i + 4 and ins.mnemonic.split()[0] in kinds:
                    out.append(at)
                    break
            i = code.find(needle, i + 1)
    # Relative calls and jumps.
    for lo, hi in image.code_ranges():
        code = image.read(lo, hi - lo)
        for op in (0xE8, 0xE9):
            j = code.find(bytes([op]))
            while j >= 0 and j + 5 <= len(code):
                rel = struct.unpack_from("<i", code, j + 1)[0]
                if lo + j + 5 + rel == target:
                    out.append(lo + j)
                j = code.find(bytes([op]), j + 1)
    return sorted(set(out))
