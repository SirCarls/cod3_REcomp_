#!/usr/bin/env python3
# [acct2-bro] NGL's precompiled shaders in Big Red One: what nglVertexShader_Init(obj, layout, program) and
# nglPixelShader_Init(obj, definition) are given, read out of the program image at their call sites in the C export.
# - Vertex programs start with a 4-byte header: u16 type (0x2078 'xv' = vertex shader, 0x7778 state shader,
#   0x7378 read/write state shader) and u16 instruction count, then 16 bytes per NV2A instruction.
# - The layout (kept at obj[1]) is a precompiled vertex layout: 16 slots of 16 bytes, one per input register, each
#   {u32 unknown, u32 byte offset, u32 NV2A attribute format, u32 unknown}; format bits 4-7 = component count,
#   0 = slot off. tools/bro_decomp/nv2a_vsh.py decodes it.
# - Two kinds of call site: constant arguments, and loops over a table of programs or definitions
#   (cdWorld_InitVertexShaders and the like). A vertex loop takes its layout from its caller's argument.
# Read-only. Usage: ngl_shaders.py IMAGE_PE DECOMP_DIR OUT_TSV
import bisect, collections, glob, os, re, struct, sys

PTR = r'PTR_(?:PTR_|DAT_|LAB_)*([0-9a-f]+)'
VS = re.compile(r'nglVertexShader_Init\(\(undefined4 \*\)&DAT_([0-9a-f]+),&DAT_([0-9a-f]+),' + PTR + r'\)')
PS = re.compile(r'nglPixelShader_Init\(\(undefined4 \*\)&DAT_([0-9a-f]+),' + PTR + r'\)')
# for (i = 0; i < count; i++) nglVertexShader_Init(&objects[i], param_1, programs[i]);   objects are 8 bytes
VS_LOOP = re.compile(r'\n\w[^\n]*?(\w+)\(undefined4 param_1\)\s*\{(?:(?!\n\}).)*?ppuVar\d+ = &' + PTR +
                     r';\s*puVar\d+ = \(undefined4 \*\)&DAT_([0-9a-f]+);\s*iVar\d+ = (\d+);\s*do \{\s*'
                     r'nglVertexShader_Init\(puVar\d+,param_1,\*ppuVar\d+\);', re.S)
# for (i = 0; i < count; i++) nglPixelShader_Init(&objects[i], definitions[i]);   objects are 4 bytes
PS_LOOP = re.compile(r'\n\w[^\n]*?(\w+)\(void\)\s*\{(?:(?!\n\}).)*?iVar\d+ = (\d+);\s*do \{\s*'
                     r'nglPixelShader_Init\(\(undefined4 \*\)\(&DAT_([0-9a-f]+) \+ iVar\d+\),\s*'
                     r'\*\(undefined4 \*\)\(\(int\)&' + PTR + r' \+ iVar\d+\)\);', re.S)
# a function header in Ghidra's C (the signature may be wrapped over several lines)
FUNC = re.compile(r'\n[A-Za-z_][^\n;]*?\b(\w+)\([^;{}]*?\)[ \t]*\n\s*\{')
HEADER = {0x2078: 'vertex program', 0x7778: 'state program', 0x7378: 'read/write state program'}


class Image:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        pe = struct.unpack_from('<I', self.b, 0x3C)[0]
        nsec = struct.unpack_from('<H', self.b, pe + 6)[0]
        opt = struct.unpack_from('<H', self.b, pe + 20)[0]
        self.base = struct.unpack_from('<I', self.b, pe + 24 + 28)[0]
        self.secs = []
        for i in range(nsec):
            o = pe + 24 + opt + i * 40
            vsize, va, rsize, raw = struct.unpack_from('<IIII', self.b, o + 8)
            self.secs.append((self.base + va, max(vsize, rsize), raw, rsize))

    def off(self, va):
        for start, size, raw, rsize in self.secs:
            if start <= va < start + size and va - start < rsize:
                return raw + va - start
        return None

    def u32(self, va):
        o = self.off(va)
        return struct.unpack_from('<I', self.b, o)[0] if o is not None and o + 4 <= len(self.b) else None


