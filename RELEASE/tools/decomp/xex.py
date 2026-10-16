"""Reads an Xbox 360 XEX2 file: its decrypted image and its import thunks.

The XEX2 layout (header fields, the 0x3FF file format info, the 0x103FF
import libraries, the retail key) is the one `tools/xex_dump.py` and
`tools/xex_imports.py` already read; this module does the same as library
functions for the decomp tools. Only "basic" compression (blocks of data
followed by zeros) is handled, which is what Call of Duty 3's executables
and level DLLs use.
"""
from __future__ import annotations

import shutil
import struct
import subprocess

import aes

# The retail XEX2 key: decrypts the per-file image key stored in the security info.
RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])

HEADER_FILE_FORMAT_INFO = 0x3FF
HEADER_IMPORT_LIBRARIES = 0x103FF
SECURITY_IMAGE_SIZE = 0x004
SECURITY_LOAD_ADDRESS = 0x110
SECURITY_IMAGE_KEY = 0x150
ENCRYPTION_NORMAL = 1
COMPRESSION_BASIC = 1


class XexError(Exception):
    pass


def _headers(data: bytes) -> dict[int, int]:
    if data[:4] != b"XEX2":
        raise XexError("not an XEX2 file")
    count = struct.unpack(">I", data[20:24])[0]
    return dict(struct.unpack(">II", data[24 + i * 8:32 + i * 8]) for i in range(count))


def _aes(mode: str, key: bytes, data: bytes) -> bytes:
    """AES-128 decryption, through openssl when it's installed (fast), else in Python."""
    if shutil.which("openssl"):
        cmd = ["openssl", "enc", "-d", f"-aes-128-{mode}", "-nopad", "-K", key.hex()]
        if mode == "cbc":
            cmd += ["-iv", "0" * 32]
        try:
            return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            pass
    return aes.decrypt_cbc(key, data) if mode == "cbc" else aes.decrypt_ecb(key, data)


def decrypt(data: bytes) -> tuple[bytes, int]:
    """The XEX2's image as it is laid out in memory, and its load address."""
    pe_offset, security = struct.unpack(">I", data[8:12])[0], struct.unpack(">I", data[16:20])[0]
    info = _headers(data)[HEADER_FILE_FORMAT_INFO]
    info_size, encryption, compression = struct.unpack(">IHH", data[info:info + 8])
    image_size = struct.unpack(">I", data[security + SECURITY_IMAGE_SIZE:][:4])[0]
    load_address = struct.unpack(">I", data[security + SECURITY_LOAD_ADDRESS:][:4])[0]
    if compression != COMPRESSION_BASIC:
        raise XexError(f"compression type {compression} is not supported (only basic)")
    payload = data[pe_offset:]
    payload = payload[:len(payload) // 16 * 16]
    if encryption == ENCRYPTION_NORMAL:
        key = _aes("ecb", RETAIL_KEY, data[security + SECURITY_IMAGE_KEY:][:16])
        payload = _aes("cbc", key, payload)
    image = bytearray()
    pos = 0
    for i in range((info_size - 8) // 8):
        data_size, zero_size = struct.unpack(">II", data[info + 8 + i * 8:info + 16 + i * 8])
        image += payload[pos:pos + data_size]
        image += bytes(zero_size)
        pos += data_size
    if image[:2] != b"MZ":
        raise XexError("the decrypted image doesn't start with MZ (wrong key or a damaged file)")
    image = image[:image_size] + bytes(max(0, image_size - len(image)))
    return bytes(image), load_address


def import_thunks(data: bytes, image: bytes, base: int) -> dict[int, tuple[str, int, bool]]:
    """Import records: address -> (library, ordinal, is_function).

    A function import's record is the first word of its 16-byte thunk, which
    the console's loader overwrites; in the file it holds (type << 24 | ordinal).
    """
    headers = _headers(data)
    if HEADER_IMPORT_LIBRARIES not in headers:
        return {}
    o = headers[HEADER_IMPORT_LIBRARIES]
    _size, strings_size, library_count = struct.unpack(">III", data[o:o + 12])
    names = [s.decode("ascii", "replace") for s in data[o + 12:o + 12 + strings_size].split(b"\0") if s]
    p = o + 12 + strings_size
    out = {}
    for _ in range(library_count):
        lib_size = struct.unpack(">I", data[p:p + 4])[0]
        name_index, count = struct.unpack(">HH", data[p + 36:p + 40])
        library = names[name_index] if name_index < len(names) else f"lib{name_index}"
        for k in range(count):
            record = struct.unpack(">I", data[p + 40 + k * 4:p + 44 + k * 4])[0]
            value = struct.unpack(">I", image[record - base:record - base + 4])[0]
            out[record] = (library, value & 0xFFFF, bool(value >> 24))
        p += lib_size
    return out
