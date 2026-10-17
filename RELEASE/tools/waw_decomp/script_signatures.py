#!/usr/bin/env python3
"""
script_signatures.py - what each script builtin takes and returns, read from its handler's C.

For the Rust script VM: a builtin's handler reads its arguments with Scr_GetInt(0), Scr_GetFloat(1),
Scr_GetVector(2)... (the number is the argument's position), checks how many it was given through
gScrVmPub.outparamcount, and returns a value with Scr_AddInt, Scr_AddString... This tool reads those
calls in the decompiled C (WawExport's output, with the script helpers named) and lists, per builtin:
the arguments by position and type, the argument counts the handler tests, what it returns, and the
handler's own messages (usage lines such as "USAGE: player visionsetlaststand( <visionset name>,
<transition time> )", which the game prints on a bad call). A handler that leaves all of that to one
helper is read through that helper ("via" column). "shared_with" lists the other names whose table
entry points at the same handler: the linker folds functions with identical code into one, so a
builtin that does nothing in this program (SP's rumble functions) shares an empty function with
other names, and the handler's C name is just one of them.

Usage:
    python3 tools/waw_decomp/script_signatures.py --decomp <export dir> --api script_api.tsv \\
        --out signatures.tsv [--usage usage.tsv] [--kisak-tables kisak_functions_tables.tsv]

--decomp: WawExport's folder (decomp/*.c, calls.tsv, strings.tsv). --api: script_api.py's table.
--usage: script_usage.py's table; only the builtins in it are listed, in its order, with its call
counts and per-map columns kept. --kisak-tables: kisak_strings.py's table list; adds the name of
KisakCOD's (Call of Duty 4's) handler for the same script name, empty for World at War's own builtins,
whose C here is the only reference.

What it can't see: an argument read by a helper the handler calls with the position in a register
(Scr_GetString() with no number), arguments only passed on to another function, and types checked
with Scr_GetType (listed as "any"). The C is the reference; this is an index into it.

Needs: nothing outside the standard library.
"""
import argparse
import collections
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from naming_common import read_tsv  # noqa: E402
from script_usage import SCRIPT_KINDS  # noqa: E402

HEADER = re.compile(r"^// ([0-9A-F]{8}) (\S+) \(\d+ bytes\)$", re.M)
GET_TYPES = {
    "Int": "int", "Float": "float", "Vector": "vector", "String": "string", "ConstString": "string",
    "ConstLowercaseString": "string", "ConstIString": "istring", "IString": "istring",
    "ConstStringIncludeNull": "string", "DebugString": "string", "Entity": "entity", "EntityRef": "entity",
    "EntityId": "entity", "Ent": "entity", "Anim": "anim", "AnimTree": "animtree", "EntAnimTree": "animtree",
    "Pathnode": "pathnode", "VehicleNodeIndex": "vehiclenode", "Object": "object",
    "FunctionHandle": "function", "Type": "any", "PointerType": "any", "TypeName": "any",
    "Bool": "bool", "Node": "node",
}
ADD_TYPES = {
    "Int": "int", "Bool": "bool", "Float": "float", "Vector": "vector", "String": "string",
    "ConstString": "string", "IString": "istring", "Object": "object", "Entity": "entity", "Array": "array",
    "ArrayStringIndexed": "array", "Undefined": "undefined", "Anim": "anim", "Struct": "struct",
}
GET = re.compile(r"\bScr_Get(\w+?)\(([^()]*(?:\([^()]*\)[^()]*)*)\)")
ADD = re.compile(r"\bScr_(?:Add(\w+?)|(MakeArray))\(")
COUNT = re.compile(r"gScrVmPub\.outparamcount\s*(==|!=|<=|>=|<|>)\s*(\d+)|case (\d+):")
SWITCH = re.compile(r"switch\s*\(\s*gScrVmPub\.outparamcount\s*\)")


def bodies(folder):
    """{address: (name, C text)} for every function of a WawExport folder."""
    out = {}
    for fn in sorted(os.listdir(os.path.join(folder, "decomp"))):
        with open(os.path.join(folder, "decomp", fn), encoding="utf-8", errors="replace") as f:
            text = f.read()
        heads = list(HEADER.finditer(text))
        for h, nxt in zip(heads, heads[1:] + [None]):
            out[int(h.group(1), 16)] = (h.group(2), text[h.end():nxt.start() if nxt else len(text)])
    return out


