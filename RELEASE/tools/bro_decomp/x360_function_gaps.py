#!/usr/bin/env python3
# [acct2-bro] Functions Ghidra knows in an Xbox 360 program that a .pdata-driven recompiler (rexglue) is likely to miss.
#
# rexglue takes its function list from the program's .pdata table, the [functions] lists in its config, and what
# direct calls reach. A function with none of these (reached only through a pointer built in code or kept in data)
# is missing, and calling it at run time fails ("Call to invalid or unregistered function"). tools/find_missing_funcs.py
# finds such functions from pointers in data; this finds them from a Ghidra export instead, which also sees pointers
# built in code. Big Red One's port met exactly this (an allocator hook set by an instruction, not stored in data).
#
# Read-only; reports only. Usage:
#   x360_function_gaps.py IMAGE_PE GHIDRA_EXPORT_DIR OUT_TSV [CONFIG_TOML ...]
# IMAGE_PE: the unpacked program (big-endian PowerPC PE). GHIDRA_EXPORT_DIR: functions.tsv and calls.tsv.
# OUT_TSV columns: address, Ghidra name, size, callers (of any kind) in Ghidra's call list, and the first place in
# data that points to it (empty when only code builds the pointer; find_missing_funcs.py cannot see those).
# Compiler helpers (__savegprlr_*, __restgprlr_*, ...) show up too; rexglue handles those on its own.
import csv, os, re, struct, sys


def read_tsv(path):
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        rows = csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE)
        head = next(rows)
        return [dict(zip(head, r)) for r in rows]


def pe_sections(img):
    e, = struct.unpack_from('<I', img, 0x3C)
    count, = struct.unpack_from('<H', img, e + 6)
    opt_size, = struct.unpack_from('<H', img, e + 20)
    base, = struct.unpack_from('<I', img, e + 24 + 28)
    s = e + 24 + opt_size
    out = []
    for _ in range(count):
        name = img[s:s + 8].rstrip(b'\0').decode('latin-1')
        vsize, va, rsize, raw = struct.unpack_from('<IIII', img, s + 8)
        flags, = struct.unpack_from('<I', img, s + 36)
        out.append((name, base + va, vsize, raw, rsize, flags))
        s += 40
    return base, out


def pdata_starts(img, secs):
    """Function starts from .pdata: 8-byte big-endian entries, the first word the function's address."""
    starts = set()
    for name, va, vsize, raw, rsize, _ in secs:
        if name != '.pdata':
            continue
        # an unpacked image may keep raw data at the virtual offset; use whichever holds entries
        data = img[raw:raw + min(vsize, rsize)] if rsize else b''
        for off in range(0, len(data) - 7, 8):
            begin, = struct.unpack_from('>I', data, off)
            if begin:
                starts.add(begin)
    return starts


def data_pointers(img, secs, targets):
    """target -> 'section@address' of the first big-endian 32-bit word in a non-code section equal to it."""
    found = {}
    for name, va, vsize, raw, rsize, flags in secs:
        if flags & 0x20000000:
            continue
        data = img[raw:raw + min(vsize, rsize)]
        for off in range(0, len(data) - 3, 4):
            v, = struct.unpack_from('>I', data, off)
            if v in targets and v not in found:
                found[v] = f'{name}@{va + off:08X}'
    return found


def main(image, export_dir, out_tsv, *tomls):
    img = open(image, 'rb').read()
    base, secs = pe_sections(img)
    code = [(va, va + vsize) for name, va, vsize, _, _, flags in secs if flags & 0x20000000]
    in_code = lambda a: any(lo <= a < hi for lo, hi in code)
    pdata = pdata_starts(img, secs)
    configured = set()
    for t in tomls:
        configured |= {int(x, 16) for x in re.findall(r'^\s*0x([0-9A-Fa-f]{8})\s*=', open(t, encoding='utf-8').read(), re.M)}

    funcs = {int(r['entry'], 16): r for r in read_tsv(os.path.join(export_dir, 'functions.tsv'))}
    direct, callers = set(), {}
    for r in read_tsv(os.path.join(export_dir, 'calls.tsv')):
        try:
            caller, callee = int(r['caller'], 16), int(r['callee'], 16)
        except (KeyError, ValueError):
            continue
        callers.setdefault(callee, set()).add(caller)
        if 'CALL' in r.get('kind', '') or 'JUMP' in r.get('kind', ''):
            direct.add(callee)

    gaps = [a for a in sorted(funcs) if in_code(a) and a not in pdata and a not in configured and a not in direct
            and funcs[a].get('thunk', '0') in ('0', 'false', '')]
    pointed = data_pointers(img, secs, set(gaps))
    with open(out_tsv, 'w', encoding='utf-8') as f:
        f.write('address\tghidra_name\tsize\tcallers\tpointed_to_from\n')
        for a in gaps:
            r = funcs[a]
            f.write(f'{a:08X}\t{r["name"]}\t{r.get("size", "")}\t{len(callers.get(a, ()))}\t{pointed.get(a, "")}\n')
    print(f'image base {base:08X}; {len(pdata)} .pdata starts, {len(configured)} configured, {len(funcs)} Ghidra '
          f'functions, {len(direct)} direct-call targets; {len(gaps)} Ghidra functions with none of these, '
          f'{len(pointed)} of them pointed to from data (find_missing_funcs.py can find those), '
          f'{len(gaps) - len(pointed)} only from code -> {out_tsv}')


if __name__ == '__main__':
    if len(sys.argv) < 4:
        sys.exit('usage: x360_function_gaps.py IMAGE_PE GHIDRA_EXPORT_DIR OUT_TSV [CONFIG_TOML ...]')
    main(*sys.argv[1:])
