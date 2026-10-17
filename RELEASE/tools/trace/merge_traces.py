#!/usr/bin/env python3
"""Merges decomp trace sessions into coverage and naming candidates.

The trace build of the game (docs/DECOMP_TRACE.md) writes one folder per
play session, userdata/decomp_trace/<session>/, with functions.tsv,
calls.tsv, evidence.tsv, markers.tsv, modules.tsv and trace.tsv. This tool
reads any number of them and writes:

  coverage.tsv             functions seen per module and per level, named and unnamed
  functions.tsv            every function seen, all sessions together
  candidates.tsv           the unnamed functions with all their evidence, and a
                           suggested name only where the evidence is strong
  curated_suggestions.txt  those suggestions as CURATED_FUNCTIONS lines, for a
                           person to check before copying any of them into
                           tools/name_functions.py (this tool never changes it)
  summary.txt              the counts in a few lines

    python3 tools/trace/merge_traces.py TRACE [TRACE ...] [-o OUT]
        [--symbols symbols.toml] [--pairs TABLE.tsv[:ADDRESS_COLUMN:NAME_COLUMN] ...]
        [--slot-names TABLE.tsv]

TRACE is a session folder or any folder above sessions (all are read, e.g.
the whole userdata/decomp_trace/). --symbols defaults to the project's
symbol map when it is found next to this tool. --pairs takes the World at
War / CoD2 pairing tables the decomp chats keep (dev/waw-decomp/cod3/*.tsv;
read by default when present): any TSV with a CoD3 address column and a
name column; give the column names when the guess is wrong. --slot-names is
a TSV of level interface slots and their builtin names (slot, name).

Python 3.8 or newer, standard library only.
"""

import argparse
import csv
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
TRACE_FORMAT = 1

# Where the symbol map and the pairing tables usually are (first one found).
DEFAULT_SYMBOLS = [
    HERE.parent.parent / "COD_3_WAW_Setup-tool" / "recomp" / "config" / "symbols.toml",  # project folder
    HERE.parent.parent / "research" / "symbols.toml",                                    # workspace
]
DEFAULT_PAIRS_DIR = HERE.parent.parent / "dev" / "waw-decomp" / "cod3"

MAIN_MODULE = "default"
STARTUP = "(start-up)"   # the level column of a function first seen before the game knew it
EVIDENCE_KINDS = ["print", "dvar", "dvar_register", "dvar_set", "file", "command", "command_handler",
                  "name", "level_slot"]
# Evidence about another function than the caller (evidence.tsv's function is the target).
TARGET_KINDS = {"command_handler", "level_slot"}

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# "CL_ParseGamestate: bad command byte", "R_Init(): ...": a print naming its function.
SELF_NAMING = re.compile(r"^\^?\d?([A-Z][A-Za-z0-9]*_[A-Za-z0-9_]*[A-Za-z0-9])(?:\(\))?:\s")
HEX = re.compile(r"^(?:0x)?([0-9A-Fa-f]{8})$")


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------

