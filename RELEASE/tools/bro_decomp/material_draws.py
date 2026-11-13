#!/usr/bin/env python3
# [acct2-bro] The material classes' draw functions in Big Red One and CoD3, side by side. Both games build each
# material class (cdChar, cdSimpleAlpha, ...) the same way; Big Red One's functions are named (cdX_Draw, cdX_GetName,
# ...). In CoD3 a class is found by its GetName, the one function that loads the class name ("cdChar" and so on, the
# s_cd... globals); its draw function is the one just before it in the same class with the most calls into Direct3D.
# For each draw this lists what it calls in order, Direct3D calls shown by name where known, and counts the callees
# that most draws share: those are NGL's per-draw helpers (in Big Red One nglSetTexture, nglSetTransformConstants,
# nglDrawMeshSection, ...), and CoD3's most-shared callees are their likely counterparts.
# Read-only. Usage: material_draws.py BRO_EXPORT_DIR COD3_EXPORT_DIR COD3_D3D_RANGE OUT_TXT [COD3_SYMBOLS_TOML]
#   COD3_D3D_RANGE: hex, end exclusive, e.g. 822E0000-82310000
import collections, csv, os, re, sys

CLASS_SPAN = 0x1400      # how far before GetName a class's draw function may start


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def hexval(s):
    try:
        return int(s, 16)
    except (TypeError, ValueError):
        return None


def load(export_dir):
    funcs = {hexval(r['entry']): r for r in read_tsv(os.path.join(export_dir, 'functions.tsv'))}
    calls = collections.defaultdict(list)
    for r in read_tsv(os.path.join(export_dir, 'calls.tsv')):
        a, s, b = hexval(r['caller']), hexval(r['site']), hexval(r['callee'])
        if None not in (a, s, b):
            calls[a].append((s, b))
    return funcs, {a: [b for _, b in sorted(v)] for a, v in calls.items()}


def shared(draws, skip):
    """callee -> number of draws calling it (each draw counted once)."""
    n = collections.Counter()
    for seq in draws.values():
        n.update({c for c in seq if not skip(c)})
    return n


def main(bro_dir, cod3_dir, d3d_range, out_path, symbols=None):
    lo, hi = (int(x, 16) for x in d3d_range.split('-'))
    lines = []

    # Big Red One: named
    bf, bc = load(bro_dir)
    bname = {a: r['name'] for a, r in bf.items()}
    bro_lib = lambda a: bname.get(a, '').startswith(('D3D', '_ftol', 'eh_', 'XGRPH'))
    bdraws = {bname[a]: bc.get(a, []) for a in bf if re.fullmatch(r'cd\w+_Draw', bname[a])}
    lines.append(f'Big Red One: {len(bdraws)} cd*_Draw functions. Callees shared by most draws (Direct3D left out):')
    for c, n in shared(bdraws, bro_lib).most_common(14):
        lines.append(f'  {n:3d}  {bname.get(c, hex(c))}')

    # CoD3: found through GetName
    cf, cc = load(cod3_dir)
    cname = {a: r['name'] for a, r in cf.items()}
    if symbols and os.path.exists(symbols):
        text = open(symbols, encoding='utf-8').read()
        for m in re.finditer(r'address\s*=\s*0x([0-9A-Fa-f]+)\s*\n\s*name\s*=\s*"([^"]+)"', text):
            cname.setdefault(int(m.group(1), 16), m.group(2))
            if cname[int(m.group(1), 16)].startswith('FUN_'):
                cname[int(m.group(1), 16)] = m.group(2)
    getname = {}
    for g in read_tsv(os.path.join(cod3_dir, 'globals.tsv')):
        m = re.fullmatch(r's_(cd[A-Za-z]+)_[0-9A-Fa-f]{8}', g['name'])
        if not m:
            continue
        for f in filter(None, g['funcs'].split(',')):
            a = hexval(f)
            if a in cf and int(cf[a]['size'] or 0) <= 64:
                getname[a] = m.group(1)
    starts = sorted(cf)
    is_d3d = lambda a: lo <= a < hi
    cdraws, where = {}, {}
    for g, cls in sorted(getname.items()):
        prev = max([x for x in getname if x < g], default=0)
        best = max((a for a in starts if max(prev, g - CLASS_SPAN) < a < g),
                   key=lambda a: sum(map(is_d3d, cc.get(a, []))), default=None)
        if best is not None and sum(map(is_d3d, cc.get(best, []))):
            cdraws[cls] = cc[best]
            where[cls] = (best, g)
    lines += ['', f'CoD3: {len(getname)} GetName functions, {len(cdraws)} with a draw function found:']
    for cls, (d, g) in sorted(where.items(), key=lambda x: x[1]):
        seq = cdraws[cls]
        lines.append(f'  {cls:22s} draw {d:08X}  GetName {g:08X}  calls {len(seq):3d}  Direct3D {sum(map(is_d3d, seq)):3d}')
    lines.append('CoD3 callees shared by most draws (Direct3D left out):')
    common = shared(cdraws, lambda a: is_d3d(a) or cname.get(a, '').startswith('__'))
    for c, n in common.most_common(14):
        lines.append(f'  {n:3d}  {c:08X} {cname.get(c, "")}  size {cf.get(c, {}).get("size", "?")}  '
                     f'Direct3D calls in it {sum(map(is_d3d, cc.get(c, [])))}')
    lines.append('CoD3 Direct3D functions shared by most draws:')
    for c, n in shared(cdraws, lambda a: not is_d3d(a)).most_common(14):
        lines.append(f'  {n:3d}  {c:08X} {cname.get(c, "")}')
    lines += ['', 'Order of the shared helpers in each CoD3 draw (Direct3D calls shown as "."):']
    top = {c for c, n in common.most_common(10)}
    for cls, seq in sorted(cdraws.items()):
        lines.append(f'  {cls:22s} ' + ' '.join(f'{c:08X}' if c in top else '.' if is_d3d(c) else '-' for c in seq))
    open(out_path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main(*sys.argv[1:])
