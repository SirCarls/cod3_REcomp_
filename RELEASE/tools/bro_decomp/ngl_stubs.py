#!/usr/bin/env python3
# [acct2-bro] Big Red One port, phase 1: logging stand-ins for NGL's functions (142 in the campaign program).
#
# NGL is the game's graphics library; in the port it is rewritten on Vulkan (ngl_vk/). Until then each NGL function
# is replaced by a stand-in that logs the call and returns, so the recompiled game can run its frame loop with no
# picture. This tool makes two things:
#   - the function table (bro/config/ngl_functions.tsv): address, name, how many argument bytes the function removes
#     from the stack when it returns ("ret N"), which registers it reads as arguments, and what it returns;
#   - the stand-ins in C, for xboxrecomp's recomp_lookup_manual(): each reads its stack arguments the way the
#     recompiled code passes them (return address at [esp], arguments from [esp+4]), logs, sets eax and pops the
#     stack exactly as the original's ret does.
# "ret N" and the register arguments are read from the program's own instructions with objdump, so nothing about
# the calling convention is guessed.
#
# Read-only. Usage:
#   ngl_stubs.py IMAGE_PE BRO_EXPORT_DIR NGL_SPEC_TSV OUT_TABLE_TSV [OUT_C [MORE_SPEC_TSV ...]]
#   ngl_stubs.py --from-table TABLE_TSV OUT_C      (the build's step: C from bro/config/stand_in_functions.tsv)
#   IMAGE_PE: the program as a PE (builds/bro_sp/image.pe); BRO_EXPORT_DIR: the Ghidra export (functions.tsv has the
#   sizes); NGL_SPEC_TSV: dev/bro-decomp/bro_sp/ngl_spec.tsv. MORE_SPEC_TSV: more functions in the same format, e.g.
#   the Direct3D and DirectSound functions xdk_stand_ins.py lists; they get stand-ins in the same table.
import csv, os, re, struct, subprocess, sys

ARG_REGS = ['eax', 'ecx', 'edx']
SUB = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx', 'dl': 'edx', 'dh': 'edx',
       'dx': 'edx'}
WRITES_FIRST = re.compile(r'^(mov\w*|lea|pop|set\w+|cdq|cvt\w+|sqrt\w+|rcp\w+|rsqrt\w+)\b')
SELF_CLEAR = ('xor', 'xorps', 'xorpd', 'pxor', 'sub')


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def image_base(path):
    with open(path, 'rb') as f:
        b = f.read(0x400)
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    return struct.unpack_from('<I', b, pe + 24 + 28)[0]


