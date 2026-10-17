#!/usr/bin/env python3
"""campaign_map_set.py - set rows of mp/config/campaign_map.tsv from a decisions file
(review helper for tools/mp_aliases.py; each line: symbol, status, mp, why, regs, jump, sp,
mp_function, tab-separated). An mp of '=' keeps the row's MP value; a symbol without a row
is added (sp from the 7th column); a status written +status adds a row for the same symbol
with that MP value (a second MP site of a mid-asm hook).

    python3 tools/campaign_map_set.py decisions.tsv [--recomp DIR]
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mp_aliases as M  # noqa: E402


def main():
    args = sys.argv[1:]
    recomp = M.find_recomp(args[args.index("--recomp") + 1] if "--recomp" in args else None)
    header, rows = M.read_table(recomp / M.TABLE)
    by = {}
    for r in rows:
        by.setdefault(r.symbol, []).append(r)
    n = 0
    for line in Path(args[0]).read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        f = line.split("\t") + [""] * 8
        sym, status, mp, why, regs, jump, sp, mp_function = (x.strip() for x in f[:8])
        targets = by.get(sym)
        if status.startswith("+"):  # another row for the same symbol (a second MP site)
            status = status[1:]
            targets = [r for r in targets or [] if r.mp == mp]
        if not targets:
            r = M.Row({"symbol": sym, "sp": sp}, 0)
            rows.append(r)
            by[sym] = [r]
            targets = [r]
        for r in targets:
            r.status = status
            if mp != "=":
                r.mp = mp
            if why:
                r.why = why
            if regs:
                r.regs = regs
            if jump:
                r.jump = jump
            if mp_function:
                r.mp_function = mp_function
        n += 1
    M.write_table(recomp / M.TABLE, header, rows)
    print(f"{n} decisions applied")


if __name__ == "__main__":
    main()
