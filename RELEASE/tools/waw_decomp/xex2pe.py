#!/usr/bin/env python3
"""
xex2pe.py - turn an Xbox 360 XEX2 executable into its plain PE image for
analysis (Ghidra, our naming tools). Handles retail encryption (AES) and both
compression kinds: "basic" (zero-filled runs) and "normal" (LZX).

Usage:
    python3 tools/waw_decomp/xex2pe.py default.xex default.pe [--info]

LZX needs the small helper library built from libmspack (see xexlzx/xexlzx.c);
set XEXLZX=<path to libxexlzx.so / xexlzx.dll> or put it next to this script.
AES uses the `cryptography` package when present, otherwise the openssl program.

Written for this project from the XEX2 layout as Xenia documents it
(src/xenia/kernel/util/xex2_info.h, xex_module.cc). The output is derived from
the game: keep it private.
"""
import argparse
import ctypes
import hashlib
import os
import struct
import subprocess
import sys

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)
HDR_FILE_FORMAT = 0x000003FF
HDR_ENTRY_POINT = 0x00010100
HDR_IMAGE_BASE = 0x00010201
HDR_ORIGINAL_PE_NAME = 0x000183FF
HDR_EXECUTION_INFO = 0x00040006


def aes_dec(mode, key, data):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        m = modes.ECB() if mode == "ecb" else modes.CBC(bytes(16))
        d = Cipher(algorithms.AES(key), m).decryptor()
        return d.update(data) + d.finalize()
    except ImportError:
        cmd = ["openssl", "enc", "-d", f"-aes-128-{mode}", "-nopad", "-K", key.hex()]
        if mode == "cbc":
            cmd += ["-iv", "0" * 32]
        return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout


def lzx(data, out_len, window_bits):
    here = os.path.dirname(os.path.abspath(__file__))
    names = [os.environ.get("XEXLZX", "")] + [os.path.join(here, n) for n in ("libxexlzx.so", "xexlzx.dll", "libxexlzx.dylib")]
    lib = None
    for n in names:
        if n and os.path.isfile(n):
            lib = ctypes.CDLL(n)
            break
    if lib is None:
        sys.exit("error: LZX-compressed XEX: build xexlzx (see xexlzx/xexlzx.c) and set XEXLZX")
    out = ctypes.create_string_buffer(out_len)
    src = ctypes.create_string_buffer(bytes(data), len(data))
    lib.xex_lzx_decompress.restype = ctypes.c_int
    err = lib.xex_lzx_decompress(src, ctypes.c_size_t(len(data)), out, ctypes.c_size_t(out_len), ctypes.c_int(window_bits))
    if err != 0:
        sys.exit(f"error: LZX decompression failed (code {err})")
    return out.raw


def parse(d):
    if d[:4] != b"XEX2":
        sys.exit("error: not an XEX2 file")
    pe_off, = struct.unpack(">I", d[8:12])
    sec_off, = struct.unpack(">I", d[16:20])
    n, = struct.unpack(">I", d[20:24])
    hdrs = {}
    for i in range(n):
        k, v = struct.unpack(">II", d[24 + i * 8:32 + i * 8])
        hdrs[k] = v
    return pe_off, sec_off, hdrs


