#!/usr/bin/env python3
# [acct2-bro] Decode Big Red One's precompiled pixel shaders: the Xbox D3DPIXELSHADERDEF each
# nglPixelShader_Init(obj, definition) is given (NV2A register-combiner state, 0xF0 bytes). Says per definition how
# many combiner stages it uses, each texture stage's mode, which registers it reads, and checks the layout before
# trusting it: the stage count must be 1-8, the stages past the count must be empty, every register and texture
# mode must be a valid one. Used to plan the build-time conversion to fragment shaders; nothing runs a shader.
# Read-only. Usage: nv2a_psh.py IMAGE_PE SHADERS_TSV OUT_TSV   (SHADERS_TSV is ngl_shaders.py's output)
#        or:  nv2a_psh.py IMAGE_PE --show VA [VA ...]   (one definition's stages written out as equations, to port it)
import collections, struct, sys

# D3DPIXELSHADERDEF, as dword indices
ALPHA_IN, FINAL_ABCD, FINAL_EFG, CONST0, CONST1, ALPHA_OUT, RGB_IN = 0, 8, 9, 10, 18, 26, 34
COMPARE, FINAL_C0, FINAL_C1, RGB_OUT, COUNT, TEXMODES, DOTMAP, INPUTTEX, C0MAP, C1MAP, FINAL_CONSTS = \
    42, 43, 44, 45, 53, 54, 55, 56, 57, 58, 59
SIZE = 60 * 4

REGS = {0: 'zero', 1: 'c0', 2: 'c1', 3: 'fog', 4: 'v0', 5: 'v1', 8: 't0', 9: 't1', 10: 't2', 11: 't3', 12: 'r0',
        13: 'r1', 14: 'v1r0sum', 15: 'efprod'}
OUT_REGS = {0: None, 4: 'v0', 5: 'v1', 8: 't0', 9: 't1', 10: 't2', 11: 't3', 12: 'r0', 13: 'r1'}
MAPS = ['unsigned', 'invert', 'expand', 'expand_neg', 'halfbias', 'halfbias_neg', 'signed', 'signed_neg']
MODE_NAMES = ['none', 'project2d', 'project3d', 'cubemap', 'passthru', 'clipplane', 'bumpenvmap', 'bumpenvmap_lum',
            'brdf', 'dot_st', 'dot_zw', 'dot_rflct_diff', 'dot_rflct_spec', 'dot_str_3d', 'dot_str_cube',
            'dpndnt_ar', 'dpndnt_gb', 'dotproduct', 'dot_rflct_spec_const']


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
            self.secs.append((self.base + va, raw, rsize))

    def read(self, va, n):
        for start, raw, rsize in self.secs:
            if start <= va and va + n <= start + rsize:
                return self.b[raw + va - start: raw + va - start + n]
        return None


def inputs(word):
    """The four 8-bit inputs A, B, C, D of a combiner input word: (register, alpha channel, mapping)."""
    return [((word >> s) & 0xF, (word >> s) & 0x10, MAPS[((word >> s) >> 5) & 7]) for s in (24, 16, 8, 0)]


def decode(d):
    w = struct.unpack('<60I', d)
    problems, regs, maps, outs = [], collections.Counter(), collections.Counter(), collections.Counter()
    count = w[COUNT] & 0xFF
    if not 1 <= count <= 8:
        problems.append(f'stage count {count}')
    for st in range(8):
        words = (w[RGB_IN + st], w[ALPHA_IN + st], w[RGB_OUT + st], w[ALPHA_OUT + st])
        if st >= count:
            if any(words):
                problems.append(f'stage {st} past the count is not empty')
            continue
        for word in words[:2]:
            for reg, alpha, mp in inputs(word):
                if reg not in REGS:
                    problems.append(f'stage {st} input register {reg}')
                if reg:
                    regs[REGS.get(reg, reg)] += 1
                    maps[mp] += 1
        for word in words[2:]:
            for reg in (word & 0xF, (word >> 4) & 0xF, (word >> 8) & 0xF):
                if reg not in OUT_REGS:
                    problems.append(f'stage {st} output register {reg}')
                elif OUT_REGS[reg]:
                    outs[OUT_REGS[reg]] += 1
            if word & (1 << 12) or word & (1 << 13):
                outs['dot product'] += 1
            if word & (1 << 14):
                outs['mux'] += 1
    for word in (w[FINAL_ABCD], w[FINAL_EFG]):
        for reg, alpha, mp in inputs(word):
            if reg not in REGS:
                problems.append(f'final combiner register {reg}')
            elif reg:
                regs[REGS[reg]] += 1
    modes = []
    for st in range(4):
        m = (w[TEXMODES] >> (5 * st)) & 0x1F
        if m >= len(MODE_NAMES):
            problems.append(f'texture stage {st} mode {m}')
        modes.append(MODE_NAMES[m] if m < len(MODE_NAMES) else f'mode{m}')
    return count, modes, regs, maps, outs, problems


OUT_MAPS = {0: '', 1: ' - 0.5', 2: ' * 2', 3: ' * 2 - 1', 4: ' * 4', 6: ' / 2'}


def operand(code):
    """One combiner input as text: register, channel and mapping (e.g. 1-t0.a)."""
    reg, alpha, mp = code & 0xF, code & 0x10, MAPS[(code >> 5) & 7]
    name = REGS.get(reg, f'reg{reg}') + ('.a' if alpha else '')
    if reg == 0:
        return {'unsigned': '0', 'invert': '1', 'expand': '-1', 'expand_neg': '1', 'halfbias': '-0.5',
                'halfbias_neg': '0.5', 'signed': '0', 'signed_neg': '0'}[mp]
    return {'unsigned': name, 'invert': f'1-{name}', 'expand': f'(2*{name}-1)', 'expand_neg': f'(1-2*{name})',
            'halfbias': f'({name}-0.5)', 'halfbias_neg': f'(0.5-{name})', 'signed': name, 'signed_neg': f'-{name}'}[mp]


