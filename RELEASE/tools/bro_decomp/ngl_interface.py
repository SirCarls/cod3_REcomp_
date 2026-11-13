#!/usr/bin/env python3
# [acct2-bro] Treyarch's NGL graphics library in Big Red One: the interface a Vulkan NGL must provide.
# Read-only. Inputs: a Ghidra export folder (functions.tsv, calls.tsv, globals.tsv), the XBE sections table.
# NGL = functions named ngl*, the material shaders (cdWorld_*, Shader*), and unnamed helpers only NGL calls.
# The interface = NGL functions that code outside NGL calls. Also lists every direct Direct3D use outside NGL,
# and the push-buffer evidence: who calls BeginPush/EndPush/RunPushBuffer and the draw calls, and which code
# shares Direct3D's own globals (the device state D3D8LTCG lets callers touch directly).
import bisect, collections, csv, os, re, sys

GROUPS = [  # first match wins, so the specific families come first
    ('quad', r'Quad'),
    ('texture', r'Texture|FrontBuffer|BackBuffer|RenderTarget|Surface|Palette|DDS'),
    ('text', r'Text(?!ure)|Font|Glyph'),
    ('material', r'Material|Shader|StateCache|Blend'),
    ('mesh', r'Mesh|Poly|Vertex|Index|Section|Strip|Node|Skin|Bone|Instance|Batch|SkipList'),
    ('light', r'Light|Ambient'),
    ('scene', r'Scene|Viewport|Perspective|Camera|Clear|Depth|ColorWrite|Fog|Frustum|Ortho|View|Matrix|Aspect|Project'),
    ('frame', r'Frame|Init|Reset|VBlank|Screen(Width|Height)|DeviceBindings|Flip|Swap|Present|Shutdown|VideoMode|Callback'),
    ('memory', r'Mem|Alloc|File|Read|Heap|Pool|AddRef|Release'),
    ('debug', r'Dump|Debug|Fps|Screenshot|Printf|Stat'),
]


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def head(name):
    if name.startswith(('FUN_', 'thunk_FUN_')):
        return '(unnamed)'
    if name.startswith('ngl'):
        return 'ngl'
    i = name.find('_', 1)
    return name[:i] if i > 0 else name


def group_of(name):
    if re.match(r'cd[A-Z]|Shader', name):
        return 'material'
    for g, rx in GROUPS:
        if re.search(rx, name):
            return g
    return 'other'


