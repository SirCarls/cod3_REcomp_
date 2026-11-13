#!/usr/bin/env python3
# [acct2-bro] Functions Ghidra found that xboxrecomp's disassembler did not, written as a run log that xboxrecomp's
# own tools.seed_from_log reads ("Failed to resolve VA 0x..." lines). seed_from_log then applies its gates (inside an
# executable section, reads as a function body) and adds what passes to a seed file for `tools.disasm
# --seed-functions`. Works for any Xbox title with a Ghidra export (functions.tsv) and an xboxrecomp disassembly.
#
# Why: a function reached only through a pointer in data (an allocator hook, a callback table) has no direct caller,
# so the disassembler never finds it; every call to it then fails at run time and is skipped. In Big Red One the
# game's allocator hook (0x00296820) was one: each allocation through it came back 0 and the game wrote through null.
#
# Read-only. Usage: ghidra_seed_log.py GHIDRA_EXPORT_DIR XBOXRECOMP_FUNCTIONS_JSON OUT_LOG
#   then: python -m tools.seed_from_log OUT_LOG default.xbe --functions XBOXRECOMP_FUNCTIONS_JSON --seeds SEEDS_JSON
import bisect, csv, json, os, sys


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def main(export_dir, functions_json, out_log):
    as_int = lambda v: int(v, 0) if isinstance(v, str) else int(v)
    with open(functions_json, encoding='utf-8') as f:
        bounds = sorted((as_int(fn['start']), as_int(fn['end'])) for fn in json.load(f))
    known = {lo for lo, _ in bounds}
    starts = [lo for lo, _ in bounds]
    ghidra = {int(r['entry'], 16): r['name'] for r in read_tsv(os.path.join(export_dir, 'functions.tsv'))
              if r.get('thunk', '0') in ('0', 'false', '')}

    def inside_known(va):
        """Ghidra splits some functions where xboxrecomp keeps one body; seeding those entries would split a working
        function, so only addresses in the gaps between xboxrecomp's functions are offered."""
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and bounds[i][0] < va < bounds[i][1]

    missing = sorted(a for a in ghidra if a not in known)
    gaps = [a for a in missing if not inside_known(a)]
    with open(out_log, 'w', encoding='utf-8') as f:
        for a in gaps:
            f.write(f'Failed to resolve VA 0x{a:08X} (Ghidra: {ghidra[a]})\n')
    print(f'{len(ghidra)} Ghidra functions, {len(known)} known to xboxrecomp; {len(missing)} missing, '
          f'{len(missing) - len(gaps)} of them inside a known function (left out); {len(gaps)} written to {out_log}')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit('usage: ghidra_seed_log.py GHIDRA_EXPORT_DIR XBOXRECOMP_FUNCTIONS_JSON OUT_LOG')
    main(*sys.argv[1:])
