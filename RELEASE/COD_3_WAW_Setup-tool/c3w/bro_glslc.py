"""glslc's command line, done with glslang: the shader compiler of Big Red One's Windows build ([acct2-ocr]
2026-11-12, owner 23:06 "Also the windows build needs to happen").

ngl_vk/CMakeLists.txt turns each shader into SPIR-V with glslc (shaderc), which comes with Linux distributions but
not with Windows (only with the Vulkan SDK's installer). Khronos' own glslang has a pinned Windows download
(bro_windows.GLSLANG_WIN), so on Windows the build is given this script, through a small glslc.bat, as its glslc:

    python -I bro_glslc.py --glslang GLSLANG [glslc's options] -o OUT IN

It understands what ngl_vk's build passes glslc: -mfmt=c, -DNAME[=VALUE], -fshader-stage=STAGE, -o OUT and the
shader. Anything else is refused (exit 2), so a new option in ngl_vk/CMakeLists.txt shows at once rather than being
dropped. glslang makes the SPIR-V for Vulkan 1.0, as glslc does by default; this writes it as glslc -mfmt=c does: the
words as a C initializer list, {0x07230203,...}, four to a line. Only the generator word differs from glslc's
(glslang's own id instead of shaderc's).
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence

SPIRV_MAGIC = 0x07230203
STAGES = ("vert", "frag", "comp", "geom", "tesc", "tese")


class UsageError(Exception):
    pass


@dataclass
class Options:
    glslang: str = ""
    output: str = ""
    source: str = ""
    stage: str = ""
    defines: List[str] = field(default_factory=list)


def parse(argv: Sequence[str]) -> Options:
    """glslc's arguments, as ngl_vk's build passes them."""
    o = Options()
    fmt_c = False
    args = list(argv)
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--glslang":
            i += 1
            if i >= len(args):
                raise UsageError("--glslang needs the glslang program")
            o.glslang = args[i]
        elif a == "-mfmt=c":
            fmt_c = True
        elif a.startswith("-D") and len(a) > 2:
            o.defines.append(a[2:])
        elif a.startswith("-fshader-stage="):
            o.stage = a.split("=", 1)[1]
            if o.stage not in STAGES:
                raise UsageError(f"unknown shader stage: {o.stage}")
        elif a == "-o":
            i += 1
            if i >= len(args):
                raise UsageError("-o needs a file")
            o.output = args[i]
        elif a.startswith("-o") and len(a) > 2:
            o.output = a[2:]
        elif a.startswith("-"):
            raise UsageError(f"option not handled here: {a}")
        elif o.source:
            raise UsageError(f"one shader at a time ({o.source}, {a})")
        else:
            o.source = a
        i += 1
    if not fmt_c:
        raise UsageError("only -mfmt=c is handled (the C initializer list ngl_vk includes)")
    if not (o.glslang and o.output and o.source):
        raise UsageError("needs --glslang, -o OUT and a shader")
    return o


def glslang_command(o: Options, spv: str) -> List[str]:
    """glslang's arguments for the same SPIR-V glslc makes: Vulkan 1.0, the shader's stage from its file name unless
    -fshader-stage says otherwise."""
    cmd = [o.glslang, "-V", "--target-env", "vulkan1.0"]
    cmd += [f"-D{d}" for d in o.defines]
    if o.stage:
        cmd += ["-S", o.stage]
    return cmd + ["-o", spv, o.source]


def c_text(words: Sequence[int]) -> str:
    """glslc -mfmt=c's text for the words."""
    lines = [",".join(f"0x{w:08x}" for w in words[i:i + 4]) for i in range(0, len(words), 4)]
    return "{" + ",\n".join(lines) + "}\n"


def spirv_words(data: bytes) -> List[int]:
    if len(data) < 20 or len(data) % 4:
        raise ValueError(f"not SPIR-V ({len(data)} bytes)")
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    if words[0] != SPIRV_MAGIC:
        raise ValueError("not SPIR-V (no magic number)")
    return words


def main(argv: Optional[Sequence[str]] = None) -> int:
    try:
        o = parse(sys.argv[1:] if argv is None else argv)
    except UsageError as e:
        print(f"bro_glslc: {e}", file=sys.stderr)
        return 2
    out = Path(o.output)
    spv = out.with_name(out.name + ".spv.tmp")
    try:
        rc = subprocess.run(glslang_command(o, str(spv)), check=False).returncode
        if rc != 0:
            print(f"bro_glslc: glslang failed on {o.source} (exit {rc})", file=sys.stderr)
            return rc or 1
        try:
            words = spirv_words(spv.read_bytes())
        except (OSError, ValueError) as e:
            print(f"bro_glslc: {o.source}: {e}", file=sys.stderr)
            return 1
        tmp = out.with_name(out.name + ".tmp")
        tmp.write_text(c_text(words), encoding="ascii", newline="\n")
        os.replace(tmp, out)
        return 0
    finally:
        try:
            spv.unlink()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