def disassemble(image, start, size):
    out = subprocess.run(['objdump', '-d', '-M', 'intel', '--no-show-raw-insn', f'--start-address={start}',
                          f'--stop-address={start + size}', image], capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\s+(.*)$', line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).split('#')[0].strip()))
    return insns


def regs_in(text):
    return {SUB.get(t, t) for t in re.findall(r'\b(e[a-d]x|[a-d][lhx]|xmm[0-7])\b', text)}


def ret_sizes(insns):
    rets = set()
    for _, text in insns:
        m = re.match(r'ret[nd]?\s*(0x[0-9a-f]+|\d+)?\s*$', text)
        if m:
            rets.add(int(m.group(1), 0) if m.group(1) else 0)
    return rets


def tail_target(insns):
    """The function a body ends in by a direct jmp (a tail call), or None."""
    for _, text in reversed(insns):
        m = re.match(r'jmp\s+(?:0x)?([0-9a-f]+)\s*$', text)   # objdump writes "jmp 0x66ed0"
        if m:
            return int(m.group(1), 16)
        if text:
            return None
    return None


def analyze(insns, follow=None):
    """(stack bytes removed by ret, register arguments read before written, problems). follow(addr) gives a
    tail-called function's instructions."""
    rets, problems = ret_sizes(insns), []
    hops = 0
    cur = insns
    while not rets and follow and hops < 4:
        t = tail_target(cur)
        if t is None:
            break
        cur = follow(t)
        if not cur:
            break
        rets = ret_sizes(cur)
        hops += 1
        if rets:
            problems.append(f'ret from tail-called 0x{t:08X}')
    if not rets:
        problems.append('no ret found (tail jump or size too small)')
    elif len(rets) > 1:
        problems.append(f'several ret sizes {sorted(rets)}')
    # register arguments: read before written, in straight-line code up to the first call or branch
    written, read = set(), []
    for _, text in insns:
        mn = text.split()[0] if text else ''
        if mn in ('call', 'jmp', 'ret') or mn.startswith('j'):
            break
        ops = text[len(mn):].strip()
        parts = [p.strip() for p in ops.split(',')]
        dst = parts[0] if parts else ''
        srcs = ','.join(parts[1:]) if len(parts) > 1 else ''
        # registers used as sources, or inside a memory operand of the destination, are reads
        used = regs_in(srcs) | (regs_in(dst) if '[' in dst else set())
        if not WRITES_FIRST.match(mn) and '[' not in dst:
            used |= regs_in(dst)        # add, cmp, test, ... read their destination too
        if mn in SELF_CLEAR and len(parts) == 2 and parts[0] == parts[1]:
            used = set()                 # xor r, r only writes
        for r in sorted(used):
            if r not in written and r not in read and (r in ARG_REGS or r.startswith('xmm')):
                read.append(r)
        if '[' not in dst:
            written |= regs_in(dst)
    return (max(rets) if rets else None), read, problems


def c_name(name):
    return re.sub(r'\W', '_', name)


# What a stand-in hands back. A caller that gets 0 where the real function returns a pointer would read or write
# guest address 0, so pointer-returning functions get a zeroed block of guest memory instead.
SCREEN = {'nglGetScreenWidth': '640', 'nglGetScreenHeight': '480'}


def return_policy(name, returns):
    """zero, one, block (a zeroed 4 KB guest block kept per function), scratch (a shared 1 MB guest buffer, for Lock
    functions whose callers write vertices or pixels into it) or a fixed number."""
    if name in SCREEN:
        return SCREEN[name]
    if name == 'nglTexture_Lock':
        return 'one'        # returns 1; the pixels go through the texture's locked rectangle (NATIVE_PART)
    if name == 'nglLoadTexture':
        # (name, type, data, size): made natively from the file in memory (bro/src/ngl/ngl_texture_load.c)
        return 'ngl_texture_load(MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16))'
    if re.search(r'Lock(?!ed)', name) and 'Unlock' not in name:
        return 'scratch'
    if returns.strip() == 'bool':
        return 'one'
    if '*' in returns or (name.startswith('ngl') and re.search(r'Get\w*(Manager|Texture|Matrix)$|Create', name)
                          and returns.strip() != 'void'):
        # (only NGL's own Create*/Get* hand back objects; Direct3D's and DirectSound's return a status, 0 = success)
        return 'block'
    return 'zero'


# Functions that hand back an object through a pointer argument instead of returning it: which stack argument
# (0 = the first, at [esp+4]) is that pointer, from the XDK's own declarations. Ghidra types those arguments
# "uint *", so they can't be found by type. The stand-in writes a zeroed block's address there, so a caller that
# reads the object back gets valid guest memory rather than whatever was in its local variable. (Every call to one
# function gets the same block; fine while all the object's methods are stand-ins too.)
OUT_POINTER_ARG = {
    'DirectSoundCreate': 1,                 # (guid, IDirectSound **, outer)
    'IDirectSound_CreateSoundBuffer': 2,    # (dsound, desc, IDirectSoundBuffer **, outer)
    'DirectSoundCreateBuffer': 1,           # (desc, IDirectSoundBuffer **)
    'DirectSoundCreateStream': 1,           # (desc, IDirectSoundStream **)
    'XFileCreateMediaObjectAsync': 2,       # (file, max packets, XMediaObject **)
}


# Stand-ins that also do part of the real function's work natively: a C call made after the log line, before the
# return value is set.
#   NGL's frame timing (bro/src/ngl/ngl_frame_clock.c): the vblank count and the frame interval's wait, which the
#   game's frame time is made from.
#   nglTexture_Lock (bro/src/ngl/ngl_texture.c): the locked rectangle its callers write pixels through.
#   nglListAddQuad (bro/src/ngl/ngl_quad.c): a copy of the quad, for drawing at the end of the frame.
NATIVE_PART = {
    'nglBeginFrame': 'ngl_frame_clock_begin_frame();',
    'nglEndFrame': 'ngl_frame_clock_end_frame();',
    'nglTexture_Lock': 'ngl_texture_lock(MEM32(g_esp + 4));',
    'nglListAddQuad': 'ngl_quad_add(MEM32(g_esp + 4));',
}


def out_pointer_arg(name):
    """The out-pointer argument's index for a library function (its name without the library prefix), or None."""
    return OUT_POINTER_ARG.get(name.split('__')[-1])


def native_function(row):
    """The C function named by a "native NAME" entry in the stand_in_return column, or None. Those rows replace a
    game function whose work can't be translated instruction by instruction (the script threads' stack switching,
    bro/src/script/bro_script_threads.c), so they get no logging stand-in."""
    policy = row['stand_in_return'].split()
    return policy[1] if len(policy) == 2 and policy[0] == 'native' else None


def stack_params(params):
    """How many of the declared parameters are on the stack ("this" travels in ecx)."""
    return len([p for p in params.split(',') if p.strip() and p.strip() != 'void' and not p.strip().endswith('this')])


def write_c(rows, path):
    lines = [
        '/* Generated by tools/bro_decomp/ngl_stubs.py from bro/config/ngl_functions.tsv. Do not edit by hand.',
        ' *',
        ' * Phase 1 stand-ins for NGL, the game\'s graphics library: each logs its call and returns, so the recompiled',
        ' * game runs its frame loop with no picture. Arguments are read the way the recompiled code passes them',
        ' * (return address at [esp], arguments from [esp+4]); each pops the stack as the original\'s "ret N" does. */',
        '#include <stdint.h>',
        '#include "recomp_types.h"',
        '#include "ngl_log.h"',
        '#include "ngl_frame_clock.h"',
        '#include "ngl_scene.h"',
        '#include "ngl_font.h"',
        '#include "ngl_texture.h"',
        '#include "ngl_quad.h"',
        '#include "ngl_texture_load.h"',
        '#include "ngl_quad_batch.h"',
        '#include "ngl_mesh.h"',
        '#include "bro_pad.h"',
        '#include "bro_media.h"',
        '#include "bro_sound.h"',
        '#include "bro_script_threads.h"',
        '',
    ]
    for r in rows:
        n = c_name(r['name'])
        native = native_function(r)
        if native:
            # done entirely in native code, which reads its own arguments and pops the stack itself
            lines.append(f'/* {r["name"]} 0x{r["address"]}: done natively by {native}() */')
            lines.append(f'void sub_{int(r["address"], 16):08X}(void) {{ {native}(); }}')
            lines.append('')
            continue
        popped = int(r['stack_bytes']) // 4 if r['stack_bytes'] not in ('', '?') else 0
        words = max(popped, stack_params(r['params']))     # caller-cleaned (ret 0) functions still have arguments
        reg_args = [x for x in r['register_args'].split(',') if x in ARG_REGS]
        lines.append(f'/* {r["name"]} 0x{r["address"]}: {r["params"] or "void"}; returns {r["returns"]} */')
        lines.append(f'static void ngl_stand_in_{n}(void)')
        lines.append('{')
        args = [f'g_{x}' for x in reg_args]
        args += [f'MEM32(g_esp + {4 + 4 * i})' for i in range(min(words, 6))]
        while len(args) < 6:
            args.append('0')
        nargs = min(len(reg_args) + words, 6)
        lines.append(f'    ngl_log_call(NGL_{n.upper()}, "{r["name"]}", {nargs}, {", ".join(args[:6])});')
        out = out_pointer_arg(r['name'])
        if out is not None:
            lines.append(f'    {{ uint32_t out = MEM32(g_esp + {4 + 4 * out}); '
                         f'if (out) MEM32(out) = ngl_stand_in_block(NGL_{n.upper()}); }}')
        if r['name'] in NATIVE_PART:
            lines.append('    ' + NATIVE_PART[r['name']])
        policy = r['stand_in_return']
        lines.append({'zero': '    g_eax = 0;', 'one': '    g_eax = 1;',
                      'block': f'    g_eax = ngl_stand_in_block(NGL_{n.upper()});',
                      'scratch': '    g_eax = ngl_stand_in_scratch();'}.get(policy, f'    g_eax = {policy};'))
        if r['stack_bytes'] in ('', '?'):
            lines.append('    g_esp += 4;  /* ret size unknown: see ngl_functions.tsv */')
        else:
            lines.append(f'    g_esp += {4 + int(r["stack_bytes"])};  /* ret {r["stack_bytes"]} */')
        lines.append('}')
        # The recompiler leaves every --manual-functions address undefined; its dispatch table names it sub_<address>
        lines.append(f'void sub_{int(r["address"], 16):08X}(void) {{ ngl_stand_in_{n}(); }}')
        lines.append('')
    lines.append('enum { NGL_FUNCTION_COUNT = %d };' % len(rows))
    lines.append('')
    lines.append('/* For recomp_lookup_manual(): the stand-in for an NGL address, or NULL. */')
    lines.append('recomp_func_t ngl_stand_in_for(uint32_t xbox_va)')
    lines.append('{')
    lines.append('    switch (xbox_va) {')
    for r in rows:
        target = native_function(r) or f'ngl_stand_in_{c_name(r["name"])}'
        lines.append(f'    case 0x{r["address"]}u: return {target};')
    lines.append('    default: return 0;')
    lines.append('    }')
    lines.append('}')
    # the ids used by ngl_log_call
    enum = ['/* Generated with ngl_stand_ins.c. */', '#pragma once', 'enum ngl_function_id {']
    enum += [f'    NGL_{c_name(r["name"]).upper()},' for r in rows]
    enum += ['    NGL_ID_COUNT', '};']
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    # the ids header keeps one name, whatever the C file is called: bro/src/ngl/ngl_log.h includes it
    with open(os.path.join(os.path.dirname(path), 'ngl_stand_ins_ids.h'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(enum) + '\n')


def main(image, export_dir, spec_tsv, out_tsv, out_c=None, *more_specs):
    sizes = {int(r['entry'], 16): int(r['size'] or 0) for r in read_tsv(os.path.join(export_dir, 'functions.tsv'))}
    starts = sorted(sizes)
    next_start = {a: b for a, b in zip(starts, starts[1:])}
    rows, problems = [], 0
    for s in [row for path in (spec_tsv,) + more_specs for row in read_tsv(path)]:
        addr = int(s['addr'], 16)
        size = sizes.get(addr, 0)
        insns = disassemble(image, addr, size) if size else []
        follow = lambda a: disassemble(image, a, sizes.get(a, 0)) if sizes.get(a) else []
        ret, regs, probs = analyze(insns, follow) if insns else (None, [], ['no size in functions.tsv'])
        if ret is None and addr in next_start:
            # the export's size can stop short of the function's real end; read up to the next function instead
            insns = disassemble(image, addr, next_start[addr] - addr)
            ret, regs, probs = analyze(insns, follow)
            probs.append(f'size {size} too small, read to next function 0x{next_start[addr]:08X}')
        problems += ret is None or any(p.startswith('several ret') for p in probs)
        rows.append({'address': f'{addr:08X}', 'name': s['name'], 'group': s.get('group', ''),
                     'stack_bytes': '?' if ret is None else str(ret), 'register_args': ','.join(regs),
                     'returns': s.get('returns', ''), 'params': s.get('params', ''), 'size': str(size),
                     'problems': '; '.join(probs),
                     'stand_in_return': return_policy(s['name'], s.get('returns', ''))})
    cols = ['address', 'name', 'group', 'stack_bytes', 'register_args', 'returns', 'stand_in_return', 'params', 'size',
            'problems']
    with open(out_tsv, 'w', encoding='utf-8') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write('\t'.join(r[c] for c in cols) + '\n')
    if out_c:
        write_c(rows, out_c)
    stack = {}
    for r in rows:
        stack[r['stack_bytes']] = stack.get(r['stack_bytes'], 0) + 1
    print(f'{len(rows)} NGL functions; unresolved: {problems}')
    print('ret sizes:', dict(sorted(stack.items(), key=lambda x: (x[0] == "?", x[0]))))
    print('returns:', {p: [r['name'] for r in rows if r['stand_in_return'] == p]
                       for p in sorted({r['stand_in_return'] for r in rows}) if p != 'zero'})
    print('register arguments:', sum(1 for r in rows if r['register_args']), 'functions:',
          sorted({r['register_args'] for r in rows if r['register_args']}))
    for r in rows:
        if r['problems']:
            print('  ', r['address'], r['name'], r['problems'])


def from_table(table_tsv, out_c):
    """The C stand-ins from a table this tool wrote before, without the program: what the build runs."""
    rows = read_tsv(table_tsv)
    write_c(rows, out_c)
    print(f'{len(rows)} stand-ins written to {out_c}')


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--from-table':
        from_table(sys.argv[2], sys.argv[3])
    else:
        main(*sys.argv[1:])
