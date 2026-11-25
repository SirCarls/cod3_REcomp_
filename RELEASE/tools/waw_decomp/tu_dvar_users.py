#!/usr/bin/env python3
"""
tu_dvar_users.py - where a title update reads one of its new settings (dvars).

An update usually puts each fix behind a new dvar. This finds the dvar's name
in the patched image, the code that registers it (the name's address built
with lis + addi, then a call, then r3 stored to a global: the dvar pointer),
and every function that loads that pointer. Each user is shown with its
partner in the disc program from tu_diff.py's table, so the fix can be read
side by side with the disc code it changes.

Usage:
    python3 tools/waw_decomp/tu_dvar_users.py patched.pe functions.tsv DVAR_NAME [DVAR_NAME ...]

Analysis only: keep the output private.
"""
import argparse
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from ppc_words import gpr_destinations  # noqa: E402
from tu_diff import LOAD_ADDRESS, pe_functions  # noqa: E402


def simm(word):
    low = word & 0xFFFF
    return low - 0x10000 if low & 0x8000 else low


def words(image, start, size):
    base = start - LOAD_ADDRESS
    for i in range(0, size, 4):
        yield start + i, struct.unpack_from(">I", image, base + i)[0]


def find_registration(image, funcs, name_address):
    """(function start, call address, dvar pointer global) for the code that registers the name: the name's address
    built into a register, then a call, then the call's result (r3, or a register it was copied to with `mr`) stored
    to a global. A `lis` counts only until its register is written again (ppc_words.py)."""
    for start, size in funcs:
        high = {}
        armed = None  # address of the instruction that built the name
        called = None
        result = set()  # registers holding the registration's result
        for at, word in words(image, start, size):
            op, rt, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            if op == 15 and ra == 0:
                high[rt] = (word & 0xFFFF) << 16
                result.discard(rt)
                continue
            if op == 14 and ra in high and ((high[ra] + simm(word)) & 0xFFFFFFFF) == name_address:
                armed, called, result = at, None, set()
            elif armed and op == 18 and word & 1:
                if called is None:
                    called, result = at, {3}
                else:
                    armed = None  # a second call before any store: not this one
            elif armed and called and op == 36 and rt in result and ra in high:
                return start, called, (high[ra] + simm(word)) & 0xFFFFFFFF
            copy_from = rt if op == 31 and (word >> 1) & 0x3FF == 444 and rt == (word >> 11) & 31 else None  # mr ra, rs
            written = gpr_destinations(word)
            if not (op == 18 and word & 1):
                result -= written
            if copy_from is not None and copy_from in result:
                result.add(ra)
            for register in written:
                high.pop(register, None)
    return None


def users_of(image, funcs, address):
    found = []
    for start, size in funcs:
        high = {}
        hits = 0
        for _, word in words(image, start, size):
            op, rt, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            if op == 15 and ra == 0:
                high[rt] = (word & 0xFFFF) << 16
                continue
            if ra in high and (op == 32 or op == 14) and ((high[ra] + simm(word)) & 0xFFFFFFFF) == address:
                hits += 1
            for register in gpr_destinations(word):
                high.pop(register, None)
        if hits:
            found.append((start, size, hits))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("patched")
    ap.add_argument("functions")
    ap.add_argument("names", nargs="+")
    args = ap.parse_args()
    image = open(args.patched, "rb").read()
    funcs = pe_functions(image)
    partner = {}
    for line in list(open(args.functions, encoding="utf-8"))[1:]:
        status, a_addr, _, b_addr, _, similarity, name = line.rstrip("\n").split("\t")
        partner[int(b_addr, 16)] = (status, a_addr, similarity, name)
    for dvar in args.names:
        offset = image.find(dvar.encode() + b"\0")
        while offset > 0 and image[offset - 1] != 0:  # a longer name that ends the same way
            offset = image.find(dvar.encode() + b"\0", offset + 1)
        if offset < 0:
            print(f"{dvar}: not in the patched image")
            continue
        name_address = LOAD_ADDRESS + offset
        reg = find_registration(image, funcs, name_address)
        if not reg:
            print(f"{dvar}: name at {name_address:08X}, no registration found")
            continue
        func, call, pointer = reg
        print(f"{dvar}: name {name_address:08X}, registered in {func:08X} (call at {call:08X}), pointer at {pointer:08X}")
        for start, size, hits in users_of(image, funcs, pointer):
            status, disc, similarity, fname = partner.get(start, ("?", "", "", ""))
            print(f"    used by {start:08X} ({size:#x} bytes, {hits}x): {status} disc {disc or '-'} {similarity} {fname}")


if __name__ == "__main__":
    main()
