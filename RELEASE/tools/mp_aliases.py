#!/usr/bin/env python3
"""mp_aliases.py - the campaign's port code compiled into cod3mp, the multiplayer program.

One source tree, two images: the same recomp/src files are compiled into cod3
(default.xex) and cod3mp (codmp_xenonf.xex). This tool makes cod3mp's build
use the multiplayer program's functions, addresses and mid-asm hook sites,
from one reviewed table, recomp/mp/config/campaign_map.tsv. The campaign's
build never sees any of it.

Generated (never edit them; edit the table and run --write):
  src/game/mp/mp_aliases.h       force-included into cod3mp's own sources only
                                 (mp/CMakeLists.txt): each campaign function name
                                 a shared file hooks or calls becomes the MP name
  src/game/mp/addresses_mp.h     addresses.h's constants with MP values
                                 (addresses.h includes it when COD3_MP is set)
  src/game/mp/campaign_only.cpp  a trap for each campaign-only function a shared
                                 file names (it never runs in cod3mp; if it did,
                                 the log says so once and it returns 0)
  mp/config/codmp_campaign.toml  the campaign's mid-asm hooks at MP addresses
                                 (cod3mp_manifest.toml includes it)

Usage:
  python3 tools/mp_aliases.py --write     regenerate the four files
  python3 tools/mp_aliases.py --check     fail on a stale file or a table problem (tests)
  python3 tools/mp_aliases.py --report    what binds how, and the rows nothing uses
  python3 tools/mp_aliases.py --sync MAP  (cloud) add rows for new symbols from the
                                          address map (sp_to_mp.tsv), then --write

The table (tab-separated, '#' comments): symbol, sp, mp, status, regs, jump, why, mp_function
  symbol  a hook / extern name (sub_82344FC0, Cvar_Get), an addresses.h constant
          (jobs::kQueue), ns:<namespace> (a rule for its plain numbers),
          midasm:<function>@<campaign address> (one row per MP site; mp_function: a
          cod3mp-only function the MP hook calls instead), lit:<file>:<campaign address>
  status  mp       checked MP counterpart (sp -> mp)
          same     the same name / value in both programs
          dead     campaign only: never runs in cod3mp (why is required); a dead
                   constant is left out of addresses_mp.h (a compile error where
                   cod3mp code uses it) unless its mp column says 0
          later    has an MP counterpart, but its feature isn't on in cod3mp yet
          mp-code  cod3mp does this in its own code (not emitted: a compile error if used)
          todo     new, not reviewed (--check fails)
The --recomp folder defaults to ../COD_3_WAW_Setup-tool/recomp next to this
tool, else this tool's parent folder.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent

ALIASES_H = "src/game/mp/mp_aliases.h"
ADDRESSES_MP_H = "src/game/mp/addresses_mp.h"
CAMPAIGN_ONLY_CPP = "src/game/mp/campaign_only.cpp"
CAMPAIGN_TOML = "mp/config/codmp_campaign.toml"
TABLE = "mp/config/campaign_map.tsv"
COLUMNS = ["symbol", "sp", "mp", "status", "regs", "jump", "why", "mp_function"]
STATUSES = {"mp", "same", "dead", "later", "mp-code", "todo"}

# Files compiled into cod3mp that are the multiplayer program's own: names in
# them are codmp_xenonf.xex's, so no campaign alias may appear in them.
MP_OWN = re.compile(r"^(main_mp\.cpp|cod3mp_app\.(h|cpp)|game/mp_hooks\.cpp|game/mp/.*)$")

HOOK_RE = re.compile(r"\bREX_(HOOK_RAW|HOOK|EXTERN)\(\s*([A-Za-z_][A-Za-z0-9_]*)")
SUB_RE = re.compile(r"^sub_([0-9A-F]{8})$")
LIT_RE = re.compile(r"\b0x8[23][0-9A-Fa-f]{6}\b")
COD3_ADDR_RE = re.compile(r"COD3_ADDR\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+|0)\s*\)")
COD3_TEXT_RE = re.compile(r'COD3_TEXT\(\s*"[^"]*?(8[23][0-9A-Fa-f]{6})[^"]*"\s*,\s*"[^"]*?(8[23][0-9A-Fa-f]{6})[^"]*"\s*\)')
CONST_RE = re.compile(r"^\s*inline constexpr\s+([A-Za-z_][A-Za-z0-9_:]*)\s+(.+?);\s*(//.*)?$")
NS_OPEN_RE = re.compile(r"^\s*namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{")
NS_CLOSE_RE = re.compile(r"^\s*\}\s*//\s*namespace")


def find_recomp(arg: str | None) -> Path:
    if arg:
        return Path(arg).resolve()
    beside = HERE.parent / "COD_3_WAW_Setup-tool" / "recomp"
    return beside if beside.is_dir() else HERE.parent


# ---------------------------------------------------------------------------
# Reading the sources
# ---------------------------------------------------------------------------
def strip_code(text: str) -> str:
    """Comments and string / character literals blanked, line breaks kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == "'" and i > 0 and text[i - 1].isalnum():
            out.append(c)  # a digit separator (1'000'000), not a character literal
            i += 1
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def drop_mp_conditionals(text: str) -> str:
    """Lines inside `#if defined(COD3_MP)` / `#ifdef COD3_MP` / `#if !defined(COD3_MP)`
    blocks (both branches) blanked: they are written for one program on purpose."""
    out, depth, skip_at = [], 0, None
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            depth += 1
            if skip_at is None and "COD3_MP" in s:
                skip_at = depth
                out.append("")
                continue
        elif s.startswith("#endif"):
            if skip_at == depth:
                skip_at = None
                depth -= 1
                out.append("")
                continue
            depth -= 1
        out.append("" if skip_at is not None else line)
    return "\n".join(out)


