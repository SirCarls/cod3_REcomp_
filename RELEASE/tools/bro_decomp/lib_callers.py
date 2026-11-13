#!/usr/bin/env python3
# [acct2-bro] Who calls into a linked library's address range (CoD3's Direct3D, for example), by named address
# spans and otherwise by 64 KB page. Works on any Ghidra export with functions.tsv and calls.tsv.
# Read-only. Usage: lib_callers.py EXPORT_DIR LIB_RANGE OUT_TXT [LABEL=START-END ...]
#   LIB_RANGE and spans are hex, end exclusive: 822E0000-82310000 "NGL, materials and particles=820C0000-82170000"
import collections, csv, os, sys


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def span(text):
    lo, hi = text.split('-')
    return int(lo, 16), int(hi, 16)


def main(export_dir, lib_range, out_path, *spans):
    lo, hi = span(lib_range)
    named = []
    for s in spans:
        label, rng = s.rsplit('=', 1)
        named.append((label, *span(rng)))
    funcs = [int(r['entry'], 16) for r in read_tsv(os.path.join(export_dir, 'functions.tsv'))]
    sites, callers, callees = collections.Counter(), collections.defaultdict(set), set()
    total = 0
    for r in read_tsv(os.path.join(export_dir, 'calls.tsv')):
        try:
            caller, callee = int(r['caller'], 16), int(r['callee'], 16)
        except ValueError:
            continue
        if not lo <= callee < hi or lo <= caller < hi:
            continue
        group = next((label for label, a, b in named if a <= caller < b), f'page {caller & 0xFFFF0000:08X}')
        sites[group] += 1
        callers[group].add(caller)
        callees.add(callee)
        total += 1
    lines = [f'Library range {lo:08X}-{hi:08X}: {sum(1 for f in funcs if lo <= f < hi)} functions; '
             f'{total} call sites from outside it, from {len(set().union(*callers.values()))} functions, into '
             f'{len(callees)} of its functions.', '', 'calls  share  functions  from']
    for group, n in sites.most_common():
        lines.append(f'{n:5d}  {100 * n / total:4.1f}%  {len(callers[group]):9d}  {group}')
    open(out_path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main(*sys.argv[1:])
