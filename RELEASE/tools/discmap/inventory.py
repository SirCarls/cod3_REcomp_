"""[waw-decomp] 2026-11-16 discmap: every guest address Call of Duty 3's own code and configs use.

The scan is rc-discs' inventory.py (waw-decomp-recomp/rc-discs/, phase 1 of 2026-11-16), turned into
functions so discmap can run it on the project's current sources each time (new code is covered).
The rules are unchanged, so its counts are comparable with phase 1's. Read only.

Rows: program, kind, addr, symbol, file, line, note.
  program: campaign | mp | level:<name> (a level DLL's own config)
  kind:
    hook          REX_HOOK_RAW(name) or a macro that expands to it (the whole function replaced)
    call          REX_EXTERN(name) without __imp__ (a guest function called by name)
    orig          REX_EXTERN(__imp__name) (the original of a hooked function; same address as the hook)
    addr-pair     a COD3_ADDR(sp, mp) / COD3_TEXT(sp, mp) argument (the side for this program)
    literal       any other 0x82xxxxxx / 0x89xxxxxx constant in the C++ sources
    midasm        a [[midasm_hook]] address in a codegen config
    midasm-target a jump / return address of a mid-asm hook
    func-bound    a [functions] entry (a function the analyzer can't find on its own)
    setjmp        setjmp_address
    name          a names.toml / names_mp.toml entry (the codegen's function names)
    symbol        a symbols.toml [[function]] / [[global]] (the symbol map, the names' source)
    map-row       mp/config/campaign_map.tsv's mp column (the campaign -> mp table)
    offset        a structure offset constant (no address; counted, not mapped)

Then each address is put in a group (what it is used for) against the reference build's function list:
  1 function hook, 2 mid-asm hook site, 3 codegen function entry, 4 function call (by name),
  5 function address (literal), 6 code address, 7 global, 8 image base, 9 level DLL address,
  A codegen name only, B symbol map only. Groups 1-9 are what the code needs ("required"); A and B are
  names only.
"""
from __future__ import annotations

import bisect
import collections
import re
from dataclasses import dataclass, field
from pathlib import Path

from common import IMAGE_BASE

Row = collections.namedtuple("Row", "program kind addr symbol file line note")

HOOK = re.compile(r"\bREX_HOOK_RAW\(\s*(\w+)\s*\)")
EXTERN = re.compile(r"\bREX_EXTERN\(\s*(\w+)\s*\)")
PAIR = re.compile(r"\bCOD3_(?:ADDR|TEXT)\(\s*(0x[0-9A-Fa-f]+|[^,()]*)\s*,\s*(0x[0-9A-Fa-f]+|[^,()]*)\s*\)")
LIT = re.compile(r"\b0x(8[29][0-9A-Fa-f]{6})\b")
RW_OFF = re.compile(r"\b(?:g::|guest::)?(?:Read|Write)(?:U32|S32|U16|S16|U8|F32|F64|U64)\(\s*base\s*,\s*[^,;]*?"
                    r"\+\s*(0x[0-9A-Fa-f]+|\d+)\s*[,)]")
CONST_SMALL = re.compile(r"\bconstexpr\s+(?:u?int\d+_t|unsigned|int|size_t|uint32_t)\s+(k\w+)\s*=\s*"
                         r"(0x[0-9A-Fa-f]+|\d+)\s*;")
NAME_LINE = re.compile(r'\s*0x([0-9A-Fa-f]{8})\s*=\s*"([^"]+)"')
TABLES = ("addresses.h", "addresses_mp.h")   # the two address tables; everything else is "loose"


def read_names(path: Path) -> dict[str, int]:
    by_name = {}
    for line in open(path, encoding="utf-8"):
        m = NAME_LINE.match(line)
        if m:
            by_name[m.group(2)] = int(m.group(1), 16)
    return by_name


class Resolver:
    """A codegen symbol name -> guest address in a program (names.toml / names_mp.toml, and
    mp_aliases.h's #define sub_<campaign> sub_<mp> for multiplayer)."""

    def __init__(self, recomp: Path):
        self.names = {"campaign": read_names(recomp / "config/names.toml"),
                      "mp": read_names(recomp / "mp/config/names_mp.toml")}
        self.alias_mp = {}
        for line in open(recomp / "src/game/mp/mp_aliases.h", encoding="utf-8"):
            m = re.match(r"#define (\w+) (\w+)", line)
            if m:
                self.alias_mp[m.group(1)] = m.group(2)

    def symbol(self, name: str, program: str) -> str:
        """The symbol a source name stands for in this program (after mp_aliases.h)."""
        if program == "mp":
            for _ in range(4):
                if name not in self.alias_mp:
                    break
                name = self.alias_mp[name]
        return name

    def resolve(self, name: str, program: str) -> int | None:
        name = self.symbol(name, program)
        table = self.names["mp" if program == "mp" else "campaign"]
        if name.startswith("__imp__"):
            name = name[7:]
        if name in table:
            return table[name]
        m = re.fullmatch(r"sub_([0-9A-Fa-f]{8})", name)
        return int(m.group(1), 16) if m else None