def convert(d):
    pe_off, sec, hdrs = parse(d)
    image_size, = struct.unpack(">I", d[sec + 4:sec + 8])
    load_addr, = struct.unpack(">I", d[sec + 0x110:sec + 0x114])
    file_key = d[sec + 0x150:sec + 0x160]
    ffi = hdrs[HDR_FILE_FORMAT]
    info_size, enc, comp = struct.unpack(">IHH", d[ffi:ffi + 8])
    payload = d[pe_off:]
    info = {"load_addr": load_addr, "image_size": image_size, "encryption": enc, "compression": comp}
    if HDR_ENTRY_POINT in hdrs:
        info["entry"] = hdrs[HDR_ENTRY_POINT]

    def decrypt(key):
        p = payload[:len(payload) // 16 * 16]
        return aes_dec("cbc", aes_dec("ecb", key, file_key), p)

    def build(dec):
        if comp == 1:  # basic: (data size, zero size) pairs
            out = bytearray()
            p = 0
            for i in range((info_size - 8) // 8):
                ds, zs = struct.unpack(">II", d[ffi + 8 + i * 8:ffi + 16 + i * 8])
                out += dec[p:p + ds]
                p += ds
                out += b"\0" * zs
            return bytes(out)
        if comp == 2:  # normal: hashed blocks of LZX chunks
            window_size, = struct.unpack(">I", d[ffi + 8:ffi + 12])
            block_size, = struct.unpack(">I", d[ffi + 12:ffi + 16])
            block_hash = d[ffi + 16:ffi + 36]
            stream = bytearray()
            p = 0
            while block_size:
                block = dec[p:p + block_size]
                if len(block) < block_size:
                    raise ValueError("truncated compressed block")
                if hashlib.sha1(block).digest() != block_hash:
                    raise ValueError("block hash mismatch")
                next_size, = struct.unpack(">I", block[0:4])
                next_hash = block[4:24]
                q = 24
                while q + 2 <= len(block):
                    cs, = struct.unpack(">H", block[q:q + 2])
                    q += 2
                    if cs == 0:
                        break
                    stream += block[q:q + cs]
                    q += cs
                p += block_size
                block_size, block_hash = next_size, next_hash
            bits = window_size.bit_length() - 1
            info["lzx_window_bits"] = bits
            return lzx(stream, image_size, bits)
        if comp == 0:
            return dec
        raise ValueError(f"compression type {comp} not supported")

    last = None
    for key in ((RETAIL_KEY, DEVKIT_KEY) if enc == 1 else (None,)):
        try:
            dec = decrypt(key) if key else payload
            img = build(dec)
            if img[:2] == b"MZ":
                info["key"] = "retail" if key == RETAIL_KEY else ("devkit" if key else "none")
                break
            last = "decrypted image doesn't start with MZ"
        except ValueError as e:
            last = str(e)
    else:
        sys.exit(f"error: could not decode the image ({last})")
    img = img[:image_size] if len(img) >= image_size else img + b"\0" * (image_size - len(img))
    return img, info


def imports(d, img, load_addr, export_dirs):
    """XEX import records -> (address, 'func'|'var', library, ordinal, name)."""
    import glob
    import re
    names = {}
    for root in export_dirs:
        for inc in glob.glob(os.path.join(root, "**", "export_table.inc"), recursive=True):
            with open(inc, encoding="utf-8", errors="replace") as f:
                for m in re.finditer(r"XE_EXPORT\((\w+),\s*0x([0-9A-Fa-f]+),\s*(\w+)", f.read()):
                    names[(m.group(1).lower(), int(m.group(2), 16))] = m.group(3)
    _, _, hdrs = parse(d)
    o = hdrs.get(0x000103FF)
    if o is None:
        return []
    size, strsz, nlib = struct.unpack(">III", d[o:o + 12])
    strs = [s.decode("latin-1") for s in d[o + 12:o + 12 + strsz].split(b"\0") if s]
    p = o + 12 + strsz
    out = []
    for _ in range(nlib):
        lsize, = struct.unpack(">I", d[p:p + 4])
        name_idx, count = struct.unpack(">HH", d[p + 36:p + 40])
        lib = strs[name_idx] if name_idx < len(strs) else f"lib{name_idx}"
        key = lib.split(".")[0].lower()
        for k in range(count):
            rec, = struct.unpack(">I", d[p + 40 + k * 4:p + 44 + k * 4])
            off = rec - load_addr
            if not 0 <= off < len(img) - 4:
                continue
            val, = struct.unpack(">I", img[off:off + 4])
            typ, ordinal = val >> 24, val & 0xFFFF
            out.append((rec, "func" if typ else "var", lib, ordinal, names.get((key, ordinal), f"{key}_ord_{ordinal}")))
        p += lsize
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xex")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--imports", help="write the import thunks/variables to this TSV")
    ap.add_argument("--exports", action="append", default=[],
                    help="folder with export_table.inc files (ReXGlue SDK src/kernel), repeatable")
    a = ap.parse_args()
    with open(a.xex, "rb") as f:
        d = f.read()
    img, info = convert(d)
    print(" ".join(f"{k}={v:#x}" if isinstance(v, int) and k != "compression" and k != "encryption" else f"{k}={v}"
                   for k, v in info.items()))
    if a.imports:
        rows = imports(d, img, info["load_addr"], a.exports)
        with open(a.imports, "w", encoding="utf-8") as f:
            f.write("addr\tkind\tlibrary\tordinal\tname\n")
            for rec, kind, lib, ordinal, name in rows:
                f.write(f"{rec:08X}\t{kind}\t{lib}\t{ordinal}\t{name}\n")
        print(f"wrote {a.imports} ({len(rows)} imports)")
    if a.out:
        with open(a.out, "wb") as f:
            f.write(img)
        print(f"wrote {a.out} ({len(img)} bytes, load at {info['load_addr']:#x})")


if __name__ == "__main__":
    main()
