#!/usr/bin/env python3
"""fh_wrap.py - wrap some of Finest Hour's recompiled functions with the port's own C, without lifting the game again.

A stand-in (fh/config/stand_in_functions.tsv) replaces a whole function: the recompiler is told to leave it out, so
nothing of the game's own body is left to call. Some fixes need the game's body kept and something done before or
after it (fh/src/fh_worlds.c: a world's objects taken out of it once it is freed). fh/config/wrapped_functions.tsv
lists those functions. For each, this finds the one file of the recompiled C that defines `void sub_<ADDRESS>(void)`
and writes a copy of that file into OUT_DIR with that one definition renamed `fh_lifted_<ADDRESS>`. The build compiles
the copies in place of those files, so:
  - every call to sub_<ADDRESS> (direct calls in any file, the same file's included, and the dispatch table's entry
    that calls through a pointer reach) lands in the port's wrapper of that name, which calls fh_lifted_<ADDRESS>;
  - the recompiler's output (FH_GEN_DIR) is never changed, and the lift's options and its cache stay as they are.

[waw-decomp/fh11] 2026-11-24: loop back edges too. fh/config/back_edge_hooks.tsv (address, hook, note) names a C
function of the port's to call each time the game's code takes the jump back at that address: the recompiler marks
such a jump RECOMP_BACK_EDGE(0x<ADDRESS>u), and the copy gets `{ extern void <hook>(void); <hook>(); }` just before
that mark. Each mark must be in exactly one file, once (fh/src/fh_jobs.c: the job thread's empty queue).

[waw-decomp/fh3] 2026-11-23. Usage (fh/CMakeLists.txt, at configure time):
    fh_wrap.py GEN_DIR TABLE_TSV OUT_DIR [BACK_EDGE_TSV]
Prints two lines: the generated files replaced, then their copies (each a CMake list, ';' between items). A copy is
rewritten only when its text changes, so a build after an unchanged configure compiles nothing again. Stops with an
error when an address is defined in no file or in more than one (e.g. it is a stand-in too). Standard library only.
"""
import csv
import os
import re
import sys


def read_rows(path):
    with open(path, encoding="utf-8", newline="") as f:
        rows = [r for r in csv.DictReader(f, delimiter="\t") if r.get("address", "").strip()]
    out = []
    for r in rows:
        a = r["address"].strip().upper()
        if not re.fullmatch(r"[0-9A-F]{8}", a):
            sys.exit(f"fh_wrap: {path}: address '{r['address']}' is not 8 hexadecimal digits")
        out.append(a)
    if len(set(out)) != len(out):
        sys.exit(f"fh_wrap: {path}: an address is listed twice")
    return out


def read_edges(path):
    if not path:
        return []
    with open(path, encoding="utf-8", newline="") as f:
        rows = [r for r in csv.DictReader(f, delimiter="\t") if r.get("address", "").strip()]
    out = []
    for r in rows:
        a, hook = r["address"].strip().upper(), r.get("hook", "").strip()
        if not re.fullmatch(r"[0-9A-F]{8}", a):
            sys.exit(f"fh_wrap: {path}: address '{r['address']}' is not 8 hexadecimal digits")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", hook):
            sys.exit(f"fh_wrap: {path}: hook '{hook}' for {a} is not a C name")
        out.append((a, hook))
    if len({a for a, _ in out}) != len(out):
        sys.exit(f"fh_wrap: {path}: an address is listed twice")
    return out


def main(argv):
    if len(argv) not in (3, 4):
        sys.exit("usage: fh_wrap.py GEN_DIR TABLE_TSV OUT_DIR [BACK_EDGE_TSV]")
    gen, table, out_dir = argv[:3]
    addrs = read_rows(table)
    edges = read_edges(argv[3] if len(argv) == 4 else "")
    marks = {a: f"RECOMP_BACK_EDGE(0x{a}u);" for a, _ in edges}
    edge_found = {a: [] for a, _ in edges}
    wanted = {f"void sub_{a}(void)": a for a in addrs}
    found = {a: [] for a in addrs}
    texts = {}
    for name in sorted(os.listdir(gen)):
        if not name.endswith(".c"):
            continue
        path = os.path.join(gen, name)
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        hit = False
        for line in text.splitlines():
            a = wanted.get(line.rstrip())
            if a:
                found[a].append(name)
                hit = True
        for a, mark in marks.items():
            n = text.count(mark)
            if n:
                edge_found[a] += [name] * n
                hit = True
        if hit:
            texts[name] = text
    bad = [f"{a} ({'nowhere' if not f else ', '.join(f)})" for a, f in found.items() if len(f) != 1]
    if bad:
        sys.exit("fh_wrap: each wrapped function must be defined in exactly one recompiled file; not so for: "
                 + "; ".join(bad) + f" (in {gen}). A stand-in's address can't be wrapped: the lift leaves it out.")
    bad = [f"{a} ({'nowhere' if not f else ', '.join(f)})" for a, f in edge_found.items() if len(f) != 1]
    if bad:
        sys.exit("fh_wrap: each hooked back edge must be marked exactly once in the recompiled C; not so for: "
                 + "; ".join(bad) + f" (in {gen}).")
    os.makedirs(out_dir, exist_ok=True)
    originals, copies = [], []
    for name, text in sorted(texts.items()):
        for a in addrs:
            if found[a][0] == name:
                text, n = re.subn(rf"(?m)^void sub_{a}\(void\)(\r?)$",
                                  rf"void fh_lifted_{a}(void) /* fh_wrap.py: wrapped, see fh/config/wrapped_functions.tsv */\1",
                                  text)
                if n != 1:
                    sys.exit(f"fh_wrap: renaming sub_{a} in {name} changed {n} lines, not 1")
        for a, hook in edges:
            if edge_found[a][0] == name:
                text = text.replace(marks[a], f"{{ extern void {hook}(void); {hook}(); /* fh_wrap.py: "
                                    f"fh/config/back_edge_hooks.tsv */ }} {marks[a]}", 1)
        dst = os.path.join(out_dir, name)
        old = None
        if os.path.exists(dst):
            with open(dst, encoding="utf-8", errors="surrogateescape", newline="") as f:
                old = f.read()
        if old != text:
            with open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(text)
        originals.append(os.path.join(gen, name).replace("\\", "/"))
        copies.append(dst.replace("\\", "/"))
    for stale in os.listdir(out_dir):
        if stale.endswith(".c") and stale not in texts:
            os.remove(os.path.join(out_dir, stale))
    print(";".join(originals))
    print(";".join(copies))


if __name__ == "__main__":
    main(sys.argv[1:])