def cmake_sources(path: Path, prefix_re: str) -> set[str]:
    out = set()
    for line in open(path, encoding="utf-8"):
        for m in re.finditer(prefix_re + r"([\w/]+\.cpp)", line):
            out.add(m.group(1))
    return out


def scan(recomp: Path) -> list[Row]:
    """Every address use in the sources and configs under recomp/ (the rows described above)."""
    src = recomp / "src"
    rows: list[Row] = []
    resolver = Resolver(recomp)

    def add(program, kind, addr, symbol, path, line, note=""):
        rows.append(Row(program, kind, addr, symbol, path.relative_to(recomp).as_posix(), line,
                        note.replace("\t", " ")[:140]))

    sp_cpp = cmake_sources(recomp / "CMakeLists.txt", r"\bsrc/")
    mp_cpp = cmake_sources(recomp / "mp/CMakeLists.txt", r"\$\{COD3_SRC\}/")

    def source_files():
        for path in sorted(src.rglob("*")):
            if path.suffix in (".cpp", ".h", ".inc") and path.is_file():
                yield path

    # Macros whose body holds REX_HOOK_RAW(param): their invocations are hooks too.
    hook_macros = set()
    for path in source_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"#define (\w+)\((\w+)[^)]*\)((?:[^\n]*\\\n)+[^\n]*)", text):
            if re.search(r"REX_HOOK_RAW\(\s*" + m.group(2) + r"\s*\)", m.group(3)):
                hook_macros.add(m.group(1))
    macro_call = re.compile(r"^\s*(" + "|".join(sorted(hook_macros)) + r")\(\s*(\w+)") if hook_macros else None

    def programs_of(rel):
        """The programs a file's code can be compiled into (before #if guards)."""
        if rel.startswith("game/mp/") or rel in ("game/mp_hooks.cpp",) or re.search(r"_mp\.(cpp|h)$", rel):
            return {"mp"}
        if rel.endswith(".cpp") or rel.endswith(".inc"):
            p = set()
            if rel in sp_cpp:
                p.add("campaign")
            if rel in mp_cpp:
                p.add("mp")
            if rel == "game/level_hooks.cpp":
                p |= {"campaign", "mp"}
            return p
        if rel.endswith("_sp.h") or rel.endswith("_sp.cpp"):
            return {"campaign"}
        return {"campaign", "mp"}   # a header: whoever includes it; guards decide

    for path in source_files():
        rel = path.relative_to(src).as_posix()
        if "/ui/fonts" in "/" + path.parent.relative_to(src).as_posix():
            continue
        base_progs = programs_of(rel)
        if not base_progs or rel == "game/mp/mp_aliases.h":
            continue   # in no target, or generated from campaign_map.tsv (counted there)
        fn = path.name
        stack = []      # each: set of programs allowed in that branch
        in_block_comment = False
        for n, raw in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
            s = raw.strip()
            # preprocessor guards on COD3_MP
            if s.startswith("#if") or s.startswith("#ifdef") or s.startswith("#ifndef"):
                c = s.replace(" ", "")
                if re.search(r"\bCOD3_MP\b", c):
                    neg = c.startswith("#ifndef") or "!defined(COD3_MP)" in c
                    stack.append(None if ("||" in c or "&&" in c) else ({"campaign"} if neg else {"mp"}))
                else:
                    stack.append(None)
            elif s.startswith("#elif"):
                if stack:
                    stack[-1] = None
            elif s.startswith("#else"):
                if stack and stack[-1] is not None:
                    stack[-1] = {"campaign", "mp"} - stack[-1]
            elif s.startswith("#endif"):
                if stack:
                    stack.pop()
            progs = set(base_progs)
            for g in stack:
                if g is not None:
                    progs &= g
            if not progs:
                continue
            code = raw
            # drop // comments and /* */ comments (addresses in comments are documentation)
            if in_block_comment:
                if "*/" in code:
                    code = code.split("*/", 1)[1]
                    in_block_comment = False
                else:
                    continue
            code = re.sub(r"/\*.*?\*/", "", code)
            if "/*" in code:
                code, in_block_comment = code.split("/*", 1)[0], True
            # strings are kept: COD3_TEXT names addresses in strings, and so do some labels
            code_nc = code.split("//", 1)[0] if "//" in code and '"' not in code.split("//", 1)[0] else code
            seen_spans = []
            for m in HOOK.finditer(code_nc):
                if s.startswith("#define") or code_nc.rstrip().endswith("\\"):
                    continue   # the macro's own body
                for p in sorted(progs):
                    add(p, "hook", resolver.resolve(m.group(1), p), m.group(1), path, n)
            if macro_call:
                m = macro_call.match(code_nc)
                if m and not s.startswith("#define"):
                    for p in sorted(progs):
                        add(p, "hook", resolver.resolve(m.group(2), p), m.group(2), path, n, "via " + m.group(1))
            for m in EXTERN.finditer(code_nc):
                if s.startswith("#define") or code_nc.rstrip().endswith("\\"):
                    continue
                nm = m.group(1)
                for p in sorted(progs):
                    add(p, "orig" if nm.startswith("__imp__") else "call", resolver.resolve(nm, p), nm, path, n)
            for m in PAIR.finditer(code_nc):
                seen_spans.append(m.span())
                for side, p in ((1, "campaign"), (2, "mp")):
                    if p not in progs:
                        continue
                    mm = re.fullmatch(r"0x(8[29][0-9A-Fa-f]{6})", m.group(side).strip())
                    if mm:
                        add(p, "addr-pair", int(mm.group(1), 16), "", path, n, s[:120])
            for m in LIT.finditer(code_nc):
                if any(a <= m.start() < b for a, b in seen_spans):
                    continue
                ident = re.search(r"\b(k\w+)\s*=\s*0x" + m.group(1), code_nc)
                for p in sorted(progs):
                    add(p, "literal", int(m.group(1), 16), ident.group(1) if ident else "", path, n, s[:120])
            for m in RW_OFF.finditer(code_nc):
                if 0 < int(m.group(1), 0) < 0x10000:
                    for p in sorted(progs):
                        add(p, "offset", None, "rw+" + m.group(1), path, n, s[:120])
            if fn in TABLES:
                for m in CONST_SMALL.finditer(code_nc):
                    if int(m.group(2), 0) < 0x10000:
                        for p in sorted(progs):
                            add(p, "offset", None, m.group(1) + "=" + m.group(2), path, n, s[:120])

    # ------------------------------------------------------------ codegen configs
    def scan_toml(path, program):
        in_functions = False
        cur_hook = None
        for n, line in enumerate(open(path, encoding="utf-8"), 1):
            s = line.split("#", 1)[0].strip()
            if s.startswith("["):
                in_functions = s == "[functions]"
                cur_hook = s == "[[midasm_hook]]"
                continue
            m = re.match(r"setjmp_address\s*=\s*0x([0-9A-Fa-f]{8})", s)
            if m:
                add(program, "setjmp", int(m.group(1), 16), "setjmp", path, n)
                continue
            m = re.match(r"0x([0-9A-Fa-f]{8})\s*=", s)
            if m and in_functions:
                add(program, "func-bound", int(m.group(1), 16), "", path, n,
                    line.split("#", 1)[1].strip() if "#" in line else "")
                continue
            m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]{8})", s)
            if m and cur_hook:
                add(program, "midasm" if m.group(1) == "address" else "midasm-target", int(m.group(2), 16),
                    m.group(1), path, n)

    scan_toml(recomp / "config/default.toml", "campaign")
    for path in sorted((recomp / "config").glob("*.toml")):
        if path.name not in ("default.toml", "names.toml", "symbols.toml"):
            scan_toml(path, "level:" + path.stem)
    scan_toml(recomp / "mp/config/codmp.toml", "mp")
    scan_toml(recomp / "mp/config/codmp_campaign.toml", "mp")

    for path, program in ((recomp / "config/names.toml", "campaign"), (recomp / "mp/config/names_mp.toml", "mp")):
        for n, line in enumerate(open(path, encoding="utf-8"), 1):
            m = NAME_LINE.match(line)
            if m:
                add(program, "name", int(m.group(1), 16), m.group(2), path, n)

    path = recomp / "config/symbols.toml"
    kind = None
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        s = line.strip()
        if s in ("[[function]]", "[[global]]"):
            kind = s.strip("[]")
            continue
        m = re.match(r"address\s*=\s*0x([0-9A-Fa-f]{8})", s)
        if m:
            add("campaign", "symbol", int(m.group(1), 16), kind, path, n)

    path = recomp / "mp/config/campaign_map.tsv"
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        if line.startswith("#") or line.startswith("symbol\t"):
            continue
        p = line.rstrip("\n").split("\t")
        if len(p) < 4:
            continue
        sym, sp, mp, st = p[0], p[1], p[2], p[3]
        if not sp and mp and re.fullmatch(r"\d+|0x[0-9A-Fa-f]+", mp):
            add("mp", "offset", None, sym + "=" + mp, path, n, "campaign_map offset row, status " + st)
            continue
        if not sp and st == "same" and not mp:
            add("mp", "offset", None, sym, path, n, "campaign_map offset row, same value")
            continue
        if re.fullmatch(r"[0-9A-Fa-f]{8}", mp or ""):
            add("mp", "map-row", int(mp, 16), sym, path, n, "campaign " + sp + ", status " + st)
    return rows


