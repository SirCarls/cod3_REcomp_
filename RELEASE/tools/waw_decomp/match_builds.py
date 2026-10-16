#!/usr/bin/env python3
"""
match_builds.py - pair up the functions of two builds of the same game (Xbox 360 and PC,
or singleplayer and multiplayer) from their Ghidra exports (WawExport.java).

The two builds are compiled from the same source for different CPUs, so their code can't be
compared byte for byte. What they share is their strings, their globals, their call graph and
their tables of function pointers:

  1. seeds:   functions with the same name on both sides (from earlier naming passes), and
              functions that are the only users of the same string, the same named global
              (dvar_cg_fov) or the same combination of them;
  2. callees: for every matched pair, the two ordered lists of calls are aligned on the callees
              that are already matched; where a gap between two aligned calls holds unmatched
              callees in the same pattern on both sides ([x, y, x] against [u, v, u]), they pair up;
  3. neighbours: an unmatched function pairs with the one on the other side that shares the
              most matched callers and callees with it (at least 2, at least half of them);
  4. data:    (with --a-image/--b-image) a matched function stored at one place in each build
              anchors the pointer tables around it: the pointers at the same offsets pair up;
  5. steps 2-4 repeat until nothing new is found.

A pair is only taken when no other candidate is as close on either side, the two functions'
sizes are in the usual ratio between the two builds, and their strings (when both have some)
overlap. `--holdout` checks the method: half of the string seeds are hidden, and the result is
compared with them.

Usage:
    python3 tools/waw_decomp/match_builds.py --a <export dir> [--a-names names.tsv] [--a-image exe] \
        --b <export dir> [--b-names names.tsv] [--b-image exe] --out matches.tsv [--holdout]

matches.tsv: a_addr, b_addr, a_name, b_name, how (name, strings, calls, data), votes.
"""
import argparse
import collections
import difflib
import os
import random
import re
import statistics
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from naming_common import Tables, read_tsv  # noqa: E402

# x86 compiler helpers (exception frames, stack probes, 64-bit arithmetic, float conversion):
# their names don't belong on another CPU's functions, whatever they pair with
X86_HELPER = re.compile(r"^(__SEH_|__EH_|_?_chkstk|__alloca_probe|_?_ftol|__dtol|__ltod|__ull|__all|__aull|_?_CI|"
                        r"__security_|@__security|___report_gsfailure|__except_handler|__local_unwind|"
                        r"__global_unwind|__NLG_|__abnormal_termination|__fpclear|__fprem|__fpmath|__fptrap)")
GENERIC_NAME = re.compile(r"^(FUN_|thunk_|table_|Dvar_Register_[0-9A-F]{8}$|LAB_|DAT_|switch|_?guard)")


class Build:
    """One build's functions: size, ordered callees, strings, names."""

    def __init__(self, raw, names_tsv=None, names=None, global_names=None, image=None):
        t = Tables(raw)
        self.funcs = t.funcs
        self.size = {a: int(r["size"]) for a, r in t.funcs.items()}
        self.seq = collections.defaultdict(list)  # caller -> callees in call-site order
        rows = []
        for r in t.calls:
            if r["kind"] != "UNCONDITIONAL_CALL" or r["callee"].startswith(("ext:", "ptr:")):
                continue
            a, s, b = int(r["caller"], 16), int(r["site"], 16), int(r["callee"], 16)
            if b in t.funcs and a != b:
                rows.append((a, s, b))
        for a, s, b in sorted(rows):
            self.seq[a].append(b)
        self.callees = {a: set(v) for a, v in self.seq.items()}
        self.strs = collections.defaultdict(set)
        for text, fs in t.str_funcs.items():
            for f in fs:
                self.strs[f].add(text)
        self.str_funcs = t.str_funcs
        self.callers = collections.defaultdict(set)
        for a, cs in self.callees.items():
            for c in cs:
                self.callers[c].add(a)
        # named globals (dvar_*, cmd_*_VAR, ...) count like strings: a function that reads
        # dvar_cg_fov and dvar_cg_fovScale is the same function in both builds
        self.globals = collections.defaultdict(set)  # name -> functions using it
        gnames = {a: n for a, n in (global_names or {}).items() if not GENERIC_NAME.match(n)}
        if names_tsv:
            for r in read_tsv(names_tsv):
                if r["kind"] == "global" and not GENERIC_NAME.match(r["name"]):
                    gnames[int(r["addr"], 16)] = r["name"]
        if gnames and os.path.exists(f"{raw}/globals.tsv"):
            for r in read_tsv(f"{raw}/globals.tsv"):
                n = gnames.get(int(r["addr"], 16))
                if n and r["funcs"]:
                    self.globals["\x00" + n].update(int(f, 16) for f in r["funcs"].split(","))
        self.tok_funcs = collections.defaultdict(set)
        for text, fs in t.str_funcs.items():
            if len(text) >= 4:
                self.tok_funcs[text] |= fs
        for g, fs in self.globals.items():
            self.tok_funcs[g] |= fs
        self.toks = collections.defaultdict(set)
        for tok, fs in self.tok_funcs.items():
            for f in fs:
                self.toks[f].add(tok)
        self.names = {}
        for a, r in t.funcs.items():
            if not t.default_name(a):
                self.names[a] = r["name"]
        if names_tsv:
            for r in read_tsv(names_tsv):
                if r["kind"] == "function":
                    self.names[int(r["addr"], 16)] = r["name"]
        if names:
            self.names.update(names)
        # function pointers stored in the data (tables of handlers, callbacks in structures)
        self.ptr_at = {}  # data address -> function
        self.ptr_locs = collections.defaultdict(list)  # function -> data addresses
        self.str_ptr_at = {}  # data address -> text of the string it points to
        if image:
            for a, kind, v in data_pointers(image, set(t.funcs)):
                if kind == "f":
                    self.ptr_at[a] = v
                    self.ptr_locs[v].append(a)
                else:
                    self.str_ptr_at[a] = v


