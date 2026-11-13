#!/usr/bin/env python3
"""Names each pixel shader a probe saved, and its constants, from the shader's own constant table.

    ps_names.py SHOTS_FOLDER DUMP_FOLDER [PREFIX]

SHOTS_FOLDER holds a run's saved shaders (<PREFIX><N>-ps-header.bin and -ps-block.bin; PREFIX is ngl-side-draw by
default). DUMP_FOLDER is an SDK shader dump (dump_shaders: shader_<hash>.ucode.bin.frag). For each saved pixel shader
it prints the dump's hash whose microcode sits in the block (as stored or with each 32-bit word turned around), then
every constant its header's table names: the register set (c float, i int, b bool, s sampler), the first register
and how many. The table is Direct3D's constant table (a 0x1C-byte header: size, creator, version, count, the
entries' offset, flags, target; entries of 20 bytes: name, set, index, count, reserved, type, default), found by
trying each place in the header; byte order is tried both ways. Paths may be Windows or Linux ones.
"""
import pathlib
import re
import struct
import sys

SETS = {0: "b", 1: "i", 2: "c", 3: "s"}


def path_of(text):
    path = pathlib.Path(text)
    return path if path.exists() else pathlib.Path(text.replace("\\", "/"))


def swapped(data):
    return b"".join(data[i:i + 4][::-1] for i in range(0, len(data) - len(data) % 4, 4))


def text_at(data, at):
    if not 0 <= at < len(data):
        return None
    end = data.find(b"\0", at, at + 96)
    if end <= at:
        return None
    raw = data[at:end]
    if not all(32 <= b < 127 for b in raw):
        return None
    return raw.decode("ascii")


def constant_table(header):
    """The best-looking table in the header: [(name, set, index, count)], or []."""
    best = []
    for order in (">", "<"):
        for start in range(0, len(header) - 0x1C, 4):
            size, _creator, _version, count, entries = struct.unpack_from(order + "5I", header, start)
            if size != 0x1C or not 0 < count <= 64:
                continue
            table = header[start:]
            if entries + 20 * count > len(table):
                continue
            found = []
            for k in range(count):
                name_at, reg_set, index, regs = struct.unpack_from(order + "I3H", table, entries + 20 * k)
                name = text_at(table, name_at)
                if name is None or reg_set > 3 or regs == 0 or regs > 256 or index > 511:
                    found = []
                    break
                found.append((name, SETS[reg_set], index, regs))
            if len(found) > len(best):
                best = found
    return best


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__.strip())
        return 2
    shots, dump = path_of(argv[1]), path_of(argv[2])
    prefix = argv[3] if len(argv) == 4 else "ngl-side-draw"
    dumps = [(d.name.split(".")[0].replace("shader_", ""), d.read_bytes()) for d in sorted(dump.glob("shader_*.ucode.bin.frag"))]
    pattern = re.compile(re.escape(prefix) + r"(\d+)-ps-header\.bin$")
    saved = sorted((int(m.group(1)), p) for p in shots.iterdir() if (m := pattern.match(p.name)))
    for n, header_path in saved:
        header = header_path.read_bytes()
        block_path = header_path.with_name(f"{prefix}{n}-ps-block.bin")
        block = block_path.read_bytes() if block_path.exists() else b""
        names = [h for h, data in dumps if data and (data in block or swapped(data) in block or data in header
                                                      or swapped(data) in header)]
        table = constant_table(header)
        listed = "; ".join(f"{name} {s}{index}" + (f"-{s}{index + regs - 1}" if regs > 1 else "")
                           for name, s, index, regs in table)
        print(f"{prefix}{n}: {','.join(names) or 'no match'} ({len(header)}-byte header, {len(block)}-byte block): "
              f"{listed or 'no constant table'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
