#!/usr/bin/env python3
# [acct2-bro] Big Red One's NGL mesh files (XBXM version 0x601): for each mesh section, which draw path it uses.
# Read-only. Finds every XBXM file inside the given archives (Big Red One's KAPF chunks are stored uncompressed),
# and reads the layout nglParseMesh / nglRelocateMesh / nglRelocateMeshSection / nglDrawMeshSection use:
#   header: 'XBXM', u32 0x601, u32 entry count, u32 entry table, u32 base (0 on disc: pointers are file offsets)
#   entry (12 bytes): byte 3 = kind (1 material, 2 mesh, 3 textures), u32 +4 object, u32 +8
#   mesh: u32 +8 section count, u32 +0xC table of (u32, u32 section) pairs
#   section: +0x28 primitive type, +0x2C index count, +0x30 index list, +0x34/+0x38 prebuilt command list,
#            +0x40 vertex count, +0x50 vertex stride in bytes (nglSetVertexStreams hands it to Direct3D)
# Usage: xbxm_sections.py OUT_TSV ARCHIVE_OR_FOLDER...
import collections, mmap, os, struct, sys

MAGIC = b'XBXM\x01\x06\x00\x00'


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0] if 0 <= o <= len(b) - 4 else None


def scan(path, rows, stats):
    with open(path, 'rb') as f:
        try:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:
            return
        pos = m.find(MAGIC)
        while pos >= 0:
            stats['files'] += 1
            parse(m, pos, path, rows, stats)
            pos = m.find(MAGIC, pos + 8)
        m.close()


def parse(m, at, path, rows, stats):
    count, table, base = u32(m, at + 8), u32(m, at + 0xC), u32(m, at + 0x10)
    if base != 0 or count is None or not 0 < count < 100000:
        stats['skipped (base not 0 or bad count)'] += 1
        return
    for i in range(count):
        e = at + table + i * 12
        kind = m[e + 3] if e + 4 <= len(m) else None
        stats[f'entry kind {kind}'] += 1
        if kind != 2:
            continue
        mesh = at + (u32(m, e + 4) or 0)
        nsec, stab = u32(m, mesh + 8), u32(m, mesh + 0xC)
        if nsec is None or not 0 < nsec < 4096 or stab is None:
            stats['bad mesh'] += 1
            continue
        for s in range(nsec):
            sp = u32(m, at + stab + s * 8 + 4)
            if not sp:
                stats['empty section slot'] += 1
                continue
            sec = at + sp
            prim, nidx, idx = u32(m, sec + 0x28), u32(m, sec + 0x2C), u32(m, sec + 0x30)
            pb_res, pb_data, pb_size = u32(m, sec + 0x34), u32(m, sec + 0x38), u32(m, sec + 0x3C)
            nvert = u32(m, sec + 0x40)
            stride = u32(m, sec + 0x50)
            stats[f'vertex stride {stride}'] += 1
            if prim is None:
                stats['section out of file'] += 1
                continue
            if pb_data:
                path_kind = 'command list + index list' if (nidx and idx) else 'command list only'
            elif nidx:
                path_kind = 'index list'
            else:
                path_kind = 'vertices only'
            stats['section: ' + path_kind] += 1
            stats[f'primitive type {prim}'] += 1
            rows.append((os.path.basename(path), f'{at:X}', i, s, prim, nidx, f'{idx:X}' if idx else '0',
                         f'{pb_res:X}' if pb_res else '0', f'{pb_data:X}' if pb_data else '0', pb_size, nvert, stride, path_kind))


def main(out, inputs):
    rows, stats = [], collections.Counter()
    for p in inputs:
        paths = [p] if os.path.isfile(p) else sorted(os.path.join(d, n) for d, _, ns in os.walk(p) for n in ns
                                                     if n.lower().endswith('.cod'))
        for q in paths:
            stats['archives'] += 1
            scan(q, rows, stats)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('archive\txbxm_at\tentry\tsection\tprimitive\tindex_count\tindex_list\tcmd_res\tcmd_data\tcmd_size\tvertex_count\tstride\tpath\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')
    for k, v in sorted(stats.items()):
        print(f'{k:42} {v}')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit('usage: xbxm_sections.py OUT_TSV ARCHIVE_OR_FOLDER...')
    main(sys.argv[1], sys.argv[2:])
