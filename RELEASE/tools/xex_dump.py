#!/usr/bin/env python3
"""Decrypt + decompress (basic compression) an XEX2 into a flat PE image for analysis."""
import struct, subprocess, sys
RETAIL = bytes([0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91])
def aes(mode, key, data):
    return subprocess.run(['openssl','enc','-d',f'-aes-128-{mode}','-nopad','-K',key.hex(),'-iv','0'*32] if mode=='cbc' else
                          ['openssl','enc','-d','-aes-128-ecb','-nopad','-K',key.hex()], input=data, capture_output=True, check=True).stdout
src, dst = sys.argv[1], sys.argv[2]
d = open(src,'rb').read()
pe_off, sec = struct.unpack('>I', d[8:12])[0], struct.unpack('>I', d[16:20])[0]
n = struct.unpack('>I', d[20:24])[0]
hdr = dict(struct.unpack('>II', d[24+i*8:32+i*8]) for i in range(n))
ffi = hdr[0x3ff]
info_size, enc, comp = struct.unpack('>IHH', d[ffi:ffi+8])
image_size, = struct.unpack('>I', d[sec+4:sec+8])
load_addr, = struct.unpack('>I', d[sec+0x110:sec+0x114])
key = aes('ecb', RETAIL, d[sec+0x150:sec+0x160])
payload = d[pe_off:]
payload = payload[:len(payload)//16*16]
dec = aes('cbc', key, payload) if enc == 1 else payload
assert comp == 1, comp
out = bytearray(); p = 0
for i in range((info_size-8)//8):
    ds, zs = struct.unpack('>II', d[ffi+8+i*8:ffi+16+i*8])
    out += dec[p:p+ds]; p += ds; out += b'\0'*zs
assert out[:2] == b'MZ', out[:4]
out = out[:image_size] if len(out) >= image_size else out + b'\0'*(image_size-len(out))
open(dst,'wb').write(out)
print(f'load 0x{load_addr:08X} size 0x{image_size:X} -> {dst}')
