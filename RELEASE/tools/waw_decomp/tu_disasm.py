#!/usr/bin/env python3
"""
tu_disasm.py - one function from the disc program next to its partner from the
title update, as a diff of their instructions (PowerPC, via llvm-mc).

Calls show the called function's name when it is known (our names.tsv for the
disc program; for the update, the name of its partner on the disc, from
tu_diff.py's table). Address halves and call distances, which change with
every relink, are written as HI/LO/name so only real changes show.

Usage:
    python3 tools/waw_decomp/tu_disasm.py disc.pe patched.pe functions.tsv DISC_ADDR|new@PATCHED_ADDR \
        [--names names.tsv] [--context 4]

Analysis only: the output is game code, keep it private.
"""
import argparse
import difflib
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from ppc_words import gpr_destinations, jump_table_addresses  # noqa: E402
from tu_diff import LOAD_ADDRESS, pe_functions  # noqa: E402

LLVM_MC = shutil.which("llvm-mc") or "llvm-mc"
VMX_OPCODES = (4, 5, 6)  # VMX128 forms llvm-mc can't read: shown as raw words


def disassemble(image, start, size):
    """[(address, word, text)] for the function, one entry per instruction."""
    base = start - LOAD_ADDRESS
    entries = []
    feed = []
    for i in range(0, size, 4):
        word = struct.unpack_from(">I", image, base + i)[0]
        entries.append([start + i, word, None])
        if word >> 26 in VMX_OPCODES or word == 0:
            feed.append(0x60000000)  # placeholder (nop) keeps one line per word
        else:
            feed.append(word)
    text = " ".join(f"0x{b:02x}" for w in feed for b in w.to_bytes(4, "big"))
    out = subprocess.run([LLVM_MC, "--disassemble", "-triple=powerpc-unknown-unknown", "-mcpu=pwr5"],
                         input=text, capture_output=True, text=True).stdout
    lines = [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith(".text")]
    for entry, line in zip(entries, lines):
        if entry[1] >> 26 in VMX_OPCODES or entry[1] == 0:
            entry[2] = f".long 0x{entry[1]:08X}"
        else:
            entry[2] = line
    for entry in entries[len(lines):]:
        entry[2] = f".long 0x{entry[1]:08X}"
    return entries


def normalize(entries, names, start, size):
    """Text with relink-dependent numbers replaced, plus the readable text. A switch's table after a `bctr` shows as
    `.table +offset` (its words are case addresses, not instructions), and a `lis` counts only until its register is
    written again (ppc_words.py says why)."""
    table = jump_table_addresses([word for _, word, _ in entries], start, size)
    lis = set()
    plain, shown = [], []
    for address, word, text in entries:
        if address in table:
            readable = f".table +{word - start:#x}"
            plain.append(readable)
            shown.append(f"{address:08X}  {readable}")
            continue
        op, rt, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        rel = re.search(r"\.([+-]\d+)", text)
        readable = text
        holds_address = None  # the register that holds an address half after this instruction
        if rel and op in (16, 18):
            target = address + int(rel.group(1))
            if start <= target < start + size:
                readable = text.replace(rel.group(0), f"+{target - start:#x}")
                norm = readable
            else:
                label = names.get(target, f"sub_{target:08X}")
                readable = text.replace(rel.group(0), label)
                norm = text.replace(rel.group(0), names.get(target, "FUNC"))
        elif op == 15 and ra == 0:
            norm = re.sub(r"-?\d+$", "HI", text)
            readable = f"{text}    ; {(word & 0xFFFF) << 16:#010x}"
            holds_address = rt
        elif (op == 24 and rt in lis) or (op != 24 and ra in lis and (op == 14 or 32 <= op <= 55 or op in (58, 62))):
            # ori's source is its RS field (bits 6-10); the others' base is RA.
            norm = re.sub(r"-?\d+(\(\d+\))", r"LO\1", text) if "(" in text else re.sub(r"-?\d+$", "LO", text)
            if op == 14:
                holds_address = rt
            elif op == 24:
                holds_address = ra
        else:
            norm = text
        lis -= gpr_destinations(word)
        if holds_address is not None:
            lis.add(holds_address)
        plain.append(norm)
        shown.append(f"{address:08X}  {readable}")
    return plain, shown


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc")
    ap.add_argument("patched")
    ap.add_argument("functions")
    ap.add_argument("which")
    ap.add_argument("--names")
    ap.add_argument("--context", type=int, default=4)
    args = ap.parse_args()
    disc = open(args.disc, "rb").read()
    patched = open(args.patched, "rb").read()
    disc_names = {}
    if args.names:
        for line in open(args.names, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            try:
                disc_names[int(parts[0], 16)] = parts[1]
            except (ValueError, IndexError):
                pass
    patched_names, pair = {}, {}
    for line in list(open(args.functions, encoding="utf-8"))[1:]:
        status, a_addr, a_size, b_addr, b_size, _, _ = line.rstrip("\n").split("\t")
        a, b = int(a_addr, 16), int(b_addr, 16)
        if a and b:
            patched_names[b] = disc_names.get(a, f"sub_{a:08X}")
        pair[a if a else ("new", b)] = (a, int(a_size, 16), b, int(b_size, 16))
        if not a:
            pair[("new", b)] = (0, 0, b, int(b_size, 16))
    if args.which.startswith("new@"):
        a, a_size, b, b_size = pair[("new", int(args.which[4:], 16))]
    else:
        a, a_size, b, b_size = pair[int(args.which, 16)]
    left = disassemble(disc, a, a_size) if a else []
    right = disassemble(patched, b, b_size) if b else []
    l_norm, l_show = normalize(left, disc_names, a, a_size)
    r_norm, r_show = normalize(right, patched_names, b, b_size)
    print(f"disc {a:08X} ({a_size:#x}) {disc_names.get(a, '')}  ->  update {b:08X} ({b_size:#x})")
    matcher = difflib.SequenceMatcher(None, l_norm, r_norm, autojunk=False)
    for group in matcher.get_grouped_opcodes(args.context):
        print("@@")
        for t, a1, a2, b1, b2 in group:
            if t == "equal":
                for k in range(a1, a2):
                    print("   " + l_show[k])
            else:
                for k in range(a1, a2):
                    print(" - " + l_show[k])
                for k in range(b1, b2):
                    print(" + " + r_show[k])


if __name__ == "__main__":
    main()
