#!/usr/bin/env python3
"""
kisak_strings.py - list, for every function in a C/C++ source tree, the string
literals it uses and the functions it calls. Made for KisakCOD (a reconstructed
Call of Duty 4 source, GPL-3.0): World at War shares much of that engine, so a
World at War function using the same distinctive strings most likely has the
same name. Only NAMES are taken into our tables; no code is copied.

Strings inside assert macros are skipped (the retail game has none).

Usage:
    python3 tools/waw_decomp/kisak_strings.py <source dir> <out.tsv>

Output columns: name, file, strings (joined by \\x1f, escaped), calls (comma list).
A second file, <out>_tables.tsv, lists the { "name", Function } table entries: string,
function, file, and the array they are in; a third, <out>_readers.tsv, the functions of the
same file that use each of those arrays (table, file, function).
"""
import os
import re
import sys

HEADER = re.compile(
    r"^(?!(?:if|for|while|switch|return|else|do)\b)[A-Za-z_][\w \t\*&:<>,]*?\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:const\s*)?$")
# a function returning a function pointer: void (__cdecl *__cdecl Player_GetMethod(const char **pName))(scr_entref_t)
HEADER_FP = re.compile(r"^[A-Za-z_][\w \t\*&:<>,]*\(\s*(?:__cdecl\s*)?\*\s*(?:__cdecl\s+)?([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\)\s*\([^;{}]*\)\s*$")
STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
ASSERT = re.compile(r"\b(?:iassert|vassert|bcassert|assert|MyAssertHandler|_ASSERT|ASSERT)\s*\(")
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "defined", "else", "do", "case"}


def c_unescape(s):
    out, i = [], 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]{1,2}", s[i + 2:])
                if m:
                    out.append(chr(int(m.group(0), 16)))
                    i += 2 + len(m.group(0))
                    continue
            if n in simple:
                out.append(simple[n])
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def esc(s):
    return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "\\r")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def remove_asserts(body):
    out, i = [], 0
    for m in ASSERT.finditer(body):
        if m.start() < i:
            continue
        out.append(body[i:m.start()])
        depth, j = 1, m.end()
        while j < len(body) and depth:
            if body[j] == "(":
                depth += 1
            elif body[j] == ")":
                depth -= 1
            elif body[j] == '"':
                k = j + 1
                while k < len(body) and body[k] != '"':
                    k += 2 if body[k] == "\\" else 1
                j = k
            j += 1
        i = j
    out.append(body[i:])
    return "".join(out)


def functions(text):
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        m = HEADER.match(line.rstrip()) or HEADER_FP.match(line.rstrip())
        if m and i + 1 < len(lines) and lines[i + 1].startswith("{"):
            name = m.group(1)
            j = i + 2
            while j < len(lines) and not lines[j].startswith("}"):
                j += 1
            yield name, "\n".join(lines[i + 1:j + 1])
            i = j + 1
            continue
        i += 1


def main():
    src, out = sys.argv[1], sys.argv[2]
    n = 0
    with open(out, "w", encoding="utf-8") as o:
        o.write("name\tfile\tstrings\tcalls\n")
        for root, _, files in os.walk(src):
            for fn in sorted(files):
                if not fn.endswith((".cpp", ".c")):
                    continue
                path = os.path.join(root, fn)
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = strip_comments(f.read())
                for name, body in functions(text):
                    body = remove_asserts(body)
                    strs = [c_unescape(s) for s in STRING.findall(body)]
                    strs = [s for s in strs if len(s) >= 6]
                    calls = sorted({c for c in CALL.findall(body) if c not in KEYWORDS and c != name})
                    rel = os.path.relpath(path, src)
                    o.write(f"{name}\t{rel}\t" + "\x1f".join(esc(s) for s in strs) + "\t" + ",".join(calls) + "\n")
                    n += 1
    print(f"{n} functions -> {out}")
    # name tables: { "script_name", Function, ... } (script builtins, console commands, ...)
    # (a handler may be cast first: { "precachemodel", (void(*)())Scr_PrecacheModel, 0 })
    entry = re.compile(r'\{\s*"([A-Za-z_][\w]*)"\s*,\s*(?:\(\s*void\s*\([^()]*\*\s*\)\s*\([^()]*\)\s*\)\s*)?&?\s*([A-Za-z_]\w*)\s*[,}]')
    # the array an entry is in: "functions" and "methods" of the same file stay apart
    array = re.compile(r"\b([A-Za-z_]\w*)\s*\[[^\]\[;]*\]\s*=\s*\{")
    tab = out[:-4] + "_tables.tsv" if out.endswith(".tsv") else out + "_tables.tsv"
    rdr = out[:-4] + "_readers.tsv" if out.endswith(".tsv") else out + "_readers.tsv"
    m = r = 0
    counts = {}  # (file, array) -> entries
    with open(tab, "w", encoding="utf-8") as o:
        o.write("string\tfunction\tfile\ttable\n")
        for root, _, files in os.walk(src):
            for fn in sorted(files):
                if not fn.endswith((".cpp", ".c", ".h")):
                    continue
                path = os.path.join(root, fn)
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = strip_comments(f.read())
                starts = [(a.start(), a.group(1)) for a in array.finditer(text)]
                k = 0
                for e in entry.finditer(text):
                    while k < len(starts) and starts[k][0] < e.start():
                        k += 1
                    arr = starts[k - 1][1] if k else ""
                    rel = os.path.relpath(path, src)
                    o.write(f"{e.group(1)}\t{e.group(2)}\t{rel}\t{arr}\n")
                    if arr:
                        counts[(rel, arr)] = counts.get((rel, arr), 0) + 1
                    m += 1
    # the functions that use each table (Player_GetMethod loops over methods): in the table's own
    # file, or in another file when the function also holds the table's size (KisakCOD's
    # Actor_GetMethod reads g_scr_main.cpp's methods and stops at 0x4E0 = 104 entries of 12 bytes)
    by_name = {}
    for (rel, arr), c in counts.items():
        by_name.setdefault(arr, []).append((rel, c))
    ident = re.compile(r"\b(" + "|".join(sorted(map(re.escape, by_name))) + r")\b") if by_name else None
    number = re.compile(r"\b(0[xX][0-9a-fA-F]+|\d+)\b")
    with open(rdr, "w", encoding="utf-8") as ro:
        ro.write("table\tfile\tfunction\n")
        for root, _, files in os.walk(src):
            for fn in sorted(files):
                if not fn.endswith((".cpp", ".c")) or ident is None:
                    continue
                path = os.path.join(root, fn)
                rel = os.path.relpath(path, src)
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = strip_comments(f.read())
                for name, body in functions(text):
                    found = set(ident.findall(body))
                    if not found:
                        continue
                    nums = {int(x, 16) if x[:2] in ("0x", "0X") else int(x) for x in number.findall(body)}
                    for arr in sorted(found):
                        for trel, c in by_name[arr]:
                            if trel == rel or nums & {c, c * 8, c * 12, c * 16}:
                                ro.write(f"{arr}\t{trel}\t{name}\n")
                                r += 1
    print(f"{m} table entries -> {tab}; {r} functions that read them -> {rdr}")

if __name__ == "__main__":
    main()
