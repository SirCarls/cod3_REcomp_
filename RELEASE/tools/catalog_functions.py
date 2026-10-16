#!/usr/bin/env python3
"""Writes docs/FUNCTIONS.md: every named Call of Duty 3 function in one list.

    python3 tools/catalog_functions.py                 # the repository this tool is in
    python3 tools/catalog_functions.py --recomp <game project> --check

For each function: its address, name, subsystem, what it does, its arguments
where known, which port file hooks or calls it, and where the name comes
from. It reads (all paths can be given; the defaults are the repository
layout around this tool):
- the symbol map, recomp/config/symbols.toml (made by tools/name_functions.py);
- the codegen names, recomp/config/names.toml (tools/gen_codegen_names.py);
- recomp/src/game/addresses.h (addresses with comments, per subsystem);
- the port's code in recomp/src: REX_HOOK_RAW / REX_EXTERN hooks, guest calls
  (guest::Call::Invoke) and the typed calls in src/game/game_api.h;
- the mid-function hooks in recomp/config/*.toml;
- docs/ENGINE.md and, when present, the helpers' notes (dev/agents/*/NOTES*.md);
- when present, the codegen's function table
  (recomp/generated/default/cod3_init.cpp), which tells a function's start
  from an address inside it.
It also lists conflicts: two names for one address, two addresses for one
name, addresses.h against the symbol map, and names.toml against the symbol
map. Only the output file is written; --check prints the conflicts instead
and returns 1 when one of them would break the build (a hook's name missing
from names.toml's source).
"""
from __future__ import annotations

import argparse
import bisect
import re
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

# Fallback code range of default.xex (REX_CODE_BASE / REX_CODE_SIZE in the
# generated cod3_pch.h); used only when the generated code isn't there.
CODE_BASE = 0x820A0000
CODE_END = 0x820A0000 + 0x4FB1B8

IDENT = r"[A-Za-z_~][A-Za-z0-9_]*(?:::~?[A-Za-z_][A-Za-z0-9_]*)?"
ADDR = r"0x(8[0-9A-Fa-f]{7})(?![0-9A-Fa-f])"
NOT_WORD_BEFORE = r"(?<![\w:~])"
# A name next to an address, in comments and notes:
#   Name (0x82..)   Name 0x82..   `Name` `0x82..`   0x82.. = Name   0x82.. (Name)
NAME_ADDR_PATTERNS = [
    (re.compile(NOT_WORD_BEFORE + r"`?(" + IDENT + r")`?\s*\(\s*`?\*?" + ADDR), "name-first"),
    (re.compile(NOT_WORD_BEFORE + r"`?(" + IDENT + r")`?\s+`?" + ADDR), "name-first"),
    (re.compile(r"(?<![\w])`?" + ADDR + r"`?\s*=\s*`?(" + IDENT + r")\b"), "addr-first"),
    (re.compile(r"(?<![\w])`?" + ADDR + r"`?\s*\(\s*`(" + IDENT + r")`\s*\)"), "addr-first"),
]
CPP_WORDS = set("""
const char int void float double bool unsigned signed long short struct class return this auto
static inline constexpr uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t
qboolean true false nullptr sizeof namespace
""".split())

SUBSYSTEMS = [
    ("core", "Core: dvars, commands, files, printing (qcommon)"),
    ("client", "Client: input, connection, screen"),
    ("cgame", "Client game (cgame): view and HUD"),
    ("server", "Server"),
    ("game", "Game: entities, rules, cheats"),
    ("shared", "Shared game code and math (bg, pmove)"),
    ("renderer", "Renderer and ngl"),
    ("assets", "Assets and archives"),
    ("frontend", "Front end: menus and message boxes"),
    ("movies", "Movies and loading screens"),
    ("hud", "In-game overlay widgets (IGO)"),
    ("interact", "Interactions"),
    ("effects", "Effects and animation"),
    ("scripts", "Level scripts (Broc, script threads)"),
    ("world", "World: scene, entities, AI paths, vegetation"),
    ("system", "System: kernel and XAM imports, C runtime, jobs"),
    ("classes", "Other classes (from RTTI)"),
    ("other", "Other"),
]
SUBSYSTEM_TITLE = dict(SUBSYSTEMS)

# addresses.h namespaces -> subsystem
NAMESPACE_SUBSYSTEM = {
    "": "core", "cvar": "core", "cvar_flag": "core", "controls": "frontend", "crosshair": "cgame",
    "cheats": "game", "fe": "frontend", "fe::adv": "frontend", "fe::options": "frontend",
    "video": "renderer", "movie": "movies", "jobs": "system", "ultrawide": "renderer", "mp": "system",
    "msgbox": "frontend", "live_video": "renderer", "pc_menu": "frontend", "main_menu": "frontend",
    "storage": "frontend", "shadows": "renderer", "prompts": "frontend",
}
CRT_NAMES = set("""
sprintf vsprintf sscanf sin cos strncmp strstr atoi atof tolower toupper isalpha srand rand strchr
fflush strncpy fmod _fsopen fopen fread fread_s fwrite ftell fseek fclose memmove printf strcpy
memset longjmp setjmp memcpy _vsnprintf strtol abs _stricmp nullsub operator_new operator_delete
QueryPerformanceCounter BaseSetLastNTError
""".split())
# Quake III names without a subsystem prefix (g_*.c / qcommon), so the prefix
# rules below can't place them.
Q3_GAME_NAMES = set("""
ConcatArgs ConsoleCommand InitTrigger InitMover SelectSpawnPoint SelectRandomFurthestSpawnPoint
FinishSpawningItem LaunchItem SetClientViewAngle MagicGrenade
""".split())
Q3_CORE_NAMES = {"generateHashValue"}


# ---------------------------------------------------------------------------
# Reading files
# ---------------------------------------------------------------------------
def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def parse_value(v: str):
    v = v.strip()
    if v.startswith('"'):
        # A basic string: up to the closing quote, with \\ \" \n \t escapes.
        out, i = [], 1
        while i < len(v) and v[i] != '"':
            if v[i] == "\\" and i + 1 < len(v):
                i += 1
                out.append({"n": "\n", "t": "\t"}.get(v[i], v[i]))
            else:
                out.append(v[i])
            i += 1
        return "".join(out)
    if v in ("true", "false"):
        return v == "true"
    if v.startswith("["):
        return re.findall(r'"([^"]*)"', v)
    if v.startswith("{"):
        return v
    try:
        return int(v.split("#")[0].strip(), 0)
    except ValueError:
        return v


def load_toml(path: Path) -> dict:
    """TOML via tomllib (Python 3.11+), else a reader for the simple layout
    our files use (tables, arrays of tables, one key = value per line)."""
    try:
        import tomllib
        with open(path, "rb") as f:
            return tomllib.load(f)
    except ModuleNotFoundError:
        pass
    except (OSError, ValueError):
        return {}
    root: dict = {}
    cur = root
    for raw in read_text(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"\[\[(.+)\]\]$", line)
        if m:
            cur = {}
            root.setdefault(m.group(1).strip(), []).append(cur)
            continue
        m = re.match(r"\[(.+)\]$", line)
        if m:
            cur = root.setdefault(m.group(1).strip(), {})
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            v = v.strip()
            if not v.startswith(('"', "[", "{")) and "#" in v:
                v = v.split("#", 1)[0]
            cur[k.strip().strip('"')] = parse_value(v)
    return root


