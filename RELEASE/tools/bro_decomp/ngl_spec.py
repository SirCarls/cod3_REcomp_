#!/usr/bin/env python3
# [acct2-bro] The spec of NGL's interface: for each function the rest of Big Red One calls (ngl_interface.py's
# ngl_api.tsv), its calling convention, return type and parameters, and the struct fields it reads and writes
# through each parameter and through NGL's globals, from Ghidra's C export. Fields are merged per struct family
# (quads, quad batches, text, meshes, textures, lights, materials) into a layout. Read-only.
# Usage: ngl_spec.py NGL_API_TSV DECOMP_DIR OUT_DIR [MP_NGL_API_TSV] [COD3_FUNCTIONS_TSV]
import collections, csv, glob, os, re, sys

CONV = re.compile(r'\b(__cdecl|__fastcall|__thiscall|__stdcall)\b')
SIG = re.compile(r'^(?P<ret>.*?)\s*\b(?P<conv>__cdecl|__fastcall|__thiscall|__stdcall)?\s*(?P<name>[A-Za-z_]\w*)\((?P<args>.*)\)\s*$')
BASE = r'(?P<p>param_\d+|ngl_\w+|g_ngl\w*)'
DEREF = re.compile(r'\*\((?P<t>[\w ]+?)\s*(?P<star>\*+)\)\s*\(\s*(?:\(int\)\s*)?' + BASE + r'\s*\+\s*(?P<o>0x[0-9a-fA-F]+|\d+)\s*\)')
INDEX = re.compile(r'\b(?P<p>param_\d+)\[(?P<i>0x[0-9a-fA-F]+|\d+)\]')
PLAIN = re.compile(r'(?<![\w)\]])\*(?P<p>param_\d+)\b(?!\s*\[)')
WRITE = re.compile(r'\s*(=(?!=)|\+=|-=|\|=|&=|\^=)')
FAMILIES = [('quad batch', r'QuadBatch'), ('quad', r'Quad'), ('texture', r'Texture|BackBuffer|FrontBuffer'),
            ('text', r'Text(?!ure)|Font'), ('mesh', r'Mesh|Section'), ('light', r'Light|Ambient'),
            ('material', r'Material|Shader')]


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def short_type(t, stars):
    t = t.strip()
    if len(stars) > 1 or t.endswith('*'):
        return 'ptr'
    if t in ('float', 'float10'):
        return 'f32'
    if t in ('double', 'undefined8', 'longlong', 'ulonglong'):
        return '64'
    if t in ('byte', 'char', 'undefined1', 'bool', 'uchar'):
        return 'u8'
    if t in ('short', 'ushort', 'undefined2', 'wchar_t'):
        return 'u16'
    if t.endswith('code'):
        return 'fn'
    return 'u32'


def elem_size(ptype):
    t = ptype.replace('*', '').strip()
    return {'undefined1': 1, 'byte': 1, 'char': 1, 'uchar': 1, 'bool': 1, 'short': 2, 'ushort': 2, 'undefined2': 2,
            'double': 8, 'undefined8': 8}.get(t, 4)


def load_bodies(decomp_dir, wanted):
    bodies = {}
    for fn in sorted(glob.glob(os.path.join(decomp_dir, '*.c'))):
        lines = open(fn, encoding='utf-8', errors='replace').read().split('\n')
        i = 0
        while i < len(lines):
            if lines[i] == '{' and i >= 2:
                # the signature: the lines since the blank line before the brace (long ones wrap)
                k = i - 1
                while k > 0 and lines[k] == '':
                    k -= 1
                start = k
                while start > 0 and lines[start - 1] != '':
                    start -= 1
                head = ' '.join(x.strip() for x in lines[start:k + 1])
                m = SIG.match(head)
                j = i + 1
                while j < len(lines) and lines[j] != '}':
                    j += 1
                if m and m.group('name') in wanted:
                    bodies[m.group('name')] = (m, '\n'.join(lines[i + 1:j]))
                i = j
            i += 1
    return bodies


def params_of(args):
    out = []
    if args.strip() in ('', 'void'):
        return out
    for a in args.split(','):
        a = a.strip()
        m = re.match(r'(.*?)(\bparam_\d+|\bthis)$', a)
        if m:
            out.append((m.group(2), m.group(1).strip()))
        else:
            out.append((a, a))
    return out