def cod3mp_sources(recomp: Path) -> list[str]:
    cmake = (recomp / "mp" / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(COD3MP_SOURCES(.*?)\)", cmake, re.S)
    body = m.group(1) if m else ""
    return re.findall(r"\$\{COD3_SRC\}/(\S+\.cpp)", body)


def read_names(path: Path) -> dict[int, str]:
    out = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r'\s*0x([0-9A-Fa-f]{8})\s*=\s*"([^"]+)"', line)
        if m:
            out[int(m.group(1), 16)] = m.group(2)
    return out


def mp_codegen_name(mp_names: dict[int, str], addr: int) -> str:
    """The name cod3mp's generated code gives the function at addr."""
    return mp_names.get(addr, f"sub_{addr:08X}")


# ---------------------------------------------------------------------------
# The table
# ---------------------------------------------------------------------------
class Row:
    def __init__(self, d: dict, line: int):
        self.symbol = d.get("symbol", "").strip()
        self.sp = d.get("sp", "").strip()
        self.mp = d.get("mp", "").strip()
        self.status = d.get("status", "").strip()
        self.regs = d.get("regs", "").strip()
        self.jump = d.get("jump", "").strip()
        self.why = d.get("why", "").strip()
        self.mp_function = (d.get("mp_function") or "").strip()
        self.line = line

    def as_list(self):
        return [self.symbol, self.sp, self.mp, self.status, self.regs, self.jump, self.why, self.mp_function]


def read_table(path: Path) -> tuple[list[str], list[Row]]:
    header, rows = [], []
    if not path.is_file():
        return header, rows
    lines = path.read_text(encoding="utf-8").splitlines()
    body = []
    for i, line in enumerate(lines, 1):
        if line.startswith("#"):
            header.append(line)
        elif line.strip():
            body.append((i, line))
    if not body:
        return header, rows
    cols = body[0][1].split("\t")
    for i, line in body[1:]:
        parts = line.split("\t")
        rows.append(Row(dict(zip(cols, parts + [""] * (len(cols) - len(parts)))), i))
    return header, rows


def write_table(path: Path, header: list[str], rows: list[Row]) -> None:
    def clean(s: str) -> str:
        return s.replace("\t", " ").replace("\n", " ")
    lines = list(header) + ["\t".join(COLUMNS)]
    lines += ["\t".join(clean(x) for x in r.as_list()).rstrip("\t") for r in rows]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def hexval(s: str) -> int | None:
    s = s.strip()
    if not s:
        return None
    try:
        return int(s, 16)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# addresses.h
