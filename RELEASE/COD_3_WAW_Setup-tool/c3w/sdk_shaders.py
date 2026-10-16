"""The ReXGlue SDK's Vulkan resolve shaders, edited for a larger emulated EDRAM.

The Xbox 360 has 10 MB of EDRAM (2048 tiles of 80x16 samples) and wraps
addresses around it. Call of Duty 3 at 1920x1080 needs 3264 tiles (colour and
depth), so the game's own resolution was capped at 1280x720. SDK patch 0010
makes the emulated EDRAM 4096 tiles; the CPU side is in that patch, but the
resolve shaders are precompiled SPIR-V (no shader compiler at build time), so
their two EDRAM-size details are edited here, word by word:

1. The wrap: addresses are taken modulo 1280 x 2048 dwords. In the unscaled
   shaders that is one constant (2621440 -> 5242880); in the scaled ones the
   2048 in that multiplication becomes a new 4096 constant.
2. The base tile: `(edram_info >> 13) & 2047` gets bit 11 from bit 31 of the
   same push constant (`| ((edram_info >> 20) & 2048)`), the bit patch 0010
   adds to ResolveEdramInfo, because a resolve can now start past tile 2047.

Every edit is checked: exactly one match per shader, or the build stops. The
D3D12 (DXBC) shaders are left alone; on D3D12 the game stays within 2048 tiles.
Files are marked once edited, so running this again changes nothing.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple

MARKER = "// c3w: resolve shader edited for a 4096-tile EDRAM (c3w/sdk_shaders.py)."
SPIRV_DIR = Path("src/graphics/shaders/vulkan_spirv")

# SPIR-V opcodes used here.
OP_TYPE_INT = 21
OP_CONSTANT = 43
OP_FUNCTION = 54
OP_IMUL = 132
OP_UMOD = 137
OP_SHIFT_RIGHT_LOGICAL = 194
OP_BITWISE_OR = 197
OP_BITWISE_AND = 199

OLD_WRAP_DWORDS = 1280 * 2048
NEW_WRAP_DWORDS = 1280 * 4096

ARRAY_RE = re.compile(r"(const uint32_t \w+\[\] = \{)(.*?)(\};)", re.S)


class ShaderEditError(Exception):
    pass


class Module:
    """A SPIR-V module as a list of words, with helpers to find and add
    instructions."""

    def __init__(self, words: List[int]):
        if len(words) < 5 or words[0] != 0x07230203:
            raise ShaderEditError("not a SPIR-V module")
        self.words = words

    @property
    def bound(self) -> int:
        return self.words[3]

    def new_id(self) -> int:
        i = self.words[3]
        self.words[3] += 1
        return i

    def instructions(self) -> List[Tuple[int, int, List[int]]]:
        """(offset, opcode, operand words) for every instruction."""
        out, pos = [], 5
        while pos < len(self.words):
            count, op = self.words[pos] >> 16, self.words[pos] & 0xFFFF
            if count == 0:
                raise ShaderEditError(f"bad instruction at word {pos}")
            out.append((pos, op, self.words[pos + 1:pos + count]))
            pos += count
        return out

    def code(self) -> List[Tuple[int, int, List[int]]]:
        """The instructions inside functions (where ids are used as operands)."""
        instrs = self.instructions()
        first = next((i for i, (_, op, _) in enumerate(instrs) if op == OP_FUNCTION), len(instrs))
        return instrs[first:]

    def uint_type(self) -> int:
        for _, op, ops in self.instructions():
            if op == OP_TYPE_INT and ops[1:] == [32, 0]:
                return ops[0]
        raise ShaderEditError("no 32-bit unsigned int type")

    def uint_constants(self) -> Dict[int, int]:
        """literal -> id of the uint constants."""
        t = self.uint_type()
        return {ops[2]: ops[1] for _, op, ops in self.instructions() if op == OP_CONSTANT and ops[0] == t}

    def constant(self, value: int) -> int:
        """The id of a uint constant with this value, added if missing (before
        the first function, after the types)."""
        found = self.uint_constants().get(value)
        if found is not None:
            return found
        first_function = next((pos for pos, op, _ in self.instructions() if op == OP_FUNCTION), None)
        if first_function is None:
            raise ShaderEditError("no function")
        cid = self.new_id()
        self.insert(first_function, [(4 << 16) | OP_CONSTANT, self.uint_type(), cid, value])
        return cid

    def insert(self, pos: int, words: List[int]) -> None:
        self.words[pos:pos] = words


def _edit_wrap(m: Module) -> None:
    consts = m.uint_constants()
    t = m.uint_type()
    if OLD_WRAP_DWORDS in consts:  # unscaled: one UMod by a constant
        cid = consts[OLD_WRAP_DWORDS]
        uses = [p for p, op, ops in m.code() if cid in ops[2:]]
        if len(uses) != 1 or m.words[uses[0]] & 0xFFFF != OP_UMOD:
            raise ShaderEditError(f"wrap constant used {len(uses)} times")
        for pos, op, ops in m.instructions():
            if op == OP_CONSTANT and ops[0] == t and ops[1] == cid:
                m.words[pos + 3] = NEW_WRAP_DWORDS
                return
    c2048 = consts.get(2048)
    if c2048 is None:
        raise ShaderEditError("no EDRAM wrap found")
    # Scaled: tile size (computed) times 2048 -> a new 4096.
    muls = [(p, ops) for p, op, ops in m.code() if op == OP_IMUL and c2048 in ops[2:]]
    others = [p for p, op, ops in m.code() if op != OP_IMUL and c2048 in ops[2:]]
    if len(muls) != 1 or others:
        raise ShaderEditError(f"2048 used {len(muls)} times in IMul, {len(others)} elsewhere")
    c4096 = m.constant(4096)
    pos, ops = next((p, o) for p, op, o in m.code() if op == OP_IMUL and c2048 in o[2:])
    k = ops.index(c2048, 2)
    m.words[pos + 1 + k] = c4096


def _edit_base(m: Module) -> None:
    consts = m.uint_constants()
    c13, c2047 = consts.get(13), consts.get(2047)
    if c13 is None or c2047 is None:
        raise ShaderEditError("no EDRAM base extraction")
    instrs = m.code()
    shifts = {ops[1]: ops[2] for _, op, ops in instrs if op == OP_SHIFT_RIGHT_LOGICAL and ops[3] == c13}
    ands = [(p, ops) for p, op, ops in instrs
            if op == OP_BITWISE_AND and ((ops[2] in shifts and ops[3] == c2047) or
                                         (ops[3] in shifts and ops[2] == c2047))]
    if len(ands) != 1:
        raise ShaderEditError(f"{len(ands)} EDRAM base extractions")
    pos, ops = ands[0]
    rtype, result = ops[0], ops[1]
    shifted = ops[2] if ops[2] in shifts else ops[3]
    edram_info = shifts[shifted]
    c20, c2048 = m.constant(20), m.constant(2048)
    # Positions moved if constants were added: find the AND again.
    pos = next(p for p, op, o in m.instructions() if op == OP_BITWISE_AND and o[1] == result)
    low = m.new_id()
    m.words[pos + 2] = low  # the original AND now makes the low 11 bits
    s, h = m.new_id(), m.new_id()
    after = pos + (m.words[pos] >> 16)
    m.insert(after, [
        (5 << 16) | OP_SHIFT_RIGHT_LOGICAL, rtype, s, edram_info, c20,
        (5 << 16) | OP_BITWISE_AND, rtype, h, s, c2048,
        (5 << 16) | OP_BITWISE_OR, rtype, result, low, h,
    ])


def edit_module(words: List[int]) -> List[int]:
    m = Module(list(words))
    _edit_wrap(m)
    _edit_base(m)
    return m.words


def _format_words(words: List[int]) -> str:
    lines = []
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join(f"0x{w:08X}" for w in words[i:i + 8]) + ",")
    return "\n" + "\n".join(lines) + "\n"


def edit_header(text: str) -> Optional[str]:
    """The edited header, or None when it was edited before."""
    if MARKER in text:
        return None
    match = ARRAY_RE.search(text)
    if not match:
        raise ShaderEditError("no uint32_t array")
    words = [int(w, 16) for w in re.findall(r"0x[0-9A-Fa-f]+", match.group(2))]
    edited = edit_module(words)
    return (MARKER + "\n// The disassembly below is of the original shader.\n" + text[:match.start()] +
            match.group(1) + _format_words(edited) + match.group(3) + text[match.end():])


def version() -> str:
    """Changes whenever this file does, so an SDK built with older edits is
    rebuilt (part of the SDK build stamp)."""
    import hashlib
    return hashlib.sha1(Path(__file__).read_bytes()).hexdigest()


def resolve_shader_headers(sdk_src: Path) -> List[Path]:
    folder = sdk_src / SPIRV_DIR
    return sorted(p for p in folder.glob("resolve_*_cs.h") if not p.name.startswith("resolve_downscale"))


def edit_sdk(sdk_src: Path) -> int:
    """Edits every Vulkan resolve shader in an SDK checkout; returns how many
    were changed now (0 when already done)."""
    headers = resolve_shader_headers(sdk_src)
    if len(headers) != 22:
        raise ShaderEditError(f"expected 22 resolve shaders in {sdk_src / SPIRV_DIR}, found {len(headers)}")
    changed = 0
    for h in headers:
        try:
            new = edit_header(h.read_text(encoding="utf-8"))
        except ShaderEditError as e:
            raise ShaderEditError(f"{h.name}: {e}") from None
        if new is not None:
            h.write_text(new, encoding="utf-8", newline="\n")
            changed += 1
    return changed
