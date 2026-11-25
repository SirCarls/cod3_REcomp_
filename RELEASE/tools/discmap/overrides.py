"""[waw-decomp] 2026-11-16 discmap: overrides_<build>.tsv, the rows of a build's map a person decides.

`match` writes one row for every address the code needs (groups 1-9) whose status is changed, gone or
unresolved, and keeps what a person already wrote there when it runs again. `emit` refuses to write
anything while one of those rows has no decision.

decision column:
  accept    use the suggested address (the match's suggestion, in `suggested`)
  address   use the address written in build_addr
  absent    this build has no such thing: A() gives 0 for it, its mid-asm hook / [functions] entry /
            name is left out of the build's configs (a feature that needs it must switch itself off)
A person can add a row for any other address too; a decision always wins over the match.
"""
from __future__ import annotations

from pathlib import Path

from common import hex8, parse_hex, read_tsv, write_tsv
from matching import NEEDS_PERSON

HEADER = ["program", "usa_addr", "status", "suggested", "decision", "build_addr", "note", "symbols", "kinds", "how",
          "uses"]
DECISIONS = ("accept", "address", "absent")


def needed(map_rows: list[dict]) -> list[dict]:
    return [r for r in map_rows if r["required"] == "yes" and r["status"] in NEEDS_PERSON]


def merge(path: Path, map_rows: list[dict], build: str) -> list[str]:
    """Write path from the map, keeping every decision already in it. Returns what changed."""
    old = {}
    if path.exists():
        for r in read_tsv(path):
            old[(r["program"], r["usa_addr"].upper())] = r
    rows, log, wanted = [], [], set()
    for m in needed(map_rows):
        key = (m["program"], m["usa_addr"])
        wanted.add(key)
        row = {h: "" for h in HEADER}
        row.update(program=m["program"], usa_addr=m["usa_addr"], status=m["status"], suggested=m["build_addr"],
                   symbols=m["symbols"], kinds=m["kinds"],
                   how=m["how"] + (f" [{m['witness']}]" if m["witness"] else ""),
                   uses=m["uses"])
        o = old.get(key)
        if o:
            row.update(decision=o["decision"], build_addr=o["build_addr"], note=o["note"])
            if o["decision"] == "accept" and o["suggested"] != m["build_addr"]:
                row["decision"] = ""
                row["note"] = (f"the suggestion changed from {o['suggested'] or 'none'} to {m['build_addr'] or 'none'}"
                               f"; decide again. {o['note']}").strip()
                log.append(f"{key[0]} {key[1]}: suggestion changed, decision cleared")
        else:
            log.append(f"{key[0]} {key[1]}: new row ({m['status']})")
        rows.append(row)
    for key, o in old.items():
        if key in wanted:
            continue
        if o["decision"]:
            o = dict(o)
            if not o["note"].startswith("now automatic"):
                o["note"] = ("now automatic; this decision still applies (remove the row to use the match). "
                             + o["note"]).strip()
            rows.append({h: o.get(h, "") for h in HEADER})
            log.append(f"{key[0]} {key[1]}: kept a decision for a row that is automatic now")
        else:
            log.append(f"{key[0]} {key[1]}: dropped (automatic now)")
    rows.sort(key=lambda r: (r["program"], r["usa_addr"]))
    meta = {"build": build,
            "what": "the addresses of this build a person decides; discmap emit refuses while a decision is empty",
            "decision": "accept (the suggested address) | address (the one in build_addr) | absent (not in this build)"}
    write_tsv(path, HEADER, [[r[h] for h in HEADER] for r in rows], meta)
    return log


def decisions(path: Path) -> tuple[dict, list[str], set]:
    """(program, usa) -> (address or None for absent, decision); the problems (empty or bad rows); and
    every (program, usa) the file has a row for."""
    out, problems, listed = {}, [], set()
    if not path.exists():
        return out, problems, listed
    for r in read_tsv(path):
        key = (r["program"], int(r["usa_addr"], 16))
        listed.add(key)
        d = r["decision"].strip().lower()
        if not d:
            what = r["symbols"] or r["kinds"]
            problems.append(f"{r['program']} {r['usa_addr']} ({r['status']}, {what}): no decision")
            continue
        if d not in DECISIONS:
            problems.append(f"{r['program']} {r['usa_addr']}: unknown decision {d!r} (use {', '.join(DECISIONS)})")
            continue
        if d == "accept":
            if not r["suggested"]:
                problems.append(f"{r['program']} {r['usa_addr']}: 'accept' but there is no suggestion")
                continue
            out[key] = (parse_hex(r["suggested"]), d)
        elif d == "address":
            try:
                out[key] = (int(r["build_addr"].strip(), 16), d)
            except ValueError:
                problems.append(f"{r['program']} {r['usa_addr']}: 'address' needs build_addr")
        else:
            out[key] = (None, d)
    return out, problems, listed


def final_table(map_rows: list[dict], path: Path) -> tuple[dict, list[str]]:
    """(program, usa) -> (build address or None when absent, source) for every map row that has one;
    plus the problems: a required row that needs a person and has no decision."""
    dec, problems, listed = decisions(path)
    table = {}
    for r in map_rows:
        key = (r["program"], int(r["usa_addr"], 16))
        if key in dec:
            table[key] = (dec[key][0], "override: " + dec[key][1])
        elif r["status"] in ("same", "moved"):
            table[key] = (parse_hex(r["build_addr"]), r["method"])
        elif r["required"] == "yes" and key not in listed:
            problems.append(f"{r['program']} {r['usa_addr']} ({r['status']}, {r['symbols'] or r['kinds']}): "
                            f"needs a person, and {path.name} has no decision for it")
    return table, sorted(set(problems))


def describe(table_entry) -> str:
    addr, source = table_entry
    return f"{hex8(addr) or 'absent'} ({source})"