# ---------------------------------------------------------------------------
class Const:
    def __init__(self, ns: str, ctype: str, name: str, value: str, comment: str):
        self.ns, self.ctype, self.name, self.value, self.comment = ns, ctype, name, value, comment

    @property
    def qual(self) -> str:
        return f"{self.ns}::{self.name}" if self.ns else self.name

    @property
    def address(self) -> int | None:
        v = self.value.strip()
        if re.fullmatch(r"0x[0-9A-Fa-f]{8}", v):
            a = int(v, 16)
            if 0x82000000 <= a < 0x84000000:
                return a
        return None


def parse_addresses(text: str) -> list:
    """Items in order: ('ns', name) / ('end',) / Const."""
    items, stack = [], []
    for line in text.splitlines():
        m = NS_OPEN_RE.match(line)
        if m:
            name = m.group(1)
            if name == "cod3::addr" and not stack:
                stack.append(name)
                continue
            stack.append(name)
            items.append(("ns", name))
            continue
        if NS_CLOSE_RE.match(line):
            if stack:
                stack.pop()
                if stack:
                    items.append(("end",))
            continue
        m = CONST_RE.match(line)
        if m and stack:
            ctype, decls, comment = m.group(1), m.group(2), (m.group(3) or "")
            ns = "::".join(stack[1:])
            for part in decls.split(","):
                k, _, v = part.partition("=")
                items.append(Const(ns, ctype, k.strip(), v.strip(), comment))
    return items


# ---------------------------------------------------------------------------
# Building everything
# ---------------------------------------------------------------------------
class Problems(list):
    def add(self, msg: str):
        self.append(msg)


def scan_files(recomp: Path, files: list[str]):
    """hooks[name] = {'defined': set(files), 'used': set(files)}; texts[file] = code text."""
    hooks: dict[str, dict] = {}
    texts = {}
    for rel in files:
        p = recomp / "src" / rel
        if not p.is_file():
            continue
        raw = p.read_text(encoding="utf-8")
        code = strip_code(raw)
        texts[rel] = (raw, code)
        if MP_OWN.match(rel):
            continue
        for kind, name in HOOK_RE.findall(drop_mp_conditionals(code)):
            name = name[len("__imp__"):] if name.startswith("__imp__") else name
            h = hooks.setdefault(name, {"defined": set(), "used": set()})
            h["used"].add(rel)
            if kind in ("HOOK_RAW", "HOOK"):
                h["defined"].add(rel)
    return hooks, texts


def short(note: str, limit: int = 110) -> str:
    """A table note as a one-line C++ comment: no backslashes, kept short."""
    note = note.replace("\\", "").strip()
    return note if len(note) <= limit else note[: limit - 3].rstrip() + "..."