def as_int(v) -> int:
    return v if isinstance(v, int) else int(str(v), 0)


def rel(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.name


def norm(name: str) -> str:
    """One spelling per name: Class::method and Class__method alike, and the
    codegen's _guest suffix (the game's own copy of a C library function)."""
    n = name.replace("::", "__")
    return n[:-6] if n.endswith("_guest") else n


def is_placeholder(name: str) -> bool:
    return bool(re.match(r"^(sub_[0-9A-Fa-f]{8}|nullsub|vfunc_\d+)$", name)
                or re.search(r"(::|__)vfunc_\d+$", name))


def plausible_args(inner: str) -> bool:
    """Looks like a parameter list ("this, panel", "const char* name", "f1 = dt"),
    not prose in parentheses ("hooked", "prints \"...\"")."""
    inner = inner.strip()
    if inner in ("", "void"):
        return True
    if re.search(r"[\"'`]|0x|^(hooked|Q3)$", inner):
        return False
    return all(re.fullmatch(r"[\w\s*&:\[\].=?+-]+", p.strip()) for p in inner.split(","))


def good_name(n: str) -> bool:
    """Looks like a function name, not a word of prose."""
    base = n.split("::")[-1]
    if n in CPP_WORDS or base in CPP_WORDS or n.startswith(("sub_", "loc_", "__imp__", "0x")):
        return False
    if re.match(r"^k[A-Z]", base) or re.match(r"^x[0-9A-Fa-f]+$", n):
        return False
    if not re.search(r"[a-z]", n):
        return False
    upper = len(re.findall(r"[A-Z]", n))
    return "_" in n or "::" in n or upper >= 2 or bool(re.match(r"^[a-z]+[A-Z]", n))


def clean(text: str, limit: int = 0) -> str:
    text = re.sub(r"\s+", " ", text.replace("`", "")).strip().strip(";,.: -")
    if limit and len(text) > limit:
        cut = text[:limit].rsplit(" ", 1)[0]
        text = cut.rstrip(",;:(") + " ..."
    return text


def md(text: str) -> str:
    """Plain text for a table cell."""
    return (text.replace("\\", "\\\\").replace("|", "\\|").replace("<", "&lt;").replace(">", "&gt;")
            .replace("*", "\\*").replace("_", "\\_") if text else "")


def code(text: str) -> str:
    return "`" + text.replace("`", "'").replace("|", "\\|") + "`" if text else ""


# ---------------------------------------------------------------------------
# The catalog
# ---------------------------------------------------------------------------
class Catalog:
    def __init__(self):
        self.sym: dict[int, dict] = {}                      # symbol map functions
        self.sym_by_name: dict[str, list[int]] = defaultdict(list)
        self.names: dict[int, str] = {}                     # names.toml
        self.names_header = ""
        self.gen: dict[int, str] = {}                       # codegen table: start -> name
        self.starts: list[int] = []
        self.code_range = (CODE_BASE, CODE_END)
        self.consts: dict[tuple[str, str], dict] = {}       # addresses.h (path, name) -> info
        self.local_consts: dict[str, dict[str, dict]] = {}  # file -> name -> info
        self.mentions: dict[int, list[dict]] = defaultdict(list)  # names next to addresses
        self.uses: dict[int, list[tuple[str, str]]] = defaultdict(list)  # (kind, where)
        self.api: dict[int, list[dict]] = defaultdict(list)         # game_api.h wrappers
        self.midasm: list[dict] = []
        self.engine_doc_names: set[str] = set()
        self.unresolved_hooks: list[tuple[str, str]] = []
        self.sources_read: list[str] = []

    # --- helpers ---------------------------------------------------------
    def is_code(self, a: int) -> bool:
        return self.code_range[0] <= a < self.code_range[1]

    def is_start(self, a: int) -> bool:
        if self.starts:
            return a in self.gen
        return a in self.sym or a in self.names

    def containing(self, a: int) -> int | None:
        if not self.starts:
            keys = sorted(self.sym)
            i = bisect.bisect_right(keys, a) - 1
            return keys[i] if i >= 0 else None
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else None

    def codegen_name(self, a: int) -> str:
        if a in self.gen:
            return self.gen[a]
        return self.names.get(a, f"sub_{a:08X}")

    def resolve_codegen(self, name: str) -> int | None:
        m = re.match(r"^sub_([0-9A-Fa-f]{8})$", name)
        if m:
            return int(m.group(1), 16)
        for table in (self.names, self.gen):
            for a, n in table.items():
                if n == name:
                    return a
        return None

    # --- inputs ----------------------------------------------------------
    def read_symbols(self, path: Path):
        data = load_toml(path)
        for f in data.get("function", []):
            a = as_int(f["address"])
            self.sym[a] = {"name": str(f.get("name", "")), "cname": str(f.get("cname", "") or ""),
                           "confidence": str(f.get("confidence", "")), "evidence": str(f.get("evidence", ""))}
            self.sym_by_name[norm(self.sym[a]["name"])].append(a)
        if self.sym:
            self.sources_read.append("symbol map")

    def read_names(self, path: Path):
        data = load_toml(path)
        for k, v in data.get("names", {}).items():
            self.names[as_int(k)] = str(v)
        for line in read_text(path).splitlines()[:5]:
            if "functions;" in line:
                self.names_header = line.lstrip("# ").strip()

    def read_generated(self, init_cpp: Path):
        text = read_text(init_cpp)
        for m in re.finditer(r"\{ 0x([0-9A-Fa-f]{8}), (\w+) \}", text):
            self.gen.setdefault(int(m.group(1), 16), m.group(2))
        self.starts = sorted(self.gen)
        pch = read_text(init_cpp.parent / "cod3_pch.h")
        b = re.search(r"#define REX_CODE_BASE (0x[0-9A-Fa-f]+)", pch)
        s = re.search(r"#define REX_CODE_SIZE (0x[0-9A-Fa-f]+)", pch)
        if b and s:
            self.code_range = (int(b.group(1), 16), int(b.group(1), 16) + int(s.group(1), 16))
        if self.gen:
            self.sources_read.append("codegen function table")

    def read_addresses(self, path: Path, where: str):
        ns: list[str] = []
        prev_comment: list[str] = []
        for raw in read_text(path).splitlines():
            line = raw.strip()
            m = re.match(r"namespace ([\w:]+) \{", line)
            if m:
                ns.append(m.group(1))
                prev_comment = []
                continue
            if re.match(r"\}\s*//\s*namespace", line):
                if ns:
                    ns.pop()
                prev_comment = []
                continue
            if line.startswith("//"):
                body = line[2:].strip()
                if re.fullmatch(r"[-=]{3,}", body):
                    prev_comment = []  # a section rule: what came before was a heading
                    continue
                prev_comment.append(body)
                continue
            m = re.match(r"inline constexpr uint32_t (k\w+) = 0x([0-9A-Fa-f]{8});\s*(?://\s*(.*))?$", line)
            if m:
                path_ns = "::".join(n for n in ns if n not in ("cod3::addr", "cod3", "addr"))
                info = {"name": m.group(1), "ns": path_ns, "value": int(m.group(2), 16),
                        "trailing": (m.group(3) or "").strip(), "before": " ".join(prev_comment)}
                self.consts[(path_ns, m.group(1))] = info
            if line:
                prev_comment = []
        self.sources_read.append("addresses.h")
        self.scan_mentions(path, where, comments_only=True)

    def const_label(self, info: dict) -> str:
        return (info["ns"] + "::" if info["ns"] else "") + info["name"]

    def scan_mentions(self, path: Path, where: str, comments_only: bool):
        """Names next to code addresses (see NAME_ADDR_PATTERNS)."""
        for raw in read_text(path).splitlines():
            text = raw
            if comments_only:
                i = raw.find("//")
                text = raw[i + 2:] if i >= 0 else ""
            for pat, order in NAME_ADDR_PATTERNS:
                for m in pat.finditer(text):
                    name, a = (m.group(1), m.group(2)) if order == "name-first" else (m.group(2), m.group(1))
                    a = int(a, 16)
                    if not good_name(name) or not self.is_code(a):
                        continue
                    # The words after the address say what it does, unless they
                    # close an outer sentence ("... Name 0x82..), the ...").
                    rest = text[m.end():].lstrip("`* ")
                    if "(" in m.group(0) and rest.startswith(")"):
                        rest = rest[1:]
                    elif rest[:1] in (")", "]", ",", ";"):
                        rest = ""
                    after = clean(rest, 110)
                    self.mentions[a].append({"name": name, "where": where, "text": after})

    def read_api(self, path: Path, where: str):
        """game_api.h: a comment naming "<function> 0x<address>: <signature>. <purpose>"
        above each wrapper declaration."""
        block: list[str] = []
        lines = read_text(path).splitlines()
        i = 0
        while i < len(lines):
            line = lines[i].strip()
            if line.startswith("//"):
                block.append(line[2:].strip())
                i += 1
                continue
            m = re.match(r"(?:[\w:<>]+[\s*&]+)+(\w+)\(", line)
            if m and block and not line.startswith(("namespace", "#", "struct")):
                decl = line
                while not decl.rstrip().endswith(";") and i + 1 < len(lines):
                    i += 1
                    decl += " " + lines[i].strip()
                comment = " ".join(block)
                for cm in re.finditer(r"(" + IDENT + r") " + ADDR + r"(?::\s*([^.]*(?:\.[^ ][^.]*)*)\.?)?",
                                      comment):
                    a = int(cm.group(2), 16)
                    sig = ""
                    tail = comment[cm.start():]
                    sm = re.match(IDENT + r" " + ADDR + r":\s*([\w:*& ]*\([^()]*\))", tail)
                    if sm:
                        sig = sm.group(2).strip()
                    purpose = ""
                    pm = re.match(IDENT + r" " + ADDR + r":\s*[\w:*& ]*\([^()]*\)\.?\s*(.*)", tail)
                    if pm:
                        purpose = pm.group(2)
                        nxt = re.search(r"\b" + IDENT + r" 0x8[0-9A-Fa-f]{7}:", purpose)
                        if nxt:
                            purpose = purpose[:nxt.start()]
                    self.api[a].append({"label": cm.group(1), "wrapper": m.group(1), "sig": sig,
                                        "purpose": clean(purpose, 140), "where": where})
                    if not re.match(r"^(?:\w+::)*k[A-Z]", cm.group(1)) and good_name(cm.group(1)):
                        self.mentions[a].append({"name": cm.group(1), "where": where, "text": ""})
            if not line.startswith("//"):
                block = [] if line or not block else block
            i += 1

    def read_source_tree(self, src: Path, root: Path, midasm_handlers: set[str]):
        """Hooks, extern calls, guest calls and constant uses in the port's code."""
        files = sorted(p for p in src.rglob("*") if p.suffix in (".cpp", ".h") and p.is_file())
        reverse_const: dict[str, list[dict]] = defaultdict(list)
        for info in self.consts.values():
            reverse_const[info["name"]].append(info)
        for path in files:
            where = rel(path, root)
            if path.name == "addresses.h":
                continue
            text = read_text(path)
            aliases = {"a": ""}
            for m in re.finditer(r"namespace (\w+)\s*=\s*(?:::)?(?:cod3::)?addr(?:::([\w:]+))?\s*;", text):
                aliases[m.group(1)] = m.group(2) or ""
            local = {}
            for m in re.finditer(r"constexpr uint32_t (k\w+) = 0x([0-9A-Fa-f]{8});\s*(?://\s*(.*))?", text):
                local[m.group(1)] = {"name": m.group(1), "ns": "", "value": int(m.group(2), 16),
                                     "trailing": (m.group(3) or "").strip(), "before": ""}
            if local:
                self.local_consts[where] = local

            def resolve(qual: str, name: str):
                parts = [p for p in qual.split("::") if p and p not in ("cod3", "addr")]
                if parts and parts[0] in aliases:
                    parts = [p for p in aliases[parts[0]].split("::") if p] + parts[1:]
                path_ns = "::".join(parts)
                if (path_ns, name) in self.consts:
                    return self.consts[(path_ns, name)]
                if not qual and name in local:
                    return local[name]
                cands = reverse_const.get(name, [])
                if len(cands) == 1 and (not path_ns or cands[0]["ns"].endswith(path_ns)):
                    return cands[0]
                return None

            block: list[str] = []  # the comment lines right above (REX_EXTERN lines skipped)
            for line in text.splitlines():
                code_part = line.split("//", 1)[0]
                stripped = line.strip()
                for m in re.finditer(r"REX_HOOK_RAW\((\w+)\)", code_part):
                    a = self.resolve_codegen(m.group(1))
                    if a is None:
                        self.unresolved_hooks.append((m.group(1), where))
                        continue
                    self.uses[a].append(("hook", where))
                    # "// Class::Method: what the hook does" above a hook names the
                    # function (the rest describes the hook, not the function).
                    hm = re.match(r"^(" + IDENT + r")\s*(?:\(|:(?!:))", block[0]) if block else None
                    if hm and ("::" in hm.group(1) or "_" in hm.group(1)) and good_name(hm.group(1)) \
                            and not is_placeholder(hm.group(1)):
                        self.mentions[a].append({"name": hm.group(1), "where": where, "text": ""})
                if stripped.startswith("//"):
                    block.append(stripped[2:].strip())
                elif stripped and not stripped.startswith("REX_EXTERN"):
                    block = []
                for m in re.finditer(r"REX_EXTERN\((\w+)\)", code_part):
                    if m.group(1).startswith("__imp__"):
                        continue
                    a = self.resolve_codegen(m.group(1))
                    if a is not None:
                        self.uses[a].append(("calls", where))
                callee = ""
                im = re.search(r"Invoke\(\s*([^,{)]+)", code_part)
                if im and "VirtualSlot" not in im.group(1):
                    callee = im.group(1)
                for m in re.finditer(r"\b((?:[A-Za-z_]\w*::)*)(k[A-Z]\w*)\b", code_part):
                    info = resolve(m.group(1), m.group(2))
                    if not info or not self.is_code(info["value"]):
                        continue
                    kind = "calls" if m.group(0) in callee else "refers to"
                    if where.endswith("game_api.cpp") and kind == "calls":
                        kind = "typed call"
                    self.uses[info["value"]].append((kind, where))
                for name in midasm_handlers:
                    if re.search(r"\b(?:void|bool|int)\s+" + re.escape(name) + r"\s*\(", code_part):
                        for h in self.midasm:
                            if h["name"] == name and where not in h["handlers"]:
                                h["handlers"].append(where)
            self.scan_mentions(path, where, comments_only=True)

    def read_configs(self, config: Path, root: Path):
        for path in sorted(config.glob("*.toml")):
            if path.name in ("symbols.toml", "names.toml"):
                continue
            where = rel(path, root)
            data = load_toml(path)
            module = "default.xex" if path.stem == "default" else path.stem + ".dll"
            for h in data.get("midasm_hook", []) or []:
                a = as_int(h.get("address", 0))
                self.midasm.append({"address": a, "name": str(h.get("name", "")), "module": module,
                                    "config": where, "handlers": [],
                                    "jump": as_int(h["jump_address_on_true"]) if "jump_address_on_true" in h else None})
            if "setjmp_address" in data:
                self.uses[as_int(data["setjmp_address"])].append(("setjmp_address", where))
            self.scan_mentions(path, where, comments_only=False)
        # Mid-asm hooks name the function they sit in.
        for h in self.midasm:
            if h["module"] != "default.xex":
                continue
            start = self.containing(h["address"])
            h["function"] = start
            if start is not None:
                self.uses[start].append(("mid-asm " + h["name"], h["config"]))

    def read_notes(self, notes: Path, skip: set[str]):
        """Names next to addresses in the helpers' notes; `skip` names folders
        left out (this tool's own notes repeat what it found)."""
        files = sorted(notes.glob("*/NOTES*.md")) + sorted(notes.glob("*/NOTES*.txt"))
        files = [p for p in files if p.parent.name not in skip]
        for path in files:
            self.scan_mentions(path, "notes/" + path.parent.name, comments_only=False)
        if files:
            self.sources_read.append(f"{len(files)} helper notes")

    def read_engine_doc(self, path: Path):
        text = read_text(path)
        self.engine_doc_names = set(re.findall(r"`(" + IDENT + r")`", text))
        self.scan_mentions(path, "docs/ENGINE.md", comments_only=False)
        if text:
            self.sources_read.append("docs/ENGINE.md")

    # --- building entries ------------------------------------------------
    def const_hint(self, info: dict) -> str:
        """The function name an addresses.h comment gives a constant, if any:
        a comment that starts with "Name(" or "Name:" (or "type Name(").
        "Class(this)" is that class's constructor, Class::ctor."""
        for text in (info["trailing"], info["before"]):
            m = re.match(r"^(?:[\w:]+\*?\s+)?(" + IDENT + r")\s*(\(|:(?!:))(\s*k[A-Z])?", text)
            if not m or m.group(3) or not good_name(m.group(1)) or is_placeholder(m.group(1)):
                continue
            name = m.group(1)
            if re.fullmatch(r"[A-Z][A-Za-z0-9]*", name) and re.match(r"\(\s*this\b", text[m.end(1):].lstrip()):
                name += "::ctor"
            return name
        return ""

    def mentions_at(self, a: int) -> list[dict]:
        if not self.is_start(a) and self.starts:
            return []
        return self.mentions.get(a, [])

    def entries(self) -> dict[int, dict]:
        out: dict[int, dict] = {}
        const_by_addr: dict[int, list[dict]] = defaultdict(list)
        for info in self.consts.values():
            if self.is_code(info["value"]):
                const_by_addr[info["value"]].append(info)
        candidates = set(self.sym) | set(self.names) | set(const_by_addr) | set(self.api) | set(self.uses)
        candidates |= {a for a in self.mentions if self.is_start(a)}
        for a in sorted(candidates):
            if not self.is_code(a):
                continue
            if self.starts and a not in self.gen and a not in self.sym:
                continue  # an instruction address (mid-asm hooks, call sites)
            s = self.sym.get(a)
            consts = const_by_addr.get(a, [])
            names: list[tuple[str, str]] = []  # (name, source)
            if s:
                names.append((s["name"], "map:" + s["confidence"]))
            if a in self.names:
                names.append((self.names[a], "names.toml"))
            for info in consts:
                hint = self.const_hint(info)
                if hint:
                    names.append((hint, "addresses.h"))
            for mnt in self.mentions_at(a):
                names.append((mnt["name"], mnt["where"]))
            if not names and not consts and not self.uses.get(a) and not self.api.get(a):
                continue
            e = {"address": a, "sym": s, "consts": consts, "names": names, "api": self.api.get(a, []),
                 "uses": sorted(set(self.uses.get(a, []))), "codegen": self.codegen_name(a)}
            e["name"], e["likely"] = self.display_name(e)
            e["subsystem"] = self.classify(e)
            e["purpose"] = self.purpose(e)
            e["signature"] = self.signature(e)
            e["sources"] = self.sources(e)
            out[a] = e
        return out

    def display_name(self, e: dict) -> tuple[str, bool]:
        s = e["sym"]
        if s and s["confidence"] in ("known", "high"):
            return s["name"], False
        if e["address"] in self.names:
            return self.names[e["address"]], False
        if s:
            return s["name"], True
        for n, src in e["names"]:
            if is_placeholder(n):
                continue
            # A note's name that the symbol map firmly gives another address is
            # a slip (listed under Conflicts), not this function's name.
            if any(t != e["address"] and self.sym[t]["confidence"] in ("known", "high")
                   for t in self.sym_by_name.get(norm(n), [])):
                continue
            return n, src != "addresses.h" and not src.endswith("game_api.h")
        if e["consts"]:
            return self.const_label(e["consts"][0]), False
        return f"sub_{e['address']:08X}", False

    def classify(self, e: dict) -> str:
        name = e["name"]
        ev = e["sym"]["evidence"] if e["sym"] else ""
        if e["consts"] and name == self.const_label(e["consts"][0]):
            # Known only as an addresses.h constant: its namespace says where it belongs.
            for c in e["consts"]:
                if NAMESPACE_SUBSYSTEM.get(c["ns"]):
                    return NAMESPACE_SUBSYSTEM[c["ns"]]
        cls = name.split("::")[0] if "::" in name else ""
        if e["sym"] and (ev.startswith(("import", "register")) or name.startswith("__")):
            return "system"
        if norm(name) in CRT_NAMES or name.endswith(("_wrapper", "_thunk")) or \
                name.startswith(("Xam", "Ke", "Rtl", "Mm", "Nt")) or re.match(r"^X[A-Z][a-z]\w*$", name) or \
                ev.startswith(("XAPI wrapper", "a lone 'b ")):
            return "system"
        if name in Q3_GAME_NAMES:
            return "game"
        if name in Q3_CORE_NAMES:
            return "core"
        prefix_map = [
            (("Com_", "COM_", "Cbuf_", "Cvar_", "FS_", "Info_", "Q_", "Z_", "Sys_", "NET_", "Netchan_", "MSG_",
              "Swap_", "VM_", "Huff", "CopyString", "va", "Field_", "Hunk_", "NameHash"), "core"),
            (("CL_", "IN_", "Key_", "SCR_", "Con_", "cl_", "UI_"), "client"),
            (("CG_", "cg_"), "cgame"),
            (("SV_", "Svcmd_"), "server"),
            (("G_", "Client", "CheatsOk", "vmMain", "SP_", "player_", "Touch_", "Respawn", "Bullet_", "Drop_",
              "Toss", "Spot", "Reached_", "Actor_", "Cheats"), "game"),
            (("BG_", "PM_", "Pmove", "Angle", "Vector", "Vec2", "Distance", "vectoangles", "vtos", "Radius",
              "CM_"), "shared"),
            (("R_", "RE_", "ngl", "GetRefAPI", "D3D", "tl", "AssertCvarRange"), "renderer"),
            (("EffectEvent",), "effects"),
            (("AssetLoad_", "AssetUnload_", "AssetType_", "AssetChunk_", "inflate"), "assets"),
            (("Broc_", "ScriptThread_", "Bro"), "scripts"),
            (("Localize", "Profile_", "ControlsPage_"), "frontend"),
            (("FX_",), "effects"),
        ]
        if name.startswith("Cmd_") and name.endswith("_f"):
            if "client command" in ev or any(c["ns"] == "cheats" for c in e["consts"]):
                return "game"
            return "server" if "server" in ev else "core"
        if name.startswith("Cmd_"):
            return "core"
        for prefixes, sub in prefix_map:
            if not cls and name.startswith(prefixes):
                return sub
        if cls:
            if cls.startswith("IGO"):
                return "hud"
            if cls.startswith("Interact"):
                return "interact"
            if re.search(r"Effect|Anim|^nal|NoteTrack|^aps|^Particle|Rumble|Sound", cls):
                return "effects"
            if re.search(r"^AeThread|^WaitTil|^Task|Script|Broc", cls):
                return "scripts"
            if re.search(r"Menu|^FE|Panel|Dialog|^String|Font|Button", cls):
                return "frontend"
            if re.search(r"Asset|Bank", cls):
                return "assets"
            if re.search(r"Scene|Entity|PathNode|LightGrid|Grass|SpeedTree|Spline|Node", cls):
                return "world"
            if re.search(r"^ngl|^D3D|Render|Texture|Shader", cls):
                return "renderer"
            return "classes"
        if ev.startswith("level slot"):
            return "scripts"
        for c in e["consts"]:
            sub = NAMESPACE_SUBSYSTEM.get(c["ns"])
            if sub:
                return sub
        return "other"

    def purpose(self, e: dict) -> str:
        for api in e["api"]:
            if api["purpose"]:
                return api["purpose"]
        for c in e["consts"]:
            text = c["trailing"] or c["before"]
            if text:
                return clean(text, 160)
        s = e["sym"]
        if s and s["evidence"]:
            ev = s["evidence"]
            ev = re.sub(r"^rtti \S+ (vtable slot \d+)", r"\1", ev)
            return clean(ev, 160)
        for mnt in self.mentions_at(e["address"]):
            if mnt["text"]:
                return clean(mnt["text"], 120)
        return ""

    def signature(self, e: dict) -> str:
        for api in e["api"]:
            if api["sig"]:
                return api["sig"]
        names = {norm(n) for n, _ in e["names"]}
        names |= {n[:-len("__ctor")] for n in names if n.endswith("__ctor")}  # "FEText(this)"
        texts = [t for c in e["consts"] for t in (c["trailing"], c["before"]) if t]
        for text in texts:
            for m in re.finditer(r"((?:[\w:]+\*?\s+)?)(" + IDENT + r")\s*\(([^()]*)\)", text):
                if norm(m.group(2)) in names and plausible_args(m.group(3)):
                    ret = m.group(1).strip()
                    if ret.endswith(":") or ret in CPP_WORDS - {"void", "int", "float", "bool", "qboolean"}:
                        ret = ""  # "Level start: Name (...)" is prose, not a return type
                    return (ret + " " if ret else "") + "(" + m.group(3).strip() + ")"
            m = re.match(r"^\s*(?:vtbl\[\d+\]|vfunc_\d+)?\s*\(([^()]*)\)", text)
            if m and plausible_args(m.group(1)):
                return "(" + m.group(1).strip() + ")"
        s = e["sym"]
        if s:
            m = re.search(r"vtable slot \d+: \(([^()]*)\)", s["evidence"])
            if m and plausible_args(m.group(1)):
                return "(" + m.group(1).strip() + ")"
            # "Name(a, b)" in the evidence; an empty "Name()" is usually a quoted
            # message ("SetCamera() requires ..."), not the arguments.
            m = re.search(re.escape(s["name"].split("::")[-1]) + r"\s*\(([^()]*)\)", s["evidence"])
            if m and m.group(1).strip() and plausible_args(m.group(1)):
                return "(" + m.group(1).strip() + ")"
        return ""

    def sources(self, e: dict) -> list[str]:
        out = []
        if e["sym"]:
            out.append("map:" + e["sym"]["confidence"])
        if e["address"] in self.names:
            out.append("names.toml")
        if e["consts"]:
            out.append("addresses.h")
        if e["api"]:
            out.append("game_api.h")
        notes = sorted({m["where"].split("/", 1)[1] for m in self.mentions_at(e["address"])
                        if m["where"].startswith("notes/")})
        if notes:
            out.append("notes: " + ", ".join(notes))
        srcs = sorted({m["where"] for m in self.mentions_at(e["address"]) if m["where"].startswith("src/")})
        if srcs:
            out.append("comments")
        names = {norm(n) for n, _ in e["names"]}
        if names & {norm(n) for n in self.engine_doc_names}:
            out.append("ENGINE.md")
        return out

    # --- conflicts -------------------------------------------------------
    def conflicts(self, entries: dict[int, dict]) -> dict[str, list[str]]:
        c: dict[str, list[str]] = defaultdict(list)
        # names.toml against the symbol map (what tools/gen_codegen_names.py would write)
        expected = {}
        for a, s in self.sym.items():
            if s["confidence"] not in ("known", "high"):
                continue
            n = s["cname"] or s["name"]
            if n.startswith("__") or s["evidence"].startswith(("import", "register ")):
                continue
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", n) or n.startswith("sub_"):
                continue
            expected[a] = n
        hooked = {a for a, us in self.uses.items() if any(k == "hook" or k == "calls" for k, _ in us)}
        for a, n in sorted(self.names.items()):
            if a not in expected:
                s = self.sym.get(a)
                users = sorted({w for k, w in self.uses.get(a, []) if k in ("hook", "calls")})
                why = (f"the symbol map has it as {code(s['name'])} ({s['confidence']})" if s
                       else "not in the symbol map")
                c["names"].append(
                    f"{code(f'0x{a:08X}')} {code(n)}: only in names.toml ({why}); regenerating names.toml "
                    f"drops it" + (f", and {', '.join(users)} use it by that name" if users else "")
                    + ". Fix: add it to KNOWN_FUNCTIONS in tools/name_functions.py.")
            elif norm(n) != norm(expected[a]):
                c["names"].append(f"{code(f'0x{a:08X}')}: names.toml {code(n)}, symbol map {code(expected[a])}")
        missing = [a for a in expected if a not in self.names]
        if missing:
            c["names"].append(f"{len(missing)} names of the symbol map are not in names.toml, e.g. " +
                              ", ".join(code(expected[a]) for a in missing[:5]) +
                              ": names.toml is older than the symbol map.")
        if self.gen:
            stale = [a for a, n in self.names.items() if a in self.gen and self.gen[a] != n]
            if stale:
                c["names"].append(f"{len(stale)} names.toml names are not in the generated code yet (run "
                                  f"the codegen), e.g. " + ", ".join(code(self.names[a]) for a in stale[:5]))
        # Two names for one address
        for a, e in sorted(entries.items()):
            distinct: dict[str, set[str]] = defaultdict(set)
            for n, src in e["names"]:
                if is_placeholder(n):
                    continue
                distinct[norm(n)].add(src if not src.startswith("map:") else "symbol map")
            # "X_thunk" (the symbol map: a lone jump to X) and "X" name the same thing.
            for n in [n for n in distinct if n.endswith("_thunk") and n[:-6] in distinct]:
                distinct[n[:-6]] |= distinct.pop(n)
            if len(distinct) > 1:
                parts = [f"{code(n)} ({', '.join(sorted(srcs))})" for n, srcs in sorted(distinct.items())]
                line = f"{code(f'0x{a:08X}')}: " + "; ".join(parts)
                if any("addresses.h" in s for s in distinct.values() for s in s) or \
                        any(src == "addresses.h" for srcs in distinct.values() for src in srcs):
                    c["addresses"].append(line)
                else:
                    c["two_names"].append(line)
        # Placeholder names with a better one elsewhere
        for a, e in sorted(entries.items()):
            s = e["sym"]
            if not s or not is_placeholder(s["name"]):
                continue
            better = sorted({n for n, src in e["names"] if not is_placeholder(n) and not src.startswith("map:")})
            if better:
                c["better"].append(f"{code(f'0x{a:08X}')} {code(s['name'])}: also called " +
                                   ", ".join(code(b) for b in better))
        # Two addresses for one name
        by_name: dict[str, dict[int, set[str]]] = defaultdict(lambda: defaultdict(set))
        for a, e in entries.items():
            for n, src in e["names"]:
                if not is_placeholder(n):
                    by_name[norm(n)][a].add("symbol map" if src.startswith("map:") else src)
        for n, addrs in sorted(by_name.items()):
            if len(addrs) > 1:
                parts = [f"{code(f'0x{a:08X}')} ({', '.join(sorted(s))})" for a, s in sorted(addrs.items())]
                c["two_addresses"].append(f"{code(n)}: " + "; ".join(parts))
        # addresses.h against the code and the symbol map
        for info in sorted(self.consts.values(), key=lambda i: i["value"]):
            a = info["value"]
            if not self.is_code(a) or not self.starts:
                continue
            label = code(self.const_label(info))
            if a not in self.gen:
                about = (info["name"] + " " + info["trailing"] + " " + info["before"]).lower()
                if not re.search(r"insn|call|hook|instruction|li r\d", about):
                    start = self.containing(a)
                    c["addresses"].append(f"{label} = {code(f'0x{a:08X}')} is not a function start (inside "
                                          f"{code(self.codegen_name(start)) if start else '?'})")
                continue
            s = self.sym.get(a)
            # Only the constant's own (trailing) comment: a block above it often
            # describes the neighbors' slots too.
            m = re.search(r"vtbl\[(\d+)\]|vfunc_(\d+)", info["trailing"])
            if s and m:
                slot = int(m.group(1) or m.group(2))
                sm = re.search(r"vfunc_(\d+)$", s["name"]) or re.search(r"vtable slot (\d+)", s["evidence"])
                if sm and int(sm.group(1)) != slot:
                    c["addresses"].append(f"{label}: addresses.h says vtable slot {slot}, the symbol map "
                                          f"{code(s['name'])} slot {sm.group(1)}")
        for a, s in sorted(self.sym.items()):
            for m in re.finditer(r"addresses\.h ((?:\w+::)*k[A-Z]\w*)", s["evidence"]):
                parts = m.group(1).split("::")
                info = self.consts.get(("::".join(parts[:-1]), parts[-1]))
                if info is None:
                    cands = [i for i in self.consts.values() if i["name"] == parts[-1]]
                    info = cands[0] if len(cands) == 1 else None
                    if info is None and any(i["value"] == a for i in cands):
                        continue
                if info is None:
                    c["addresses"].append(f"{code(s['name'])} {code(f'0x{a:08X}')}: the symbol map cites "
                                          f"addresses.h {code(m.group(1))}, which isn't there any more")
                elif info["value"] != a:
                    c["addresses"].append(f"{code(s['name'])}: the symbol map has {code(f'0x{a:08X}')}, "
                                          f"addresses.h {code(self.const_label(info))} = "
                                          f"{code('0x%08X' % info['value'])}")
        # Several constants for one function
        by_value: dict[int, list[str]] = defaultdict(list)
        for info in self.consts.values():
            if self.is_code(info["value"]) and (not self.starts or info["value"] in self.gen):
                by_value[info["value"]].append(self.const_label(info))
        for a, labels in sorted(by_value.items()):
            if len(labels) > 1:
                c["duplicates"].append(f"{code(f'0x{a:08X}')}: " + ", ".join(code(l) for l in sorted(labels)))
        # Hooks on sub_ names that the symbol map names
        for a, us in sorted(self.uses.items()):
            if not any(k == "hook" for k, _ in us):
                continue
            cg = self.codegen_name(a)
            s = self.sym.get(a)
            if cg.startswith("sub_") and s and not is_placeholder(s["name"]):
                files = sorted({w for k, w in us if k == "hook"})
                c["sub_hooks"].append(f"{code(cg)} ({', '.join(files)}): the symbol map calls it "
                                      f"{code(s['name'])} ({s['confidence']})")
        for name, where in self.unresolved_hooks:
            c["errors"].append(f"{where}: REX_HOOK_RAW({name}) names no known function")
        return c


# ---------------------------------------------------------------------------
# Writing FUNCTIONS.md
# ---------------------------------------------------------------------------
def use_text(uses: list[tuple[str, str]], short: bool = False) -> str:
    by_kind: dict[str, list[str]] = defaultdict(list)
    for kind, where in uses:
        w = where.split("/")[-1] if short else where
        if w not in by_kind[kind]:
            by_kind[kind].append(w)
    order = ["hook", "typed call", "calls", "setjmp_address", "refers to"]
    kinds = sorted(by_kind, key=lambda k: (order.index(k) if k in order else 2, k))
    return "; ".join(f"{k} {', '.join(by_kind[k])}" for k in kinds)


def write_markdown(cat: Catalog, entries: dict[int, dict], conflicts: dict[str, list[str]], out: Path):
    L: list[str] = []
    add = L.append
    named = [e for e in entries.values() if not e["name"].startswith("sub_")]
    helpers = [e for e in named if e["sym"] and e["sym"]["evidence"].startswith("register")]
    listed = [e for e in named if e not in helpers]
    used = [e for e in entries.values() if e["uses"] or e["api"]]
    by_conf = defaultdict(int)
    for e in named:
        by_conf[e["sym"]["confidence"] if e["sym"] else "port"] += 1

    add("# Call of Duty 3 functions")
    add("")
    add("Every named function of `default.xex` in one list, with what the port does with it. Made by "
        "`tools/catalog_functions.py` from " + ", ".join(cat.sources_read) + ", `game_api.h` and the port's "
        "hooks; edit those and run the tool again (`python3 tools/catalog_functions.py`), not this file.")
    add("")
    add(f"{len(named)} named functions: {by_conf['known']} the port already relied on, {by_conf['high']} "
        f"certain and {by_conf['medium']} likely (the symbol map's confidence; see [ENGINE.md](ENGINE.md)), "
        f"plus {by_conf['port']} named only by the port (addresses.h, game_api.h, notes). "
        f"{len(used)} of them are hooked or called by the port.")
    add("")
    add("Columns: **codegen** is the C++ name the generated code gives the function, the name a "
        "`REX_HOOK_RAW` must use; **port use** is a hook (`REX_HOOK_RAW`), a mid-function hook "
        "(`config/*.toml`), a typed call (`game_api.h`), another call (`guest::Call` / `REX_EXTERN`) or a "
        "plain reference; **sources**: `map` the symbol map (known / high / medium), `names.toml` the "
        "codegen names, `addresses.h`, `game_api.h`, `notes` the helpers' working notes, `comments` the "
        "port's code comments, `ENGINE.md`.")
    add("")

    # --- calling functions ----------------------------------------------
    add("## Calling a game function from port code")
    add("")
    add("- **Use `src/game/game_api.h`.** One typed wrapper per function the port calls, named after it, "
        "with the address from `addresses.h` (one place). It replaces raw `guest::Call(...).Invoke(addr, "
        "{...})` calls. Game main thread only (a hook, or a task queued with `Engine::QueueGuestTask`). A "
        "call goes through the port's own hook of that function, as the game's own calls do.")
    add("- **Dvars:** write them with `Engine::CvarSet` (the cheat guard decides when `force` is allowed, "
        "then `game::CvarSet2`), read them with `game::CvarFind` and `game::CvarString` / `CvarValue` / "
        "`CvarInteger`. A dvar is never set by sending console text.")
    add("- **Console text** (`game::CbufAddText`) is for real console lines only (the console, "
        "`cod3_autoexec`): they need the game's parser, run in order at the next `Cbuf_Execute` and pass "
        "the cheat guard first.")
    add("- **Hooks:** `REX_HOOK_RAW(<codegen>)` and `REX_EXTERN(__imp__<codegen>)` for the original. A "
        "function without a name in `names.toml` is `sub_<address>`; when it gets a name, its hooks must "
        "be renamed (the list under Conflicts).")
    add("- **One instruction:** a `[[midasm_hook]]` in `config/default.toml` (the table below).")
    add("")
    api_rows = []
    for a, apis in sorted(cat.api.items()):
        e = entries.get(a)
        for api in apis:
            api_rows.append((api["wrapper"], e["name"] if e else api["label"], a, api["sig"]))
    if api_rows:
        add("| Wrapper (`cod3::game::`) | Game function | Address | Guest signature |")
        add("|---|---|---|---|")
        for wrapper, name, a, sig in sorted(api_rows, key=lambda r: (r[2], r[0])):
            add(f"| {code(wrapper)} | {code(name)} | {code(f'0x{a:08X}')} | {code(sig)} |")
        add("")
        add("The element and menu item wrappers (`ElementSet*`, `ItemSet*`) are virtual calls; their "
            "slots are in `addresses.h` (`fe::kElemSet*`, `fe::kItemSet*`).")
        add("")
    add("### Where a direct call replaces a slower path")
    add("")
    add("- Settings (F2, the game's own menus, the `fov` console command) already set dvars with a "
        "direct `Cvar_Set2` call, not a console line; nothing in the port sends console text for a value "
        "it could set directly. Console lines and `cod3_autoexec` stay text on purpose (see above).")
    add("- `Engine` (engine.cpp) and the cheat guard (cheat_guard.cpp) call the game through "
        "`game_api.h`, with the same calls and reads as before; the Cvar_Get hook passes its own name "
        "argument straight to `Cvar_FindVar` (`game::CvarFindGuest`), without a copy, as it did.")
    add("")
    add("Not changed, could be (nothing here was timed, so no gain is claimed; none changes game "
        "speed):")
    add("")
    add("- `cg_fov` (engine.cpp, `OnGuestFrame`) and `introScreen` (cutscene_skip.cpp) are looked up by "
        "name with `Cvar_FindVar` every frame until the game registers them (all through the menus); "
        "`cvar_t` pointers never move, so a lookup every 30th frame, like `SyncWatchedCvars` does, would do.")
    add("- `Engine::SyncWatchedCvars` reads both watched dvars' strings every frame, though only "
        "`cg_drawFPS` is needed every frame (the HUD); `com_maxfps` is compared only every 30th frame.")
    add("- `CheatWatch::Update` (every 10th frame while a cheat is on) copies each watched dvar's value "
        "and default into two `std::string`s to compare them; comparing the two guest strings in place "
        "would need no copy.")
    add("- Menu code (menu_fov.cpp, menu_video.cpp, pc_menu.cpp, multiplayer.cpp, button_prompts.cpp, "
        "cutscene_skip.cpp, render_resolution.cpp) still calls the same functions with raw "
        "`guest::Call` + `addresses.h` constants. Most of those calls have a `game_api.h` wrapper with "
        "the same arguments, so moving them over is a plain substitution, file by file, by each file's "
        "owner; the few without one (`storage::kPauseQuit`, `prompts::kStringFromChars`, "
        "`fe::adv::kSyncValueHighlight`, the page virtual calls) get one when their file moves.")
    add("")

    # --- conflicts ------------------------------------------------------
    add("## Conflicts")
    add("")
    sections = [
        ("errors", "Hooks that name no function"),
        ("names", "names.toml and the symbol map"),
        ("addresses", "addresses.h and the symbol map"),
        ("two_names", "Two names for one address"),
        ("two_addresses", "Two addresses for one name"),
        ("sub_hooks", "Hooks by sub_ name that the symbol map names (rename the hook if the name is "
                      "promoted to names.toml)"),
        ("better", "Placeholder names (vfunc_N) with a better name elsewhere"),
        ("duplicates", "One function under several addresses.h constants (fine, but worth knowing)"),
    ]
    for key, title in sections:
        items = conflicts.get(key, [])
        add(f"### {title} ({len(items)})")
        add("")
        if not items:
            add("None.")
        for item in items:
            add("- " + item)
        add("")

    # --- port use -------------------------------------------------------
    add(f"## Functions the port uses ({len(used)})")
    add("")
    add("| Address | Name | Codegen | Subsystem | What it does | Arguments | Port use | Sources |")
    add("|---|---|---|---|---|---|---|---|")
    for e in sorted(used, key=lambda e: (SUBSYSTEMS.index((e["subsystem"], SUBSYSTEM_TITLE[e["subsystem"]])),
                                         e["address"])):
        uses = use_text(e["uses"])
        if e["api"]:
            uses = "; ".join(filter(None, ["game_api " + ", ".join(sorted({x["wrapper"] for x in e["api"]})),
                                           uses]))
        add(f"| {code(f'0x{e['address']:08X}')} | {code(e['name'])}{' (likely)' if e['likely'] else ''} | "
            f"{code(e['codegen'])} | {e['subsystem']} | {md(e['purpose'])} | {code(e['signature'])} | "
            f"{md(uses)} | {md('; '.join(e['sources']))} |")
    add("")

    # --- mid-asm hooks --------------------------------------------------
    add(f"## Mid-function hooks ({len(cat.midasm)})")
    add("")
    add("| Address | Module | In function | Hook | Handler | Config |")
    add("|---|---|---|---|---|---|")
    in_xex = [h for h in cat.midasm if h["module"] == "default.xex"]
    for h in sorted(in_xex, key=lambda h: h["address"]):
        fn = h.get("function")
        inside = ""
        if fn is not None:
            e = entries.get(fn)
            inside = code(e["name"] if e else cat.codegen_name(fn)) + f" (+0x{h['address'] - fn:X})"
        add(f"| {code(f'0x{h['address']:08X}')} | {h['module']} | {inside} | {code(h['name'])} | "
            f"{md(', '.join(h['handlers']))} | {h['config']} |")
    add("")
    # The level DLLs' hooks repeat one hook per level: one line each.
    in_levels: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for h in cat.midasm:
        if h["module"] != "default.xex":
            in_levels[(h["name"], ", ".join(h["handlers"]))].append(h)
    for (name, handlers), hooks in sorted(in_levels.items()):
        modules = sorted({h["module"] for h in hooks})
        add(f"- {code(name)} ({md(handlers)}): {len(hooks)} sites in {len(modules)} level DLLs "
            f"(`config/<level>.toml`): " + ", ".join(m.rsplit(".", 1)[0] for m in modules) + ".")
    if in_levels:
        add("")

    # --- all functions --------------------------------------------------
    add("## All named functions by subsystem")
    add("")
    groups: dict[str, list[dict]] = defaultdict(list)
    for e in listed:
        groups[e["subsystem"]].append(e)
    add(" · ".join(f"[{title}](#{anchor(title, len(groups[key]))}) ({len(groups[key])})"
                   for key, title in SUBSYSTEMS if groups.get(key)))
    add("")
    for key, title in SUBSYSTEMS:
        rows = groups.get(key)
        if not rows:
            continue
        add(f"### {title} ({len(rows)})")
        add("")
        add("| Address | Name | What it does | Arguments | Port use | Sources |")
        add("|---|---|---|---|---|---|")
        for e in sorted(rows, key=lambda e: e["address"]):
            name = code(e["name"]) + (" (likely)" if e["likely"] else "")
            if e["codegen"] != e["name"] and e["codegen"] != e["name"].replace("::", "__"):
                name += " " + md("[" + e["codegen"] + "]") if not e["codegen"].startswith("sub_") else ""
            uses = use_text(e["uses"], short=True)
            if e["api"]:
                uses = "; ".join(filter(None, ["game_api", uses]))
            purpose = clean(e["purpose"], 100)
            add(f"| {code(f'0x{e['address']:08X}')} | {name} | {md(purpose)} | {code(e['signature'])} | "
                f"{md(uses)} | {md('; '.join(s.replace('map:', '') for s in e['sources']))} |")
        add("")
    if helpers:
        add(f"### Compiler helpers ({len(helpers)})")
        add("")
        add("Register save and restore routines the compiler calls in prologues and epilogues; the codegen "
            "names them itself: " + ", ".join(code(e["name"]) for e in sorted(helpers, key=lambda e: e["address"])[:6])
            + " ...")
        add("")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L).rstrip() + "\n", encoding="utf-8")


