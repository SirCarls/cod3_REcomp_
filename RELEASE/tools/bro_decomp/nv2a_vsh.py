#!/usr/bin/env python3
# [acct2-bro] Decode Big Red One's precompiled NV2A vertex shaders: which vertex inputs (v0-v15) each program reads,
# which outputs it writes, its opcodes, and the vertex layout it is paired with. Used to plan the build-time
# conversion to SPIR-V and the Vulkan vertex input state; nothing here runs a shader.
# - Program: u16 type, u16 count, then 4 dwords per instruction (dword 0 unused, FINAL = dword 3 bit 0). The script
#   checks the field layout on every program before trusting it: dword 0 must be 0, FINAL must be set on the last
#   instruction only, and every opcode must be in range.
# - Layout (nglVertexShader_Init's second argument, kept at obj[1]): 16 slots of 16 bytes, one per input register,
#   {u32 unknown (0 so far), u32 byte offset, u32 NV2A attribute format, u32 unknown}. Format bits 0-3 = type,
#   bits 4-7 = component count (0 = slot off). The script checks every input a program reads is switched on.
# Read-only. Usage: nv2a_vsh.py IMAGE_PE SHADERS_TSV OUT_TSV   (SHADERS_TSV is ngl_shaders.py's output)
#        or:  nv2a_vsh.py IMAGE_PE --show VA [VA ...]   (a program written out instruction by instruction, to port it;
#             constants as the NV2A numbers them, c[n] here being Direct3D's c(n - 96))
# Output: one row per program and layout pair, with the functions that set it up.
import collections, struct, sys

# (dword, bit, size)
F = {
    'ilu': (1, 25, 3), 'mac': (1, 21, 4), 'const': (1, 13, 8), 'v': (1, 9, 4),
    'a_mux': (2, 26, 2), 'a_r': (2, 28, 4), 'b_mux': (2, 11, 2), 'b_r': (2, 13, 4),
    'c_mux': (3, 28, 2), 'c_r_lo': (3, 30, 2), 'c_r_hi': (2, 0, 2),
    'out_mac_mask': (3, 24, 4), 'out_r': (3, 20, 4), 'out_ilu_mask': (3, 16, 4), 'out_o_mask': (3, 12, 4),
    'out_orb': (3, 11, 1), 'out_address': (3, 3, 8), 'out_mux': (3, 2, 1), 'a0x': (3, 1, 1), 'final': (3, 0, 1),
}
MAC = ['NOP', 'MOV', 'MUL', 'ADD', 'MAD', 'DP3', 'DPH', 'DP4', 'DST', 'MIN', 'MAX', 'SLT', 'SGE', 'ARL']
ILU = ['NOP', 'MOV', 'RCP', 'RCC', 'RSQ', 'EXP', 'LOG', 'LIT']
# which of A, B, C each MAC opcode reads; ILU opcodes read C only
MAC_USES = {'MOV': 'A', 'MUL': 'AB', 'ADD': 'AC', 'MAD': 'ABC', 'DP3': 'AB', 'DPH': 'AB', 'DP4': 'AB', 'DST': 'AB',
            'MIN': 'AB', 'MAX': 'AB', 'SLT': 'AB', 'SGE': 'AB', 'ARL': 'A'}
PARAM_V = 2
PARAM_C = 3
OUT_NAMES = {0: 'oPos', 3: 'oD0', 4: 'oD1', 5: 'oFog', 6: 'oPts', 7: 'oB0', 8: 'oB1', 9: 'oT0', 10: 'oT1', 11: 'oT2',
             12: 'oT3'}
# NV2A vertex attribute types (NV097_SET_VERTEX_DATA_ARRAY_FORMAT) and bytes per component
TYPES = {0: ('d3dcolor', 1), 1: ('snorm16', 2), 2: ('float', 4), 4: ('unorm8', 1), 5: ('short', 2), 6: ('packed', 4)}


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

    def read(self, va, n):
        for start, size, raw, rsize in self.secs:
            if start <= va and va + n <= start + rsize:
                return self.b[raw + va - start: raw + va - start + n]
        return None


def field(words, name):
    d, bit, size = F[name]
    return (words[d] >> bit) & ((1 << size) - 1)