def generate(recomp: Path, problems: Problems, report: list | None = None):
    files = cod3mp_sources(recomp)
    hooks, texts = scan_files(recomp, files)
    sp_names = read_names(recomp / "config" / "names.toml")
    sp_by_name = {n: a for a, n in sp_names.items()}
    mp_names = read_names(recomp / "mp" / "config" / "names_mp.toml")
    mp_by_name = {n: a for a, n in mp_names.items()}
    _, rows = read_table(recomp / TABLE)
    by_symbol: dict[str, list[Row]] = {}
    for r in rows:
        if r.status not in STATUSES:
            problems.add(f"{TABLE}:{r.line}: {r.symbol}: unknown status '{r.status}'")
        if r.status == "todo":
            problems.add(f"{TABLE}:{r.line}: {r.symbol}: not reviewed yet (todo)")
        if r.status == "dead" and not r.why:
            problems.add(f"{TABLE}:{r.line}: {r.symbol}: a dead row needs its reason")
        by_symbol.setdefault(r.symbol, []).append(r)
    for sym, rs in by_symbol.items():
        if len(rs) > 1 and not sym.startswith("midasm:"):
            problems.add(f"{TABLE}: {sym} has {len(rs)} rows")
    used_rows: set[int] = set()

    # --- function names -------------------------------------------------------
    aliases: dict[str, tuple[str, str]] = {}   # campaign name -> (target, comment)
    traps: dict[str, str] = {}                  # campaign name -> reason
    for name in sorted(hooks):
        sp = None
        m = SUB_RE.match(name)
        if m:
            sp = int(m.group(1), 16)
        elif name in sp_by_name:
            sp = sp_by_name[name]
        if sp is None:
            continue  # not a campaign function name (a host function, an MP-own name)
        where = ", ".join(sorted(hooks[name]["used"]))
        rs = by_symbol.get(name)
        if not rs:
            problems.add(f"{name} (0x{sp:08X}, {where}): no row in {TABLE}")
            continue
        r = rs[0]
        used_rows.add(id(r))
        if hexval(r.sp) not in (None, sp):
            problems.add(f"{TABLE}:{r.line}: {name}: sp {r.sp} but the campaign has it at 0x{sp:08X}")
        if r.status in ("mp", "same"):
            mp = hexval(r.mp)
            if mp is None:
                problems.add(f"{TABLE}:{r.line}: {name}: no MP address")
                continue
            target = mp_codegen_name(mp_names, mp)
            if r.status == "same" and target != name:
                problems.add(f"{TABLE}:{r.line}: {name}: 'same', but names_mp.toml calls 0x{mp:08X} {target}")
                continue
            if target != name:
                aliases[name] = (target, f"0x{sp:08X} -> 0x{mp:08X}")
            elif mp_by_name.get(name) not in (None, mp):
                problems.add(f"{TABLE}:{r.line}: {name}: names_mp.toml has it at 0x{mp_by_name[name]:08X}")
        elif r.status in ("dead", "later"):
            reason = r.why or r.status
            if r.status == "later":
                reason = "not on in cod3mp yet: " + reason
            traps[name] = reason
            target = f"cod3mp_unused_{name}" if hooks[name]["defined"] else f"cod3mp_campaign_only_{name}"
            aliases[name] = (target, f"0x{sp:08X}: {reason}")
        elif r.status == "mp-code":
            problems.add(f"{name} ({where}): cod3mp has its own code for it (mp-code); "
                         "leave the shared file out of cod3mp")
        elif r.status == "todo":
            pass
    # No chains, no two names on one MP function.
    seen: dict[str, str] = {}
    for name, (target, _) in aliases.items():
        if target in aliases:
            problems.add(f"alias chain: {name} -> {target} -> {aliases[target][0]}; give the MP "
                         "function a role name in names_mp.toml")
        if target in seen and not target.startswith("cod3mp_"):
            problems.add(f"{seen[target]} and {name} both become {target}")
        seen[target] = name
    # MP-own files keep their own names.
    for rel, (raw, code) in texts.items():
        if not MP_OWN.match(rel) or rel in (CAMPAIGN_ONLY_CPP[4:],):
            continue
        for name in aliases:
            if re.search(rf"\b{re.escape(name)}\b", code):
                problems.add(f"{rel} names {name}, which the alias header renames in cod3mp")

    alias_lines = []
    for name in sorted(aliases):
        target, note = aliases[name]
        if name in traps:
            alias_lines.append(f"#define {name} {target}  // {short(note)}")
            alias_lines.append(f"#define __imp__{name} cod3mp_campaign_only_{name}")
        else:
            alias_lines.append(f"#define {name} {target}  // {note}")
            alias_lines.append(f"#define __imp__{name} __imp__{target}")

    # --- mid-asm hooks --------------------------------------------------------
    with open(recomp / "config" / "default.toml", "rb") as f:
        cfg = tomllib.load(f)
    shared_code = "\n".join(c for rel, (_, c) in texts.items() if not MP_OWN.match(rel))
    own_code = "\n".join(c for rel, (_, c) in texts.items() if MP_OWN.match(rel))
    midasm_out = []
    for hook in cfg.get("midasm_hook", []):
        fname = hook.get("name", "")
        if not re.search(rf"\b(?:void|bool)\s+{re.escape(fname)}\s*\(", shared_code):
            continue
        sym = f"midasm:{fname}@{hook['address']:08X}"
        rs = by_symbol.get(sym)
        if not rs:
            problems.add(f"mid-asm hook {fname} at 0x{hook['address']:08X} (default.toml): no row {sym}")
            continue
        for r in rs:
            used_rows.add(id(r))
            if r.status != "mp":
                continue
            mp = hexval(r.mp)
            if mp is None:
                problems.add(f"{TABLE}:{r.line}: {sym}: no MP address")
                continue
            name = r.mp_function or fname
            if r.mp_function and not re.search(rf"\b(?:void|bool)\s+{re.escape(name)}\s*\(", own_code):
                problems.add(f"{TABLE}:{r.line}: {sym}: {name} isn't defined in a cod3mp-only file")
            block = {"address": mp, "name": name}
            regs = [x.strip() for x in r.regs.split(",") if x.strip()] if r.regs else hook.get("registers")
            if regs:
                block["registers"] = regs
            for key, value in hook.items():
                if key in ("address", "name", "registers"):
                    continue
                if key.startswith("jump_address"):
                    j = hexval(r.jump)
                    if j is None:
                        problems.add(f"{TABLE}:{r.line}: {sym}: the hook has {key}; give its MP address")
                        continue
                    block[key] = j
                else:
                    block[key] = value
            midasm_out.append((sym, r, block))

    # --- addresses.h ------------------------------------------------------------
    addr_text = (recomp / "src" / "game" / "addresses.h").read_text(encoding="utf-8")
    items = parse_addresses(addr_text)
    ns_rules = {s[3:]: rs[0] for s, rs in by_symbol.items() if s.startswith("ns:")}
    addr_lines = []
    indent = ""
    for it in items:
        if isinstance(it, tuple):
            if it[0] == "ns":
                addr_lines.append(f"{indent}namespace {it[1]} {{")
                indent += ""
            else:
                addr_lines.append("}")
            continue
        c: Const = it
        rs = by_symbol.get(c.qual)
        r = rs[0] if rs else None
        rule = ns_rules.get(c.ns or "-")
        if r:
            used_rows.add(id(r))
        if rule:
            used_rows.add(id(rule))
        if r and r.status in ("mp", "same"):
            value = c.value if r.status == "same" else r.mp
            if c.address is not None and r.status == "mp" and re.fullmatch(r"[0-9A-Fa-f]{8}", value):
                value = "0x" + value.upper()  # the table writes addresses without 0x
            if c.address is not None and r.status == "mp" and hexval(r.sp) not in (None, c.address):
                problems.add(f"{TABLE}:{r.line}: {c.qual}: sp {r.sp} but addresses.h has {c.value}")
            note = f"campaign {c.value}" if r.status == "mp" and value != c.value else "same"
            if r.why:
                note += f"; {r.why}"
            addr_lines.append(f"inline constexpr {c.ctype} {c.name} = {value};  // {short(note)}")
        elif r and r.status in ("dead", "later"):
            note = ("campaign only: " if r.status == "dead" else "not on in cod3mp yet: ") + (r.why or "")
            if r.mp == "0":
                # Code that uses it never runs in cod3mp (the row says why).
                zero = "0.0f" if c.ctype == "float" else "kNotInMp"
                addr_lines.append(f"inline constexpr {c.ctype} {c.name} = {zero};  // {short(note)}")
            else:
                addr_lines.append(f"// {c.name}: {short(note)}")
        elif r and r.status == "mp-code":
            addr_lines.append(f"// {c.name}: cod3mp has its own code ({short(r.why)})")
        elif r and r.status == "todo":
            addr_lines.append(f"// {c.name}: not reviewed yet")
        elif c.address is None and rule and rule.status == "same":
            addr_lines.append(f"inline constexpr {c.ctype} {c.name} = {c.value};  // same ({c.ns or '-'})")
        elif c.address is not None:
            problems.add(f"addresses.h {c.qual} = {c.value}: no row in {TABLE}")
            addr_lines.append(f"// {c.name}: no row")
        else:
            addr_lines.append(f"// {c.name}: no MP value (not used by cod3mp)")

    # --- COD3_ADDR literals and raw addresses ------------------------------------
    for rel, (raw, code) in sorted(texts.items()):
        if MP_OWN.match(rel):
            continue
        body = drop_mp_conditionals(code)
        for m in COD3_ADDR_RE.finditer(body):
            sp, mp = int(m.group(1), 16), int(m.group(2), 16)
            sym = f"lit:{rel}:{sp:08X}"
            rs = by_symbol.get(sym)
            if not rs:
                problems.add(f"{rel}: COD3_ADDR(0x{sp:08X}, ...) has no row {sym}")
                continue
            r = rs[0]
            used_rows.add(id(r))
            want = hexval(r.mp) if r.status in ("mp", "same") else 0
            if r.status == "same":
                want = sp
            if want is None or mp != want:
                problems.add(f"{rel}: COD3_ADDR(0x{sp:08X}, 0x{mp:08X}) but the table says "
                             f"{r.status} {r.mp or '0'}")
        # COD3_TEXT("XThread8212A780", "XThread821424B0"): the address in each string.
        for m in COD3_TEXT_RE.finditer(drop_mp_conditionals(raw)):
            sp, mp = int(m.group(1), 16), int(m.group(2), 16)
            sym = f"lit:{rel}:{sp:08X}"
            rs = by_symbol.get(sym)
            if not rs:
                problems.add(f"{rel}: COD3_TEXT(...{sp:08X}...) has no row {sym}")
                continue
            used_rows.add(id(rs[0]))
            if hexval(rs[0].mp) != mp:
                problems.add(f"{rel}: COD3_TEXT names {mp:08X} but the table says {rs[0].mp}")
        rest = COD3_ADDR_RE.sub("", body)
        for m in LIT_RE.finditer(rest):
            problems.add(f"{rel}: raw campaign address {m.group(0)} (use COD3_ADDR or addresses.h)")

    # --- the output files ----------------------------------------------------------
    gen_note = ("// Generated by tools/mp_aliases.py from mp/config/campaign_map.tsv; edit the\n"
                "// table and run the tool, not this file.\n")
    aliases_h = (
        "// cod3mp: the campaign's function names in the multiplayer program.\n" + gen_note +
        "//\n"
        "// Force-included into cod3mp's own sources only (mp/CMakeLists.txt), never into\n"
        "// the generated code or the campaign. A shared file's hook of a campaign\n"
        "// function binds to the MP function of the same role; a campaign-only hook\n"
        "// becomes a function nothing calls, and its original a trap (campaign_only.cpp).\n"
        "#pragma once\n\n" + "\n".join(alias_lines) + "\n")
    addresses_h = (
        "// cod3mp: addresses.h's constants in the multiplayer program (codmp_xenonf.xex,\n"
        "// SHA-1 907985e2f1bdea255b52226ef39acf627c2d7ffc).\n" + gen_note +
        "// addresses.h includes this file when COD3_MP is set. kNotInMp marks what only\n"
        "// the campaign has (code that never runs in cod3mp); a constant left out has\n"
        "// no MP value, so code that needs it doesn't compile for cod3mp.\n"
        "#pragma once\n\n#include <cstdint>\n\nnamespace cod3::addr {\n\n"
        "inline constexpr uint32_t kNotInMp = 0;\n\n" + "\n".join(addr_lines) +
        "\n\n}  // namespace cod3::addr\n")
    trap_lines = []
    for name in sorted(traps):
        trap_lines.append(f"// {short(traps[name])}")
        trap_lines.append(f'extern "C" REX_FUNC(cod3mp_campaign_only_{name}) {{ Trap(ctx, "{name}"); }}')
    campaign_only = (
        "// cod3mp: campaign-only functions the shared files name.\n" + gen_note +
        "//\n"
        "// Each one is the original of a hook that only the campaign uses (a page,\n"
        "// a film, a battle action ...). Nothing in cod3mp calls them; if one ever\n"
        "// ran, the log names it once and it returns 0.\n\n"
        "#include <atomic>\n\n#include <rex/hook.h>\n#include <rex/logging.h>\n\n"
        "namespace {\n"
        "void Trap(PPCContext& ctx, const char* name) {\n"
        "  static std::atomic<bool> logged{false};\n"
        "  if (!logged.exchange(true)) REXLOG_ERROR(\"cod3mp: campaign-only code ran: {}\", name);\n"
        "  ctx.r3.u64 = 0;\n"
        "}\n"
        "}  // namespace\n\n" + "\n".join(trap_lines) + "\n")
    toml_lines = [
        "# cod3mp: the campaign's mid-asm hooks (config/default.toml) at the multiplayer",
        "# program's addresses. Generated by tools/mp_aliases.py from",
        "# mp/config/campaign_map.tsv; edit the table and run the tool, not this file.",
        "# cod3mp_manifest.toml includes it next to codmp.toml.",
    ]
    for sym, r, block in midasm_out:
        toml_lines.append("")
        toml_lines.append(f"# {sym}{': ' + short(r.why, 200) if r.why else ''}")
        toml_lines.append("[[midasm_hook]]")
        for key, value in block.items():
            if isinstance(value, bool):
                v = "true" if value else "false"
            elif isinstance(value, int):
                v = f"0x{value:08X}"
            elif isinstance(value, list):
                v = "[" + ", ".join(f'"{x}"' for x in value) + "]"
            else:
                v = f'"{value}"'
            toml_lines.append(f"{key} = {v}")
    if not midasm_out:
        toml_lines.append("")
        toml_lines.append("# (none yet)")
    campaign_toml = "\n".join(toml_lines) + "\n"

    if report is not None:
        report.append(f"cod3mp sources: {len(files)}")
        report.append(f"aliases: {sum(1 for n in aliases if n not in traps)}, campaign-only names: {len(traps)}")
        for name in sorted(aliases):
            report.append(f"  {name} -> {aliases[name][0]}")
        report.append(f"mid-asm hooks: {len(midasm_out)}")
        unused = [r for r in rows if id(r) not in used_rows and not r.symbol.startswith("ns:")]
        report.append(f"rows nothing in cod3mp uses (yet): {len(unused)}")
    return {ALIASES_H: aliases_h, ADDRESSES_MP_H: addresses_h, CAMPAIGN_ONLY_CPP: campaign_only,
            CAMPAIGN_TOML: campaign_toml}