def anchor(title: str, count: int) -> str:
    """GitHub's anchor for '### <title> (<count>)'."""
    text = f"{title} ({count})".lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", type=Path, default=REPO, help="repository root (docs/, dev/agents/)")
    ap.add_argument("--recomp", type=Path, help="the game project: config/, src/, generated/ "
                    "(default: <repo>/COD_3_WAW_Setup-tool/recomp)")
    ap.add_argument("--symbols", type=Path, help="symbol map (default: <recomp>/config/symbols.toml, else "
                    "the repository's)")
    ap.add_argument("--names", type=Path, help="codegen names (default: <recomp>/config/names.toml)")
    ap.add_argument("--generated", type=Path, help="codegen function table (default: "
                    "<recomp>/generated/default/cod3_init.cpp when present)")
    ap.add_argument("--notes", type=Path, help="helper notes folder (default: <repo>/dev/agents when present)")
    ap.add_argument("--skip-notes", action="append", default=None, metavar="FOLDER",
                    help="a notes folder to leave out (default: function-catalog, this tool's own notes)")
    ap.add_argument("--engine-doc", type=Path, help="default: <repo>/docs/ENGINE.md")
    ap.add_argument("-o", "--output", type=Path, help="default: <repo>/docs/FUNCTIONS.md")
    ap.add_argument("--check", action="store_true", help="print the conflicts, write nothing")
    a = ap.parse_args(argv)

    repo = a.repo
    recomp = a.recomp or repo / "COD_3_WAW_Setup-tool" / "recomp"
    symbols = a.symbols or recomp / "config" / "symbols.toml"
    if not symbols.is_file():
        symbols = repo / "COD_3_WAW_Setup-tool" / "recomp" / "config" / "symbols.toml"
    names = a.names or recomp / "config" / "names.toml"
    generated = a.generated or recomp / "generated" / "default" / "cod3_init.cpp"
    notes = a.notes or repo / "dev" / "agents"
    engine_doc = a.engine_doc or repo / "docs" / "ENGINE.md"
    output = a.output or repo / "docs" / "FUNCTIONS.md"
    for need in (symbols, recomp / "src" / "game" / "addresses.h"):
        if not need.is_file():
            print(f"missing {need}", file=sys.stderr)
            return 2

    cat = Catalog()
    cat.read_symbols(symbols)
    cat.read_names(names)
    if generated.is_file():
        cat.read_generated(generated)
    elif cat.sym:
        cat.code_range = (min(cat.sym), max(cat.sym) + 4)
    cat.read_addresses(recomp / "src" / "game" / "addresses.h", "src/game/addresses.h")
    api = recomp / "src" / "game" / "game_api.h"
    if api.is_file():
        cat.read_api(api, "src/game/game_api.h")
    cat.read_configs(recomp / "config", recomp)
    cat.read_source_tree(recomp / "src", recomp, {h["name"] for h in cat.midasm})
    cat.read_engine_doc(engine_doc)
    if notes.is_dir():
        cat.read_notes(notes, set(a.skip_notes if a.skip_notes is not None else ["function-catalog"]))

    entries = cat.entries()
    conflicts = cat.conflicts(entries)
    total = sum(len(v) for k, v in conflicts.items() if k not in ("duplicates", "better"))
    if a.check:
        for key, items in conflicts.items():
            for item in items:
                print(f"{key}: {item}")
        breaking = conflicts.get("errors", []) + [i for i in conflicts.get("names", []) if "use it" in i]
        return 1 if breaking else 0
    write_markdown(cat, entries, conflicts, output)
    print(f"{len(entries)} functions, {total} conflicts -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