def fields(body, ptypes):
    acc = collections.defaultdict(dict)  # base -> offset -> (type, 'r'/'w')

    def add(base, off, typ, end):
        w = 'w' if WRITE.match(body, end) else 'r'
        old = acc[base].get(off)
        acc[base][off] = (typ if not old else old[0], 'rw' if old and old[1] != w else w)
    for m in DEREF.finditer(body):
        add(m.group('p'), int(m.group('o'), 0), short_type(m.group('t'), m.group('star')), m.end())
    for m in INDEX.finditer(body):
        p = m.group('p')
        es = elem_size(ptypes.get(p, 'undefined4 *'))
        add(p, int(m.group('i'), 0) * es, {1: 'u8', 2: 'u16', 8: '64'}.get(es, 'u32'), m.end())
    for m in PLAIN.finditer(body):
        add(m.group('p'), 0, 'u32', m.end())
    return acc


def fmt(fs):
    return ' '.join(f'+{o:X}:{t}:{rw}' for o, (t, rw) in sorted(fs.items()))


def main(api_path, decomp_dir, out_dir, mp_api=None, cod3_funcs=None):
    api = read_tsv(api_path)
    names = {r['name'] for r in api}
    bodies = load_bodies(decomp_dir, names)
    mp = {r['name']: r['addr'] for r in read_tsv(mp_api)} if mp_api else {}
    cod3 = {}
    if cod3_funcs:
        cod3 = {r['entry']: (r['name'], r['signature']) for r in read_tsv(cod3_funcs)}
    os.makedirs(out_dir, exist_ok=True)
    family_fields = collections.defaultdict(lambda: collections.defaultdict(set))
    global_fields = collections.defaultdict(lambda: collections.defaultdict(set))
    convs = collections.Counter()
    missing = []
    with open(os.path.join(out_dir, 'ngl_spec.tsv'), 'w', encoding='utf-8') as f:
        f.write('addr\tmp_addr\tname\tgroup\toutside_sites\tconvention\treturns\tparams\tfields_by_param\tngl_globals\t'
                'body_lines\tcod3\tcod3_signature\n')
        for r in sorted(api, key=lambda x: (x['group'], -int(x['outside_sites']))):
            n = r['name']
            if n not in bodies:
                missing.append(n)
                continue
            m, body = bodies[n]
            ps = params_of(m.group('args'))
            ptypes = dict(ps)
            acc = fields(body, ptypes)
            conv = m.group('conv') or '(none)'
            convs[conv] += 1
            by_param = '; '.join(f'{p}: {fmt(acc[p])}' for p, _ in ps if acc.get(p))
            globs = '; '.join(f'{g}: {fmt(fs)}' for g, fs in sorted(acc.items()) if not g.startswith('param_'))
            fam = next((k for k, rx in FAMILIES if re.search(rx, n)), None)
            if fam and acc.get('param_1'):
                for o, (t, rw) in acc['param_1'].items():
                    family_fields[fam][(o, t)].add(n + ('(w)' if 'w' in rw else ''))
            for g, fs in acc.items():
                if not g.startswith('param_'):
                    for o, (t, rw) in fs.items():
                        global_fields[g][(o, t)].add(n + ('(w)' if 'w' in rw else ''))
            c3 = cod3.get(r.get('cod3', ''), ('', ''))
            f.write('\t'.join([r['addr'], mp.get(n, ''), n, r['group'], r['outside_sites'], conv, m.group('ret').strip(),
                               ', '.join(f'{t} {p}'.strip() for p, t in ps) or 'void', by_param, globs,
                               str(body.count(chr(10)) + 1), r.get('cod3', ''), c3[1]]) + '\n')
    with open(os.path.join(out_dir, 'ngl_structs.txt'), 'w', encoding='utf-8') as f:
        f.write('Struct layouts seen through each family\'s first parameter (offset, type, functions; (w) = writes)\n')
        for fam, _ in FAMILIES:
            if fam not in family_fields:
                continue
            f.write(f'\n== {fam}\n')
            for (o, t), fn in sorted(family_fields[fam].items()):
                f.write(f'  +{o:03X} {t:4} {", ".join(sorted(fn))}\n')
        f.write('\nNGL globals the interface functions read or write\n')
        for g in sorted(global_fields):
            f.write(f'\n== {g}\n')
            for (o, t), fn in sorted(global_fields[g].items()):
                fl = sorted(fn)
                f.write(f'  +{o:03X} {t:4} {", ".join(fl[:8])}{" ..." if len(fl) > 8 else ""}\n')
    print(f'functions {len(api)}, with C {len(api) - len(missing)}, missing {missing}')
    print('conventions:', dict(convs))
    print('families:', {k: len(v) for k, v in family_fields.items()}, ' globals:', {k: len(v) for k, v in global_fields.items()})


if __name__ == '__main__':
    if len(sys.argv) < 4:
        sys.exit('usage: ngl_spec.py NGL_API_TSV DECOMP_DIR OUT_DIR [MP_NGL_API_TSV] [COD3_FUNCTIONS_TSV]')
    main(*sys.argv[1:])
