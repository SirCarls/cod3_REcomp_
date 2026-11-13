#!/usr/bin/env python3
"""xbe2pe.py - turn an original Xbox program (.xbe) into a PE file Ghidra's PE
loader maps at the same addresses, plus a names table for its kernel imports.

    python3 -I xbe2pe.py default.xbe image.pe [--names names.tsv] [--sections sections.tsv]

An XBE is a PE that the Xbox build tools re-packed: sections keep their
virtual addresses (base 0x10000), the entry point and the kernel import table
address are XOR-encoded (retail or debug key), and kernel imports are a plain
array of ordinals | 0x80000000. This keeps every byte where the program
expects it:

  * one PE section per XBE section, same virtual address and size (section
    and file alignment 0x20, which XBE section addresses always meet);
  * the XBE header block (image header, certificate, library list) as a
    section `.xbeh` at the base, since the runtime reads it from there;
  * the entry point decoded;
  * names.tsv (addr, name, kind, pass, evidence: WawApplyNames.java's format):
    each kernel import slot as `__imp_<KernelExport>`, the entry point, the
    header fields.

The first line printed is a summary the jobs read:
  xbe2pe: base=0x10000 entry=0x... key=retail sections=N kernel_imports=N title=...
Nothing of the program is run; it is read as data.
"""
import argparse
import os
import struct
import sys

KEYS = {  # (entry key, kernel thunk key)
    "retail": (0xA8FC57AB, 0x5B6D40B6),
    "debug": (0x94859D4B, 0xEFB1F152),
    "beta": (0xE682F45B, 0x46437DCD),
}
ALIGN = 0x20
EXPORTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xboxkrnl_exports.tsv")


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def cstr(d, o):
    e = d.index(b"\0", o)
    return d[o:e].decode("latin-1")


def load_exports(path=EXPORTS):
    out = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#") or line.startswith("ordinal"):
                continue
            k, _, n = line.rstrip("\n").partition("\t")
            if k.isdigit():
                out[int(k)] = n
    return out


def parse_xbe(d):
    if d[:4] != b"XBEH":
        raise ValueError("not an XBE (no XBEH magic)")
    base = u32(d, 0x104)
    x = {
        "base": base,
        "headers_size": u32(d, 0x108),
        "image_size": u32(d, 0x10C),
        "timestamp": u32(d, 0x114),
        "cert": u32(d, 0x118),
        "nsec": u32(d, 0x11C),
        "sech": u32(d, 0x120),
        "entry_enc": u32(d, 0x128),
        "tls": u32(d, 0x12C),
        "thunk_enc": u32(d, 0x158),
        "nlib": u32(d, 0x160),
        "libh": u32(d, 0x164),
    }

    def fo(va):  # header virtual address -> file offset (headers are mapped 1:1 at base)
        return va - base

    secs = []
    for i in range(x["nsec"]):
        o = fo(x["sech"]) + i * 56
        flags, va, vs, ra, rs, name_va = struct.unpack_from("<IIIIII", d, o)
        secs.append({"name": cstr(d, fo(name_va)), "flags": flags, "va": va, "vsize": vs, "raw": ra, "rsize": rs})
    x["sections"] = secs
    c = fo(x["cert"])
    x["title_id"] = u32(d, c + 8)
    x["title"] = d[c + 0xC:c + 0xC + 80].decode("utf-16-le", "replace").split("\0", 1)[0]
    libs = []
    for i in range(x["nlib"]):
        o = fo(x["libh"]) + i * 16
        libs.append((d[o:o + 8].rstrip(b"\0").decode("latin-1"),) + struct.unpack_from("<HHH", d, o + 8))
    x["libraries"] = libs

    # Which key: the decoded entry point must land in an executable section.
    def in_code(va):
        return any(s["va"] <= va < s["va"] + s["vsize"] and (s["flags"] & 4) for s in secs)
    x["key"] = None
    for name, (ek, tk) in KEYS.items():
        if in_code(x["entry_enc"] ^ ek):
            x["key"], x["entry"], x["thunk"] = name, x["entry_enc"] ^ ek, x["thunk_enc"] ^ tk
            break
    if x["key"] is None:
        raise ValueError("entry point decodes into no executable section with any known key")
    return x


def read_va(d, x, va, n):
    for s in x["sections"]:
        if s["va"] <= va and va + n <= s["va"] + s["vsize"]:
            off = va - s["va"]
            if off + n <= s["rsize"]:
                return d[s["raw"] + off:s["raw"] + off + n]
            return (d[s["raw"] + off:s["raw"] + s["rsize"]] + b"\0" * n)[:n]
    if x["base"] <= va and va + n <= x["base"] + x["headers_size"]:
        return d[va - x["base"]:va - x["base"] + n]
    raise ValueError(f"address 0x{va:08X} is in no section")