def write_rows(rows: list[Row], path: Path) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as w:
        w.write("program\tkind\taddr\tsymbol\tfile\tline\tnote\n")
        for r in rows:
            a = "" if r.addr is None else f"{r.addr:08X}"
            w.write(f"{r.program}\t{r.kind}\t{a}\t{r.symbol}\t{r.file}\t{r.line}\t{r.note}\n")


# ---------------------------------------------------------------- groups (rc-discs' rc_join.py)
PRIORITY = ["hook", "midasm", "midasm-target", "func-bound", "setjmp", "call", "orig", "addr-pair", "literal",
            "map-row", "name", "symbol"]
GROUP_NAMES = {
    "1": "function hook", "2": "mid-asm hook site", "3": "codegen function entry", "4": "function call (by name)",
    "5": "function address (literal)", "6": "code address (call site/patch/return)", "7": "global (data)",
    "8": "image base", "9": "level DLL address", "A": "codegen name only", "B": "symbol map only",
}


class RefFunctions:
    """The reference build's functions: its .pdata list, plus a Ghidra export's list when there is one
    (functions without .pdata entries). Tells what an address is: func / code / data / level."""

    def __init__(self, pdata: dict[int, int], extra: dict[int, int] | None = None):
        self.f = dict(extra or {})
        self.f.update(pdata)
        self.pdata = pdata
        self.starts = sorted(self.f)
        self.pdata_starts = sorted(pdata)
        self.text = (min(pdata), max(a + s for a, s in pdata.items()))

    def classify(self, a: int) -> tuple[str, int | None]:
        if a == IMAGE_BASE:
            return "image-base", None
        if a in self.f:
            return "func", a
        k = bisect.bisect_right(self.starts, a) - 1
        if k >= 0 and a < self.starts[k] + self.f[self.starts[k]]:
            return "code", self.starts[k]
        if self.text[0] <= a < self.text[1]:
            return "code", self.starts[k] if k >= 0 else None
        if a >= 0x89000000:
            return "level", None
        return "data", None

    def pdata_of(self, a: int) -> int | None:
        """The .pdata function holding a, or None."""
        k = bisect.bisect_right(self.starts, a) - 1
        while k >= 0 and self.starts[k] not in self.pdata:
            k -= 1
        if k >= 0 and a < self.starts[k] + max(self.f[self.starts[k]], 4):
            return self.starts[k]
        return None