def layout_slots(img, va):
    """How many of the layout's 16 slots are switched on, or '?' if it is not readable."""
    fmts = [img.u32(va + 16 * i + 8) for i in range(16)]
    return '?' if None in fmts else sum(1 for f in fmts if (f >> 4) & 0xF)


def main(image, decomp, out):
    img = Image(image)
    text = ''.join(open(f, encoding='utf-8', errors='replace').read() for f in sorted(glob.glob(os.path.join(decomp, '*.c'))))
    heads = [(m.start(), m.group(1)) for m in FUNC.finditer(text)]
    starts = [p for p, _ in heads]

    def enclosing(pos):
        i = bisect.bisect_right(starts, pos) - 1
        return heads[i][1] if i >= 0 else '?'

    # (kind, object, layout, pointer, function, how) for every vertex and pixel setup
    setups = [('vertex', *m.groups(), enclosing(m.start()), 'constant') for m in VS.finditer(text)]
    setups += [('pixel', m.group(1), '', m.group(2), enclosing(m.start()), 'constant') for m in PS.finditer(text)]
    loops = 0
    for fn, table, objs, count in VS_LOOP.findall(text):
        callers = re.findall(r'\b' + fn + r'\(&DAT_([0-9a-f]+)\);', text)
        loops += 1
        for lay in sorted(set(callers)) or ['?']:
            for i in range(int(count)):
                setups.append(('vertex', f'{int(objs, 16) + 8 * i:08x}', lay, f'{int(table, 16) + 4 * i:08x}', fn, 'table'))
    for fn, count, objs, table in PS_LOOP.findall(text):
        loops += 1
        for i in range(int(count)):
            setups.append(('pixel', f'{int(objs, 16) + 4 * i:08x}', '', f'{int(table, 16) + 4 * i:08x}', fn, 'table'))

    rows, kinds, progs, lays, psdefs = [], collections.Counter(), {}, set(), set()
    for kind, obj, lay, ptr, fn, how in setups:
        target = img.u32(int(ptr, 16))
        if kind == 'pixel':
            psdefs.add(target)
            rows.append((kind, obj, '', ptr, f'{target:08X}' if target else '?', '', '', '', fn, how))
            continue
        hdr = img.u32(target) if target else None
        typ, cnt = (hdr & 0xFFFF, hdr >> 16) if hdr is not None else (None, None)
        kinds[HEADER.get(typ, f'other {typ}')] += 1
        progs[target] = cnt
        lays.add(lay)
        rows.append((kind, obj, lay, ptr, f'{target:08X}' if target else '?', f'{typ:04X}' if typ is not None else '?',
                     cnt, layout_slots(img, int(lay, 16)) if lay != '?' else '?', fn, how))
    with open(out, 'w', encoding='utf-8') as f:
        f.write('kind\tobject\tlayout\tpointer\tprogram_or_definition\theader_type\tinstructions\tlayout_slots_on\tfunction\targuments\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    vcount = [c for c in progs.values() if c]
    sites = len(re.findall(r'ngl(?:Vertex|Pixel)Shader_Init\(', text)) - 2  # minus the two definitions
    print(f'vertex shader setups {sum(1 for r in rows if r[0] == "vertex")}: {dict(kinds)}; distinct programs {len(progs)}, '
          f'instructions total {sum(vcount)}, max {max(vcount) if vcount else 0}; distinct layouts {len(lays)}')
    print(f'pixel shader setups {sum(1 for r in rows if r[0] == "pixel")}; distinct definitions {len(psdefs)}')
    constant = sum(1 for s in setups if s[5] == 'constant')
    print(f'call sites {sites}: {constant} with constant arguments, {loops} table loops expanded, '
          f'{sites - constant - loops} not understood')


if __name__ == '__main__':
    main(*sys.argv[1:])