def kernel_imports(d, x, exports):
    out = []
    va = x["thunk"]
    for i in range(1024):
        v = struct.unpack("<I", read_va(d, x, va + 4 * i, 4))[0]
        if v == 0:
            break
        if not v & 0x80000000:
            raise ValueError(f"kernel import slot {i} holds 0x{v:08X} (no ordinal flag)")
        k = v & 0x7FFFFFFF
        out.append((va + 4 * i, k, exports.get(k, f"xboxkrnl_{k}")))
    return out


def build_pe(d, x):
    base = x["base"]
    secs = [{"name": ".xbeh", "va": base, "vsize": x["headers_size"], "data": d[:x["headers_size"]],
             "flags": 0}] + [
        {"name": s["name"], "va": s["va"], "vsize": max(s["vsize"], 1),
         "data": d[s["raw"]:s["raw"] + min(s["rsize"], s["vsize"])], "flags": s["flags"]}
        for s in x["sections"]]
    secs.sort(key=lambda s: s["va"])
    for a, b in zip(secs, secs[1:]):
        if a["va"] + a["vsize"] > b["va"]:
            raise ValueError(f"sections {a['name']} and {b['name']} overlap")
    nsec = len(secs)
    # PE headers live in front of the image base so .xbeh can sit at the base itself.
    pe_base = base - 0x1000
    hdr_size = 0x400 if 0x40 + 4 + 20 + 224 + 40 * nsec <= 0x400 else 0x1000
    if 0x40 + 4 + 20 + 224 + 40 * nsec > 0x1000:
        raise ValueError("too many sections")
    raw = bytearray()
    pos = hdr_size
    table = bytearray()
    for s in secs:
        data = s["data"]
        rsize = (len(data) + ALIGN - 1) // ALIGN * ALIGN
        ch = 0x40000000  # readable
        if s["flags"] & 4:
            ch |= 0x20000000 | 0x00000020  # executable, code
        else:
            ch |= 0x00000040  # initialized data
        if s["flags"] & 1:
            ch |= 0x80000000  # writable
        nm = s["name"].encode("latin-1")[:8].ljust(8, b"\0")
        table += nm + struct.pack("<IIIIIIHHI", s["vsize"], s["va"] - pe_base, rsize, pos if rsize else 0,
                                  0, 0, 0, 0, ch)
        raw += data + b"\0" * (rsize - len(data))
        pos += rsize
    last = secs[-1]
    size_of_image = (last["va"] + last["vsize"] - pe_base + 0xFFF) & ~0xFFF
    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    coff = struct.pack("<HHIIIHH", 0x14C, nsec, x["timestamp"], 0, 0, 224, 0x0102 | 0x0020)
    opt = bytearray(224)
    struct.pack_into("<HBBIIIIIII", opt, 0, 0x10B, 7, 10, 0, 0, 0, x["entry"] - pe_base, 0, 0, pe_base)
    struct.pack_into("<IIHHHHHHIIIIHHIIIIII", opt, 32, ALIGN, ALIGN, 4, 0, 0, 0, 4, 0, 0, size_of_image,
                     hdr_size, 0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    head = bytes(dos) + b"PE\0\0" + coff + bytes(opt) + bytes(table)
    return head + b"\0" * (hdr_size - len(head)) + bytes(raw)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe")
    ap.add_argument("out_pe")
    ap.add_argument("--names", help="write names.tsv (kernel imports, entry point)")
    ap.add_argument("--sections", help="write sections.tsv")
    a = ap.parse_args(argv)
    d = open(a.xbe, "rb").read()
    x = parse_xbe(d)
    imps = kernel_imports(d, x, load_exports())
    pe = build_pe(d, x)
    with open(a.out_pe + ".part", "wb") as fh:
        fh.write(pe)
    os.replace(a.out_pe + ".part", a.out_pe)
    print(f"xbe2pe: base=0x{x['base']:X} entry=0x{x['entry']:08X} key={x['key']} sections={len(x['sections'])} "
          f"kernel_imports={len(imps)} title={x['title']} [{x['title_id']:08X}]")
    print("xbe2pe: libraries " + ", ".join(f"{n} {a_}.{b}.{c}" for n, a_, b, c in x["libraries"]))
    if a.names:
        with open(a.names, "w", encoding="utf-8") as fh:
            fh.write("addr\tname\tkind\tpass\tevidence\n")
            fh.write(f"{x['entry']:08X}\txbe_entry_point\tfunction\txbe\tXBE header entry point ({x['key']} key)\n")
            fh.write(f"{x['thunk']:08X}\txbe_kernel_thunk_table\tglobal\txbe\tXBE header kernel import table\n")
            for va, k, n in imps:
                fh.write(f"{va:08X}\t__imp_{n}\tglobal\txbe\txboxkrnl ordinal {k}\n")
    if a.sections:
        with open(a.sections, "w", encoding="utf-8") as fh:
            fh.write("name\tva\tvsize\traw\trsize\tflags\n")
            for s in x["sections"]:
                fh.write(f"{s['name']}\t{s['va']:08X}\t{s['vsize']:X}\t{s['raw']:X}\t{s['rsize']:X}\t{s['flags']:X}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