def main(export_dir, sections_path, out_dir, cod3_matches=None):
    cod3 = {}
    if cod3_matches:
        for r in read_tsv(cod3_matches):
            cod3[int(r['bro'], 16)] = r['cod3']
    secs = {s['name']: (int(s['va'], 16), int(s['va'], 16) + int(s['vsize'], 16)) for s in read_tsv(sections_path)}
    d3d_lo, d3d_hi = secs['D3D']
    xg_lo, xg_hi = secs['XGRPH']
    funcs = {int(f['entry'], 16): f for f in read_tsv(os.path.join(export_dir, 'functions.tsv'))}
    in_d3d = lambda a: d3d_lo <= a < d3d_hi
    in_xg = lambda a: xg_lo <= a < xg_hi
    calls = []
    for c in read_tsv(os.path.join(export_dir, 'calls.tsv')):
        try:
            calls.append((int(c['caller'], 16), int(c['callee'], 16)))
        except ValueError:
            pass
    callers = collections.defaultdict(list)
    callees = collections.defaultdict(set)
    for s, d in calls:
        callers[d].append(s)
        callees[s].add(d)
    nm = lambda a: funcs[a]['name'] if a in funcs else '?%08X' % a

    member = {a: 'named' for a, f in funcs.items() if f['name'].startswith('ngl')}
    member.update({a: 'material' for a, f in funcs.items() if re.match(r'(cd[A-Z]|Shader[A-Z])', f['name'])})
    named = [a for a, k in member.items() if k == 'named']
    lo = min(named) & ~0xFFF  # NGL's address range, from its first to its last ngl* function
    hi = (max(named) + 0x1000) & ~0xFFF
    calls_d3d = {s for s, d in calls if in_d3d(d) or in_xg(d)}
    changed = True
    while changed:
        # unnamed functions in NGL's address range that only NGL calls, or that nothing calls directly (vtable
        # methods such as the material shaders' render-state setters) and that call Direct3D or NGL
        changed = False
        for a, f in funcs.items():
            if a in member or not f['name'].startswith(('FUN_', 'thunk_FUN_')) or not lo <= a < hi:
                continue
            cs = [s for s in callers.get(a, []) if not in_d3d(s)]
            if cs and all(s in member for s in cs):
                member[a] = 'helper'
                changed = True
            elif not cs and (a in calls_d3d or any(d in member for d in callees.get(a, ()))):
                member[a] = 'method'
                changed = True

    ext_sites = collections.Counter()
    ext_from = collections.defaultdict(collections.Counter)
    int_sites = collections.Counter()
    d3d_by_member = collections.Counter()
    d3d_outside = collections.defaultdict(collections.Counter)
    for s, d in calls:
        if d in member:
            if s in member:
                int_sites[d] += 1
            elif not in_d3d(s) and not in_xg(s):
                ext_sites[d] += 1
                ext_from[d][head(nm(s))] += 1
        if in_d3d(d) and not in_d3d(s):
            if s in member:
                d3d_by_member[d] += 1
            else:
                d3d_outside[head(nm(s))][nm(d).split('__')[-1]] += 1

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'ngl_api.tsv'), 'w', encoding='utf-8') as f:
        f.write('addr\tname\tgroup\tsize\toutside_sites\tinside_sites\toutside_callers\tcod3\tsignature\n')
        for a in sorted(ext_sites, key=lambda x: (group_of(nm(x)), -ext_sites[x])):
            fr = ', '.join(f'{k} {v}' for k, v in ext_from[a].most_common(5))
            f.write(f"{a:08X}\t{nm(a)}\t{group_of(nm(a))}\t{funcs[a]['size']}\t{ext_sites[a]}\t{int_sites[a]}\t{fr}\t{cod3.get(a, "")}\t{funcs[a]['signature']}\n")
    with open(os.path.join(out_dir, 'ngl_members.tsv'), 'w', encoding='utf-8') as f:
        f.write('addr\tname\tkind\tgroup\tsize\toutside_sites\tinside_sites\n')
        for a in sorted(member):
            f.write(f"{a:08X}\t{nm(a)}\t{member[a]}\t{group_of(nm(a))}\t{funcs[a]['size']}\t{ext_sites[a]}\t{int_sites[a]}\n")

    s = []
    kinds = collections.Counter(member.values())
    size = sum(int(funcs[a]['size'] or 0) for a in member)
    s.append(f'NGL members: {len(member)} ({", ".join(f"{k} {v}" for k, v in kinds.items())}), {size} bytes of code, '
             f'0x{lo:X}-0x{hi:X}')
    s.append(f'interface (called from outside NGL): {len(ext_sites)} functions, {sum(ext_sites.values())} call sites')
    g = collections.Counter()
    gs = collections.Counter()
    for a in ext_sites:
        g[group_of(nm(a))] += 1
        gs[group_of(nm(a))] += ext_sites[a]
    s.append('  by group (functions/sites): ' + ', '.join(f'{k} {g[k]}/{gs[k]}' for k, _ in gs.most_common()))
    if cod3:
        paired = [a for a in ext_sites if a in cod3]
        s.append(f'  paired with a CoD3 function (match_builds.py): {len(paired)} of {len(ext_sites)} interface functions, '
                 f'{sum(1 for a in member if a in cod3)} of {len(member)} members')
    allfrom = collections.Counter()
    for a in ext_from:
        allfrom.update(ext_from[a])
    s.append('  callers: ' + ', '.join(f'{k} {v}' for k, v in allfrom.most_common(20)))
    s.append(f'Direct3D calls from NGL: {sum(d3d_by_member.values())}; from outside NGL: '
             f'{sum(sum(c.values()) for c in d3d_outside.values())}')
    for k, c in sorted(d3d_outside.items(), key=lambda kv: -sum(kv[1].values()))[:14]:
        s.append(f'  {k:24} {sum(c.values()):4}  ' + ', '.join(f'{x} {y}' for x, y in c.most_common(5)))
    s.append('Draw and push-buffer entry points, by caller:')
    for a, f in sorted(funcs.items()):
        n = f['name']
        if in_d3d(a) and re.search(r'(Draw|Push|RunPushBuffer|InsertCallback|InsertFence|Swap|SetVertexShaderInputDirect)', n):
            cs = collections.Counter(nm(x) for x in callers.get(a, []) if not in_d3d(x))
            if cs:
                s.append(f'  {n.split("__")[-1]:38} ' + ', '.join(f'{k} {v}' for k, v in cs.most_common(6)))
    s.append('Direct3D globals that code outside the D3D section reads or writes (LTCG device state):')
    rows = []
    for gl in read_tsv(os.path.join(export_dir, 'globals.tsv')):
        refs = [int(x, 16) for x in gl['funcs'].split(',') if x]
        nd = sum(1 for r in refs if in_d3d(r))
        if nd < 8:
            continue
        nn = sum(1 for r in refs if r in member)
        no = [r for r in refs if not in_d3d(r) and r not in member]
        if nn or no:
            rows.append((nd, gl['addr'], gl['name'], nn, len(no), collections.Counter(head(nm(r)) for r in no)))
    for nd, addr, name, nn, no, oc in sorted(rows, reverse=True)[:20]:
        s.append(f'  {addr} {name[:34]:34} D3D {nd:3}  NGL {nn:3}  other {no:3}  ' + ', '.join(f'{k} {v}' for k, v in oc.most_common(4)))
    text = '\n'.join(s) + '\n'
    with open(os.path.join(out_dir, 'ngl_summary.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    print(text)


if __name__ == '__main__':
    if len(sys.argv) not in (4, 5):
        sys.exit('usage: ngl_interface.py EXPORT_DIR SECTIONS_TSV OUT_DIR [MATCHES_COD3_TSV]')
    main(*sys.argv[1:])
