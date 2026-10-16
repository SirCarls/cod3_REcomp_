"""
naming_common.py - naming passes shared by name_pc.py (x86, PC) and name_x360.py
(PowerPC, Xbox 360). Each pass reads the tables Ghidra exported (WawExport.java)
and adds (address, name, kind, pass, evidence) rows to a Namer. A name or an
address is taken by the first pass that claims it, so callers run the strongest
passes first.
"""
import collections
import csv
import re

csv.field_size_limit(1 << 30)

# "Name: ...", "Name(): ...", "^1Name: ..." (color codes), "ERROR: Name: ..."
SELF_RE = re.compile(r"^(?:\^\d)?(?:(?:ERROR|WARNING|Error|Warning)\s*:\s*)?([A-Z][A-Za-z0-9]*_[A-Za-z0-9_]+|[a-z]+_[A-Za-z0-9_]+)\s*(?:\(\s*\))?\s*:")
DVAR_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{1,63}$")
# C++ method names: Demonware logs name the method ("bdCore::init", "bdAddr::set(const bdNChar8 *) ...")
# and assertions carry the compiler's function signature ("bool __cdecl bdUnicastConnection::handleData(...)")
METHOD_RE = re.compile(r"^(bd[A-Z]\w*)::(~?[A-Za-z_]\w*)")
FUNCSIG_RE = re.compile(r"\b__(?:cdecl|thiscall|stdcall|fastcall)\s+((?:[A-Za-z_]\w*::)+~?[A-Za-z_]\w*)\(")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def read_tsv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def unesc(s):
    return s.replace("\\t", "\t").replace("\\n", "\n").replace("\\r", "\r").replace("\\\\", "\\")


class Namer:
    def __init__(self):
        self.by_addr = {}
        self.by_name = {}
        self.notes = {}  # addr -> (pass, text): a comment for a function that has no name

    def note(self, addr, pas, text):
        self.notes[addr] = (pas, text)

    def add(self, addr, name, kind, pas, evidence):
        if addr in self.by_addr or name in self.by_name:
            return False
        self.by_addr[addr] = (name, kind, pas, evidence)
        self.by_name[name] = addr
        return True

    def rename(self, addr, name):
        """Gives an address another name (kept when the name is taken)."""
        old = self.by_addr.get(addr)
        if old and name not in self.by_name:
            del self.by_name[old[0]]
            self.by_addr[addr] = (name,) + old[1:]
            self.by_name[name] = addr

    def func_name(self, addr):
        v = self.by_addr.get(addr)
        return v[0] if v and v[1] == "function" else None

    def rows(self):
        for a in sorted(set(self.by_addr) | set(self.notes)):
            if a in self.by_addr:
                name, kind, pas, ev = self.by_addr[a]
                yield f"{a:08X}", name, kind, pas, ev
            else:
                yield f"{a:08X}", "", "note", self.notes[a][0], self.notes[a][1]

    def write(self, path):
        with open(path, "w", encoding="utf-8") as o:
            o.write("addr\tname\tkind\tpass\tevidence\n")
            for row in self.rows():
                o.write("\t".join(x.replace("\t", " ") for x in row) + "\n")


class Tables:
    """The Ghidra export of one program."""

    def __init__(self, raw):
        self.funcs = {int(r["entry"], 16): r for r in read_tsv(f"{raw}/functions.tsv")}
        self.entries = sorted(self.funcs)
        self.calls = read_tsv(f"{raw}/calls.tsv")
        self.strings = read_tsv(f"{raw}/strings.tsv")
        self.str_at = {int(r["addr"], 16): unesc(r["text"]) for r in self.strings}
        self.str_funcs = collections.defaultdict(set)  # text -> functions referencing it
        for r in self.strings:
            if r["funcs"]:
                self.str_funcs[unesc(r["text"])].update(int(f, 16) for f in r["funcs"].split(","))
        self.callees = collections.defaultdict(set)
        self.callers = collections.defaultdict(set)
        for r in self.calls:
            if r["callee"].startswith(("ext:", "ptr:")) or r["kind"] != "UNCONDITIONAL_CALL":
                continue
            a, b = int(r["caller"], 16), int(r["callee"], 16)
            if b in self.funcs and a != b:
                self.callees[a].add(b)
                self.callers[b].add(a)

    def default_name(self, addr):
        f = self.funcs.get(addr)
        return f is None or f["source"] == "DEFAULT" or f["name"].startswith(("FUN_", "thunk_FUN_"))


