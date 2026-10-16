#!/usr/bin/env python3
"""
t4sp_types.py - turn the World at War singleplayer type headers of the T4SP
server plugin (github.com/JezuzLizard/T4SP-Server-Plugin, AGPL-3.0; structures
with checked offsets for this exact build) into one plain C header that
Ghidra's C parser reads, plus a table of typed globals and function prototypes.

The result types the PRIVATE decompile only (dev/waw-decomp/): nothing from
those headers goes into a published file.

Usage:
    python3 tools/waw_decomp/t4sp_types.py <T4SP-Server-Plugin checkout> <out dir>
Writes <out dir>/t4sp_types.h and <out dir>/t4sp_symbols.tsv (addr, kind, name, type).
"""
import os
import re
import sys

PRELUDE = """
/* Windows and CRT types the headers use */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HINSTANCE;
typedef void *HMODULE;
typedef void *HDC;
typedef void *HKEY;
typedef long LONG;
typedef unsigned int UINT;
typedef unsigned int size_t;
typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef struct _RTL_CRITICAL_SECTION { char opaque[0x18]; } CRITICAL_SECTION;
typedef struct _jmp_buf { int regs[16]; } jmp_buf;
typedef struct _iobuf { char *_ptr; int _cnt; char *_base; int _flag; int _file; int _charbuf; int _bufsiz; char *_tmpfname; } FILE;
typedef struct _GUID { DWORD Data1; WORD Data2; WORD Data3; BYTE Data4[8]; } GUID;
typedef struct _OVERLAPPED { DWORD a, b, c, d; HANDLE hEvent; } OVERLAPPED;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT;
typedef struct tagPOINT { LONG x, y; } POINT;
typedef struct _D3DMATRIX { float m[4][4]; } D3DMATRIX;
typedef int D3DFORMAT;
typedef int D3DRESOURCETYPE;
typedef int D3DPRIMITIVETYPE;
typedef int D3DTEXTUREFILTERTYPE;
typedef int D3DMULTISAMPLE_TYPE;
typedef int D3DDEVTYPE;
typedef int D3DQUERYTYPE;
typedef int D3DCUBEMAP_FACES;
typedef int HRESULT;
typedef short SHORT;
typedef unsigned short USHORT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef int INT;
typedef float FLOAT;
typedef void *LPVOID;
typedef const char *LPCSTR;
typedef char *LPSTR;
typedef unsigned short WCHAR;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef union _LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; } u; long long QuadPart; } LARGE_INTEGER;
typedef unsigned int SOCKET;
typedef unsigned int WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef unsigned char boolean;
typedef struct HINSTANCE__ { int unused; } HINSTANCE__;
typedef struct HWND__ { int unused; } HWND__;
typedef struct HDC__ { int unused; } HDC__;
typedef struct HICON__ { int unused; } HICON__;
typedef struct HKEY__ { int unused; } HKEY__;
/* IDA-style names used in the headers */
typedef unsigned char _BYTE;
typedef unsigned short _WORD;
typedef unsigned int _DWORD;
typedef unsigned long long _QWORD;
typedef int _BOOL4;
typedef char _BOOL1;
typedef unsigned char _UNKNOWN;
"""

D3D_IFACES = ["IDirect3D9", "IDirect3DDevice9", "IDirect3DTexture9", "IDirect3DVolumeTexture9",
              "IDirect3DCubeTexture9", "IDirect3DBaseTexture9", "IDirect3DSurface9", "IDirect3DVertexBuffer9",
              "IDirect3DIndexBuffer9", "IDirect3DVertexShader9", "IDirect3DPixelShader9",
              "IDirect3DVertexDeclaration9", "IDirect3DQuery9", "IDirect3DStateBlock9", "IDirect3DResource9",
              "IDirect3DSwapChain9", "IDirectSound", "IDirectSoundBuffer", "IUnknown"]


