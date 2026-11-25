#!/usr/bin/env python3
"""[waw-decomp] 2026-11-16 discmap: Call of Duty 3's addresses on every disc.

The sources are written for one build of each program (USA: default.xex, codmp_xenonf.xex and the 15
level DLLs). discmap finds every address the sources and codegen configs use in another disc's build
and writes that build's configs and address table, so the same sources build for that disc.

  discmap.py [--project DIR] [--work DIR] [--overrides DIR] COMMAND ...

  identify SOURCE...     each program on a disc (an image, a folder, a .7z): the SHA-1 of its decrypted
                         image and which build it is (builds.tsv); an unknown image is a new build
  match REF OTHER        where each address REF's code uses is in OTHER: same / moved / changed /
                         gone / unresolved (map_OTHER.tsv, map_OTHER.txt), and overrides_OTHER.tsv
                         with the rows a person decides
  verify BUILD           the code (and data) at each carried address compared in both programs
  emit BUILD OUTDIR      the build's address map, codegen configs, address table and
                         build_addresses.h (refuses while overrides_BUILD.tsv has empty decisions)
  inventory OUT          every address the sources and configs use, as a table

--work: discmap's own folder (default ./discmap-work): what identify saw, the decrypted images (game
data: they stay on this computer; delete the folder when done), the tools' outputs, the maps.
--overrides: where overrides_<build>.tsv live (default: the work folder); the reviewed ones belong
in the project.

Nothing is ever run, patched or written on a disc or in the project.
"""
from __future__ import annotations

import argparse
import collections
import datetime
import hashlib
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
import emit  # noqa: E402
import inventory  # noqa: E402
import matching  # noqa: E402
import overrides  # noqa: E402
import verify  # noqa: E402
from common import (HERE, IMAGE_BASE, LEVEL_BASE, MAIN_FILES, Project, Work, hex8, part_program,  # noqa: E402
                    read_meta, read_tsv, write_tsv)

VERSION = "discmap 1 (2026-11-16)"


# ---------------------------------------------------------------- known builds
class Builds:
    def __init__(self, path: Path):
        self.rows = read_tsv(path)
        self.by_sha1 = {r["sha1_decrypted"]: r for r in self.rows}

    def parts(self, build: str) -> dict[str, dict]:
        return {r["part"]: r for r in self.rows if r["build"] == build}

    def main(self, build: str) -> dict | None:
        return next((r for r in self.rows if r["build"] == build and r["part"] in MAIN_FILES), None)


def setup(args):
    project = Project(common.find_project(args.project))
    work = Work(Path(args.work).resolve())
    known = Builds(HERE / "builds.tsv")
    return project, work, known


def overrides_path(args, work: Work, build: str) -> Path:
    folder = Path(args.overrides).resolve() if args.overrides else work.folder
    return folder / f"overrides_{build}.tsv"


# ---------------------------------------------------------------- identify
def identify_source(work: Work, known: Builds, source: str) -> list[list[str]]:
    import disc_get  # tools/x360_family
    src = disc_get.open_source(source)
    found = []
    try:
        for part, entry in common.program_files(src.entries()):
            data = disc_get.read_small(src, entry)
            raw = hashlib.sha1(data).hexdigest()
            try:
                image, load = common.decrypt_xex(data)
            except Exception as e:  # noqa: BLE001
                print(f"  {part}: cannot decrypt ({e})")
                continue
            sha1 = hashlib.sha1(image).hexdigest()
            work.store_image(sha1, image)
            k = known.by_sha1.get(sha1)
            found.append({"part": part, "sha1": sha1, "raw": raw, "load": load, "known": k["build"] if k else ""})
    finally:
        src.close()
    # which build each program is: a campaign is its default.xex and every level DLL together
    rows = []
    for program in ("campaign", "mp"):
        parts = [f for f in found if (part_program(f["part"]) == program
                                      or (program == "campaign" and f["part"].startswith("sp/")))]
        if not parts:
            continue
        main = next((f for f in parts if f["part"] in MAIN_FILES), None)
        build = main["known"] if main else ""
        if build:
            want = {p: r["sha1_decrypted"] for p, r in known.parts(build).items()}
            have = {f["part"]: f["sha1"] for f in parts}
            if want != have:
                build = ""
        if not build:
            digest = hashlib.sha1("".join(sorted(f["sha1"] for f in parts)).encode()).hexdigest()
            build = f"{'sp' if program == 'campaign' else 'mp'}_{digest[:8]}"
        for f in parts:
            rows.append([build, part_program(f["part"]), f["part"], f["sha1"], f["raw"], f"{f['load']:08X}",
                         str(Path(source).resolve()), f["known"] or "NEW"])
    return rows