def group_of(kind: str, cls: str) -> str:
    if kind in ("hook", "orig"):
        return "1"
    if kind in ("midasm", "midasm-target"):
        return "2"
    if kind in ("func-bound", "setjmp"):
        return "3"
    if kind == "call":
        return "4"
    if kind in ("addr-pair", "literal", "map-row"):
        return {"func": "5", "code": "6", "data": "7", "image-base": "8", "level": "9"}[cls]
    if kind == "name":
        return "A"
    return "B"


@dataclass
class Entry:
    """One address a program uses, with everything the sources say about it."""
    program: str
    usa: int
    kinds: list[str]
    group: str
    cls: str
    func: int | None
    syms: list[str]
    files: list[str]
    loose: bool
    loose_files: list[str] = field(default_factory=list)

    @property
    def required(self) -> bool:
        return self.group not in ("A", "B")


def entries(rows: list[Row], program: str, funcs: RefFunctions | None) -> list[Entry]:
    """The unique addresses one program (campaign, mp, or level:<name>) uses. For a level, funcs is
    None and every address counts as a codegen entry or mid-asm site (its config is all there is)."""
    uses = collections.defaultdict(list)
    for r in rows:
        if r.addr is not None and r.program == program:
            uses[r.addr].append(r)
    out = []
    for a, rs in sorted(uses.items()):
        kinds = sorted({r.kind for r in rs}, key=PRIORITY.index)
        if funcs is None:
            cls, fn = "level", None
        else:
            cls, fn = funcs.classify(a)
        loose_rows = [r for r in rs if r.kind in ("literal", "addr-pair") and not r.file.endswith(TABLES)]
        out.append(Entry(program, a, kinds, group_of(kinds[0], cls), cls, fn,
                         sorted({r.symbol for r in rs if r.symbol})[:3],
                         sorted({f"{r.file}:{r.line}" for r in rs if r.kind not in ("name", "symbol")}),
                         bool(loose_rows), sorted({f"{r.file}:{r.line}" for r in loose_rows})))
    return out
