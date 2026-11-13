#!/usr/bin/env python3
# [acct2-bro] Names across Big Red One (x86) and Call of Duty 3 (PowerPC), both ways, from match_builds.py's pairs.
# Read-only: writes candidate tables; never edits symbols.toml or Big Red One's names.
# Usage: xcheck_cod3.py MATCHES_TSV BRO_FUNCTIONS_TSV COD3_SYMBOLS_TOML COD3_CANDIDATE_NAMES_TSV OUT_DIR
import collections, csv, os, re, sys

GENERIC = re.compile(r'^(FUN_|thunk_|LAB_|DAT_|table_|switch|_?guard|sub_)')
LIBRARY = re.compile(r'^[A-Z0-9]+__')  # XDK / C library names, like D3D8LTCG__x or LIBCMT__x: Xbox-only
X86_HELPER = re.compile(r'^(__SEH_|__EH_|_?_chkstk|__alloca_probe|_?_ftol|__security_|__except_handler|'
                        r'__local_unwind|__global_unwind|__NLG_)')


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def read_symbols_toml(path):
    out = {}
    state = {'block': {}, 'kind': None}

    def flush():
        b = state['block']
        if state['kind'] == 'function' and 'address' in b and 'name' in b:
            out[int(b['address'], 16)] = (b['name'], b.get('confidence', ''))
    with open(path, encoding='utf-8') as f:
        for line in f:
            s = line.strip()
            if s.startswith('[['):
                flush()
                state['block'], state['kind'] = {}, s.strip('[]')
            elif '=' in s and not s.startswith('#'):
                k, v = s.split('=', 1)
                state['block'][k.strip()] = v.strip().strip('"')
    flush()
    return out


def real(name):
    return bool(name) and not GENERIC.match(name)


def norm(name):
    n = re.sub(r'^(broc_|gscr_|scrcmd_|scr_)', '', name.lower())
    return n.replace('_', '')


def head_of(name):
    if name.startswith('ngl'):
        return 'ngl'
    i = name.find('_', 1)
    return name[:i] if i > 0 else name


def main(matches_path, bro_funcs_path, symbols_path, cand_path, out_dir):
    pairs = read_tsv(matches_path)
    bro = {int(r['entry'], 16): r['name'] for r in read_tsv(bro_funcs_path)}
    sym = read_symbols_toml(symbols_path)
    cand = {int(r['addr'], 16): r['name'] for r in read_tsv(cand_path) if r.get('kind') == 'function'}
    to_cod3, to_bro, differ = [], [], []
    both = agree = lib = 0
    agree_how = collections.Counter()
    both_how = collections.Counter()
    gain = collections.Counter()
    ngl = []
    for p in pairs:
        b, c = int(p['bro'], 16), int(p['cod3'], 16)
        bn = bro.get(b, p['bro_name'])
        cs = sym.get(c, ('', ''))[0]
        cc = cand.get(c, '') or p['cod3_name']
        cn = cs or (cc if real(cc) else '')
        b_ok = real(bn) and not X86_HELPER.match(bn)
        if b_ok and LIBRARY.match(bn):
            lib += 1
        elif b_ok and not cs:
            to_cod3.append((c, b, bn, p['how'], p['votes'], cc if real(cc) else ''))
            gain[head_of(bn)] += 1
        if cn and not b_ok:
            to_bro.append((b, c, cn, 'symbols.toml' if cs else 'candidate', p['how'], p['votes']))
        if b_ok and cn:
            both += 1
            both_how[p['how']] += 1
            if norm(bn) == norm(cn):
                agree += 1
                agree_how[p['how']] += 1
            else:
                differ.append((b, c, bn, cn, 'symbols.toml' if cs else 'candidate', p['how'], p['votes']))
        if bn.startswith('ngl') or 'ngl' in cn:
            ngl.append((b, c, bn, cn, p['how']))
    os.makedirs(out_dir, exist_ok=True)

    def write(name, head, rows):
        with open(os.path.join(out_dir, name), 'w', encoding='utf-8') as f:
            f.write('\t'.join(head) + '\n')
            for r in sorted(rows):
                f.write('\t'.join('%08X' % x if isinstance(x, int) else str(x) for x in r) + '\n')
    write('cod3_names_from_bro.tsv', ['cod3', 'bro', 'bro_name', 'how', 'votes', 'cod3_candidate'], to_cod3)
    write('bro_names_from_cod3.tsv', ['bro', 'cod3', 'cod3_name', 'source', 'how', 'votes'], to_bro)
    write('name_disagreements.tsv', ['bro', 'cod3', 'bro_name', 'cod3_name', 'cod3_source', 'how', 'votes'], differ)
    write('ngl_pairs.tsv', ['bro', 'cod3', 'bro_name', 'cod3_name', 'how'], ngl)
    s = []
    s.append(f'pairs {len(pairs)}; BRO named {sum(1 for n in bro.values() if real(n))} of {len(bro)}; '
             f'CoD3 symbols.toml functions {len(sym)}; CoD3 candidates {len(cand)}')
    s.append(f'names CoD3 can take from BRO (no symbols.toml name yet): {len(to_cod3)}; Xbox-library names skipped {lib}')
    s.append(f'  by subsystem: ' + ', '.join(f'{k} {v}' for k, v in gain.most_common(40)))
    s.append(f'names BRO can take from CoD3 (BRO unnamed): {len(to_bro)} '
             f'({sum(1 for r in to_bro if r[3] == "symbols.toml")} from symbols.toml)')
    s.append(f'both named: {both}; same meaning by normalized name: {agree}; differ: {len(differ)}')
    s.append('  agreement by how: ' + ', '.join(f'{h} {agree_how[h]}/{both_how[h]}' for h in both_how))
    s.append(f'ngl pairs: {len(ngl)}')
    for r in sorted(ngl)[:40]:
        s.append('  %08X %08X %-40s %-40s %s' % r)
    s.append('sample disagreements:')
    for r in differ[:25]:
        s.append('  %08X %08X %-36s %-36s %s %s %s' % r)
    text = '\n'.join(s) + '\n'
    with open(os.path.join(out_dir, 'xcheck_summary.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    print(text)


if __name__ == '__main__':
    if len(sys.argv) != 6:
        sys.exit(__doc__ or 'usage: xcheck_cod3.py MATCHES BRO_FUNCTIONS SYMBOLS_TOML CANDIDATES OUT_DIR')
    main(*sys.argv[1:])
