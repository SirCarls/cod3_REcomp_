#!/usr/bin/env python3
"""
tu_diff.py - list which functions a title update changed, by comparing the disc
program's image with the patched image xexp_apply.py rebuilds.

Both images come from the same source and compiler, so most functions are the
same instructions at new addresses. Each image's function table (.pdata) gives
the functions; each function is reduced to a fingerprint that ignores what moves
when code is relinked (call targets, the high halves of addresses and the low
halves that pair with them). Then:

  1. same:     fingerprints found once on each side pair up;
  2. changed:  between two such pairs, functions left over on both sides pair up
               in order when their sizes are close (the linker keeps the order);
  3. added / removed: whatever is left.

Usage:
    python3 tools/waw_decomp/tu_diff.py disc.pe patched.pe out.tsv [--names names.tsv]

names.tsv (optional): address <tab> name for the disc program, to label rows.
out.tsv: status, disc address, disc size, patched address, patched size,
similarity (for changed rows: the share of cleared instructions both keep,
1.000 = only relinking moved), name.
Analysis only: the images are game code, keep them and the table private.
"""
import argparse
import bisect
import difflib
import hashlib
import struct
import sys

LOAD_ADDRESS = 0x82000000


def pe_functions(image):
    """(start, size) for every function in the image's .pdata table."""
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\0\0":
        sys.exit("error: not a PE image")
    opt = pe + 24
    magic = struct.unpack_from("<H", image, opt)[0]
    dirs = opt + (96 if magic == 0x10B else 112)
    pdata_rva, pdata_size = struct.unpack_from("<II", image, dirs + 3 * 8)  # exception directory
    entries = [struct.unpack_from(">II", image, off) for off in range(pdata_rva, pdata_rva + pdata_size, 8)]
    entries = sorted(e for e in entries if e[0])
    # The length (in instructions) is a 22-bit field next to an 8-bit prolog
    # length and two flag bits; take the reading where functions end where the
    # next one starts most often.
    readings = {"bits 8-29": lambda info: (info >> 8) & 0x3FFFFF, "bits 2-23": lambda info: (info >> 2) & 0x3FFFFF}
    def fit(read):
        return sum(1 for (s, i), (n, _) in zip(entries, entries[1:]) if s + read(i) * 4 == n)
    label = max(readings, key=lambda k: fit(readings[k]))
    read = readings[label]
    print(f"  .pdata: {len(entries)} functions, length in {label} ({fit(read)} end where the next starts)")
    return [(s, read(i) * 4) for s, i in entries]


def fingerprint(code):
    """SHA-1 of the instructions with relink-dependent fields cleared.

    Cleared: the target of b/bl (opcode 18), the immediate of lis/addis
    (opcode 15), and the 16-bit offset of addi, ori and loads/stores whose
    base register holds an address built from a lis earlier in the function
    (the low half of an address; an addi or ori on such a register makes its
    target one too). Stack and structure offsets stay, so a changed field
    offset still shows as a change."""
    from_lis = set()
    out = bytearray()
    for i in range(0, len(code) - 3, 4):
        word = struct.unpack_from(">I", code, i)[0]
        op = word >> 26
        rt = (word >> 21) & 31
        ra = (word >> 16) & 31
        if op == 18:
            word &= 0xFC000003
        elif op == 15:
            word &= 0xFFFF0000
            from_lis.add(rt)
        elif ra in from_lis and (op == 14 or op == 24):  # addi / ori: the result is an address too
            word &= 0xFFFF0000
            from_lis.add(rt if op == 14 else ra)
        elif ra in from_lis and 32 <= op <= 55:  # loads and stores, D form
            word &= 0xFFFF0000
        elif ra in from_lis and op in (58, 62):  # ld / std, DS form: keep the two kind bits
            word &= 0xFFFF0003
        out += struct.pack(">I", word)
    return hashlib.sha1(bytes(out)).hexdigest(), bytes(out)