def register_core_filter(cands, votes, callees, callers, refs=(), depth=5):
    """The dvar registration functions among the candidates (functions called with a name
    string whose result goes into a global). Real ones all end up, within a few calls, in the
    same core function of the dvar system; Material_RegisterHandle or a sound alias lookup,
    which are called the same way, never do. The core is the most specific function (fewest
    callers) that the biggest candidates (or the given known ones) all reach.
    Returns (kept, dropped)."""

    def reach(f):
        seen, front = {f}, {f}
        for _ in range(depth):
            front = {c for x in front for c in callees.get(x, ())} - seen
            seen |= front
        return seen

    refs = [r for r in refs if r in cands] or sorted(cands, key=lambda c: (-votes.get(c, 0), c))[:3]
    if len(refs) < 2:
        return set(cands), set()
    common = set.intersection(*(reach(r) for r in refs)) - set(refs)
    if not common:
        return set(cands), set()
    core = min(common, key=lambda c: (len(callers.get(c, ())), c))
    kept = {c for c in cands if c in refs or core in reach(c)}
    return kept, set(cands) - kept


def pass_selfname(namer, t, stats):
    """A function's own name in its error/log strings ("BG_ParseCommands: expected animation")."""
    votes = collections.defaultdict(collections.Counter)
    for r in t.strings:
        m = SELF_RE.match(unesc(r["text"]))
        if not m or not r["funcs"]:
            continue
        name = m.group(1)
        if len(name) < 5 or name.upper() == name and "_" not in name[1:]:
            continue
        for f in r["funcs"].split(","):
            votes[int(f, 16)][name] += 1
    cand = collections.defaultdict(list)
    for f, c in votes.items():
        total = sum(c.values())
        name, n = c.most_common(1)[0]
        if len(c) > 1 and c.most_common(2)[1][1] == n:
            continue
        size = int(t.funcs[f]["size"]) if f in t.funcs else 1 << 30
        cand[name].append((n / total, n, -size, f))
    for name, lst in sorted(cand.items()):
        lst.sort(reverse=True)
        share, n, _, f = lst[0]
        if len(lst) > 1 and lst[1][:2] == lst[0][:2]:
            stats["selfname_ambiguous"] += 1
            continue
        if namer.add(f, name, "function", "selfname", f"{n} own-name string(s), {share:.0%} of its self-naming strings"):
            stats["selfname"] += 1


def method_name(path):
    """'bdUnicastConnection::handleData' -> 'bdUnicastConnection__handleData'; constructors and
    destructors become Class__ctor and Class__dtor (the codegen's style)."""
    parts = path.split("::")
    meth = parts[-1]
    if meth == parts[-2]:
        meth = "ctor"
    elif meth.startswith("~"):
        meth = "dtor"
    return "__".join(parts[:-1] + [meth])


