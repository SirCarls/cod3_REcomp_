#!/usr/bin/env python3
"""
xexp_apply.py - rebuild the program image a title update describes, so it can
be compared with the disc program (which functions an update changed).

Reads the disc program (default.xex) and the update's delta patch
(default.xexp), applies the patch and writes the patched PE image. It checks
the work three ways: the patch names the disc program it was made for (a
SHA-1 of its signature), every patch block carries a SHA-1, and the patched
headers carry SHA-1s of the finished image's pages.

Usage:
    python3 tools/waw_decomp/xexp_apply.py default.xex default.xexp out.pe [--headers out.hdr]

Needs the xexlzx helper with xex_lzx_decompress_ref (see xexlzx/xexlzx.c).

Written for this project from the XEX2 delta-patch layout as Xenia documents it
(src/xenia/cpu/xex_module.cc ApplyPatch, src/xenia/cpu/lzx.cc). Analysis only:
the output is game code, so keep it private. The ports always build from the
disc program; what an update changed is redone as our own code.
"""
import argparse
import ctypes
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xex2pe  # noqa: E402  (our own XEX reader, next to this file)

HDR_DELTA_PATCH = 0x000005FF
SECURITY_PAGE_DESCRIPTORS = 0x180


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def load_helper():
    here = os.path.dirname(os.path.abspath(__file__))
    names = [os.environ.get("XEXLZX", "")] + [os.path.join(here, n) for n in ("libxexlzx.so", "xexlzx.dll", "libxexlzx.dylib")]
    for name in names:
        if name and os.path.isfile(name):
            lib = ctypes.CDLL(name)
            if not hasattr(lib, "xex_lzx_decompress_ref"):
                sys.exit(f"error: {name} is an older xexlzx without xex_lzx_decompress_ref; rebuild it")
            lib.xex_lzx_decompress_ref.restype = ctypes.c_int
            return lib
    sys.exit("error: build xexlzx (see xexlzx/xexlzx.c) and set XEXLZX")


def lzx_with_reference(lib, data, out_len, window_bits, reference):
    out = ctypes.create_string_buffer(out_len)
    src = ctypes.create_string_buffer(bytes(data), len(data))
    ref = ctypes.create_string_buffer(bytes(reference), len(reference))
    err = lib.xex_lzx_decompress_ref(src, ctypes.c_size_t(len(data)), out, ctypes.c_size_t(out_len),
                                     ctypes.c_int(window_bits), ref, ctypes.c_size_t(len(reference)))
    if err != 0:
        raise ValueError(f"LZX delta failed (code {err})")
    return out.raw


def apply_records(lib, records, dest, window_bits, address_base, counts):
    """Applies a run of delta records to dest (a bytearray) in place.

    Each record: old address, new address (u32 each), new length, data length
    (u16 each), then the data. Data length 0 = fill with zeros, 1 = copy from
    the old address, otherwise LZX data that rebuilds the new bytes using the
    old bytes as reference. Four zero fields end the run."""
    pos = 0
    while pos + 12 <= len(records):
        old, new, new_len, data_len = struct.unpack_from(">IIHH", records, pos)
        if old == 0 and new == 0 and new_len == 0 and data_len == 0:
            break
        old -= address_base
        new -= address_base
        if new < 0 or old < 0 or new + new_len > len(dest) or old + new_len > len(dest):
            raise ValueError(f"record at {pos:#x} points outside the image (old {old:#x}, new {new:#x}, {new_len:#x} bytes)")
        if data_len == 0:
            dest[new:new + new_len] = bytes(new_len)
            counts["fill"] += 1
            pos += 12
        elif data_len == 1:
            dest[new:new + new_len] = dest[old:old + new_len]
            counts["copy"] += 1
            pos += 12
        else:
            payload = records[pos + 12:pos + 12 + data_len]
            dest[new:new + new_len] = lzx_with_reference(lib, payload, new_len, window_bits, dest[old:old + new_len])
            counts["delta"] += 1
            pos += 12 + data_len
        counts["bytes"] += new_len
    return pos


