#!/usr/bin/env python3
"""Check reference function boundaries (XenonRecomp config: functions = [
{ address = .., size = .. }]) against the functions rexglue codegen emitted:
any of our function starts strictly inside a reference function means codegen
cut it into pieces.

    check_ref_functions.py <ref.toml> <generated/<module>/<project>_init.cpp> [--toml]
"""
import bisect
import re
import sys

ref = [(int(a, 16), int(s, 16)) for a, s in
       re.findall(r"address = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+)", open(sys.argv[1]).read())]
init = open(sys.argv[2], errors="replace").read()
known = sorted({int(x, 16) for x in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), \w+ \}", init)})
as_toml = "--toml" in sys.argv
bad = 0
for a, s in ref:
    i = bisect.bisect_right(known, a)
    inside = [k for k in known[i:i + 4096] if k < a + s]
    start_known = a in known
    if inside or not start_known:
        bad += 1
        if as_toml:
            print(f"0x{a:08X} = {{ size = 0x{s:X} }}")
        else:
            print(f"0x{a:08X} size 0x{s:X}: start {'known' if start_known else 'MISSING'}, "
                  f"{len(inside)} of our starts inside: " + " ".join(f"{k:08X}" for k in inside[:8]))
if not as_toml:
    print(f"{len(ref)} reference functions, {bad} differ")