def read_sig(text):
    """(arguments {position: types}, argument counts tested, returns, unnumbered reads)."""
    args, loose = collections.defaultdict(set), set()
    for kind, params in GET.findall(text):
        t = GET_TYPES.get(kind)
        if not t:
            continue
        # the position is the one plain number among the arguments (Scr_GetInt(1),
        # Scr_GetVector(&v, 2)); none when the call passes it in a register
        nums = [x.strip() for x in params.split(",") if re.fullmatch(r"\s*\d+\s*", x)]
        if len(nums) == 1:
            args[int(nums[0])].add(t)
        else:
            loose.add(t)
    counts = set()
    for op, n, case in COUNT.findall(text):
        if case:
            if SWITCH.search(text):
                counts.add(int(case))
        else:
            counts.add(int(n))
    rets = set()
    for kind, make in ADD.findall(text):
        t = "array" if make else ADD_TYPES.get(kind)
        if t:
            rets.add(t)
    return args, counts, rets, loose


def fmt_args(args):
    return " ".join(f"{p}:{'|'.join(sorted(t))}" for p, t in sorted(args.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--api", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--usage")
    ap.add_argument("--kisak-tables")
    a = ap.parse_args()
    kisak = {}
    for r in read_tsv(a.kisak_tables) if a.kisak_tables else []:
        kisak.setdefault(r["string"].lower(), r["function"])
    funcs = bodies(a.decomp)
    callees = collections.defaultdict(list)
    for r in read_tsv(os.path.join(a.decomp, "calls.tsv")):
        if re.fullmatch(r"[0-9A-Fa-f]{8}", r["callee"]):  # direct calls only (not "ptr:..." rows)
            callees[int(r["caller"], 16)].append(int(r["callee"], 16))
    texts = collections.defaultdict(list)
    for r in read_tsv(os.path.join(a.decomp, "strings.tsv")):
        for f in r["funcs"].split(","):
            if f:
                texts[int(f, 16)].append(r["text"])
    api = collections.defaultdict(list)
    by_handler = collections.defaultdict(set)  # one handler for several names: identical code folded by the linker
    for r in read_tsv(a.api):
        by_handler[r["handler"]].add(r["script_name"].lower())
        if r["table_kind"] not in SCRIPT_KINDS and "GetFunction" not in r["read_by"] and "GetMethod" not in r["read_by"]:
            continue  # menu, effect and spawn keyword tables
        vm = "csc" if r["table_kind"] == "CScr" or r["read_by"].startswith("CScr_") else "gsc"
        api[(vm, r["script_name"].lower())].append(r)
    if a.usage:
        usage = [r for r in read_tsv(a.usage) if r.get("handler")]
        extra = [c for c in usage[0].keys() if c not in ("vm", "script_name", "table_kind", "handler", "c_name")] if usage else []
    else:
        usage = [{"vm": vm, "script_name": n, "handler": rows[0]["handler"]} for (vm, n), rows in sorted(api.items())]
        extra = []
    stats = collections.Counter()
    with open(a.out, "w", encoding="utf-8", newline="") as o:
        w = csv.writer(o, delimiter="\t", lineterminator="\n")
        w.writerow(["vm", "script_name", "kind", "handler", "c_name", "args", "arg_counts", "returns",
                    "unnumbered_reads", "via", "shared_with", "kisakcod", "messages"] + extra)
        for u in usage:
            rows = [r for r in api.get((u["vm"], u["script_name"]), []) if r["handler"] == u["handler"]] or \
                api.get((u["vm"], u["script_name"]), [])
            if not rows:
                continue
            r = rows[0]
            h = int(r["handler"], 16)
            kind = "method" if "GetMethod" in r["read_by"] or r["table_kind"].endswith("Cmd") or r["table_kind"] == "CMD" else "function"
            name, text = funcs.get(h, ("", ""))
            args, counts, rets, loose = read_sig(text)
            via = ""
            if not args and not rets and not loose:
                # a thin handler: read its one helper that does take arguments
                for c in callees.get(h, []):
                    cname, ctext = funcs.get(c, ("", ""))
                    if cname.startswith("Scr_") or not ctext:
                        continue
                    cargs, ccounts, crets, cloose = read_sig(ctext)
                    if cargs or crets or cloose:
                        args, counts, rets, loose, via = cargs, counts | ccounts, crets, cloose, cname
                        break
            msgs = [t.replace("\t", " ").replace("\n", " ").strip() for t in texts.get(h, [])
                    if re.search(r"usage|USAGE|must|expect|invalid|not a|\(", t)]
            stats["with args or returns"] += bool(args or rets or loose)
            stats["with a message"] += bool(msgs)
            stats["builtins"] += 1
            w.writerow([u["vm"], u["script_name"], kind, r["handler"], r["c_name"], fmt_args(args),
                        ",".join(str(n) for n in sorted(counts)), ",".join(sorted(rets)), ",".join(sorted(loose)),
                        via, ",".join(sorted(by_handler[r["handler"]] - {u["script_name"]})), kisak.get(u["script_name"], ""),
                        " | ".join(msgs[:3])[:400]] + [u.get(c, "") for c in extra])
    print(", ".join(f"{k}: {v}" for k, v in stats.items()))


if __name__ == "__main__":
    main()
