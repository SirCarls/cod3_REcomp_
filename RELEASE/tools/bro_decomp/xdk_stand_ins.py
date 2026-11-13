#!/usr/bin/env python3
# [acct2-bro] Big Red One port, phase 1: the Direct3D and DirectSound functions the game calls from outside NGL.
#
# Direct3D and DirectSound are linked into the program. In the port neither runs: NGL's functions are replaced
# (bro/config/ngl_functions.tsv), so Direct3D is only reached from code that calls it directly (the effects renderer,
# the Xbox Live UI library, the disc-error screen, ...), and DirectSound only from the game's sound layer, which the
# native sound replaces later (ROADMAP phase 6). Each of those library functions gets a logging stand-in the same way
# NGL's do; this tool lists them and writes them in ngl_spec.tsv's format for ngl_stubs.py.
#
# Read-only. Usage: xdk_stand_ins.py BRO_EXPORT_DIR NGL_FUNCTIONS_TSV OUT_REPORT_TXT [OUT_SPEC_TSV]
import collections, csv, os, re, sys


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


GPU = re.compile(r'^(D3D|Direct3D|XGRPH|CDevice)')
SOUND = re.compile(r'^(DSOUND|XAudio)')
NGL_NAME = re.compile(r'^(ngl|cd[A-Z]|Shader|R_Draw)')


def split_signature(sig, name):
    """Ghidra's 'ret name(params)' -> (ret, params)."""
    m = re.match(r'^(.*?)\s*\b' + re.escape(name) + r'\s*\((.*)\)\s*$', sig or '')
    return (m.group(1).strip(), m.group(2).strip()) if m else ('', '')


def main(export_dir, ngl_tsv, out_path, spec_out=None):
    rows = read_tsv(os.path.join(export_dir, 'functions.tsv'))
    funcs = {int(r['entry'], 16): r['name'] for r in rows}
    sigs = {int(r['entry'], 16): r.get('signature', '') for r in rows}
    ngl = {int(r['address'], 16) for r in read_tsv(ngl_tsv)}
    callers, callees = collections.defaultdict(set), collections.defaultdict(set)
    for r in read_tsv(os.path.join(export_dir, 'calls.tsv')):
        try:
            a, b = int(r['caller'], 16), int(r['callee'], 16)
        except (KeyError, ValueError):
            continue
        callers[b].add(a)
        callees[a].add(b)

    # The libraries sit in one block; their unnamed helpers inside it belong to them (but not another library's
    # named functions).
    named = [a for a in funcs if GPU.match(funcs[a]) or SOUND.match(funcs[a])]
    lib_lo, lib_hi = min(named), max(named)
    in_lib = lambda a: lib_lo <= a <= lib_hi and bool(GPU.match(funcs.get(a, '')) or SOUND.match(funcs.get(a, ''))
                                                 or funcs.get(a, '').startswith('FUN_'))
    kind = lambda a: 'sound' if SOUND.match(funcs.get(a, '')) else 'gpu'

    # NGL: its own functions, the material classes' functions (cd*, Shader*), and the unnamed functions between them.
    # Material draws and render passes are reached through tables (indirect calls), which a direct-call walk misses,
    # so an unnamed function counts as NGL when the nearest named functions on both sides are NGL's.
    ngl_named = lambda a: a in ngl or bool(NGL_NAME.match(funcs.get(a, '')))
    order = sorted(funcs)
    named_side = {}
    last = None
    for a in order:
        if not funcs[a].startswith('FUN_'):
            last = a
        named_side[a] = [last, None]
    last = None
    for a in reversed(order):
        if not funcs[a].startswith('FUN_'):
            last = a
        named_side[a][1] = last
    inside = {a for a in order if not in_lib(a) and (ngl_named(a) or (
        funcs[a].startswith('FUN_') and all(n is not None and ngl_named(n) for n in named_side[a])))}
    changed = True
    while changed:
        changed = False
        for a in list(inside):
            for b in callees.get(a, ()):
                if b not in inside and b in funcs and not in_lib(b) and callers[b] and callers[b] <= inside:
                    inside.add(b)
                    changed = True

    outside = {}
    for d in sorted(a for a in funcs if in_lib(a)):
        out = sorted(c for c in callers.get(d, ()) if c not in inside and not in_lib(c))
        if out:
            outside[d] = out
    lines = [f'Libraries {lib_lo:08X}-{lib_hi:08X}: {sum(map(in_lib, funcs))} functions. NGL and what only it '
             f'calls: {len(inside)} functions.',
             f'{len(outside)} library functions called from outside NGL '
             f'({sum(kind(d) == "gpu" for d in outside)} Direct3D, {sum(kind(d) == "sound" for d in outside)} '
             f'DirectSound):']
    by_caller = collections.defaultdict(list)
    for d, out in outside.items():
        lines.append(f'  {kind(d):5s} {d:08X} {funcs[d]:48s} <- ' + ', '.join(funcs.get(c, '?') for c in out[:5])
                     + (f' (+{len(out) - 5})' if len(out) > 5 else ''))
        for c in out:
            by_caller[c].append(funcs[d])
    lines += ['', f'{len(by_caller)} callers outside NGL:']
    for c, ds in sorted(by_caller.items(), key=lambda x: (-len(x[1]), x[0])):
        lines.append(f'  {c:08X} {funcs.get(c, "?"):44s} {len(ds):3d}')
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:2]))

    if spec_out:
        with open(spec_out, 'w', encoding='utf-8') as f:
            f.write('addr\tname\tgroup\treturns\tparams\n')
            for d in outside:
                ret, params = split_signature(sigs.get(d, ''), funcs[d])
                f.write(f'{d:08X}\t{funcs[d]}\t{"direct3d" if kind(d) == "gpu" else "directsound"}\t{ret}\t{params}\n')


if __name__ == '__main__':
    main(*sys.argv[1:])