# ---------------------------------------------------------------------------
# --sync: rows for new symbols from the address map (cloud)
# ---------------------------------------------------------------------------
def sync(recomp: Path, map_tsv: Path) -> int:
    with open(map_tsv, newline="", encoding="utf-8") as f:
        mrows = list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))
    by_sp: dict[int, dict] = {}
    by_name: dict[str, dict] = {}
    for r in mrows:
        a = hexval(r["sp_addr"])
        if a is not None:
            by_sp.setdefault(a, r)
        for n in r["name"].split(" / "):
            by_name.setdefault(n.strip(), r)
    header, rows = read_table(recomp / TABLE)
    have = {r.symbol for r in rows}
    problems = Problems()
    generate(recomp, problems)
    added = []
    for p in problems:
        m = re.match(r"(\S+) \(0x([0-9A-F]{8}), .*\): no row", p)
        sym = sp = None
        if m:
            sym, sp = m.group(1), int(m.group(2), 16)
        m = re.match(r"addresses\.h (\S+) = (0x[0-9A-Fa-f]+): no row", p)
        if m:
            sym, sp = m.group(1), int(m.group(2), 16)
        m = re.match(r"mid-asm hook (\S+) at 0x([0-9A-F]{8}) .*: no row (\S+)", p)
        if m:
            sym, sp = m.group(3), int(m.group(2), 16)
        if not sym or sym in have:
            continue
        mr = by_sp.get(sp) or by_name.get(sym)
        mp, ev, st = "", "not in the address map", "todo"
        if mr:
            mp = mr["mp_addr"]
            ev = f"map: {mr['status']}: {mr['evidence'][:160]}"
            if mr["status"] == "verified" and mr["evidence"].startswith("identical (masked)") \
                    and not sym.startswith("midasm:"):
                st = "mp"
        rows.append(Row({"symbol": sym, "sp": f"{sp:08X}", "mp": mp, "status": st, "why": ev}, 0))
        have.add(sym)
        added.append((sym, st))
    write_table(recomp / TABLE, header, rows)
    for sym, st in added:
        print(f"added {st:5} {sym}")
    print(f"{len(added)} rows added; review the todo rows, then run --write")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recomp", help="the recomp folder (src/, config/, mp/)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--report", action="store_true")
    g.add_argument("--sync", metavar="SP_TO_MP_TSV")
    a = ap.parse_args(argv)
    recomp = find_recomp(a.recomp)
    if a.sync:
        return sync(recomp, Path(a.sync))
    problems = Problems()
    report = [] if a.report else None
    out = generate(recomp, problems, report)
    if a.report:
        print("\n".join(report))
    stale = []
    for rel, text in out.items():
        p = recomp / rel
        if a.write:
            p.parent.mkdir(parents=True, exist_ok=True)
            if not p.is_file() or p.read_text(encoding="utf-8") != text:
                p.write_text(text, encoding="utf-8")
                print(f"wrote {rel}")
        elif not p.is_file() or p.read_text(encoding="utf-8") != text:
            stale.append(rel)
    for s in stale:
        problems.add(f"{s} is out of date: run tools/mp_aliases.py --write")
    for p in problems:
        print("problem:", p)
    return 1 if problems and not a.report else 0


if __name__ == "__main__":
    sys.exit(main())
