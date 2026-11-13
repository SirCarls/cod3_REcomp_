#!/usr/bin/env python3
# [acct2-bro] Pair Big Red One's functions with CoD3's by the strings they use. A string that appears in both
# programs and is used by exactly one function on each side pairs those two functions; more such strings are more
# votes. The result is checked against the pairs match_builds.py found from the call graph (not from strings), so
# the agreement rate is an independent check of the method.
# Read-only. Usage:
#   string_pairs.py BRO_EXPORT_DIR COD3_EXPORT_DIR MATCHES_COD3_TSV OUT_TSV [COD3_SYMBOLS_TOML]
# Export folders hold functions.tsv and strings.tsv (addr, funcs, sites, text) from the Ghidra exports.
import collections, csv, os, re, sys

MIN_LEN = 8          # shorter strings ("%s %d", file extensions) are too common to pair on
GRAPHICS = re.compile(r'^(ngl|cd[A-Z]|Shader|nl[A-Z]|tl[A-Z]|FX|Fx|R_|RB_|RE_)')


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def users(export_dir):
    """text -> set of functions that use it."""
    out = collections.defaultdict(set)
    for r in read_tsv(os.path.join(export_dir, 'strings.tsv')):
        text = r.get('text', '')
        if len(text) < MIN_LEN or not r.get('funcs'):
            continue
        for fn in r['funcs'].split(','):
            try:
                out[text].add(int(fn, 16))
            except ValueError:
                pass
    return out


def names(export_dir):
    return {int(r['entry'], 16): r['name'] for r in read_tsv(os.path.join(export_dir, 'functions.tsv'))}


def toml_names(path):
    out, text = {}, open(path, encoding='utf-8').read()
    for block in text.split('[[function]]')[1:]:
        a = re.search(r'address = 0x([0-9A-Fa-f]+)', block)
        n = re.search(r'\nname = "([^"]+)"', block)
        if a and n:
            out[int(a.group(1), 16)] = n.group(1)
    return out


def main(bro_dir, cod3_dir, matches_path, out_path, symbols=None):
    bro, cod3 = users(bro_dir), users(cod3_dir)
    bro_names, cod3_names = names(bro_dir), names(cod3_dir)
    if symbols:
        cod3_names.update(toml_names(symbols))
    shared = set(bro) & set(cod3)
    votes = collections.defaultdict(collections.Counter)   # bro function -> {cod3 function: votes}
    examples = collections.defaultdict(list)
    for text in shared:
        if len(bro[text]) == 1 and len(cod3[text]) == 1:
            b, c = next(iter(bro[text])), next(iter(cod3[text]))
            votes[b][c] += 1
            if len(examples[(b, c)]) < 2:
                examples[(b, c)].append(text[:60])
    # one CoD3 function per Big Red One function (most votes); drop a CoD3 function claimed by two
    best = {b: cs.most_common(1)[0] for b, cs in votes.items()}
    claimed = collections.Counter(c for c, _ in best.values())
    known, known_any = {}, {}   # call-graph pairs (independent check); all earlier pairs
    for r in read_tsv(matches_path):
        known_any[int(r['bro'], 16)] = int(r['cod3'], 16)
        if r.get('how') != 'strings':
            known[int(r['bro'], 16)] = int(r['cod3'], 16)

    rows, status = [], collections.Counter()
    for b, (c, n) in sorted(best.items()):
        split = len(votes[b]) > 1
        if claimed[c] > 1:
            st = 'conflict'
        elif b in known:
            st = 'agrees' if known[b] == c else 'disagrees'
        elif b in known_any:
            st = 'known' if known_any[b] == c else 'disagrees'
        else:
            st = 'new'
        status[st] += 1
        bn, cn = bro_names.get(b, ''), cod3_names.get(c, '')
        rows.append((f'{b:08X}', f'{c:08X}', bn, cn if not cn.startswith('FUN_') else '', n,
                     st + (' (split votes)' if split else ''), ' | '.join(examples[(b, c)])))
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('bro\tcod3\tbro_name\tcod3_name\tvotes\tstatus\tstrings\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    checked = status['agrees'] + status['disagrees']
    print(f'strings in both programs (>= {MIN_LEN} chars): {len(shared)}; pairs: {len(rows)} {dict(status)}')
    if checked:
        print(f'against the call-graph pairs: {status["agrees"]} of {checked} agree ({100 * status["agrees"] / checked:.1f}%)')
    gfx = [r for r in rows if GRAPHICS.match(r[2]) and not r[5].startswith(('conflict', 'disagrees'))]
    print(f'graphics-layer pairs (Big Red One name ngl/cd/Shader/FX/R_/...): {len(gfx)}, '
          f'new {sum(1 for r in gfx if r[5].startswith("new"))}')


if __name__ == '__main__':
    main(*sys.argv[1:])
