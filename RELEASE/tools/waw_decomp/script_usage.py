#!/usr/bin/env python3
"""
script_usage.py - which of the game's built-in script functions a set of GSC/CSC
scripts calls, with the C function behind each (from script_api.py).

Made for the Rust engine's script VM: run it on a mode's scripts (for example
the zombies maps) to get the list of builtins that mode needs, most used first.

Usage:
    python3 tools/waw_decomp/script_usage.py --api script_api.tsv --out usage.tsv <script folder>... \
        [--crossref crossref.tsv --build pc_sp]

With --crossref (crossref.py's table) the handler's address in the other builds is added too;
--build names the crossref column of the build script_api.tsv was made from.

--column NAME=FOLDER (repeatable) counts each folder on its own as well and adds a "NAME" column
of calls, for example one per map (the scripts a map loads), so a table shows which maps need a
builtin. The folders of the columns are counted together with the positional folders for "calls".
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from naming_common import read_tsv  # noqa: E402

CALL = re.compile(r"(?<![\w:\\])([A-Za-z_]\w*)\s*\(")
# tables the script VMs look builtins up in (the others are menu, effect and spawn keyword parsers)
SCRIPT_KINDS = {"GScr", "Scr", "ScrCmd", "PlayerCmd", "ActorCmd", "SentientCmd", "HECmd", "VehicleCmd", "CScr", "CMD"}
DEF = re.compile(r"^\s*([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:\{.*)?$", re.M)  # "name( args )", body on the next line or this one
# statements and directives the compiler turns into opcodes, not builtin calls (KisakCOD scr_compiler:
# waittillmatch, prof_begin and prof_end are statements; #using_animtree is a directive)
KEYWORDS = {"if", "while", "for", "foreach", "switch", "return", "wait", "waittill", "waittillframeend", "notify",
            "endon", "thread", "isdefined", "else", "case", "default", "break", "continue", "waitrealtime",
            "waittillmatch", "prof_begin", "prof_end", "using_animtree"}


def strip(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)
    return text


def count(folders):
    """Calls per (vm, name) in the scripts under folders: (calls, files, calls written as methods, scripts)."""
    uses = collections.Counter()
    files = collections.defaultdict(set)
    defined = {"gsc": set(), "csc": set()}
    texts = []
    for folder in folders:
        for root, _, fs in os.walk(folder):
            for fn in fs:
                if not fn.endswith((".gsc", ".csc")):
                    continue
                vm = "csc" if fn.endswith(".csc") else "gsc"
                with open(os.path.join(root, fn), encoding="latin-1") as f:
                    text = strip(f.read())
                texts.append((vm, fn, text))
                defined[vm].update(m.lower() for m in DEF.findall(text))
    method_use = collections.Counter()
    for vm, fn, text in texts:
        for m in CALL.finditer(text):
            name = m.group(1).lower()
            if name in KEYWORDS or name in defined[vm]:
                continue
            uses[(vm, name)] += 1
            files[(vm, name)].add(fn)
            before = text[max(0, m.start() - 40):m.start()].rstrip()
            # "ent foo(" / "self foo(" / "players[i] foo(" call a method; "foo(" after an operator is a function
            if before and (before[-1].isalnum() or before[-1] in "_])") and not re.search(r"\b(return|thread|wait|else|case)$", before):
                method_use[(vm, name)] += 1
    return uses, files, method_use, len(texts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--api", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("folders", nargs="*")
    ap.add_argument("--column", action="append", metavar="NAME=FOLDER", help="also count FOLDER alone, as column NAME")
    ap.add_argument("--crossref", help="crossref.py output: add the handler's address in the other builds")
    ap.add_argument("--build", default="pc_sp", help="the crossref column of the --api build (default pc_sp)")
    a = ap.parse_args()
    xref, others = {}, []
    if a.crossref:
        rows = read_tsv(a.crossref)
        others = [c for c in (rows[0].keys() if rows else []) if c not in ("name", "other_names", a.build)]
        xref = {r[a.build]: r for r in rows if r.get(a.build)}
    api = collections.defaultdict(list)  # (vm, name) -> rows
    for r in read_tsv(a.api):
        if r["table_kind"] not in SCRIPT_KINDS and "GetFunction" not in r["read_by"] and "GetMethod" not in r["read_by"]:
            continue
        vm = "csc" if r["table_kind"] == "CScr" or r["read_by"].startswith("CScr_") else "gsc"
        # methods are looked up in the *GetMethod tables and the per-class command tables
        is_method = "GetMethod" in r["read_by"] or r["table_kind"] in ("ScrCmd", "PlayerCmd", "ActorCmd", "SentientCmd", "HECmd", "VehicleCmd")
        api[(vm, r["script_name"].lower())].append((1 if is_method else 0, r))
    columns = []
    for spec in a.column or []:
        name, _, folder = spec.partition("=")
        if not folder:
            ap.error(f"--column wants NAME=FOLDER, not {spec}")
        columns.append((name, count([folder])[0]))
    uses, files, method_use, nscripts = count(a.folders + [c.partition("=")[2] for c in a.column or []])
    found = missing = 0
    with open(a.out, "w", encoding="utf-8") as o:
        o.write("vm\tscript_name\tcalls\tfiles\ttable_kind\thandler\tc_name" + "".join(f"\t{c}" for c in others) +
                "".join(f"\t{c}" for c, _ in columns) + "\n")
        for (vm, name), n in uses.most_common():
            rows = api.get((vm, name))
            if rows:
                found += 1
                want_method = 1 if method_use[(vm, name)] * 2 > n else 0
                rows = sorted(rows, key=lambda x: x[0] != want_method)
                r = rows[0][1]
                x = xref.get(r["handler"], {})
                o.write(f"{vm}\t{name}\t{n}\t{len(files[(vm, name)])}\t{r['table_kind']}\t{r['handler']}\t{r['c_name']}" +
                        "".join(f"\t{x.get(c, '')}" for c in others) + "".join(f"\t{u[(vm, name)]}" for _, u in columns) + "\n")
            else:
                missing += 1
                o.write(f"{vm}\t{name}\t{n}\t{len(files[(vm, name)])}\t(not a builtin here: a script function from another file?)\t\t" +
                        "\t" * len(others) + "".join(f"\t{u[(vm, name)]}" for _, u in columns) + "\n")
    print(f"{nscripts} scripts: {found} builtins used, {missing} other calls (script functions defined elsewhere)")


if __name__ == "__main__":
    main()
