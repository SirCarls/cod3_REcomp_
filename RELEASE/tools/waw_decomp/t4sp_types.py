#!/usr/bin/env python3
"""
t4sp_types.py - turn the World at War singleplayer type headers of the T4SP
server plugin (github.com/JezuzLizard/T4SP-Server-Plugin, AGPL-3.0; structures
with checked offsets for this exact build) into one plain C header that
Ghidra's C parser reads, plus a table of typed globals and function prototypes.

The result types the PRIVATE decompile only (dev/waw-decomp/): nothing from
those headers goes into a published file.

Usage:
    python3 tools/waw_decomp/t4sp_types.py <T4SP-Server-Plugin checkout> <out dir> [--check]
Writes <out dir>/t4sp_types.h and <out dir>/t4sp_symbols.tsv (addr, kind, name, type).

--check compiles the header with clang (when installed) under MSVC's 32-bit layout rules and checks
every "//OFS: SIZE:" comment and every ASSERT_STRUCT_SIZE of the plugin's headers against it, once
as written and once with __declspec(align(N)) removed, which is how Ghidra's C parser reads it.
Layout rules that broke offsets before: an "enum X : short" member is 2 bytes, not an enum's 4;
a structure must be defined before it is used by value (structs.hpp includes the asset and script
headers part way through); __declspec(align(N)) pads a structure's size.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

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
typedef struct _OVERLAPPED { DWORD a, b, c, d; HANDLE hEvent; } OVERLAPPED, _OVERLAPPED;
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
        m = re.match(r'#include\s+"([^"]+)"', s)
        if m and m.group(1) in INLINE:
            out.append(f"/*@inline {m.group(1)}@*/")  # load() copies the header in here
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
    # enum X : type { ... }; -> enum X_values { ... }; typedef type X; so members of type X keep the
    # enum's size (Ghidra's and C's enums are 4 bytes: a 2-byte DvarFlags would shift dvar_s by 2)
    text = re.sub(r"\benum\s+(\w+)\s*:\s*([\w ]+?)\s*\{(.*?)\}\s*;",
                  lambda m: f"enum {m.group(1)}_values {{{m.group(3)}}};\ntypedef {SIZED.get(' '.join(m.group(2).split()), m.group(2).strip())} {m.group(1)};",
                  text, flags=re.S)
    text = re.sub(r"\benum\s+class\s+", "enum ", text)
    text = re.sub(r"\balignas\s*\(\s*\d+\s*\)", "", text)
    text = re.sub(r"\bconstexpr\b", "const", text)
    text = re.sub(r"\bnullptr\b", "0", text)
    return text


SIZED = {"__int8": "char", "unsigned __int8": "unsigned char", "__int16": "short", "unsigned __int16": "unsigned short",
         "__int32": "int", "unsigned __int32": "unsigned int", "int8_t": "char", "uint8_t": "unsigned char",
         "int16_t": "short", "uint16_t": "unsigned short", "int32_t": "int", "uint32_t": "unsigned int"}


INLINE = ("xasset.hpp", "clientscript/clientscript_public.hpp")


def unalias(enum, rest):
    """An enum with two names for one value (CONTENTS_FAKE_ACTOR = CONTENTS_ACTOR = 0x8000) makes
    Ghidra put a warning above every function that uses it; keep the first name of each value when
    every member is "NAME = number" and the dropped names are not used anywhere else."""
    m = re.search(r"\{(.*)\}", enum, re.S)
    items = [x.strip() for x in m.group(1).split(",") if x.strip()]
    parsed = [re.fullmatch(r"(\w+)\s*=\s*(-?(?:0x[0-9A-Fa-f]+|\d+))", x) for x in items]
    if not items or not all(parsed):
        return enum
    seen, keep = set(), []
    for x, p in zip(items, parsed):
        v = int(p.group(2), 0)
        if v in seen and not re.search(rf"\b{p.group(1)}\b", rest):
            continue
        seen.add(v)
        keep.append(x)
    if len(keep) == len(items):
        return enum
    return enum[:m.start(1)] + "\n\t\t" + ",\n\t\t".join(keep) + ",\n\t" + enum[m.end(1):]


SIZES = {}  # structure name -> size from the headers' ASSERT_STRUCT_SIZE lines


def load(game, fn):
    """A header, cleaned, with the INLINE headers it includes copied in where they are included."""
    with open(os.path.join(game, *fn.split("/")), encoding="utf-8-sig", errors="replace") as f:
        text = f.read()
    for name, size in re.findall(r"ASSERT_STRUCT_SIZE\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)", text):
        SIZES[name] = int(size, 0)
    text = clean(text)
    return re.sub(r"^/\*@inline (\S+)@\*/$", lambda m: f"/* ---- {m.group(1)} ---- */\n" + load(game, m.group(1)),
                  text, flags=re.M)


def pad_aligned(text):
    """Ghidra's C parser ignores __declspec(align(N)), so a structure that MSVC pads out to N bytes
    comes out short (BrushWrapper 0x58 instead of 0x60). Give such a structure an explicit tail
    member up to its asserted size; offsets inside are unchanged."""
    def fix(m):
        kind, name, body = m.group(1), m.group(2), m.group(3)
        depth, end = 0, 0
        for line in body.split("\n"):
            f = re.search(r"//OFS: 0x([0-9A-Fa-f]+) SIZE: 0x([0-9A-Fa-f]+)", line) if depth == 0 else None
            if f:
                end = max(end, (0 if kind == "union" else int(f.group(1), 16)) + int(f.group(2), 16))
            depth += line.count("{") - line.count("}")
        size = SIZES.get(name, 0)
        if not end or size <= end:
            return m.group(0)
        at = 0 if kind == "union" else end
        pad = f"\t\tchar _align_pad[{size - at}]; //OFS: 0x{at:X} SIZE: 0x{size - at:X} (__declspec(align) tail)"
        return m.group(0)[:m.start(4) - m.start(0)] + "\n" + pad + m.group(0)[m.start(4) - m.start(0):]
    return re.sub(r"\n\s*(struct|union) __declspec\(align\(\d+\)\) (\w+)\s*\n\s*\{(.*?)(\n\s*\};)", fix, text, flags=re.S)


def symbols(path):
    """WEAK symbol<TYPE> name{ mp, sp }; -> (sp addr, name, TYPE)."""
    rows = []
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        text = f.read()
    for m in re.finditer(r"WEAK\s+symbol\s*<(.*?)>\s*(\w+)\s*\{\s*(0x[0-9A-Fa-f]+|0)\s*,\s*(0x[0-9A-Fa-f]+|0)\s*\}", text, re.S):
        typ, name, sp = m.group(1), m.group(2), m.group(4)  # group 3 is the MP address
        a = int(sp, 16) if sp.startswith("0x") else 0
        if a:
            rows.append((a, name, " ".join(typ.split())))
    return rows


def check(header):
    """Problems found by clang in the header's layout: (as written, as Ghidra reads it)."""
    clang = shutil.which("clang++")
    if not clang:
        print("check: clang++ not found, skipped")
        return None
    with open(header, encoding="utf-8") as f:
        text = f.read()
    tests = ["template<unsigned A, unsigned E> struct Same { static_assert(A == E, \"layout\"); };"]
    struct_re = re.compile(r"\n\s*(struct|union) (?:__declspec\(align\(\d+\)\) )?(\w+)\s*\n\s*\{(.*?)\n\s*\};", re.S)
    field_re = re.compile(r"^\s*(.*?)\b(\w+)\s*(\[[^;]*\])?\s*;\s*//OFS: 0x([0-9A-Fa-f]+) SIZE:")
    for m in struct_re.finditer(text):
        if m.group(1) == "union":
            continue
        depth = 0
        for line in m.group(3).split("\n"):
            f = field_re.match(line) if depth == 0 else None
            if f and ":" not in f.group(1):  # bit fields have no offsetof
                tests.append(f"template struct Same<__builtin_offsetof(struct {m.group(2)}, {f.group(2)}), 0x{f.group(4)}>;")
            depth += line.count("{") - line.count("}")
    tests += [f"template struct Same<sizeof({n}), {v}>;" for n, v in sorted(SIZES.items()) if n != "CRITICAL_SECTION"]
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in (("as_written.h", text), ("as_ghidra.h", re.sub(r"__declspec\(align\(\d+\)\) ", "", text))):
            with open(os.path.join(tmp, name), "w", encoding="utf-8") as f:
                f.write(body)
            cpp = os.path.join(tmp, name + ".cpp")
            with open(cpp, "w", encoding="utf-8") as f:
                f.write(f'#include "{name}"\n' + "\n".join(tests) + "\n")
            r = subprocess.run([clang, "--target=i686-pc-windows-msvc", "-fsyntax-only", "-fms-extensions",
                                "-fms-compatibility", "-Wno-everything", "-ferror-limit=0", "-nostdinc", cpp],
                               capture_output=True, text=True)
            results.append(r.stderr.count("error:"))
    print(f"check: {len(tests) - 1} offsets and sizes; clang errors as written {results[0]}, as Ghidra reads it {results[1]}")
    return results