def pass_methods(namer, t, stats):
    """C++ methods that name themselves: a compiler function signature in an assertion, or a
    Demonware log line that starts with Class::method. A string seen in several functions is an
    inlined helper's (bdLinkedList::GetHead), so each function takes the name fewest functions share,
    and a name shared by more than two functions (overloads) names none."""
    found = collections.defaultdict(collections.Counter)
    for r in t.strings:
        if not r["funcs"]:
            continue
        text = unesc(r["text"])
        m = FUNCSIG_RE.search(text)
        name = method_name(m.group(1)) if m else None
        if name is None:
            m = METHOD_RE.match(text)
            name = method_name(f"{m.group(1)}::{m.group(2)}") if m else None
        if name is None:
            continue
        for f in r["funcs"].split(","):
            found[int(f, 16)][name] += 1
    users = collections.Counter(n for c in found.values() for n in c)
    claims = collections.defaultdict(list)
    for f, c in found.items():
        best = min(c, key=lambda n: (users[n], -c[n], n))
        if users[best] > 2:
            stats["methods_inlined_only"] += 1
            continue
        claims[best].append(f)
    for name, fs in sorted(claims.items()):
        for k, f in enumerate(sorted(fs)):
            alias = name if k == 0 else f"{name}_{k + 1}"
            ev = f"its own method name in {found[f][name]} string(s)" + (" (overload)" if len(fs) > 1 else "")
            if namer.add(f, alias, "function", "method", ev):
                stats["method"] += 1


def pass_transfer(namer, t, other, other_names, label, stats, min_len=6):
    """Names from the other build of the game (PC <-> Xbox 360): a function here and a named
    function there that are the only users of the same strings."""
    votes = collections.defaultdict(collections.Counter)
    for text, here in t.str_funcs.items():
        if len(text) < min_len or len(here) != 1:
            continue
        there = other.str_funcs.get(text)
        if not there or len(there) != 1:
            continue
        o = next(iter(there))
        name = other_names.get(o)
        if name:
            votes[next(iter(here))][name] += 1
    for f, c in sorted(votes.items()):
        name, n = c.most_common(1)[0]
        if len(c) > 1 and c.most_common(2)[1][1] == n:
            stats[f"{label}_ambiguous"] += 1
            continue
        if namer.add(f, name, "function", label, f"{n} string(s) used only by {name} in the other build"):
            stats[label] += 1


def pass_kisak(namer, t, kisak_tsv, stats):
    """Names from KisakCOD (reconstructed CoD4 source) by distinctive strings only it and one
    function here use. Names only; no code is taken."""
    votes = collections.defaultdict(collections.Counter)
    for r in read_tsv(kisak_tsv):
        for s in r["strings"].split("\x1f"):
            s = unesc(s)
            if len(s) < 8:
                continue
            fs = t.str_funcs.get(s)
            if fs and len(fs) == 1:
                votes[next(iter(fs))][r["name"]] += 1
    for f, c in sorted(votes.items()):
        name, n = c.most_common(1)[0]
        if len(c) > 1 and c.most_common(2)[1][1] == n:
            continue
        if namer.add(f, name, "function", "kisak", f"{n} string(s) shared with KisakCOD {name}"):
            stats["kisak"] += 1


def kisak_table_index(kisak_tables_tsv):
    """KisakCOD's { "name", handler } tables, one group per array ("file:array"), so tables that
    share keywords (menuDef and itemDef keywords, functions and methods) stay apart. Older
    exports without the array column are grouped by file and handler prefix."""
    by_group = collections.defaultdict(dict)  # group -> script name (lower) -> C name
    if kisak_tables_tsv:
        for r in read_tsv(kisak_tables_tsv):
            if r["file"].startswith("radiant"):
                continue
            fn = r["function"]
            if fn.startswith("KISAK_"):
                continue  # KisakCOD's placeholder for functions it hasn't written (KISAK_NULLSUB)
            if r.get("table"):
                group = r["table"]
            else:
                group = fn.rsplit("_", 1)[0] + "_" if "_" in fn else ""
            by_group[f'{r["file"]}:{group}'].setdefault(r["string"].lower(), fn)
    return by_group


def other_build_tables(names_tsv, script_api_tsv=None):
    """Another build's { "name", handler } tables: table address -> (table name, {script name:
    handler name}), from the table pass's rows in its names.tsv and, when given, from its
    script_api.tsv (script_api.py), which has every entry, also those another pass named."""
    tables = collections.defaultdict(lambda: ["", {}])
    if script_api_tsv:
        for r in read_tsv(script_api_tsv):
            if r["c_name"] and not r["c_name"].startswith(("FUN_", "thunk_")):
                tables[int(r["table"], 16)][1][r["script_name"]] = r["c_name"]
    for r in read_tsv(names_tsv):
        if r["pass"] != "table":
            continue
        if r["kind"] == "global":
            tables[int(r["addr"], 16)][0] = r["name"]
            continue
        m = re.match(r'handler of "([^"]+)" in table ([0-9A-F]{8})', r["evidence"])
        if m:
            tables[int(m.group(2), 16)][1].setdefault(m.group(1), r["name"])
    return {a: (v[0], v[1]) for a, v in tables.items()}