def unescape(text):
    """Undoes the writer's TSV escaping (\\t \\n \\r \\\\)."""
    if "\\" not in text:
        return text
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            out.append({"t": "\t", "n": "\n", "r": "\r", "\\": "\\"}.get(text[i + 1], "\\" + text[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_tsv(path):
    """Rows of a TSV with a header line, as dicts of unescaped text."""
    if not path.exists():
        return []
    with path.open(encoding="utf-8", errors="replace", newline="") as f:
        lines = f.read().splitlines()
    if not lines:
        return []
    header = lines[0].split("\t")
    rows = []
    for line in lines[1:]:
        if not line:
            continue
        fields = line.split("\t")
        fields += [""] * (len(header) - len(fields))
        rows.append({h: unescape(v) for h, v in zip(header, fields)})
    return rows


def parse_address(text):
    m = HEX.match((text or "").strip())
    return int(m.group(1), 16) if m else None


def find_sessions(paths):
    """Every session folder (one with functions.tsv) at or under the given paths."""
    found = []
    for p in paths:
        p = Path(p)
        if (p / "functions.tsv").is_file():
            found.append(p)
        elif p.is_dir():
            found += sorted(f.parent for f in p.rglob("functions.tsv"))
    seen, out = set(), []
    for s in found:
        key = s.resolve()
        if key not in seen:
            seen.add(key)
            out.append(s)
    return out


def read_symbols(path):
    """address -> (name, confidence, evidence) from symbols.toml's [[function]] blocks."""
    if not path or not Path(path).is_file():
        return {}
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    try:
        import tomllib  # Python 3.11+
    except ImportError:
        return parse_symbols_simple(text)
    data = tomllib.loads(text)
    return {int(f["address"]): (f.get("cname") or f.get("name", ""), f.get("confidence", ""), f.get("evidence", ""))
            for f in data.get("function", []) if "address" in f}


def parse_symbols_simple(text):
    """read_symbols for Python before 3.11: the map's simple form, one `key = value` per line."""
    symbols, block = {}, None

    def flush():
        if block is not None and "address" in block:
            symbols[block["address"]] = (block.get("cname") or block.get("name", ""),
                                         block.get("confidence", ""), block.get("evidence", ""))

    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[["):
            flush()
            block = {} if line == "[[function]]" else None
            continue
        if block is None or "=" not in line or line.startswith("#"):
            continue
        key, value = (s.strip() for s in line.split("=", 1))
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        elif key == "address":
            value = int(value, 0)
        block[key] = value
    flush()
    return symbols


def slot_names_from_symbols(symbols):
    """Level interface slot -> builtin name, from evidence like "level slot 0x1AC (anglestoforward)"."""
    names = {}
    for _, (_, _, evidence) in symbols.items():
        for m in re.finditer(r"level (?:interface )?slot 0x([0-9A-Fa-f]+)\s*\(([A-Za-z_][A-Za-z0-9_]*)\)", evidence):
            names[int(m.group(1), 16)] = m.group(2)
    return names


def read_slot_names(path):
    names = {}
    for row in read_tsv(Path(path)):
        values = list(row.values())
        if len(values) < 2:
            continue
        slot = values[0].strip()
        try:
            names[int(slot, 0)] = values[1].strip()
        except ValueError:
            continue
    return names


def read_pairs(spec):
    """(source, {address: name}) from a pairing table; spec = path[:address_column:name_column]."""
    path, address_col, name_col = spec, None, None
    parts = spec.rsplit(":", 2)
    if len(parts) == 3 and not Path(spec).exists() and Path(parts[0]).exists():
        path, address_col, name_col = parts
    rows = read_tsv(Path(path))
    if not rows:
        return Path(path).stem, {}
    header = list(rows[0].keys())

    def looks_like_cod3(col):
        values = [parse_address(r.get(col, "")) for r in rows[:200]]
        values = [v for v in values if v is not None]
        return values and sum(0x82000000 <= v < 0x82C30000 for v in values) >= len(values) * 0.8

    if address_col is None:
        ranked = sorted(header, key=lambda h: (("cod3" not in h.lower()), ("addr" not in h.lower())))
        address_col = next((h for h in ranked if looks_like_cod3(h)), None)
    if name_col is None and address_col:
        candidates = [h for h in header if h != address_col and
                      ("name" in h.lower() or h.lower() in ("candidate", "suggested", "suggestion"))]
        # A CoD3 column holds the current name; the relative's name is what we want.
        candidates.sort(key=lambda h: ("cod3" in h.lower(), h.lower() != "candidate"))
        name_col = candidates[0] if candidates else None
    if not address_col or not name_col:
        return Path(path).stem, {}
    pairs = {}
    for r in rows:
        a = parse_address(r.get(address_col, ""))
        name = (r.get(name_col) or "").strip()
        if a is not None and IDENT.match(name) and not name.startswith("sub_"):
            pairs[a] = name
    return f"{Path(path).stem}:{name_col}", pairs


# ---------------------------------------------------------------------------
# Merging
# ---------------------------------------------------------------------------

class Function:
    def __init__(self, module, address, name):
        self.module = module
        self.address = address
        self.name = name
        self.sessions = set()
        self.first = []                    # (session, seconds, level, state)
        self.markers = []                  # "session: 3: planted the bomb"
        self.calls = 0
        self.callers = Counter()           # caller text -> sessions
        self.caller_functions = set()      # (module, address, name)
        self.callees = set()               # (module, address, name)
        self.evidence = defaultdict(Counter)   # kind -> value -> count
        self.levels = set()

    @property
    def key(self):
        return (self.module, self.address if self.address is not None else self.name)


class Merge:
    def __init__(self):
        self.functions = {}
        self.module_totals = {}
        self.sessions = []
        self.markers = []
        self.warnings = []

    def function(self, module, address, name):
        key = (module, address if address is not None else name)
        f = self.functions.get(key)
        if f is None:
            f = self.functions[key] = Function(module, address, name)
        elif name and (f.name.startswith("sub_") or not f.name) and not name.startswith("sub_"):
            f.name = name  # a later build knows its name
        return f

    def add_session(self, folder):
        session = folder.name
        info = {r.get("key", ""): r.get("value", "") for r in read_tsv(folder / "trace.tsv")}
        fmt = info.get("format", "")
        if fmt and fmt != str(TRACE_FORMAT):
            self.warnings.append(f"{folder}: trace format {fmt}, this tool reads {TRACE_FORMAT}; read anyway")
        self.sessions.append((session, info))
        for r in read_tsv(folder / "modules.tsv"):
            try:
                total = int(r.get("functions", "0"))
            except ValueError:
                continue
            self.module_totals[r["module"]] = max(total, self.module_totals.get(r["module"], 0))
        for r in read_tsv(folder / "markers.tsv"):
            self.markers.append((session, r.get("number", ""), r.get("time_s", ""), r.get("level", ""),
                                 r.get("text", ""), r.get("functions", "")))
        for r in read_tsv(folder / "functions.tsv"):
            f = self.function(r.get("module", "?"), parse_address(r.get("address")), r.get("name", ""))
            f.sessions.add(session)
            f.first.append((session, r.get("first_seen_s", ""), r.get("level", ""), r.get("state", "")))
            # First seen before the game said where it is: while it starts up.
            f.levels.add(r.get("level") or STARTUP)
            if r.get("marker"):
                f.markers.append(f"{session}: {r['marker']}")
            try:
                f.calls += int(r.get("calls") or 0)
            except ValueError:
                pass
            for caller in filter(None, (c.strip() for c in (r.get("callers") or "").split(", "))):
                if caller != "...":
                    f.callers[caller] += 1
        for r in read_tsv(folder / "calls.tsv"):
            callee = self.function(r.get("callee_module", "?"), parse_address(r.get("callee_address")),
                                   r.get("callee_name", ""))
            caller = (r.get("caller_module", "?"), parse_address(r.get("caller_address")), r.get("caller_name", "?"))
            callee.caller_functions.add(caller)
            if caller[1] is not None:
                c = self.functions.get((caller[0], caller[1]))
                if c is not None:
                    c.callees.add((callee.module, callee.address, callee.name))
        for r in read_tsv(folder / "evidence.tsv"):
            kind = r.get("kind", "")
            value = r.get("value", "")
            address = parse_address(r.get("function_address"))
            if not value or address is None:
                continue
            f = self.function(r.get("module", "?"), address, r.get("function_name", ""))
            try:
                count = int(r.get("count") or 1)
            except ValueError:
                count = 1
            f.evidence[kind][value] += count
            if kind in TARGET_KINDS and r.get("via"):
                f.evidence[kind + "_via"][r["via"]] += count

    def fill_callees(self):
        """calls.tsv lines read before their caller's functions.tsv line: link them now."""
        for f in self.functions.values():
            for module, address, _ in f.caller_functions:
                c = self.functions.get((module, address))
                if c is not None:
                    c.callees.add((f.module, f.address, f.name))


# ---------------------------------------------------------------------------
# Names
# ---------------------------------------------------------------------------

def known_name(f, symbols):
    """The function's name if it has one already (symbol map or the build's names), else ""."""
    if f.module == MAIN_MODULE and f.address in symbols and symbols[f.address][0]:
        return symbols[f.address][0]
    return "" if (not f.name or f.name.startswith("sub_") or f.name == "?") else f.name


CALLER = re.compile(r"^sub_([0-9A-Fa-f]{8})(\+0x[0-9A-Fa-f]+)?$")


def pretty_caller(text, symbols):
    """A caller of the executable's code ("sub_82349A98+0xB8") with the symbol
    map's name when the build didn't have it ("Com_Frame+0xB8")."""
    m = CALLER.match(text)
    if not m:
        return text
    name = symbols.get(int(m.group(1), 16), ("",))[0]
    return f"{name}{m.group(2) or ''}" if name else text


def callee_name(module, address, name, symbols):
    if module == MAIN_MODULE and address in symbols and symbols[address][0]:
        return symbols[address][0]
    return name


def camel(words):
    return "_".join(w[:1].upper() + w[1:] for w in words.split("_") if w)


def command_handler_name(command, via):
    """Q3 style handler name: the registering function's prefix (CL_Init -> CL_) + the command + _f."""
    m = re.match(r"^(?:[A-Za-z0-9]+!)?([A-Z][A-Za-z0-9]*_)", via or "")
    prefix = m.group(1) if m else "Cmd_"
    words = re.sub(r"[^A-Za-z0-9_]", "_", command.lstrip("+-"))
    if not words:
        return None
    sign = {"+": "Plus_", "-": "Minus_"}.get(command[:1], "")
    return f"{prefix}{sign}{camel(words)}_f"


def suggest(f, slot_names, pairs_by_source):
    """(name, confidence, reason) when the evidence is strong, else None."""
    ev = f.evidence
    # 1. The handler of exactly one console command.
    handlers = ev.get("command_handler", {})
    if len(handlers) == 1:
        command = next(iter(handlers))
        via = next(iter(ev.get("command_handler_via", {})), "")
        name = command_handler_name(command, via)
        if name:
            return name, "high", f"handler of the console command '{command}'" + (f" (added by {via})" if via else "")
    # 2. The function behind a level interface slot whose builtin is known.
    slots = ev.get("level_slot", {})
    if len(slots) == 1:
        slot = int(next(iter(slots)).split("0x")[-1], 16)
        if slot in slot_names:
            builtin = slot_names[slot]
            return f"Broc_{builtin[:1].upper()}{builtin[1:]}", "high", \
                f"level interface slot 0x{slot:X} is the builtin '{builtin}'"
    # 3. Prints that name their function ("R_Init: ...").
    named = {m.group(1) for fmt in ev.get("print", {}) for m in [SELF_NAMING.match(fmt)] if m}
    if len(named) == 1:
        ident = next(iter(named))
        return ident, "high", f"prints '{ident}: ...' (the Quake III habit of naming the function)"
    # 4. Two pairing tables that agree.
    names = Counter(p[f.address] for p in pairs_by_source.values() if f.address in p)
    if names:
        name, votes = names.most_common(1)[0]
        if votes >= 2 and len(names) == 1:
            sources = [s for s, p in pairs_by_source.items() if p.get(f.address) == name]
            return name, "medium", f"paired with the same name by {', '.join(sources)}"
    return None


def evidence_text(f, limit=6):
    parts = []
    for kind in EVIDENCE_KINDS:
        values = f.evidence.get(kind)
        if not values:
            continue
        top = [v for v, _ in values.most_common(limit)]
        more = f" (+{len(values) - limit})" if len(values) > limit else ""
        parts.append(f"{kind}: " + " | ".join(top) + more)
    return "; ".join(parts)


def sanitize(text, limit=400):
    text = text.replace("\t", " ").replace("\n", " ").replace("\r", " ")
    return text if len(text) <= limit else text[:limit - 3] + "..."


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------

def hex_or(address):
    return f"0x{address:08X}" if address is not None else "?"


def write_tsv(path, header, rows):
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n", quoting=csv.QUOTE_NONE, escapechar="\\")
        w.writerow(header)
        for row in rows:
            w.writerow([sanitize(str(v), 2000) for v in row])


def run(args):
    sessions = find_sessions(args.traces)
    if not sessions:
        print("No trace sessions found (folders with functions.tsv).", file=sys.stderr)
        return 1
    symbols_path = args.symbols or next((p for p in DEFAULT_SYMBOLS if p.is_file()), None)
    symbols = read_symbols(symbols_path) if symbols_path else {}
    slot_names = slot_names_from_symbols(symbols)
    if args.slot_names:
        slot_names.update(read_slot_names(args.slot_names))
    pair_specs = list(args.pairs or [])
    if not pair_specs and DEFAULT_PAIRS_DIR.is_dir():
        pair_specs = [str(p) for p in sorted(DEFAULT_PAIRS_DIR.glob("*.tsv"))]
    pairs_by_source = {}
    for spec in pair_specs:
        source, pairs = read_pairs(spec)
        if pairs:
            pairs_by_source[source] = pairs

    merge = Merge()
    for s in sessions:
        merge.add_session(s)
    merge.fill_callees()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    functions = sorted(merge.functions.values(),
                       key=lambda f: (f.module != MAIN_MODULE, f.module, f.address if f.address is not None else 0))
    seen = [f for f in functions if f.sessions]

    # coverage.tsv
    coverage = defaultdict(lambda: [0, 0])
    for f in seen:
        named = 1 if known_name(f, symbols) else 0
        for level in {"(all)"} | f.levels:
            c = coverage[(f.module, level)]
            c[0] += 1
            c[1] += named
    rows = []
    for (module, level), (count, named) in sorted(coverage.items(), key=lambda kv: (kv[0][0] != MAIN_MODULE, kv[0])):
        total = merge.module_totals.get(module, "")
        share = f"{100.0 * count / total:.1f}%" if level == "(all)" and total else ""
        rows.append([module, level, total if level == "(all)" else "", count, share, named, count - named])
    write_tsv(out / "coverage.tsv", ["module", "first_seen_in", "functions", "seen", "seen_share", "named",
                                     "unnamed"], rows)

    # functions.tsv
    def callers_text(f, limit=12):
        return ", ".join(pretty_caller(c, symbols) for c, _ in f.callers.most_common(limit)) + \
            (", ..." if len(f.callers) > limit else "")

    def callees_text(f, limit=12):
        names = {callee_name(module, address, name, symbols) for module, address, name in f.callees}
        named = sorted(n for n in names if n and not n.startswith("sub_") and n != "?")
        return ", ".join(named[:limit]) + (", ..." if len(named) > limit else "")

    write_tsv(out / "functions.tsv",
              ["module", "address", "name", "symbol_name", "sessions", "first_seen", "markers", "calls", "callers",
               "named_callees", "evidence"],
              [[f.module, hex_or(f.address), f.name, known_name(f, symbols), len(f.sessions),
                " | ".join(f"{lvl or STARTUP}:{st}" for lvl, st in sorted({(x[2], x[3]) for x in f.first})),
                " | ".join(f.markers), f.calls, callers_text(f), callees_text(f), evidence_text(f, 3)]
               for f in seen])

    # candidates.tsv and the suggestions
    taken = {name: a for a, (name, _, _) in symbols.items() if name}
    suggestions, rows = [], []
    proposed = Counter()
    unnamed = [f for f in seen if not known_name(f, symbols)]
    picks = {}
    for f in unnamed:
        s = suggest(f, slot_names, pairs_by_source)
        if s:
            picks[f.key] = s
            proposed[s[0]] += 1
    for f in unnamed:
        s = picks.get(f.key)
        note = ""
        if s:
            name = s[0]
            if name in taken and taken[name] != f.address:
                note = f"not suggested: {name} is already 0x{taken[name]:08X} in the symbol map"
                s = None
            elif proposed[name] > 1:
                note = f"not suggested: {proposed[name]} functions would get {name}"
                s = None
        pair_text = "; ".join(f"{src}: {p[f.address]}" for src, p in pairs_by_source.items() if f.address in p)
        rows.append([f.module, hex_or(f.address), f.name, s[0] if s else "", s[1] if s else "", s[2] if s else note,
                     len(f.sessions), " | ".join(sorted(f.levels)), " | ".join(f.markers[:6]), f.calls,
                     callers_text(f, 8), callees_text(f, 8), pair_text, evidence_text(f)])
        if s and f.module == MAIN_MODULE and f.address is not None:
            suggestions.append((f.address, s[0], s[1], s[2], len(f.sessions)))
    rows.sort(key=lambda r: (r[3] == "", r[0] != MAIN_MODULE, r[0], r[1]))
    write_tsv(out / "candidates.tsv",
              ["module", "address", "name", "suggested_name", "confidence", "reason", "sessions", "levels",
               "markers", "calls", "callers", "named_callees", "pairs", "evidence"], rows)

    lines = [f"# Suggested from {len(sessions)} decomp trace session(s) by tools/trace/merge_traces.py, "
             f"{datetime.now():%Y-%m-%d}.",
             "# Check each one against the code before copying it into CURATED_FUNCTIONS in",
             "# tools/name_functions.py (your own block at its end), then regenerate the symbol map.", ""]
    for address, name, confidence, reason, n in sorted(suggestions):
        reason = reason.replace("\\", "\\\\").replace('"', "'")
        lines.append(f'    (0x{address:08X}, "{name}", "{confidence}", "trace ({n} session'
                     f'{"s" if n != 1 else ""}): {reason}"),')
    (out / "curated_suggestions.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # summary.txt
    main_seen = [f for f in seen if f.module == MAIN_MODULE]
    main_named = sum(1 for f in main_seen if known_name(f, symbols))
    level_seen = [f for f in seen if f.module != MAIN_MODULE]
    summary = [
        f"Sessions: {len(sessions)} ({', '.join(s for s, _ in merge.sessions)})",
        f"Functions seen: {len(seen)} ({len(main_seen)} in default.xex, {len(level_seen)} in level libraries)",
        f"default.xex: {main_named} of the seen ones are named, {len(main_seen) - main_named} are not"
        + (f"; {len(main_seen)} of {merge.module_totals[MAIN_MODULE]} functions seen"
           if merge.module_totals.get(MAIN_MODULE) else ""),
        f"Markers: {len(merge.markers)}",
        f"Unnamed functions with evidence: {sum(1 for f in unnamed if f.evidence)}",
        f"Suggested names: {sum(1 for r in rows if r[3])} ({len(suggestions)} in default.xex, in "
        "curated_suggestions.txt)",
        f"Symbol map: {symbols_path if symbols else 'not found (--symbols)'}",
        f"Pairing tables: {', '.join(pairs_by_source) if pairs_by_source else 'none'}",
    ] + [f"Warning: {w}" for w in merge.warnings]
    (out / "summary.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")
    if not args.quiet:
        print("\n".join(summary))
        print(f"Written to {out}")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    p.add_argument("traces", nargs="+", help="session folders, or folders holding them")
    p.add_argument("-o", "--out", default="decomp_trace_merged", help="output folder (default: %(default)s)")
    p.add_argument("--symbols", type=Path, help="the symbol map (research/symbols.toml)")
    p.add_argument("--pairs", action="append",
                   help="a pairing table, TABLE.tsv or TABLE.tsv:ADDRESS_COLUMN:NAME_COLUMN (repeatable)")
    p.add_argument("--slot-names", help="TSV of level interface slots and their builtin names")
    p.add_argument("-q", "--quiet", action="store_true", help="print nothing but errors")
    return run(p.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