def read_names(path):
    names = {}
    if path:
        for line in open(path, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2:
                try:
                    names[int(parts[0], 16)] = parts[1]
                except ValueError:
                    pass
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc")
    ap.add_argument("patched")
    ap.add_argument("out")
    ap.add_argument("--names")
    args = ap.parse_args()

    sides = []
    for path in (args.disc, args.patched):
        image = open(path, "rb").read()
        funcs = pe_functions(image)
        prints, words = zip(*[fingerprint(image[s - LOAD_ADDRESS:s - LOAD_ADDRESS + n]) for s, n in funcs])
        sides.append((funcs, list(prints), list(words)))
    (a_funcs, a_prints, a_words), (b_funcs, b_prints, b_words) = sides
    print(f"functions: disc {len(a_funcs)}, patched {len(b_funcs)}")

    # 1. same: fingerprints that occur once on each side
    a_count, b_count = {}, {}
    for p in a_prints:
        a_count[p] = a_count.get(p, 0) + 1
    for p in b_prints:
        b_count[p] = b_count.get(p, 0) + 1
    b_index = {p: i for i, p in enumerate(b_prints) if b_count[p] == 1}
    pair = {}
    for i, p in enumerate(a_prints):
        if a_count[p] == 1 and p in b_index:
            pair[i] = b_index[p]
    # Keep only pairs that agree with the link order (longest increasing run).
    anchors = sorted(pair.items())
    tails, links, idx = [], [None] * len(anchors), []
    for k, (_, j) in enumerate(anchors):
        pos = bisect.bisect_left(tails, j)
        if pos == len(tails):
            tails.append(j)
            idx.append(k)
        else:
            tails[pos] = j
            idx[pos] = k
        links[k] = idx[pos - 1] if pos else None
    chain, k = [], idx[-1] if idx else None
    while k is not None:
        chain.append(anchors[k])
        k = links[k]
    chain.reverse()
    print(f"same: {len(pair)} ({len(chain)} in link order, {len(pair) - len(chain)} moved)")

    # 2. changed: leftovers between consecutive in-order pairs, paired in order by size
    rows = []
    used_b = set(pair.values())
    bounds = [(-1, -1)] + chain + [(len(a_funcs), len(b_funcs))]
    for (a0, b0), (a1, b1) in zip(bounds, bounds[1:]):
        left = [i for i in range(a0 + 1, a1) if i not in pair]
        right = [j for j in range(b0 + 1, b1) if j not in used_b]
        ri = 0
        for i in left:
            size = a_funcs[i][1]
            match = None
            for jj in range(ri, len(right)):
                other = b_funcs[right[jj]][1]
                if abs(other - size) <= max(64, size // 4):
                    match = jj
                    break
            if match is None:
                rows.append(("removed", i, None))
                continue
            for jj in range(ri, match):
                rows.append(("added", None, right[jj]))
            rows.append(("changed", i, right[match]))
            ri = match + 1
        for jj in range(ri, len(right)):
            rows.append(("added", None, right[jj]))
    for i, j in pair.items():
        rows.append(("same", i, j))
    # Pairs from step 2 whose fingerprints agree (short functions that occur
    # more than once, so step 1 left them) are the same too.
    rows = [("same", i, j) if s == "changed" and a_prints[i] == b_prints[j] else (s, i, j) for s, i, j in rows]

    names = read_names(args.names)
    counts = {}
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("status\tdisc_addr\tdisc_size\tpatched_addr\tpatched_size\tsimilarity\tname\n")
        for status, i, j in sorted(rows, key=lambda r: (r[1] if r[1] is not None else -1, r[2] or -1)):
            counts[status] = counts.get(status, 0) + 1
            a = a_funcs[i] if i is not None else (0, 0)
            b = b_funcs[j] if j is not None else (0, 0)
            name = names.get(a[0], "") if i is not None else ""
            similarity = ""
            if status == "changed":
                wa = [a_words[i][k:k + 4] for k in range(0, len(a_words[i]), 4)]
                wb = [b_words[j][k:k + 4] for k in range(0, len(b_words[j]), 4)]
                similarity = f"{difflib.SequenceMatcher(None, wa, wb, autojunk=False).ratio():.3f}"
            f.write(f"{status}\t{a[0]:08X}\t{a[1]:#x}\t{b[0]:08X}\t{b[1]:#x}\t{similarity}\t{name}\n")
    print("rows: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
