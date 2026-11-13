#!/usr/bin/env python3
# [acct2-bro] Grow the Big Red One <-> CoD3 function pairs through the call graph. For each known pair, the
# callees of both functions are listed in call order (library code left out: Big Red One's Xbox library sections,
# CoD3's address ranges given on the command line). When both lists are the same length and every callee already
# paired sits at the same place in both, the unpaired callees at the same places are paired too. Repeated until
# nothing changes. A pair is kept only if no other candidate competes for either side.
# The method is checked first by holding back every fifth seed pair that came from the call graph and seeing how
# many of those it finds again, and how many it gets wrong.
# Read-only. Usage:
#   call_pairs.py BRO_EXPORT_DIR BRO_SECTIONS_TSV COD3_EXPORT_DIR COD3_LIB_RANGES OUT_TSV SEEDS_TSV...
# COD3_LIB_RANGES: comma-separated hex ranges, e.g. 822E0000-82310000. SEEDS_TSV: tables with bro and cod3 columns
# (match_builds.py's matches_cod3.tsv, string_pairs.py's output; rows marked conflict or disagrees are skipped).
import bisect, collections, csv, os, sys

LIBS = {'D3D', 'XGRPH', 'D3DX', 'DSOUND', 'XONLINE', 'XNET', 'XPP', 'DOLBY', 'XACTENG', 'XMV', 'WMADEC'}
ROUNDS = 20


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


def callee_lists(export_dir, is_library):
    """caller -> distinct callees in call-site order, without library code, thunks or recursion."""
    thunks = {hexval(r['entry']) for r in read_tsv(os.path.join(export_dir, 'functions.tsv')) if r.get('thunk') == '1'}
    sites = collections.defaultdict(list)
    for r in read_tsv(os.path.join(export_dir, 'calls.tsv')):
        caller, site, callee = hexval(r['caller']), hexval(r['site']), hexval(r['callee'])
        if None in (caller, site, callee) or callee == caller or callee in thunks or is_library(callee):
            continue
        sites[caller].append((site, callee))
    out = {}
    for caller, calls in sites.items():
        seen, order = set(), []
        for _, callee in sorted(calls):
            if callee not in seen:
                seen.add(callee)
                order.append(callee)
        out[caller] = order
    return out


def grow(seeds, bro_calls, cod3_calls):
    """seeds: {bro: cod3}. Returns {bro: (cod3, round, parent bro function)} for the new pairs."""
    pairs = dict(seeds)
    back = {c: b for b, c in pairs.items()}
    found = {}
    for rnd in range(1, ROUNDS + 1):
        cand = collections.defaultdict(set)      # bro -> {cod3}
        rcand = collections.defaultdict(set)     # cod3 -> {bro}
        parent = {}
        for b, c in list(pairs.items()):
            lb, lc = bro_calls.get(b), cod3_calls.get(c)
            if not lb or not lc or len(lb) != len(lc):
                continue
            if any((x in pairs) != (y in back) or (x in pairs and pairs[x] != y) for x, y in zip(lb, lc)):
                continue
            for x, y in zip(lb, lc):
                if x not in pairs and y not in back:
                    cand[x].add(y)
                    rcand[y].add(x)
                    parent.setdefault((x, y), b)
        added = 0
        for x, ys in cand.items():
            if len(ys) == 1:
                y = next(iter(ys))
                if len(rcand[y]) == 1 and x not in pairs and y not in back:
                    pairs[x], back[y] = y, x
                    found[x] = (y, rnd, parent[(x, y)])
                    added += 1
        if not added:
            break
    return found


def main(bro_dir, bro_sections, cod3_dir, cod3_ranges, out_path, *seed_paths):
    secs = sorted((int(s['va'], 16), int(s['va'], 16) + int(s['vsize'], 16), s['name']) for s in read_tsv(bro_sections))
    starts = [s[0] for s in secs]

    def bro_lib(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and a < secs[i][1] and secs[i][2] in LIBS

    ranges = [tuple(int(x, 16) for x in r.split('-')) for r in cod3_ranges.split(',') if r]

    def cod3_lib(a):
        return any(lo <= a < hi for lo, hi in ranges)

    bro_calls, cod3_calls = callee_lists(bro_dir, bro_lib), callee_lists(cod3_dir, cod3_lib)
    bro_names = {hexval(r['entry']): r['name'] for r in read_tsv(os.path.join(bro_dir, 'functions.tsv'))}

    seeds, graph_seeds = {}, []
    for path in seed_paths:
        for r in read_tsv(path):
            if any(w in r.get('status', '') for w in ('conflict', 'disagrees')):
                continue
            b, c = hexval(r.get('bro')), hexval(r.get('cod3'))
            if b is None or c is None or b in seeds:
                continue
            seeds[b] = c
            if r.get('how') == 'calls':
                graph_seeds.append(b)
    # one-to-one seeds only
    count = collections.Counter(seeds.values())
    seeds = {b: c for b, c in seeds.items() if count[c] == 1}

    # check: hold back every fifth call-graph seed
    held = set(sorted(b for b in graph_seeds if b in seeds)[::5])
    test = grow({b: c for b, c in seeds.items() if b not in held}, bro_calls, cod3_calls)
    right = sum(1 for b in held if b in test and test[b][0] == seeds[b])
    wrong = sum(1 for b in held if b in test and test[b][0] != seeds[b])
    # wrong also counts new pairs that took a held-back CoD3 function for another Big Red One function
    held_cod3 = {seeds[b]: b for b in held}
    stolen = sum(1 for b, (c, _, _) in test.items() if c in held_cod3 and held_cod3[c] != b and b not in held)
    print(f'seeds {len(seeds)}; check with {len(held)} held back: found again {right}, wrong {wrong}, '
          f'held-back CoD3 function given to another {stolen} '
          f'(precision {100 * right / max(1, right + wrong + stolen):.1f}%, recall {100 * right / max(1, len(held)):.1f}%)')

    found = grow(seeds, bro_calls, cod3_calls)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('bro\tcod3\tbro_name\tround\tfrom_pair\n')
        for b, (c, rnd, par) in sorted(found.items()):
            f.write(f'{b:08X}\t{c:08X}\t{bro_names.get(b, "")}\t{rnd}\t{bro_names.get(par, f"{par:08X}")}\n')
    named = [b for b in found if not bro_names.get(b, 'FUN_').startswith(('FUN_', 'thunk_'))]
    gfx = [b for b in named if bro_names[b].startswith(('ngl', 'cd', 'Shader', 'Fx', 'FX'))]
    print(f'new pairs {len(found)}, of them with a Big Red One name {len(named)}, graphics layer {len(gfx)}')


if __name__ == '__main__':
    main(*sys.argv[1:])