def pass_tables(namer, words_by_section, cstr, is_code, kis_by_file, stats, users=lambda tbl: set(),
                other_tables=None, other_label="the other build", bounds=lambda tbl, size: ()):
    """Arrays of { "name", handler, ... } in the data sections (script builtins, methods,
    parsers). The handler gets KisakCOD's name for that string (from the KisakCOD table
    sharing the most names) or that table's prefix + the string. With other_tables (another
    build's tables, from other_build_tables), a table with the same entries there gives its
    names, so both builds name the same builtin the same way. bounds(table, entry size) gives
    the entry counts that the functions reading the table stop at: a table is cut there (two
    arrays back to back, such as World at War's hud element and player methods).
    Returns the tables: (address, entry size, script names, handlers, KisakCOD array or None).
    """

    def from_other(snames):
        best, hits = None, 0
        sset = set(snames)
        for a, (tname, m) in (other_tables or {}).items():
            h = len(sset & m.keys())
            if h > hits:
                best, hits = a, h
        if best is None or hits < max(4, len(snames) // 2):
            return None
        tname, m = other_tables[best]
        known = {sn: m[sn] for sn in snames if sn in m}
        pre = collections.Counter(n.rsplit("_", 1)[0] + "_" for n in known.values() if "_" in n)
        return known, (pre.most_common(1)[0][0] if pre else "Scr_"), tname

    def ranked_arrays(snames):
        low = [s.lower() for s in snames]
        return sorted(((sum(1 for s in low if s in m), f) for f, m in kis_by_file.items()), reverse=True)

    def copy_of(f, p):
        """KisakCOD array f is (mostly) a copy of array p: the SP and MP versions of one table."""
        a, b = kis_by_file[f], kis_by_file[p]
        return len(a.keys() & b.keys()) * 2 >= min(len(a), len(b))

    def table_names(snames, primaries):
        """KisakCOD names for a table's entries: from the KisakCOD array sharing the most names;
        then from the other version (SP or MP) of that array; then from other arrays sharing at
        least 3 names, mostly new ones (World at War SP merged CoD4's player methods and hud
        element methods into one table). An array that is another table's first choice only
        counts as one block of this table (two tables back to back)."""
        low = [s.lower() for s in snames]
        ranked = ranked_arrays(snames)
        if not ranked or ranked[0][0] < 2:
            return {}, "Scr_"
        m = kis_by_file[ranked[0][1]]
        pre = collections.Counter(m[s].rsplit("_", 1)[0] + "_" for s in low if s in m and "_" in m[s])
        known = {sn: m[sn.lower()] for sn in snames if sn.lower() in m}
        for h, f in ranked[1:]:
            if h < 3:
                break
            # an unrelated array only when its shared names are mostly new ones (a merged table),
            # not when it overlaps the first (menuDef and itemDef keywords)
            new = [sn for sn in snames if sn not in known and sn.lower() in kis_by_file[f]]
            own = ranked[0][1]
            if not copy_of(f, own) and (f in primaries or any(copy_of(f, p) for p in primaries)):
                # another table's array (or a copy of one, like KisakCOD's SP and MP script tables):
                # only as one block of this table (two arrays back to back with no name repeated,
                # such as the methods and functions in World at War MP), not a few scattered names
                pos = [i for i, sn in enumerate(snames) if sn in new]
                if len(pos) < 6 or len(pos) * 10 < (pos[-1] - pos[0] + 1) * 6:
                    continue
            if copy_of(f, own) and new or len(new) >= 3 and len(new) * 2 >= h:
                for sn in new:
                    known[sn] = kis_by_file[f][sn.lower()]
        return known, (pre.most_common(1)[0][0] if pre else "Scr_")

    # find every table first, then name them best KisakCOD match first, so the table that really
    # is KisakCOD's gets its names (World at War SP has the same builtins twice: the server
    # script VM and the client script VM, "CSC")
    tables = []
    for s0, words in words_by_section:
        n = len(words)
        i = 0
        while i < n - 1:
            hit = None
            for stride in (2, 3, 4):
                run, j, seen = 0, i, set()
                # a name seen again means the next table starts here (two arrays back to back,
                # such as the menuDef and itemDef keywords)
                while j + 1 < n and is_code(words[j + 1]) and cstr(words[j]) and cstr(words[j]).lower() not in seen:
                    seen.add(cstr(words[j]).lower())
                    run += 1
                    j += stride
                if run >= 6:
                    hit = (stride, run)
                    break
            if not hit:
                i += 1
                continue
            stride, run = hit
            tbl = s0 + i * 4
            cut = [n for n in bounds(tbl, stride * 4) if 2 <= n < run]
            if cut:
                run = min(cut)
            snames = [cstr(words[i + k * stride]) for k in range(run)]
            handlers = [words[i + k * stride + 1] for k in range(run)]
            o = from_other(snames)
            if o:
                known, prefix, hint = o
                tables.append((1 + len(known) / run, tbl, stride, snames, handlers, known, prefix, hint))
            else:
                tables.append((None, tbl, stride, snames, handlers, None, None, None))
            i += run * stride
    primaries = set()
    for _, tbl, stride, snames, handlers, known, prefix, hint in tables:
        if known is None:
            ranked = ranked_arrays(snames)
            if ranked and ranked[0][0] >= 2:
                primaries.add(ranked[0][1])
    for k, (_, tbl, stride, snames, handlers, known, prefix, hint) in enumerate(tables):
        if known is None:
            known, prefix = table_names(snames, primaries)
            tables[k] = (len(known) / len(snames), tbl, stride, snames, handlers, known, prefix, hint)
    found = []
    for _, tbl, stride, snames, handlers, known, prefix, hint in sorted(tables, key=lambda x: -x[0]):
        # The function that reads the table tells which VM it belongs to: World at War SP has the
        # builtins twice, for the server scripts (GSC) and the client scripts (CSC, CScr_GetFunction
        # and CScr_GetMethod).
        who = sorted(n for n in users(tbl) if n)
        if any(n.startswith("CScr_") for n in who):
            known, prefix = {}, "CScr_"
        tname = f"{who[0]}_table" if len(who) == 1 else f"table_{tbl:08X}"
        if hint and not who and not hint.startswith("table_"):
            tname = hint
        ev = f"{len(snames)} entries of (name, handler), stride {stride * 4}" + (f"; read by {', '.join(who)}" if who else "")
        if hint is not None:
            ev += f"; names from {other_label}'s table"
        if not namer.add(tbl, tname, "global", "table", ev):
            namer.add(tbl, f"table_{tbl:08X}", "global", "table", ev)
        for sn, fn in zip(snames, handlers):
            if namer.add(fn, known.get(sn) or f"{prefix}{sn}", "function", "table", f'handler of "{sn}" in table {tbl:08X}'):
                stats["table"] += 1
        ranked = ranked_arrays(snames) if hint is None else []
        found.append((tbl, stride * 4, snames, handlers, ranked[0][1] if ranked and ranked[0][0] >= 2 else None))
    return found


def pass_readers(namer, tables, readers_tsv, refs, bound, stats):
    """The function that looks a name up in a table: KisakCOD's reader of the same array
    (kisak_strings.py's _readers.tsv: Player_GetMethod reads the player methods). refs(table)
    gives the functions whose code holds the table's address; bound(function, entry size) the
    entry counts it stops at. A reader is named when it is the only one stopping at the table's
    size."""
    readers = collections.defaultdict(set)  # "file:array" -> KisakCOD function names
    for r in read_tsv(readers_tsv):
        readers[f'{r["file"]}:{r["table"]}'].add(r["function"])
    per_group = collections.Counter(t[4] for t in tables)
    for tbl, size, snames, handlers, group in tables:
        if per_group[group] != 1:
            continue  # two tables like the same KisakCOD array (World at War's GSC and CSC builtins)
        names = sorted(n for n in readers.get(group, ()) if n.endswith(("GetMethod", "GetFunction")))
        if len(names) != 1:
            continue
        cands = [f for f in refs(tbl) if len(snames) in bound(f, size)]
        if len(cands) != 1:
            continue
        old = namer.by_addr.get(cands[0])
        if old and old[1] == "function":
            continue
        if namer.add(cands[0], names[0], "function", "reader",
                     f"looks names up in table {tbl:08X} ({len(snames)} entries, as many as its loop runs), which"
                     f" matches KisakCOD's {group.split(':')[1]} ({group.split(':')[0]}), read by {names[0]}"):
            stats["reader"] += 1
            t = namer.by_addr.get(tbl)
            if t and "read by " not in t[3]:
                namer.by_addr[tbl] = t[:3] + (t[3] + f"; read by {names[0]}",)
            if t and t[0].startswith("table_"):
                namer.rename(tbl, f"{names[0]}_table")


def pass_kisak_graph(namer, t, kisak_tsv, stats, is_library=lambda a: False):
    """Names from the call graph against KisakCOD: a named function whose KisakCOD counterpart
    calls exactly one function not yet placed, while it calls exactly one unnamed function here;
    and an unnamed function whose named callers all share exactly one unplaced KisakCOD callee."""
    kis_calls = collections.defaultdict(set)
    for r in read_tsv(kisak_tsv):
        kis_calls[r["name"]].update(c for c in r["calls"].split(",") if c)
    kis_defined = set(kis_calls)
    fname = namer.func_name

    def fits(g, m):
        """g's own named callees must all be callees of KisakCOD's m."""
        named = {fname(x) for x in t.callees[g]} - {None}
        return named <= kis_calls.get(m, set())
    for _ in range(8):
        made = 0
        for f in list(namer.by_addr):
            n = fname(f)
            if not n or n not in kis_calls:
                continue
            open_ = [g for g in t.callees[f] if fname(g) is None and g not in namer.by_addr]
            if any(is_library(g) for g in open_):
                continue
            if len(open_) != 1:
                continue
            named_here = {fname(g) for g in t.callees[f]} - {None}
            # the two must agree: every named callee here is a callee there, and exactly one
            # KisakCOD callee is left over (any more means inlining or a changed function)
            if not named_here <= kis_calls[n]:
                continue
            left = [m for m in kis_calls[n] - named_here if m in kis_defined]
            if len(left) == 1 and left[0] not in namer.by_name and fits(open_[0], left[0]) and namer.add(
                    open_[0], left[0], "function", "kisak-graph", f"only unnamed callee of {n}; KisakCOD {n} calls {left[0]}"):
                made += 1
        for g in t.entries:
            if g in namer.by_addr or not t.callers[g] or is_library(g) or len(t.callers[g]) < 2:
                continue
            cands = None
            for f in t.callers[g]:
                n = fname(f)
                if not n or n not in kis_calls:
                    cands = None
                    break
                named_here = {fname(x) for x in t.callees[f]} - {None}
                if not named_here <= kis_calls[n]:
                    cands = None
                    break
                s = {m for m in kis_calls[n] - named_here if m in kis_defined and m not in namer.by_name}
                cands = s if cands is None else cands & s
            if cands is not None and len(cands) == 1:
                m = next(iter(cands))
                if fits(g, m) and namer.add(g, m, "function", "kisak-graph", f"the one KisakCOD callee shared by its {len(t.callers[g])} named callers"):
                    made += 1
        stats["kisak-graph"] += made
        if made == 0:
            break
