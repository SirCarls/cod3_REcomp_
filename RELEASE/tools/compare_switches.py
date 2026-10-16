#!/usr/bin/env python3
"""Compare XenonAnalyse switch tables (reference project) with the jump
tables rexglue codegen actually emitted, and print the missing ones in
rexglue [[switch_tables]] format.

    compare_switches.py <ref_switch_tables.toml> <generated/<module>/> [--toml]
"""
import glob
import re
import sys


def parse_ref(path):
    out, cur = [], None
    for line in open(path):
        line = line.strip()
        if line == "[[switch]]":
            cur = {"labels": []}
            out.append(cur)
        elif cur is None:
            continue
        elif line.startswith("base"):
            cur["base"] = int(line.split("=")[1], 0)
        elif line.startswith("r ") or line.startswith("r="):
            cur["r"] = int(line.split("=")[1], 0)
        elif line.startswith("default"):
            cur["default"] = int(line.split("=")[1], 0)
        elif line.startswith("0x"):
            cur["labels"].append(int(line.rstrip(","), 16))
    return out


def parse_generated(gen_dir):
    tables = []
    for f in glob.glob(gen_dir + "/*.cpp"):
        txt = open(f, errors="replace").read()
        for m in re.finditer(r"\tswitch \(ctx\.r(\d+)\.u32\) \{\n(.*?)\n\tdefault:", txt, re.S):
            body = m.group(2)
            labels = []
            for c in re.finditer(r"\tcase \d+:\n\t\t(?:goto loc_([0-9A-F]+);|(sub_[0-9A-F]+|\w+)\(ctx, base\);|__builtin_trap)", body):
                if c.group(1):
                    labels.append(int(c.group(1), 16))
                elif c.group(2) and c.group(2).startswith("sub_"):
                    labels.append(int(c.group(2)[4:], 16))
                else:
                    labels.append(-1)
            tables.append((int(m.group(1)), tuple(labels)))
    return tables


def main():
    ref = parse_ref(sys.argv[1])
    gen = parse_generated(sys.argv[2])
    as_toml = "--toml" in sys.argv
    gen_sets = {t[1] for t in gen}
    gen_first = {}
    for r, labels in gen:
        if labels:
            gen_first.setdefault(labels[0], []).append(labels)
    missing, partial = [], []
    for s in ref:
        lab = tuple(s["labels"])
        if lab in gen_sets:
            continue
        cands = gen_first.get(lab[0], [])
        if cands:
            partial.append((s, cands[0]))
        else:
            missing.append(s)
    if not as_toml:
        print(f"reference {len(ref)}  generated {len(gen)}  missing {len(missing)}  differing {len(partial)}")
        for s in missing:
            print(f"  MISSING bctr 0x{s['base']:08X} r{s['r']} {len(s['labels'])} labels")
        for s, g in partial:
            print(f"  DIFFER  bctr 0x{s['base']:08X} r{s['r']} ref {len(s['labels'])} labels, ours {len(g)}")
    else:
        for s in missing + [p[0] for p in partial]:
            print("[[switch_tables]]")
            print(f"address = 0x{s['base']:08X}")
            print(f"register = {s['r']}")
            print("labels = [" + ", ".join(f"0x{v:08X}" for v in s["labels"]) + "]")
            print()


if __name__ == "__main__":
    main()