def cmd_identify(args):
    project, work, known = setup(args)
    all_rows = []
    for source in args.sources:
        print(f"== {source}")
        rows = identify_source(work, known, source)
        for b, program, part, sha1, raw, load, _, k in rows:
            what = f"known: {k}" if k != "NEW" else "NEW: an image discmap doesn't know"
            print(f"  {part:28s} {sha1}  build {b:12s} {what}")
        for b in sorted({r[0] for r in rows}):
            status = "known build" if known.main(b) else "NEW BUILD (match it against its reference: discmap match)"
            print(f"  -> {b}: {status}")
        all_rows += rows
    work.remember(all_rows)


# ---------------------------------------------------------------- images of a build
def build_parts(work: Work, known: Builds, build: str) -> dict[str, dict]:
    """part -> {sha1, path, load, raw} for every program file of a build identify has seen."""
    out = {}
    for r in work.seen():
        if r["build"] != build or r["part"] in out:
            continue
        path = work.image_path(r["sha1_decrypted"])
        if not path.exists():
            print(f"  reading {build}'s images again from {r['source']}")
            identify_source(work, known, r["source"])
        if not path.exists():
            sys.exit(f"error: no image for {build} {r['part']}")
        out[r["part"]] = {"sha1": r["sha1_decrypted"], "path": path, "load": int(r["load"], 16),
                          "raw": r["sha1_raw"]}
    if not out:
        sys.exit(f"error: build {build} not seen: run `discmap.py identify` on its disc first (or pass --disc)")
    return out


def export_folder(project: Project, known: Builds, build: str) -> Path | None:
    main = known.main(build)
    if main and main["export"]:
        folder = project.root / main["export"]
        if (folder / "functions.tsv").exists():
            return folder
    return None