def page_check(image, header, sec):
    """Counts image pages whose SHA-1 matches the page list in the security info.

    Tries the plain hash and the chained hash (page data + the next
    descriptor) and both page sizes, and reports the best fit."""
    count = u32(header, sec + SECURITY_PAGE_DESCRIPTORS)
    flags = u32(header, sec + 0x10C)
    descs = [header[sec + 0x184 + i * 0x18: sec + 0x184 + (i + 1) * 0x18] for i in range(count)]
    best = None
    for page_size in ((0x1000, 0x10000) if flags & 0x10000000 else (0x10000, 0x1000)):
        for chained in (False, True):
            for count_shift in (4, 0):
                ok = pos = 0
                for i, desc in enumerate(descs):
                    pages = (u32(desc, 0) >> count_shift) if count_shift else (u32(desc, 0) & 0x0FFFFFFF)
                    size = pages * page_size
                    data = image[pos:pos + size]
                    if chained and i + 1 < count:
                        data += descs[i + 1]
                    if hashlib.sha1(data).digest() == desc[4:24]:
                        ok += 1
                    pos += size
                result = (ok, count, page_size, chained, count_shift)
                if best is None or ok > best[0]:
                    best = result
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("patch")
    ap.add_argument("out")
    ap.add_argument("--headers", help="also write the patched XEX headers here")
    args = ap.parse_args()

    base = open(args.base, "rb").read()
    patch = open(args.patch, "rb").read()
    base_image, base_info = xex2pe.convert(base)
    b_pe, b_sec, b_hdrs = xex2pe.parse(base)
    p_pe, p_sec, p_hdrs = xex2pe.parse(patch)
    if HDR_DELTA_PATCH not in p_hdrs:
        sys.exit("error: the patch file has no delta-patch descriptor")
    d = p_hdrs[HDR_DELTA_PATCH]
    desc = dict(zip(
        ("size", "target_version", "source_version"), struct.unpack_from(">III", patch, d)))
    desc["digest_source"] = patch[d + 0x0C:d + 0x20]
    desc["image_key_source"] = patch[d + 0x20:d + 0x30]
    (desc["size_of_target_headers"], desc["headers_source_offset"], desc["headers_source_size"],
     desc["headers_target_offset"], desc["image_source_offset"], desc["image_source_size"],
     desc["image_target_offset"]) = struct.unpack_from(">7I", patch, d + 0x30)
    print(f"base: image {len(base_image):#x} bytes at {base_info['load_addr']:#x}, key {base_info.get('key')}")
    print("patch descriptor: " + ", ".join(
        f"{k} {v:#x}" for k, v in desc.items() if isinstance(v, int)))

    base_sig = base[b_sec + 8:b_sec + 8 + 0x100]
    match = hashlib.sha1(base_sig).digest() == desc["digest_source"]
    print(f"made for this disc program: {'yes' if match else 'NO (the patch expects another version)'}")

    lib = load_helper()
    ffi = p_hdrs[xex2pe.HDR_FILE_FORMAT]
    info_size, encryption, compression = struct.unpack_from(">IHH", patch, ffi)
    if compression not in (2, 3):  # 2 = normal (LZX), 3 = delta; both use hashed LZX blocks
        sys.exit(f"error: patch compression {compression}; only the LZX kinds are handled")
    window_size, block_size = struct.unpack_from(">II", patch, ffi + 8)
    block_hash = patch[ffi + 16:ffi + 36]
    window_bits = window_size.bit_length() - 1

    # Headers: start from the disc program's headers and apply the
    # descriptor's own records.
    src = base[desc["headers_source_offset"]:desc["headers_source_offset"] + desc["headers_source_size"]]
    room = max(desc["size_of_target_headers"], b_pe, desc["headers_target_offset"] + len(src))
    headers = bytearray(room + 0x10000)  # spare room while records grow the headers
    headers[desc["headers_target_offset"]:desc["headers_target_offset"] + len(src)] = src
    counts = {"fill": 0, "copy": 0, "delta": 0, "bytes": 0}
    apply_records(lib, patch[d + 0x4C:d + desc["size"]], headers, window_bits, 0, counts)
    headers = headers[:max(desc["size_of_target_headers"], 0x200)]
    print(f"headers patched: {counts}")
    new_sec = u32(headers, 0x10)
    new_size = u32(headers, new_sec + 4)
    new_load = u32(headers, new_sec + 0x110)
    print(f"patched program: image {new_size:#x} bytes at {new_load:#x}")

    # Keys, as Xenia does it: the program key comes from the file key in the
    # patched headers, opened with the console key; the patch file's own file
    # key opens with that program key. Other combinations are tried as well,
    # and the first block's SHA-1 picks the right one.
    console_key = xex2pe.RETAIL_KEY if base_info.get("key") == "retail" else xex2pe.DEVKIT_KEY
    base_key = xex2pe.aes_dec("ecb", console_key, base[b_sec + 0x150:b_sec + 0x160])
    patched_key = xex2pe.aes_dec("ecb", console_key, headers[new_sec + 0x150:new_sec + 0x160])
    patch_file_key = patch[p_sec + 0x150:p_sec + 0x160]
    candidates = [("patch file key, opened with the patched program key",
                   xex2pe.aes_dec("ecb", patched_key, patch_file_key))]
    for opener, key in (("disc program key", base_key), ("console key", console_key)):
        candidates.append((f"patch file key, opened with the {opener}", xex2pe.aes_dec("ecb", key, patch_file_key)))
    candidates.append(("none", None))
    raw_body = patch[p_pe:]
    body = None
    for label, patch_key in candidates:
        if patch_key is None:
            if encryption == 1:
                continue
            candidate = raw_body
        else:
            if encryption != 1:
                continue
            candidate = xex2pe.aes_dec("cbc", patch_key, raw_body[:len(raw_body) // 16 * 16])
        if hashlib.sha1(candidate[:block_size]).digest() == block_hash:
            body = candidate
            print(f"patch key: {label}")
            if patch_key is not None:
                opened = xex2pe.aes_dec("ecb", patch_key, desc["image_key_source"])
                print(f"image key source matches the disc program key: {'yes' if opened == base_key else 'no'}")
            break
    if body is None:
        sys.exit("error: no key opens the patch's first block")

    image = bytearray(max(new_size, len(base_image)))
    image[:len(base_image)] = base_image
    # Some updates first move a run of the old image (source -> target), then
    # clear what lies past the moved run up to the old image's end, as Xenia does.
    if desc["image_source_offset"]:
        src, size, dst = desc["image_source_offset"], desc["image_source_size"], desc["image_target_offset"]
        image[dst:dst + size] = image[src:src + size]
        end = dst + size
        if end < len(base_image):
            image[end:len(base_image)] = bytes(len(base_image) - end)
        print(f"moved {size:#x} bytes of the old image from {src:#x} to {dst:#x} first")
    counts = {"fill": 0, "copy": 0, "delta": 0, "bytes": 0}
    pos = blocks = 0
    address_base = None
    while block_size:
        block = body[pos:pos + block_size]
        if len(block) < block_size or hashlib.sha1(block).digest() != block_hash:
            sys.exit(f"error: patch block {blocks} is short or fails its SHA-1 (wrong key?)")
        next_size, next_hash = u32(block, 0), block[4:24]
        records = block[24:]
        if address_base is None:
            first_new = u32(records, 4)
            address_base = new_load if first_new >= new_load else 0
            print(f"record addresses are {'absolute' if address_base else 'image offsets'}")
        apply_records(lib, records, image, window_bits, address_base, counts)
        pos += block_size
        blocks += 1
        block_size, block_hash = next_size, next_hash
    image = bytes(image[:new_size])
    print(f"image patched: {blocks} blocks, {counts}")
    pe = struct.unpack_from("<I", image, 0x3C)[0] if image[:2] == b"MZ" else 0
    if not pe or image[pe:pe + 4] != b"PE\0\0":
        sys.exit("error: the patched image has no valid PE header")

    ok, total, page_size, chained, shift = page_check(base_image, base, b_sec)
    print(f"page hashes, disc program: {ok}/{total} (page {page_size:#x}, chained {chained}, shift {shift})")
    ok, total, page_size, chained, shift = page_check(image, headers, new_sec)
    print(f"page hashes, patched program: {ok}/{total} (page {page_size:#x}, chained {chained}, shift {shift})")

    with open(args.out, "wb") as f:
        f.write(image)
    if args.headers:
        with open(args.headers, "wb") as f:
            f.write(headers)
    changed = sum(1 for i in range(0, min(len(image), len(base_image)), 4) if image[i:i + 4] != base_image[i:i + 4])
    print(f"wrote {args.out}: {len(image):#x} bytes, {changed} words differ from the disc program")


if __name__ == "__main__":
    main()