def stage_text(inputs_word, outputs_word, channel):
    """A combiner stage's equations for one channel ('rgb' or 'a'), one line per register written."""
    a, b, c, d = (operand((inputs_word >> s) & 0xFF) for s in (24, 16, 8, 0))
    cd_reg, ab_reg, sum_reg = outputs_word & 0xF, (outputs_word >> 4) & 0xF, (outputs_word >> 8) & 0xF
    cd_dot, ab_dot, mux = outputs_word & (1 << 12), outputs_word & (1 << 13), outputs_word & (1 << 14)
    scale = OUT_MAPS.get((outputs_word >> 15) & 7, f' (map {(outputs_word >> 15) & 7})')
    ab = f'dot({a}, {b})' if ab_dot else f'{a} * {b}'
    cd = f'dot({c}, {d})' if cd_dot else f'{c} * {d}'
    lines = []
    for reg, text in ((ab_reg, ab), (cd_reg, cd)):
        if OUT_REGS.get(reg):
            lines.append(f'{OUT_REGS[reg]}.{channel} = ({text}){scale}')
    if OUT_REGS.get(sum_reg):
        both = f'r0.a > 0.5 ? {cd} : {ab}' if mux else f'{ab} + {cd}'
        lines.append(f'{OUT_REGS[sum_reg]}.{channel} = ({both}){scale}')
    return lines


def show(image, addresses):
    img = Image(image)
    for text in addresses:
        va = int(text, 16)
        d = img.read(va, SIZE)
        if d is None:
            print(f'{va:08X}: not readable')
            continue
        w = struct.unpack('<60I', d)
        count = w[COUNT] & 0xFF
        modes = [MODE_NAMES[(w[TEXMODES] >> (5 * st)) & 0x1F] if ((w[TEXMODES] >> (5 * st)) & 0x1F) < len(MODE_NAMES)
                 else 'bad' for st in range(4)]
        print(f'{va:08X}: {count} stage(s), textures {" ".join(modes)}, input texture {w[INPUTTEX]:08X}, '
              f'combiner count word {w[COUNT]:08X}')
        for st in range(count):
            for line in stage_text(w[RGB_IN + st], w[RGB_OUT + st], 'rgb'):
                print(f'  stage {st}: {line}')
            for line in stage_text(w[ALPHA_IN + st], w[ALPHA_OUT + st], 'a'):
                print(f'  stage {st}: {line}')
            if w[CONST0 + st] or w[CONST1 + st]:
                print(f'  stage {st}: c0 = {w[CONST0 + st]:08X}, c1 = {w[CONST1 + st]:08X} (ARGB)')
        a, b, c, dd = (operand((w[FINAL_ABCD] >> s) & 0xFF) for s in (24, 16, 8, 0))
        e, f, g = (operand((w[FINAL_EFG] >> s) & 0xFF) for s in (24, 16, 8))
        flags = w[FINAL_EFG] & 0xFF
        print(f'  final: rgb = {a} * {b} + (1-{a}) * {c} + {dd}; alpha = {g}; efprod = {e} * {f}; '
              f'flags {flags:02X}; final c0 {w[FINAL_C0]:08X} c1 {w[FINAL_C1]:08X}')


def main(image, shaders_tsv, out):
    img = Image(image)
    defs = collections.OrderedDict()
    for line in open(shaders_tsv, encoding='utf-8').read().splitlines()[1:]:
        c = line.split('\t')
        if c[0] == 'pixel' and c[4] != '?':
            defs.setdefault(int(c[4], 16), set()).add(c[8] if len(c) > 8 else '?')
    rows, bad = [], 0
    all_modes, all_regs, all_maps, all_outs, counts = (collections.Counter() for _ in range(5))
    for va, fns in defs.items():
        d = img.read(va, SIZE)
        if d is None:
            rows.append((f'{va:08X}', ','.join(sorted(fns)), '', '', '', '', 'not readable'))
            bad += 1
            continue
        count, modes, regs, maps, outs, problems = decode(d)
        bad += bool(problems)
        counts[count] += 1
        all_modes.update(m for m in modes if m != 'none')
        all_regs.update(regs)
        all_maps.update(maps)
        all_outs.update(outs)
        rows.append((f'{va:08X}', ','.join(sorted(fns)), count, ' '.join(modes),
                     ' '.join(sorted(regs)), ' '.join(sorted(k for k in outs if k in ('dot product', 'mux'))),
                     '; '.join(problems[:3])))
    with open(out, 'w', encoding='utf-8') as f:
        f.write('definition\tfunctions\tstages\ttexture_modes\tregisters_read\tspecial_ops\tproblems\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    print(f'{len(rows)} definitions; checks failed on {bad}')
    print('combiner stages:', dict(sorted(counts.items())))
    print('texture modes (stage uses):', dict(all_modes.most_common()))
    print('registers read:', dict(all_regs.most_common()))
    print('input mappings:', dict(all_maps.most_common()))
    print('outputs and special ops:', dict(all_outs.most_common()))
    for r in rows[:6]:
        print('  ', *r)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[2] == '--show':
        show(sys.argv[1], sys.argv[3:])
    else:
        main(*sys.argv[1:])
