#!/usr/bin/env python3
# [acct2-bro] Big Red One: which Xbox library functions (D3D, XGRPH, DSOUND, ...) the engine and missions call.
# Read-only. Inputs: a Ghidra export folder (functions.tsv, calls.tsv, globals.tsv), the XBE sections table and
# the kernel import names. Output: a plain-text report.
import bisect, collections, csv, os, re, sys

MISSIONS = {'PROLOGUE', 'ORAN', 'TUNISIA', 'KASSPASS', 'KASSRINE', 'LIBEATOR', 'GELA', 'PIANLUPO', 'TROINA',
            'OMAHEACH', 'MONS1', 'EILEDORF', 'BUCHHOLZ', 'DRAGEETH'}
LIBS = ['D3D', 'XGRPH', 'D3DX', 'DSOUND', 'XONLINE', 'XNET', 'XPP', 'DOLBY']
PREFIXES = ('ngl', 'D3DDevice_', 'D3DResource_', 'D3DBaseTexture_', 'D3DTexture_', 'D3DCubeTexture_',
            'D3DVolumeTexture_', 'D3DSurface_', 'D3DVertexBuffer_', 'D3DIndexBuffer_', 'D3DPalette_',
            'D3DPushBuffer_', 'D3D_', 'Direct3D_', 'XG', 'IDirectSound', 'CDirectSound', 'CMcpx', 'DirectSound')


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def prefix(name):
    if name.startswith(('FUN_', 'thunk_FUN_', 'LAB_')):
        return '(unnamed)'
    for p in PREFIXES:
        if name.startswith(p):
            return p.rstrip('_')
    i = name.find('_', 1)
    return name[:i] if i > 0 else name


def main(export_dir, sections_path, kernel_path, out_path):
    secs = sorted((int(s['va'], 16), int(s['va'], 16) + int(s['vsize'], 16), s['name'])
                  for s in read_tsv(sections_path))
    starts = [s[0] for s in secs]

    def section_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return secs[i][2] if i >= 0 and addr < secs[i][1] else '?'

    def group_of(sec):
        return 'missions' if sec in MISSIONS else sec

    funcs = {}
    for f in read_tsv(os.path.join(export_dir, 'functions.tsv')):
        a = int(f['entry'], 16)
        funcs[a] = (f['name'], int(f['size'] or 0), section_of(a))

    def nm(a):
        return funcs.get(a, ('?%08X' % a, 0, ''))[0]

    out = []
    w = out.append
    w('A. Sections: functions, named, code bytes')
    per = collections.defaultdict(lambda: [0, 0, 0])
    for name, size, sec in funcs.values():
        p = per[sec]
        p[0] += 1
        p[2] += size
        if prefix(name) != '(unnamed)':
            p[1] += 1
    for lo, hi, sec in secs:
        if sec in per:
            p = per[sec]
            w(f'  {sec:10} {hi - lo:>8X}  {p[0]:6} funcs  {p[1]:6} named  {p[2]:9} bytes')

    calls = read_tsv(os.path.join(export_dir, 'calls.tsv'))
    into = collections.defaultdict(collections.Counter)
    callee_sites = collections.defaultdict(collections.Counter)
    callee_from = collections.defaultdict(collections.Counter)
    caller_pref = collections.defaultdict(collections.Counter)
    caller_funcs = collections.defaultdict(set)
    for c in calls:
        try:
            src = int(c['caller'], 16)
            dst = int(c['callee'], 16)
        except ValueError:
            continue
        ssec = section_of(src)
        dsec = section_of(dst)
        if dsec not in LIBS or ssec == dsec:
            continue
        g = group_of(ssec)
        into[dsec][g] += 1
        callee_sites[dsec][dst] += 1
        callee_from[dst][g] += 1
        if g == '.text':
            caller_pref[dsec][prefix(nm(src))] += 1
            caller_funcs[dsec].add(src)
    w('')
    w('B. Direct call sites into each library section, by where the caller is')
    for lib in LIBS:
        cnt = into[lib]
        w(f'  {lib:8} sites {sum(cnt.values()):6}  callees {len(callee_sites[lib]):5}  .text callers '
          f'{len(caller_funcs[lib]):5}  ' + ', '.join(f'{k} {v}' for k, v in cnt.most_common()))
    for lib, top in (('D3D', 140), ('XGRPH', 30), ('D3DX', 30), ('DSOUND', 40)):
        w('')
        w(f'C. {lib}: functions called from outside it (sites; from)')
        for dst, n in callee_sites[lib].most_common(top):
            w(f'  {dst:08X} {nm(dst):45} {n:5}  ' + ', '.join(f'{k} {v}' for k, v in callee_from[dst].most_common()))
        if len(callee_sites[lib]) > top:
            w(f'  ... {len(callee_sites[lib]) - top} more')
        w('  .text caller prefixes: ' + ', '.join(f'{k} {v}' for k, v in caller_pref[lib].most_common(25)))

    pc = collections.Counter(prefix(n) for n, s, sec in funcs.values() if sec == '.text')
    w('')
    w('D. .text function name prefixes (top 90)')
    items = pc.most_common(90)
    for i in range(0, len(items), 6):
        w('  ' + '  '.join(f'{k} {v}' for k, v in items[i:i + 6]))

    w('')
    w('E. Globals with D3D / device / push-buffer names, and who references them')
    for g in read_tsv(os.path.join(export_dir, 'globals.tsv')):
        n = g['name']
        if n.startswith('DAT_') or not any(k in n.lower() for k in ('d3d', 'device', 'pushbuf', 'g_pdev', 'nv2a', 'gpu')):
            continue
        refs = [int(x, 16) for x in g['funcs'].split(',') if x]
        byg = collections.Counter(group_of(section_of(r)) for r in refs)
        addr = g['addr']
        w(f'  {addr} {n:44} {len(refs):5} funcs  ' + ', '.join(f'{k} {v}' for k, v in byg.most_common()))

    w('')
    w('F. Kernel imports by family')
    kn = [k['name'] for k in read_tsv(kernel_path) if not k['name'].startswith('xbe_')]
    fam = collections.Counter()
    for n in kn:
        m = re.match(r'(Xbox|Xc|Xe|Hal|Av|Nt|Ke|Ki|Kf|Mm|Ex|Io|Ob|Ps|Rtl|Dbg|Fsc|Ld|Phy|Mu|Hc)', n)
        fam[m.group(1) if m else n[:3]] += 1
    w(f'  {len(kn)} names: ' + ', '.join(f'{k} {v}' for k, v in fam.most_common()))
    w('  ' + ', '.join(sorted(kn)))
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')
    print(f'wrote {out_path}: {len(out)} lines')


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit('usage: gfx_layer.py EXPORT_DIR SECTIONS_TSV KERNEL_NAMES_TSV OUT_TXT')
    main(*sys.argv[1:])