def decode(img, va):
    hdr = img.read(va, 4)
    typ, count = struct.unpack('<HH', hdr)
    body = img.read(va + 4, 16 * count)
    problems, ins = [], []
    for i in range(count):
        w = struct.unpack_from('<4I', body, 16 * i)
        mac, ilu = field(w, 'mac'), field(w, 'ilu')
        if w[0] != 0:
            problems.append(f'#{i} dword0={w[0]:08X}')
        if mac >= len(MAC):
            problems.append(f'#{i} mac={mac}')
        if field(w, 'final') != (1 if i == count - 1 else 0):
            problems.append(f'#{i} final={field(w, "final")}')
        ins.append(w)
    return typ, count, ins, problems


def layout(img, va):
    """{slot: (offset, type name, components, bytes, first word, last word)} for the switched-on slots."""
    d = img.read(va, 256)
    slots = {}
    for i in range(16):
        first, offset, fmt, last = struct.unpack_from('<4I', d, 16 * i)
        typ, comps = fmt & 0xF, (fmt >> 4) & 0xF
        if comps:
            name, size = TYPES.get(typ, (f'type{typ}', 4))
            slots[i] = (offset, name, comps, 4 if typ == 6 else size * comps, first, last)
    return slots


def operand_reads(w):
    """The operands (A, B, C) this instruction actually reads."""
    mac, ilu = MAC[field(w, 'mac')] if field(w, 'mac') < len(MAC) else '?', ILU[field(w, 'ilu')]
    used = set(MAC_USES.get(mac, ''))
    if ilu != 'NOP':
        used.add('C')
    return mac, ilu, used


SWIZZLE = 'xyzw'
# each operand's negate bit and four swizzle fields (dword, bit), as xemu's vsh.c maps them
OPERAND_FIELDS = {
    'A': ((1, 8), [(1, 6), (1, 4), (1, 2), (1, 0)]),
    'B': ((2, 25), [(2, 23), (2, 21), (2, 19), (2, 17)]),
    'C': ((2, 10), [(2, 8), (2, 6), (2, 4), (2, 2)]),
}


def bits(words, d, bit, size):
    return (words[d] >> bit) & ((1 << size) - 1)


def operand(w, which):
    """One input as text: -r0.xyzw, v3.x, c[96+a0.x].yyyy, ..."""
    mux = field(w, which.lower() + '_mux')
    if which == 'A':
        reg = field(w, 'a_r')
    elif which == 'B':
        reg = field(w, 'b_r')
    else:
        reg = (field(w, 'c_r_hi') << 2) | field(w, 'c_r_lo')
    if mux == 1:
        name = 'oPos' if reg == 12 else f'r{reg}'
    elif mux == PARAM_V:
        name = f'v{field(w, "v")}'
    elif mux == PARAM_C:
        name = f'c[{field(w, "const")}+a0.x]' if field(w, 'a0x') else f'c{field(w, "const")}'
    else:
        name = f'?{mux}'
    neg, swz = OPERAND_FIELDS[which]
    sw = ''.join(SWIZZLE[bits(w, d, b, 2)] for d, b in swz)
    return ('-' if bits(w, neg[0], neg[1], 1) else '') + name + ('' if sw == 'xyzw' else '.' + sw)


def mask(m):
    return ''.join(c for i, c in enumerate('xyzw') if m & (8 >> i))


def disassemble(w):
    """The instruction's lines: the MAC unit's and the ILU's, each with every register it writes."""
    mac, ilu, _ = operand_reads(w)
    lines = []
    out_mask, out_target = field(w, 'out_o_mask'), field(w, 'out_address')
    out_name = (OUT_NAMES.get(out_target, f'o{out_target}') if field(w, 'out_orb') else f'c{out_target}')
    if mac != 'NOP':
        args = ', '.join(operand(w, o) for o in MAC_USES.get(mac, ''))
        dests = []
        if field(w, 'out_mac_mask'):
            dests.append(f'r{field(w, "out_r")}.{mask(field(w, "out_mac_mask"))}')
        if out_mask and field(w, 'out_mux') == 0:
            dests.append(f'{out_name}.{mask(out_mask)}')
        if mac == 'ARL':
            dests.append('a0.x')
        lines.append(f'{mac} {" ".join(dests) or "(nothing)"} = {args}')
    if ilu != 'NOP':
        dests = []
        if field(w, 'out_ilu_mask'):
            dests.append(f'r{1 if mac != "NOP" else field(w, "out_r")}.{mask(field(w, "out_ilu_mask"))}')
        if out_mask and field(w, 'out_mux') == 1:
            dests.append(f'{out_name}.{mask(out_mask)}')
        lines.append(f'{ilu} {" ".join(dests) or "(nothing)"} = {operand(w, "C")}')
    return lines