def data_pointers(path, funcs):
    """("address", "f", function) for every aligned 32-bit word in the data sections of a PE
    image that holds a function's address, and (address, "s", text) for every word that points to
    a string (2 to 63 printable characters ending in a zero byte) in the data sections. An Xbox 360
    image from xex2pe.py (machine 0x1F2) is big-endian and flat (file offset = address - base); a
    PC executable is mapped through its section table."""
    with open(path, "rb") as f:
        img = f.read()
    pe, = struct.unpack_from("<I", img, 0x3C)
    machine, nsec = struct.unpack_from("<HH", img, pe + 4)
    optsz, = struct.unpack_from("<H", img, pe + 20)
    base, = struct.unpack_from("<I", img, pe + 24 + 28)
    big = machine == 0x1F2
    s = pe + 24 + optsz
    secs = []
    for _ in range(nsec):
        name = img[s:s + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", img, s + 8)
        ch, = struct.unpack_from("<I", img, s + 36)
        s += 40
        if ch & 0x20000000 or name in (".pdata", ".reloc", ".rsrc", ".idata", ".XBLD", ".XEXID"):
            continue
        off, n = (va, vsize) if big else (raw, min(vsize, rsize))
        n = max(0, min(n, len(img) - off))
        secs.append((base + va, base + va + n, off))

    def text_at(w):
        for lo, hi, off in secs:
            if lo <= w < hi:
                o = off + (w - lo)
                b = img[o:o + 64]
                z = b.find(b"\0")
                if 2 <= z and all(32 <= c < 127 for c in b[:z]) and (o == 0 or img[o - 1] == 0 or w % 4 == 0):
                    return b[:z].decode("ascii")
                return None
        return None

    out = []
    for lo, hi, off in secs:
        n = (hi - lo) // 4
        words = struct.unpack_from((">" if big else "<") + "%dI" % n, img, off)
        for i, w in enumerate(words):
            if w in funcs:
                out.append((lo + 4 * i, "f", w))
            elif w and lo - 0x10000000 < w < hi + 0x10000000:
                t = text_at(w)
                if t is not None:
                    out.append((lo + 4 * i, "s", t))
    return out


class Matcher:
    def __init__(self, A, B, min_str=6):
        self.A, self.B = A, B
        self.min_str = min_str
        self.ab, self.ba = {}, {}
        self.how = {}
        self.ratio = (0.0, float("inf"))

    def take(self, a, b, how, votes):
        if a in self.ab or b in self.ba:
            return False
        self.ab[a], self.ba[b] = b, a
        self.how[a] = (how, votes)
        return True

    # --- seeds
    def name_seeds(self):
        na = collections.defaultdict(list)
        nb = collections.defaultdict(list)
        for a, n in self.A.names.items():
            na[n].append(a)
        for b, n in self.B.names.items():
            nb[n].append(b)
        n_new = 0
        for n, la in na.items():
            lb = nb.get(n)
            if len(la) == 1 and lb and len(lb) == 1 and not GENERIC_NAME.match(n):
                n_new += self.take(la[0], lb[0], "name", 1)
        return n_new

    def string_votes(self):
        votes = collections.defaultdict(collections.Counter)
        for tok, fa in self.A.tok_funcs.items():
            if len(fa) != 1 or (len(tok) < self.min_str and not tok.startswith("\x00")):
                continue
            fb = self.B.tok_funcs.get(tok)
            if fb and len(fb) == 1:
                votes[next(iter(fa))][next(iter(fb))] += 1
        # the same combination of strings and globals, used by one function on each side
        sig_a = collections.defaultdict(list)
        sig_b = collections.defaultdict(list)
        for f, ts in self.A.toks.items():
            sig_a[frozenset(ts)].append(f)
        for f, ts in self.B.toks.items():
            sig_b[frozenset(ts)].append(f)
        for sig, fa in sig_a.items():
            fb = sig_b.get(sig)
            if len(fa) == 1 and fb and len(fb) == 1 and (len(sig) >= 2 or len(next(iter(sig))) >= self.min_str):
                votes[fa[0]][fb[0]] += 1
        return votes

    def string_seeds(self, votes):
        rev = collections.defaultdict(collections.Counter)
        for a, c in votes.items():
            for b, n in c.items():
                rev[b][a] += n
        n_new = 0
        for a, c in sorted(votes.items()):
            (b, n), = c.most_common(1)
            if len(c) > 1 or len(rev[b]) > 1:
                continue  # some strings point elsewhere: leave it to the call graph
            n_new += self.take(a, b, "strings", n)
        return n_new

    def calibrate(self):
        """The usual size ratio between the two builds, from the pairs so far."""
        r = sorted(self.B.size[b] / self.A.size[a] for a, b in self.ab.items()
                   if self.A.size.get(a, 0) > 32 and self.B.size.get(b, 0) > 0)
        if len(r) >= 50:
            lo, hi = r[len(r) // 100], r[len(r) * 99 // 100]
            self.ratio = (lo / 1.5, hi * 1.5)
        return statistics.median(r) if r else None

    def plausible(self, a, b):
        sa, sb = self.A.size.get(a, 0), self.B.size.get(b, 0)
        if sa > 16 and sb > 16 and not self.ratio[0] <= sb / sa <= self.ratio[1]:
            return False
        ta, tb = self.A.toks.get(a, set()), self.B.toks.get(b, set())
        if ta and tb and not ta & tb:
            return False
        return True

    # --- data
    def pointer_votes(self, reach=256):
        """Matched functions stored at one place in each build anchor the two tables (or
        structures) around them; the function pointers at the same offsets pair up, until the
        layouts disagree: a pointer on one side only, a pair matched to something else, or
        different strings at the same offset (two script tables whose entries differ)."""
        A, B = self.A, self.B
        votes = collections.defaultdict(collections.Counter)
        if not A.ptr_at or not B.ptr_at:
            return votes
        for a, b in self.ab.items():
            la, lb = A.ptr_locs.get(a), B.ptr_locs.get(b)
            if not la or not lb or len(la) != 1 or len(lb) != 1:
                continue
            for step in (4, -4):
                for d in range(step, step * (reach // 4 + 1), step):
                    sx, sy = A.str_ptr_at.get(la[0] + d), B.str_ptr_at.get(lb[0] + d)
                    if sx != sy:
                        break
                    x, y = A.ptr_at.get(la[0] + d), B.ptr_at.get(lb[0] + d)
                    if (x is None) != (y is None):
                        break
                    if x is None:
                        continue
                    # the strings next to it (a script table's names) must agree too
                    if any(A.str_ptr_at.get(la[0] + d + k) != B.str_ptr_at.get(lb[0] + d + k) for k in (-8, -4, 4, 8)):
                        break
                    if x in self.ab or y in self.ba:
                        if self.ab.get(x) != y:
                            break  # matched to something else: not the same layout
                        continue
                    votes[x][y] += 1
        return votes

    # --- call graph
    def callee_votes(self):
        votes = collections.defaultdict(collections.Counter)
        for a, b in list(self.ab.items()):
            sa, sb = self.A.seq.get(a), self.B.seq.get(b)
            if not sa or not sb:
                continue
            ta = [("m", self.ab[x]) if x in self.ab else ("u", x) for x in sa]
            tb = [("m", y) if y in self.ba else ("u", y) for y in sb]
            sm = difflib.SequenceMatcher(None, ta, tb, autojunk=False)
            for op, i1, i2, j1, j2 in sm.get_opcodes():
                if op != "replace":
                    continue
                ga, gb = ta[i1:i2], tb[j1:j2]
                if any(k == "m" for k, _ in ga) or any(k == "m" for k, _ in gb):
                    continue
                pa, pb = pattern(ga), pattern(gb)
                if pa[0] == pb[0] or len(pa[1]) == len(pb[1]) == 1:
                    for x, y in zip(pa[1], pb[1]):
                        votes[x[1]][y[1]] += 1
            # the only unmatched caller on both sides
            ua = [x for x in self.A.callers.get(a, ()) if x not in self.ab]
            ub = [y for y in self.B.callers.get(b, ()) if y not in self.ba]
            if len(ua) == 1 and len(ub) == 1:
                votes[ua[0]][ub[0]] += 1
        return votes

    def neighbour_scores(self, max_deg=60):
        """For each unmatched function here, the unmatched functions there that share matched
        callers or callees with it: (shared, shared / all) for each candidate."""
        A, B, ab, ba = self.A, self.B, self.ab, self.ba

        def around_a(x):
            return {("r", ab[n]) for n in A.callers.get(x, ()) if n in ab} | \
                {("e", ab[n]) for n in A.callees.get(x, ()) if n in ab}

        cache = {}

        def around_b(y):
            if y not in cache:
                cache[y] = {("r", n) for n in B.callers.get(y, ()) if n in ba} | \
                    {("e", n) for n in B.callees.get(y, ()) if n in ba}
            return cache[y]

        scores = {}
        for x in A.funcs:
            if x in ab:
                continue
            mx = around_a(x)
            if not mx:
                continue
            cands = set()
            for rel, n in mx:
                pool = B.callees.get(n, ()) if rel == "r" else B.callers.get(n, ())
                if len(pool) <= max_deg:
                    cands.update(y for y in pool if y not in ba)
            sc = {}
            for y in cands:
                my = around_b(y)
                k = len(mx & my)
                if k:
                    sc[y] = (k, k / len(mx | my), len(mx), len(my))
            if sc:
                scores[x] = sc
        return scores

    def accept_neighbours(self, scores):
        best_for_b = {}
        for x, sc in scores.items():
            for y, v in sc.items():
                if y not in best_for_b or v[:2] > best_for_b[y][0]:
                    best_for_b[y] = (v[:2], x, best_for_b.get(y, ((0, 0), None, 0))[0])
                elif v[:2] >= best_for_b[y][2]:
                    best_for_b[y] = (best_for_b[y][0], best_for_b[y][1], v[:2])
        n_new = 0
        for x, sc in sorted(scores.items()):
            ranked = sorted(sc.items(), key=lambda kv: kv[1][:2], reverse=True)
            y, (k, j, nx, ny) = ranked[0]
            second = ranked[1][1][:2] if len(ranked) > 1 else (0, 0)
            if second[0] >= k or second[1] >= j:
                continue
            bb = best_for_b[y]
            if bb[1] != x or bb[2][0] >= k or bb[2][1] >= j:
                continue
            if k >= 2 and j >= 0.5 or k == 1 and nx == ny == 1 and len(sc) == 1:
                if self.plausible(x, y):
                    n_new += self.take(x, y, "calls", k)
        return n_new

    def accept(self, votes, how):
        rev = collections.defaultdict(collections.Counter)
        for a, c in votes.items():
            for b, n in c.items():
                rev[b][a] += n
        n_new = 0
        for a, c in sorted(votes.items()):
            if a in self.ab:
                continue
            (b, n), = c.most_common(1)
            if b in self.ba or len(c) > 1 or len(rev[b]) > 1 or not self.plausible(a, b):
                continue
            n_new += self.take(a, b, how, n)
        return n_new

    def run(self, seeds_names=True, string_votes=None, log=print):
        if seeds_names:
            log(f"  name seeds: {self.name_seeds()}")
        sv = self.string_votes() if string_votes is None else string_votes
        log(f"  string seeds: {self.string_seeds(sv)}")
        med = self.calibrate()
        log(f"  size ratio b/a: median {med:.2f}, allowed {self.ratio[0]:.2f}-{self.ratio[1]:.2f}" if med else
            "  size ratio: not enough pairs")
        rnd = 0
        while True:
            rnd += 1
            n = self.accept(self.callee_votes(), "calls")
            n += self.accept_neighbours(self.neighbour_scores())
            n += self.accept(self.pointer_votes(), "data")
            log(f"  call graph round {rnd}: +{n}")
            if not n or rnd >= 30:
                break
        return self.ab


def pattern(g):
    """([index of first occurrence...], [distinct items in order]) of a gap."""
    first, items, shape = {}, [], []
    for k in g:
        if k not in first:
            first[k] = len(items)
            items.append(k)
        shape.append(first[k])
    return tuple(shape), items


HOW = {"strings": "the only users of the same strings and named globals",
       "data": "the same place in a table of function pointers",
       "calls": "the same place in the call graph", "name": "the same name"}


def pass_graph(namer, raw_here, raw_there, names_there, label, where, stats, notes=False,
               image_here=None, image_there=None, cross_cpu=False):
    """A naming pass: match this build's functions with another build's (seeded with the names
    so far) and take that build's names. With notes=True, matched functions that stay unnamed
    get a note with their counterpart's address. With cross_cpu, the other build's x86 compiler
    helpers (exception frames, stack probes) give no names. Returns the Matcher."""
    here_f = {a: v[0] for a, v in namer.by_addr.items() if v[1] == "function"}
    here_g = {a: v[0] for a, v in namer.by_addr.items() if v[1] == "global"}
    A = Build(raw_here, names=here_f, global_names=here_g, image=image_here)
    B = Build(raw_there, names_there, image=image_there)
    m = Matcher(A, B)
    m.run(log=lambda s: None)
    for a in sorted(m.ab):
        b = m.ab[a]
        how, votes = m.how[a]
        n = B.names.get(b)
        if n and (n.startswith("FID_conflict:") or cross_cpu and X86_HELPER.match(n)):
            n = None  # an ambiguous library match, or a helper of the other CPU
        if a in namer.by_addr:
            if n and namer.by_addr[a][0] != n and not GENERIC_NAME.match(n):
                stats[f"{label}_other_name"] += 1
            continue
        if n and not GENERIC_NAME.match(n) and namer.add(
                a, n, "function", label, f"{n} at {b:08X} in {where}: {HOW[how]} ({votes})"):
            stats[label] += 1
        elif notes:
            namer.note(a, label, f"{where} {b:08X}" + (f" ({n})" if n else "") + f": {HOW[how]} ({votes})")
            stats[label + "_notes"] += 1
    stats[label + "_matched"] = len(m.ab)
    return m


def holdout(A, B, log=print):
    """Hide half of the string seeds, match, and compare with the hidden half."""
    m0 = Matcher(A, B)
    votes = m0.string_votes()
    truth = {}
    for a, c in votes.items():
        if len(c) == 1:
            truth[a] = next(iter(c))
    keys = sorted(truth)
    random.Random(1).shuffle(keys)
    hidden = set(keys[: len(keys) // 2])
    shown = {a: c for a, c in votes.items() if a not in hidden}
    m = Matcher(A, B)
    m.run(string_votes=shown, log=lambda s: None)
    ok = sum(1 for a in hidden if m.ab.get(a) == truth[a] and m.how[a][0] in ("calls", "data"))
    bad = sum(1 for a in hidden if a in m.ab and m.ab[a] != truth[a])
    log(f"holdout: {len(hidden)} hidden string pairs; the call graph found {ok} of them, "
        f"{bad} matched elsewhere ({100 * ok / max(1, ok + bad):.1f}% agree)")
    return ok, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True)
    ap.add_argument("--a-names")
    ap.add_argument("--b", required=True)
    ap.add_argument("--b-names")
    ap.add_argument("--a-image", help="the executable (PC) or xex2pe.py image (Xbox 360), for function pointers in the data")
    ap.add_argument("--b-image")
    ap.add_argument("--out", required=True)
    ap.add_argument("--holdout", action="store_true")
    args = ap.parse_args()
    A = Build(args.a, args.a_names, image=args.a_image)
    B = Build(args.b, args.b_names, image=args.b_image)
    if args.holdout:
        holdout(A, B)
    m = Matcher(A, B)
    m.run()
    with open(args.out, "w", encoding="utf-8") as o:
        o.write("a_addr\tb_addr\ta_name\tb_name\thow\tvotes\n")
        for a in sorted(m.ab):
            b = m.ab[a]
            o.write(f"{a:08X}\t{b:08X}\t{A.names.get(a, '')}\t{B.names.get(b, '')}\t{m.how[a][0]}\t{m.how[a][1]}\n")
    c = collections.Counter(h for h, _ in m.how.values())
    print(f"matched {len(m.ab)} of {len(A.funcs)} / {len(B.funcs)} functions: " +
          ", ".join(f"{k} {v}" for k, v in c.most_common()))


if __name__ == "__main__":
    sys.exit(main())