def main():
    src, out = sys.argv[1], sys.argv[2]
    game = os.path.join(src, "src", "game")
    os.makedirs(out, exist_ok=True)
    parts = [PRELUDE]
    for iface in D3D_IFACES:
        parts.append(f"typedef struct {iface} {{ void *vtbl; }} {iface};")
    # structs.hpp includes the asset and script headers part way through (after the structures they
    # use by value); expand them in place so every structure is defined before it is used
    body = "\n".join(f"/* ---- {fn} ---- */\n" + load(game, fn) for fn in ("enums.hpp", "structs.hpp"))
    # every enum first (some are used before they are defined), each with a typedef so its bare name
    # works in C; a sized enum is already "enum X_values {...}; typedef <type> X;"
    enums = []

    def hoist(m):
        enums.append(m.group(0).strip() + ("" if m.group(3) else f"\ntypedef enum {m.group(1)} {m.group(1)};"))
        return ""
    body = re.sub(r"^[ \t]*enum\s+(\w+)\s*\{[^{}]*\}\s*;[ \t]*\n(\s*typedef [\w ]+ (\w+);\n)?", hoist, body, flags=re.M)
    enums = [unalias(e, body) for e in enums]
    parts.append("/* ---- enums ---- */\n" + "\n".join(enums))
    parts.append(pad_aligned(body))
    header = "\n".join(parts)
    # forward typedefs so 'Name' works without 'struct'/'union' (C++ style); enums have theirs above
    names = re.findall(r"^\s*(struct|union)\s+(?:__declspec\(align\(\d+\)\)\s+)?(\w+)\s*(?:\{|$|;)", header, re.M)
    fwd, seen = [], set()
    for kind, n in names:
        if n not in seen:
            seen.add(n)
            fwd.append(f"typedef {kind} {n} {n};")
    header = "/* generated by t4sp_types.py from T4SP-Server-Plugin headers (AGPL-3.0); private use */\n" + \
        "\n".join(fwd) + "\n" + header
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
    print(f"wrote t4sp_types.h ({len(header)} bytes, {len(seen)} structures, {len(enums)} enums) and t4sp_symbols.tsv ({len(rows)} symbols)")
    if "--check" in sys.argv[3:]:
        check(os.path.join(out, "t4sp_types.h"))


if __name__ == "__main__":
    main()