def show(image, addresses):
    img = Image(image)
    for text in addresses:
        va = int(text, 16)
        typ, count, ins, problems = decode(img, va)
        print(f'{va:08X}: type {typ:04X}, {count} instructions{"; " + "; ".join(problems) if problems else ""}')
        for i, w in enumerate(ins):
            for line in disassemble(w):
                print(f'  {i:2d}: {line}')


def main(image, shaders_tsv, out):
    img = Image(image)
    progs = collections.OrderedDict()
    for line in open(shaders_tsv, encoding='utf-8').read().splitlines()[1:]:
        c = line.split('\t')
        if c[0] == 'vertex' and c[4] != '?':
            progs.setdefault((int(c[4], 16), int(c[2], 16)), set()).add(c[8] if len(c) > 8 else '?')
    rows, bad, ops, strides = [], 0, collections.Counter(), collections.Counter()
    for (va, lva), objs in progs.items():
        typ, count, ins, problems = decode(img, va)
        slots = layout(img, lva)
        inputs, outputs, consts, used_ops = set(), set(), set(), set()
        for w in ins:
            mac, ilu, used = operand_reads(w)
            ops[mac] += mac != 'NOP'
            ops['ilu ' + ilu] += ilu != 'NOP'
            used_ops |= {o for o in (mac, ilu) if o != 'NOP'}
            muxes = {'A': field(w, 'a_mux'), 'B': field(w, 'b_mux'), 'C': field(w, 'c_mux')}
            if any(muxes[o] == PARAM_V for o in used):
                inputs.add(field(w, 'v'))
            if any(muxes[o] == PARAM_C for o in used):
                consts.add(field(w, 'const'))
            if field(w, 'out_o_mask') and field(w, 'out_orb') == 1:
                outputs.add(field(w, 'out_address'))
        problems += [f'reads v{v}, slot off' for v in sorted(inputs - set(slots))]
        problems += [f'slot v{v} word0={s[4]:X} word3={s[5]:X}' for v, s in slots.items() if s[4] or s[5]]
        bad += bool(problems)
        stride = max((s[0] + s[3] for s in slots.values()), default=0)
        strides[stride] += 1
        rows.append((f'{va:08X}', f'{lva:08X}', f'{typ:04X}', count, ','.join(sorted(objs)),
                     ' '.join(f'v{v}' for v in sorted(inputs)),
                     ' '.join(f'v{v}@{s[0]}:{s[1]}x{s[2]}' for v, s in sorted(slots.items())), stride,
                     ' '.join(OUT_NAMES.get(o, f'o{o}') for o in sorted(outputs)),
                     f'c{min(consts)}-c{max(consts)}' if consts else '', ' '.join(sorted(used_ops)),
                     '; '.join(problems[:4])))
    with open(out, 'w', encoding='utf-8') as f:
        f.write('program\tlayout\theader_type\tinstructions\tfunctions\tinputs_read\tvertex_layout\t'
                'stride\toutputs_written\tconstants\topcodes\tproblems\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print(f'{len(rows)} program and layout pairs, {len({r[0] for r in rows})} programs, {len({r[1] for r in rows})} '
          f'layouts; checks failed on {bad}')
    print('strides from the layouts:', dict(sorted(strides.items())))
    print('opcodes:', dict(ops.most_common()))
    for r in rows:
        print('  ', r[0], r[3], r[4], '|', r[5], '|', r[7], '|', r[9], '|', 'ARL' if 'ARL' in r[10] else '', '|', r[11])


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[2] == '--show':
        show(sys.argv[1], sys.argv[3:])
    else:
        main(*sys.argv[1:])
