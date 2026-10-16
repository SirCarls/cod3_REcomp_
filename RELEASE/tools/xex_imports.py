#!/usr/bin/env python3
"""Lists the XEX2 import thunks (address -> library!name), naming them from
the ReXGlue SDK's kernel export tables.

    python3 xex_imports.py <default.xex> <decrypted image> <rexglue-sdk folder>

The image is the decrypted PE from xex_dump.py; the SDK folder is a checkout
of github.com/rexglue/rexglue-sdk (its src/kernel/*/export_table.inc files).
"""
import glob
import re
import struct
import sys
from pathlib import Path

if len(sys.argv) != 4:
    sys.exit(__doc__)
xex, pe, sdk = sys.argv[1], sys.argv[2], Path(sys.argv[3])
d = open(xex, 'rb').read()
img = open(pe, 'rb').read()
names = {}
for inc in glob.glob(str(sdk / 'src' / 'kernel' / '*' / 'export_table.inc')):
    for m in re.finditer(r'XE_EXPORT\((\w+),\s*0x([0-9A-Fa-f]+),\s*(\w+)', open(inc).read()):
        names[(m.group(1).lower(), int(m.group(2), 16))] = m.group(3)
if not names:
    sys.exit(f"No export tables found under {sdk / 'src' / 'kernel'}")
n = struct.unpack('>I', d[20:24])[0]
hdr = dict(struct.unpack('>II', d[24 + i * 8:32 + i * 8]) for i in range(n))
o = hdr[0x103ff]   # XEX2 import libraries header key
size, strsz, nlib = struct.unpack('>III', d[o:o + 12])
strs = d[o + 12:o + 12 + strsz].split(b'\0')
strs = [s.decode() for s in strs if s]
p = o + 12 + strsz
for li in range(nlib):
    lsize, = struct.unpack('>I', d[p:p + 4])
    name_idx, count = struct.unpack('>HH', d[p + 36:p + 40])
    lib = strs[name_idx]
    key = lib.split('.')[0].lower()
    for k in range(count):
        rec, = struct.unpack('>I', d[p + 40 + k * 4:p + 44 + k * 4])
        # The image is loaded at 0x82000000 (default.xex) or 0x89000000 (level DLLs).
        val, = struct.unpack('>I', img[rec - (0x89000000 if rec >= 0x89000000 else 0x82000000):][:4])
        typ, ordinal = val >> 24, val & 0xFFFF
        print(f"{rec:08X} {'func' if typ else 'var '} {lib}!{names.get((key, ordinal), f'ord_{ordinal}')}")
    p += lsize
