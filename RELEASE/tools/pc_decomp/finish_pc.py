#!/usr/bin/env python3
"""
finish_pc.py - the last tables of a run_pc.sh run.

  finish_pc.py skiplist --work <work dir>
      Prints the functions whose C is left out of the decompile: C runtime functions that
      Ghidra's Function ID recognized (they are listed, by name, but not decompiled).

  finish_pc.py finish --work <work dir> --out <out dir>
      Writes, from the work dir's final/ tables and names.tsv:
        functions.tsv  address, size, name, pass, evidence, library, thunk
        globals.tsv    address, name, pass, evidence, section, type, refs
        strings.tsv    address, text (escaped), functions (names)
        stats.txt      counts per pass, decompile results, file sizes, step times
"""
import argparse
import collections
import csv
import os
import re
import sys

csv.field_size_limit(1 << 30)


def read_tsv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def ghidra_named(r):
    return r["source"] != "DEFAULT" and not r["name"].startswith(("FUN_", "thunk_FUN_", "LAB_"))


def library(rows, names):
    """C runtime functions: named by Ghidra's Function ID and by none of our passes."""
    out = []
    for r in rows:
        a = int(r["entry"], 16)
        if a in names or r["thunk"]:
            continue
        # Catch@ / Unwind@ are the game's own exception-handling funclets, not library code
        if r["source"] == "ANALYSIS" and ghidra_named(r) and "@" not in r["name"] \
                and not r["name"].startswith(("caseD_", "switchD_", "LAB_")):
            out.append((a, r["name"]))
    return out


def load_names(work):
    p = os.path.join(work, "names.tsv")
    return {int(r["addr"], 16): r for r in read_tsv(p)} if os.path.exists(p) else {}


def cmd_skiplist(args):
    names = load_names(args.work)
    for a, n in library(read_tsv(os.path.join(args.work, "raw", "functions.tsv")), names):
        print(f"{a:08X}\t{n}")


def esc(s):
    return s.replace("\t", " ").replace("\n", " ")


def cmd_finish(args):
    work, out = args.work, args.out
    names = load_names(work)
    funcs = read_tsv(os.path.join(work, "final", "functions.tsv"))
    lib = {a for a, _ in library(funcs, names)}
    by_addr = {int(r["entry"], 16): r for r in funcs}
    passes = collections.Counter()
    with open(os.path.join(out, "functions.tsv"), "w", encoding="utf-8") as o:
        o.write("address\tsize\tname\tpass\tevidence\tlibrary\tthunk\n")
        for a in sorted(by_addr):
            r = by_addr[a]
            n = names.get(a)
            if n and n["kind"] == "function":
                name, pas, ev = n["name"], n["pass"], n["evidence"]
            elif ghidra_named(r):
                name, pas = r["name"], "ghidra"
                ev = {"ANALYSIS": "Ghidra Function ID (library signature)" if not r["thunk"] else "Ghidra: thunk",
                      "IMPORTED": "the executable's own symbols (import or entry point)"}.get(r["source"], r["source"])
                if r["thunk"].startswith("ext:"):
                    ev = "import thunk: jumps to " + r["thunk"][4:]
            else:
                name, pas, ev = r["name"], "-", ""
            passes[pas] += 1
            o.write("\t".join(esc(x) for x in (f"{a:08X}", r["size"], name, pas, ev, "yes" if a in lib else "",
                                                r["thunk"])) + "\n")
    # globals: every global code uses, plus every named global
    gl = {int(r["addr"], 16): r for r in read_tsv(os.path.join(work, "final", "globals.tsv"))}
    gpasses = collections.Counter()
    with open(os.path.join(out, "globals.tsv"), "w", encoding="utf-8") as o:
        o.write("address\tname\tpass\tevidence\tsection\ttype\trefs\n")
        for a in sorted(set(gl) | {a for a, n in names.items() if n["kind"] == "global"}):
            r = gl.get(a, {})
            n = names.get(a)
            if n and n["kind"] == "global":
                name, pas, ev = n["name"], n["pass"], n["evidence"]
            else:
                name, pas, ev = r.get("name", ""), "-", ""
            gpasses[pas] += 1
            o.write("\t".join(esc(x) for x in (f"{a:08X}", name, pas, ev, r.get("section", ""), r.get("type", ""),
                                                r.get("nrefs", ""))) + "\n")
    fname = {a: (names[a]["name"] if a in names and names[a]["kind"] == "function" else by_addr[a]["name"]) for a in by_addr}
    nstr = 0
    with open(os.path.join(out, "strings.tsv"), "w", encoding="utf-8") as o:
        o.write("address\ttext\tfunctions\n")
        for r in read_tsv(os.path.join(work, "final", "strings.tsv")):
            fs = [fname.get(int(f, 16), f) for f in r["funcs"].split(",") if f]
            o.write(f"{r['addr']}\t{r['text']}\t{','.join(fs)}\n")
            nstr += 1
    # stats
    logs = os.path.join(work, "logs")

    def grep(path, pat):
        p = os.path.join(logs, path)
        if not os.path.exists(p):
            return []
        with open(p, encoding="utf-8", errors="replace") as f:
            return [re.sub(r"^INFO\s+\S+> |\s*\(GhidraScript\)\s*$", "", ln.rstrip()) for ln in f if re.search(pat, ln)]

    dec = os.path.join(out, "decomp")
    files = sorted(os.listdir(dec)) if os.path.isdir(dec) else []
    size = sum(os.path.getsize(os.path.join(dec, f)) for f in files)
    lines = [f"executable: {os.path.basename(open(os.path.join(work, 'exe.txt')).read().strip())}",
             f"functions: {len(by_addr)} ({len(lib)} C runtime functions listed without their C)"]
    lines += grep("2-find.log", r"PcFindFunctions: (before|after|round)")
    lines += grep("4-decomp.log", r"PcApplyNames: \d|PcExport: decompiled")
    lines.append("function names by pass: " + ", ".join(f"{k} {v}" for k, v in passes.most_common()))
    lines.append("global names by pass: " + ", ".join(f"{k} {v}" for k, v in gpasses.most_common()))
    lines.append(f"strings: {nstr}")
    lines.append(f"decomp: {len(files)} files, {size / 1e6:.1f} MB")
    p = os.path.join(logs, "3-names.txt")
    if os.path.exists(p):
        lines.append("naming passes (name_pc.py):")
        lines += ["  " + ln.rstrip() for ln in open(p, encoding="utf-8")][1:]
    p = os.path.join(logs, "times.txt")
    if os.path.exists(p):
        ts = [ln.rstrip().split(" ", 1) for ln in open(p, encoding="utf-8") if ln.strip()]
        took = {}
        for (t0, what), (t1, nxt) in zip(ts, ts[1:]):
            if what not in ("done", "start") and nxt != "start":
                took[what] = int(t1) - int(t0)
        lines.append("step times (seconds, latest run of each step): " + ", ".join(f"{k} {v}" for k, v in took.items()))
    with open(os.path.join(out, "stats.txt"), "w", encoding="utf-8") as o:
        o.write("\n".join(lines) + "\n")
    print("\n".join(lines[:12]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["skiplist", "finish"])
    ap.add_argument("--work", required=True)
    ap.add_argument("--out")
    args = ap.parse_args()
    return cmd_skiplist(args) if args.mode == "skiplist" else cmd_finish(args)


if __name__ == "__main__":
    sys.exit(main())