# ---------------------------------------------------------------- match
def cmd_match(args):
    project, work, known = setup(args)
    if args.disc:
        work.remember([row for source in args.disc for row in identify_source(work, known, source)])
    ref_parts, other_parts = build_parts(work, known, args.ref), build_parts(work, known, args.other)
    main = next((p for p in ref_parts if p in MAIN_FILES), None)
    if main is None or main not in other_parts:
        sys.exit(f"error: {args.ref} and {args.other} are not builds of the same program")
    program = part_program(main)
    folder = work.match_dir(args.ref, args.other)
    log = folder / "match.log"
    started = datetime.datetime.now()
    with open(log, "a", encoding="utf-8") as w:
        w.write(f"== {started:%Y-%m-%d %H:%M:%S} {VERSION}: match {args.ref} {args.other}\n")

    rows = inventory.scan(project.recomp)
    inventory.write_rows(rows, folder / "inventory.tsv")
    import build_diff  # tools/x360_family
    ref, other = ref_parts[main], other_parts[main]
    ref_img, other_img = build_diff.Image(ref["path"], ref["load"]), build_diff.Image(other["path"], other["load"])
    ref_raw, other_raw = export_folder(project, known, args.ref), export_folder(project, known, args.other)
    ref_pdata, other_pdata = matching.pdata_functions(ref_img), matching.pdata_functions(other_img)
    funcs = inventory.RefFunctions(ref_pdata, matching.export_functions(ref_raw))
    ents = inventory.entries(rows, program, funcs)

    notes = []
    if ref["sha1"] == other["sha1"]:
        method = "identical"
        results = matching.match_identical(ents)
    elif len(ref_pdata) == len(other_pdata):
        method = "regional rebuild (build_diff.py)"
        results = matching.match_regional(project, folder, ref["path"], other["path"], ref_img, other_img, ents, log)
        stored = project.root / "dev/x360-family/cod3_regions" / args.other / "addresses.tsv"
        notes.append(matching.compare_stored_map(results, program, stored))
    else:
        method = "later build (rebuild_diff.py + carry_addresses.py, match_builds.py as witness)"
        results, witness_note = matching.match_later(project, folder, ref["path"], other["path"], ref_raw, other_raw,
                                                     ref_img, other_img, funcs, ents, log, not args.no_witness,
                                                     args.witness_timeout)
        notes.append(witness_note)
        country, _, kind = args.other.partition("_")
        notes.append(matching.compare_stored_pairs(folder / "rebuild_pairs.tsv", project.root /
                                                   "dev/x360-family/cod3_regions" / country / f"{kind}_pairs.tsv"))

    level_lines = []
    if program == "campaign":
        levels = set(project.level_names()) | {r.program[6:] for r in rows if r.program.startswith("level:")}
        for level in sorted(levels):
            part = f"sp/{level}/{level}.dll"
            lents = inventory.entries(rows, "level:" + level, None)
            if not lents:
                continue
            rp, op = ref_parts.get(part), other_parts.get(part)
            if rp is None:
                sys.exit(f"error: {args.ref} has no {part}")
            lres, what = matching.match_level(project, folder, level, rp["path"], op["path"] if op else None,
                                              bool(op) and op["sha1"] == rp["sha1"], rp["load"] or LEVEL_BASE,
                                              lents, log)
            results.update(lres)
            ents += lents
            c = collections.Counter(r.status for r in lres.values())
            level_lines.append(f"  {level:10s} {len(lents):4d} config addresses: " +
                               ", ".join(f"{k} {c[k]}" for k in ("same", "moved", "changed", "gone", "unresolved")
                                         if c[k]) + f"  ({what})")

    map_rows = matching.map_rows(ents, results)
    meta = {"build": args.other, "reference": args.ref, "program": program, "method": method,
            "reference_sha1": ref["sha1"], "build_sha1": other["sha1"], "build_sha1_raw": other["raw"],
            "inventory_rows": str(len(rows)), "made": f"{started:%Y-%m-%d %H:%M} by {VERSION}"}
    out = work.map_path(args.other)
    common.write_tsv(out, matching.MAP_HEADER, map_rows, meta)
    write_tsv(work.folder / f"functions_{args.other}.tsv", ["start", "size"],
              [[f"{a:08X}", s] for a, s in sorted(other_pdata.items())])
    lines = summarize(args.other, args.ref, program, method, ents, results, notes, level_lines)
    lines.append(f"time: {(datetime.datetime.now() - started).total_seconds():.0f} s")
    (work.folder / f"map_{args.other}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    ov = overrides_path(args, work, args.other)
    changes = overrides.merge(ov, read_tsv(out), args.other)
    print(f"overrides: {ov} ({len(overrides.needed(read_tsv(out)))} rows need a person; "
          f"{len(changes)} changes this run)")
    for c in changes[:20]:
        print("  " + c)


def summarize(build, ref, program, method, ents, results, notes, level_lines) -> list[str]:
    main = [e for e in ents if e.program == program]
    lines = [f"== {build} ({program}) against {ref}: {method}", f"   {len(main)} unique addresses in the reference's"
             " sources and configs"]
    statuses = ("same", "moved", "changed", "gone", "unresolved")
    lines.append(f"  {'group':44s} {'total':>6s} " + " ".join(f"{s:>10s}" for s in statuses))
    by_group = collections.defaultdict(collections.Counter)
    for e in main:
        by_group[e.group][results[(e.program, e.usa)].status] += 1
    for g in sorted(by_group):
        c = by_group[g]
        lines.append(f"  {g + ' ' + inventory.GROUP_NAMES[g]:44s} {sum(c.values()):6d} "
                     + " ".join(f"{c[s]:10d}" for s in statuses))
    req = [e for e in main if e.required]
    c = collections.Counter(results[(e.program, e.usa)].status for e in req)
    auto = c["same"] + c["moved"]
    share = 100 * auto / max(1, len(req))
    lines.append(f"  used by code or codegen (groups 1-9): {len(req)}; automatic {auto} ({share:.1f}%)"
                 f" = same {c['same']} + moved {c['moved']}; needs a person: changed {c['changed']}, gone {c['gone']}, "
                 f"unresolved {c['unresolved']}")
    methods = collections.Counter(results[(e.program, e.usa)].method for e in req
                                  if results[(e.program, e.usa)].status in ("same", "moved"))
    lines.append("  automatic, by method: " + ", ".join(f"{k} {v}" for k, v in methods.most_common()))
    disagree = [e for e in req if results[(e.program, e.usa)].how.endswith("the witnesses disagree")]
    if any(r.witness for r in results.values()):
        w = collections.Counter()
        for e in req:
            for part in results[(e.program, e.usa)].witness.split("; "):
                if part:
                    w[part.split(" ")[0] + (" agrees" if part.endswith("agrees") else " differs")] += 1
        lines.append("  witnesses on the required rows: " + ", ".join(f"{k} {v}" for k, v in sorted(w.items())))
        lines.append(f"  rows the witnesses disagree on (made unresolved): {len(disagree)}; without that check: "
                     f"automatic "
                     f"{auto + len(disagree)} ({100 * (auto + len(disagree)) / max(1, len(req)):.1f}%), unresolved "
                     f"{c['unresolved'] - len(disagree)}")
    near = [e for e in req if results[(e.program, e.usa)].status in ("same", "moved")
            and "carry_addresses.py: near" in results[(e.program, e.usa)].how]
    if near:
        lines.append(f"  of the automatic ones, {len(near)} are carry_addresses.py's 'near' (a global at the same "
                     "distance from a voted one; its own word for it is 'check')")
    loose = [e for e in req if e.loose]
    differ = [e for e in loose if results[(e.program, e.usa)].addr not in (None, e.usa)]
    lpending = [e for e in loose if results[(e.program, e.usa)].status in matching.NEEDS_PERSON]
    lines.append(f"  loose literals (outside addresses.h / addresses_mp.h): {len(loose)} unique; {len(differ)} at "
                 f"another address in this build; {len(lpending)} need a person")
    hook_rows = [e for e in req if e.group in ("1", "2")
                 and results[(e.program, e.usa)].status in matching.NEEDS_PERSON]
    if hook_rows:
        in_changed = [e for e in hook_rows if results[(e.program, e.usa)].status in ("changed", "gone")]
        names = sorted({(e.syms or [f"{e.usa:08X}"])[0] for e in hook_rows})
        lines.append(f"  hooks / mid-asm sites that need a person: {len(hook_rows)} ({len(in_changed)} in changed or "
                     f"gone functions, {len(hook_rows) - len(in_changed)} unresolved): " + " ".join(names)[:600])
    lines += ["  " + n for n in notes if n]
    if level_lines:
        lent = [e for e in ents if e.program.startswith("level:")]
        lc = collections.Counter(results[(e.program, e.usa)].status for e in lent)
        lines.append(f"== level DLL configs: {len(lent)} addresses; same {lc['same']}, moved {lc['moved']}, changed "
                     f"{lc['changed']}, gone {lc['gone']}, unresolved {lc['unresolved']}")
        lines += level_lines
    return lines


# ---------------------------------------------------------------- verify
def load_map(work: Work, build: str) -> tuple[list[dict], dict]:
    path = work.map_path(build)
    if not path.exists():
        sys.exit(f"error: no map for {build}: run `discmap.py match REF {build}` first")
    return read_tsv(path), read_meta(path)


def function_pairs(paths: list[Path]) -> dict:
    """The functions that kept their code, from the first of a match folder's build_diff.py /
    rebuild_diff.py tables that exists."""
    for path in paths:
        if path.exists():
            return verify.pairs_from_map_folder(read_tsv(path))
    return {}


def cmd_verify(args):
    project, work, known = setup(args)
    map_rows, meta = load_map(work, args.build)
    ref, program = meta["reference"], meta["program"]
    ref_parts, other_parts = build_parts(work, known, ref), build_parts(work, known, args.build)
    folder = work.match_dir(ref, args.build)
    import build_diff
    images, moved = {}, {}
    for part, rp in ref_parts.items():
        op = other_parts.get(part)
        if op is None or op["sha1"] == rp["sha1"]:
            continue        # identical images need no check
        prog = part_program(part)
        images[prog] = (build_diff.Image(rp["path"], rp["load"]), build_diff.Image(op["path"], op["load"]))
        if prog == program:
            tables = [folder / "rebuild_pairs.tsv", folder / "build_diff" / "functions.tsv"]
        else:
            tables = [folder / f"level_{prog[6:]}_pairs.tsv"]
        moved[prog] = verify.MovedFunctions(function_pairs(tables))
    table, _ = overrides.final_table(map_rows, overrides_path(args, work, args.build))
    pending = {(r["program"], int(r["usa_addr"], 16)): common.parse_hex(r["build_addr"]) for r in map_rows
               if r["status"] in matching.NEEDS_PERSON}
    lines, counts = verify.verify(images, moved, map_rows, table, pending)
    head = [f"== verify {args.build} against {ref} ({VERSION})"] + verify.summary(counts)
    if not images:
        head.append("the images are identical: nothing to compare")
    (work.folder / f"verify_{args.build}.txt").write_text("\n".join(head + lines) + "\n", encoding="utf-8")
    print("\n".join(head))
    print("\n".join(line for line in lines if line.startswith("DIFFERS"))[:4000])
    bad = sum(v for (kind, family, ok), v in counts.items() if kind == "automatic" and family == "code" and not ok)
    return 1 if bad else 0


# ---------------------------------------------------------------- emit
def cmd_emit(args):
    project, work, known = setup(args)
    map_rows, meta = load_map(work, args.build)
    build, program = args.build, meta["program"]
    outdir = Path(args.outdir).resolve()
    ov = overrides_path(args, work, build)

    # the map must cover what the sources use now
    rows = inventory.scan(project.recomp)
    in_map = {(r["program"], int(r["usa_addr"], 16)) for r in map_rows}
    progs = {program}
    if program == "campaign":
        progs |= {r.program for r in rows if r.program.startswith("level:")}
    missing = sorted({(r.program, r.addr) for r in rows if r.addr is not None and r.program in progs} - in_map)
    if missing:
        print(f"error: the sources use {len(missing)} addresses the map of {build} doesn't have (the sources changed "
              f"since the match): run `discmap.py match {meta['reference']} {build}` again. First ones:")
        for p, a in missing[:20]:
            print(f"  {p} {a:08X}")
        return 2
    final, problems = overrides.final_table(map_rows, ov)
    if problems:
        print(f"error: {len(problems)} rows of {ov.name} need a decision before anything is written:")
        for p in problems[:40]:
            print("  " + p)
        return 2
    by_program = collections.defaultdict(dict)
    for (p, a), v in final.items():
        by_program[p][a] = v

    report, problems, files = [], emit.Problems(), {}
    main_part = "default.xex" if program == "campaign" else "codmp_xenonf.xex"
    sha1 = meta["build_sha1"]
    ref_raw = {x for x in (known.main(meta["reference"]) or {}).get("sha1_raw", "").split(",") if x}
    build_raw = (meta.get("build_sha1_raw") or "").split(",")[0]
    if program == "campaign":
        tomls = [("config/default.toml", "campaign"), ("config/names.toml", "campaign")]
        tomls += [(f"config/{p[6:]}.toml", p) for p in sorted(progs) if p.startswith("level:")]
        header, names_file = "src/game/addresses.h", "config/names.toml"
    else:
        tomls = [("mp/config/codmp.toml", "mp"), ("mp/config/codmp_campaign.toml", "mp"),
                 ("mp/config/names_mp.toml", "mp")]
        header, names_file = "src/game/mp/addresses_mp.h", "mp/config/names_mp.toml"
    for rel, p in tomls:
        text = (project.recomp / rel).read_text(encoding="utf-8")
        known_keys = {int(r["usa_addr"], 16) for r in map_rows if r["program"] == p}
        body = emit.carry_toml(text, by_program[p], known_keys, rel, problems, report, names_file=rel == names_file)
        for raw in ref_raw:
            if raw in body and build_raw:
                body = body.replace(raw, f"{build_raw} ({build}; was {raw})")
        part = main_part if p == program else f"sp/{p[6:]}/{p[6:]}.dll"
        files[rel] = emit.generated_header("#", build, part, sha1, "file of the same name") + body
    # hook names, at the end of the names file
    names_by_addr = {}
    for line in files[names_file].splitlines():
        m = inventory.NAME_LINE.match(line)
        if m:
            names_by_addr[int(m.group(1), 16)] = m.group(2)
    func_file = work.folder / f"functions_{build}.tsv"
    func_rows = read_tsv(func_file) if func_file.exists() else []
    functions = {int(r["start"], 16) for r in func_rows}
    ref_funcs = {int(r["usa_addr"], 16) for r in map_rows if r["program"] == program and r["class"] == "func"}
    resolver = inventory.Resolver(project.recomp)
    # [waw-decomp/discs2] 2026-11-24: a hooked address with no .pdata entry (a callback the codegen is told about,
    # such as menus_mp.cpp's sub_82412170) is a function to the codegen as well, so it needs its name too: without
    # it the German cod3mp did not link (__imp__sub_82412170 undefined).
    hook_rows = [r for r in rows if r.addr in ref_funcs or r.kind in ("hook", "orig")]
    files[names_file] += "".join(emit.hook_names(hook_rows, resolver, program, by_program[program], names_by_addr,
                                                 functions, build, problems, report))
    hsrc = (project.recomp / header).read_text(encoding="utf-8")
    files[header] = emit.generated_header("//", build, main_part, sha1, Path(header).name) + \
        emit.carry_header(hsrc, by_program[program], header, report)
    required = {int(r["usa_addr"], 16) for r in map_rows if r["program"] == program and r["required"] == "yes"}
    files["build_addresses.h"] = emit.build_addresses_h(build, main_part, sha1,
                                                        {a: v for a, v in by_program[program].items() if a in required})
    if problems:
        print(f"error: {len(problems)} problems; nothing written:")
        for p in problems[:40]:
            print("  " + p)
        return 2
    for rel, text in files.items():
        emit.write(outdir / rel, text)
    emit.write_map(outdir / "address_map.tsv", map_rows, final,
                   {"build": build, "reference": meta["reference"], "made": f"{datetime.datetime.now():%Y-%m-%d %H:%M}"
                    f" by {VERSION}", "overrides": str(ov)})
    sources = collections.Counter(src for _, src in final.values())
    summary = [f"== emit {build} ({program}) into {outdir}",
               f"addresses: {len(final)} with an answer ("
               + ", ".join(f"{k} {v}" for k, v in sources.most_common()) + ")",
               f"files: {', '.join(sorted(files))}, address_map.tsv",
               f"changed lines and notes: {len(report)}"]
    emit.write(outdir / "emit_report.txt", "\n".join(summary + report) + "\n")
    print("\n".join(summary))
    return 0


def cmd_inventory(args):
    project = Project(common.find_project(args.project))
    rows = inventory.scan(project.recomp)
    inventory.write_rows(rows, Path(args.out))
    c = collections.Counter((r.program if not r.program.startswith("level:") else "levels", r.kind) for r in rows)
    print(f"{len(rows)} rows: " + ", ".join(f"{p} {k} {v}" for (p, k), v in sorted(c.items())))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", help="the project folder (default: $COD3_PROJECT or the folder above tools/)")
    ap.add_argument("--work", default="discmap-work", help="discmap's work folder (default ./discmap-work)")
    ap.add_argument("--overrides", help="the folder of overrides_<build>.tsv (default: the work folder)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("identify", help="which build each program on a disc is")
    p.add_argument("sources", nargs="+")
    p = sub.add_parser("match", help="carry the reference build's addresses to another build")
    p.add_argument("ref")
    p.add_argument("other")
    p.add_argument("--disc", action="append", default=[], help="identify this disc first (repeatable)")
    p.add_argument("--no-witness", action="store_true", help="don't run match_builds.py (later builds)")
    p.add_argument("--witness-timeout", type=float, default=3600, help="seconds for match_builds.py (default 3600)")
    p = sub.add_parser("verify", help="compare the code at each carried address")
    p.add_argument("build")
    p = sub.add_parser("emit", help="write a build's map, configs and headers")
    p.add_argument("build")
    p.add_argument("outdir")
    p = sub.add_parser("inventory", help="every address the sources and configs use")
    p.add_argument("out")
    args = ap.parse_args(argv)
    handler = {"identify": cmd_identify, "match": cmd_match, "verify": cmd_verify, "emit": cmd_emit,
               "inventory": cmd_inventory}[args.cmd]
    return handler(args) or 0


if __name__ == "__main__":
    sys.exit(main())