def clean(text):
    text = text.lstrip("﻿")
    # keep only the C side of "#ifdef __cplusplus ... #else ... #endif"
    out, stack = [], []
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#ifdef __cplusplus") or s.startswith("#if defined(__cplusplus)"):
            stack.append("cpp")
            continue
        if s.startswith("#ifndef __cplusplus"):
            stack.append("c")
            continue
        if s.startswith(("#ifdef", "#ifndef", "#if ")):
            stack.append("other")
            continue
        if s.startswith("#else"):
            if stack and stack[-1] in ("cpp", "c"):
                stack[-1] = "c" if stack[-1] == "cpp" else "cpp"
            continue
        if s.startswith("#endif"):
            if stack:
                stack.pop()
            continue
        if "cpp" in stack:
            continue
        if s.startswith(("#pragma once", "#include")):
            continue
        if s.startswith("ASSERT_STRUCT") or s.startswith("static_assert"):
            continue
        out.append(line)
    text = "\n".join(out)
    # C++-only syntax
    text = re.sub(r"\benum\s+(\w+)\s*:\s*[\w ]+?\s*\{", r"enum \1 {", text)  # enum X : type {
    text = re.sub(r"\benum\s+class\s+", "enum ", text)
    text = re.sub(r"\balignas\s*\(\s*\d+\s*\)", "", text)
    text = re.sub(r"\bconstexpr\b", "const", text)
    text = text.replace("__declspec(align(16))", "").replace("__declspec(align(4))", "")
    text = re.sub(r"\bnullptr\b", "0", text)
    return text


def symbols(path):
    """WEAK symbol<TYPE> name{ mp, sp }; -> (sp addr, name, TYPE)."""
    rows = []
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        text = f.read()
    for m in re.finditer(r"WEAK\s+symbol\s*<(.*?)>\s*(\w+)\s*\{\s*(0x[0-9A-Fa-f]+|0)\s*,\s*(0x[0-9A-Fa-f]+|0)\s*\}", text, re.S):
        typ, name, _mp, sp = m.group(1), m.group(2), m.group(3), m.group(4)
        a = int(sp, 16) if sp.startswith("0x") else 0
        if a:
            rows.append((a, name, " ".join(typ.split())))
    return rows


def main():
    src, out = sys.argv[1], sys.argv[2]
    game = os.path.join(src, "src", "game")
    os.makedirs(out, exist_ok=True)
    parts = [PRELUDE]
    for iface in D3D_IFACES:
        parts.append(f"typedef struct {iface} {{ void *vtbl; }} {iface};")
    order = ["enums.hpp", "xasset.hpp", os.path.join("clientscript", "clientscript_public.hpp"), "structs.hpp"]
    for fn in order:
        with open(os.path.join(game, fn), encoding="utf-8-sig", errors="replace") as f:
            parts.append(f"/* ---- {fn} ---- */\n" + clean(f.read()))
    header = "\n".join(parts)
    # forward typedefs so 'Name' works without 'struct'/'union'/'enum' (C++ style)
    names = re.findall(r"^\s*(struct|union|enum)\s+(\w+)\s*(?:\{|$|;)", header, re.M)
    fwd = []
    seen = set()
    for kind, n in names:
        if n in seen:
            continue
        seen.add(n)
        fwd.append(f"typedef {kind} {n} {n};" if kind != "enum" else f"typedef int {n};")
    # enums become int typedefs only when declared before use; keep the real enum bodies too
    header = "/* generated by t4sp_types.py from T4SP-Server-Plugin headers (AGPL-3.0); private use */\n" + \
        "\n".join(f for f in fwd if not f.startswith("typedef int")) + "\n" + header
    with open(os.path.join(out, "t4sp_types.h"), "w", encoding="utf-8") as f:
        f.write(header)
    rows = []
    for root, _, files in os.walk(game):
        for fn in files:
            if fn.endswith(".hpp"):
                rows += symbols(os.path.join(root, fn))
    with open(os.path.join(out, "t4sp_symbols.tsv"), "w", encoding="utf-8") as f:
        f.write("addr\tkind\tname\ttype\n")
        for a, n, typ in sorted(set(rows)):
            kind = "function" if re.search(r"\)\s*$", typ) and "(" in typ else "global"
            f.write(f"{a:08X}\t{kind}\t{n}\t{typ}\n")
    print(f"wrote t4sp_types.h ({len(header)} bytes, {len(seen)} types) and t4sp_symbols.tsv ({len(rows)} symbols)")


if __name__ == "__main__":
    main()
