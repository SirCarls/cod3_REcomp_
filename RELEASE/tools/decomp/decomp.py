#!/usr/bin/env python3
"""The Rust decomp's command line. See docs/DECOMP.md for the whole workflow.

    python3 tools/decomp/decomp.py setup              decrypt the game's images, check the tools
    python3 tools/decomp/decomp.py dis NAME|ADDR      readable disassembly of one function
    python3 tools/decomp/decomp.py call NAME|ADDR ARG...
                                                      run one original function (ARG: i:5, f:1.5,
                                                      s:text, x:4 for 4 zero bytes); prints r3/f1
    python3 tools/decomp/decomp.py calls NAME|ADDR    the functions one function calls (and jumps to)
    python3 tools/decomp/decomp.py callers NAME|ADDR  every call (and jump) to one function
    python3 tools/decomp/decomp.py check [NAME...]    compare Rust ports with the originals
                                                      (--cases N, --seed S, --area AREA)
    python3 tools/decomp/decomp.py status             what is ported and checked, by area
    python3 tools/decomp/decomp.py golden             record the original's answers for the float
                                                      functions, so `cargo test` checks them on any
                                                      platform without the game (--cases N, --all)
    python3 tools/decomp/decomp.py asm                write dev/decomp/default.asm and .names for tools/ppcx.py

The game's files come from dev/decomp-input/ (or COD3_DECOMP_INPUT); they are
never part of the repository.
"""
from __future__ import annotations

import argparse
import dataclasses
import importlib
import random
import re
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import image  # noqa: E402

CRATE = image.ROOT / "engine" / "crates" / "cod3_game" / "src"
GOLDEN = image.ROOT / "engine" / "tools" / "decomp_check" / "golden"
# A fixed seed, so regenerating the golden files without changes gives the same files.
GOLDEN_SEED = 20261013
ORIGINAL_LINE = re.compile(r"^\s*///.*?\bOriginal:\s*`([^`]+)`\s*(?:\(|at\s+)?(0x[0-9A-Fa-f]{8})?")


def load_specs() -> dict:
    import harness
    for path in sorted((HERE / "specs").glob("*.py")):
        if path.name != "__init__.py":
            importlib.import_module(f"specs.{path.stem}")
    return harness.SPECS


def ported_in_rust() -> dict[str, tuple[str, str]]:
    """Original name -> (Rust file, address text), from the `/// Original:` doc lines."""
    found = {}
    for path in sorted(CRATE.rglob("*.rs")):
        for line in path.read_text(encoding="utf-8").splitlines():
            m = ORIGINAL_LINE.match(line)
            if m:
                found[m.group(1)] = (path.relative_to(CRATE).as_posix(), m.group(2) or "")
    return found


def cmd_setup(_args) -> int:
    for name in image.FILE_NAMES:
        try:
            img = image.load(name)
            print(f"  {name:<9} {len(img.data) / 1e6:5.1f} MB at 0x{img.base:08X}, {len(img.functions):>6} functions")
        except SystemExit as e:
            print(f"  {name:<9} missing ({str(e).splitlines()[0]})")
    import oracle  # noqa: F401  (checks the Unicorn build)
    oracle.Oracle(image.load("default"))
    print("  oracle    OK (Unicorn runs PowerPC floating point)")
    try:
        import capstone  # noqa: F401  (only checking that it's installed)
        print("  capstone  OK")
    except ImportError:
        print("  capstone  missing: pip install capstone (only needed for `dis`)")
    return 0


def cmd_dis(args) -> int:
    import disasm
    img = image.load(args.image)
    start = img.resolve(args.what)
    end = start + 4 * args.count if args.count else None
    print(f"== {img.describe(start)} (0x{start:08X})")
    print("\n".join(disasm.disassemble(img, start, end)))
    return 0


def _branch_target(address: int, word: int) -> tuple[int, bool] | None:
    """(target, is_call) for a relative `b`/`bl` at `address`, else None."""
    if word >> 26 != 18 or word & 2:  # not an I-form branch, or an absolute one
        return None
    offset = word & 0x03FFFFFC
    if offset & 0x02000000:
        offset -= 0x04000000
    return (address + offset) & 0xFFFFFFFF, bool(word & 1)


def cmd_calls(args) -> int:
    """Calls (`bl`) and tail jumps (`b` out of the function) of one function."""
    img = image.load(args.image)
    start = img.resolve(args.what)
    span = img.function_at(start)
    end = span[1] if span else start + 4
    print(f"== {img.describe(start)} (0x{start:08X}) calls:")
    for address in range(start, end, 4):
        found = _branch_target(address, img.u32(address))
        if found is None:
            continue
        target, is_call = found
        if is_call or not start <= target < end:
            kind = "call" if is_call else "jump"
            print(f"  {address:08X} {kind} 0x{target:08X} {img.describe(target)}")
    return 0


def cmd_callers(args) -> int:
    """Every `bl`/`b` in the image whose target is the given function."""
    img = image.load(args.image)
    target = img.resolve(args.what)
    count = 0
    for lo, hi in img.code_ranges():
        raw = img.read(lo, hi - lo)
        words = struct.unpack(f">{len(raw) // 4}I", raw[:len(raw) // 4 * 4])
        for i, word in enumerate(words):
            if word >> 26 != 18:
                continue
            address = lo + 4 * i
            found = _branch_target(address, word)
            if found and found[0] == target:
                count += 1
                print(f"  {address:08X} {'call' if found[1] else 'jump'} from {img.describe(address)}")
    print(f"{count} call(s) and jump(s) to {img.describe(target)}")
    return 0


def _parse_call_arg(o, text: str):
    kind, _, value = text.partition(":")
    if kind == "i":
        return "gpr", int(value, 0)
    if kind == "f":
        return "fpr", float(value)
    if kind == "s":
        return "gpr", o.put_cstr(value.encode("latin-1"))
    if kind == "x":
        return "gpr", o.put(bytes(int(value, 0)))
    raise SystemExit(f"unknown argument {text!r} (use i:, f:, s:, x:)")


def cmd_call(args) -> int:
    import oracle
    o = oracle.for_image(args.image)
    img = o.images[-1]
    gprs, fprs = [], []
    for a in args.args:
        reg, v = _parse_call_arg(o, a)
        (fprs if reg == "fpr" else gprs).append(v)
    address = img.resolve(args.what)
    try:
        r = o.call(address, gprs, fprs)
    except oracle.GuestFault as e:
        print(f"fault: {e}")
        return 1
    text = ""
    if 0x40000000 <= r.u32 < 0x41000000 or img.contains(r.u32):
        try:
            text = f'  -> "{o.read_cstr(r.u32, 200).decode("latin-1")}"'
        except Exception:
            text = ""
    f32 = struct.unpack(">f", struct.pack(">f", r.f1))[0]
    print(f"r3 = 0x{r.u32:08X} ({r.i32}){text}\nf1 = {r.f1!r} (as f32: {f32!r})")
    if r.entered:
        print("called: " + ", ".join(sorted(img.describe(a) for a in r.entered)))
    return 0


def cmd_check(args) -> int:
    import harness
    import oracle
    specs = load_specs()
    chosen = [s for s in specs.values()
              if (not args.names or s.name in args.names) and (not args.area or s.rust.startswith(args.area))]
    if not chosen:
        print("No specs match.")
        return 1
    if args.image:
        # The same ports against another program's copy of each function (by
        # name in its symbol map). Specs that write globals at the spec's own
        # program's addresses can't be moved over and are left out.
        target = image.load(args.image)
        moved = []
        for s in chosen:
            fixed = s.setup is not None or any(isinstance(a, harness.GlobalU32) for a in s.args)
            if s.name not in target.addresses:
                print(f"--   {s.name:<28} not named in {args.image}")
            elif fixed:
                print(f"--   {s.name:<28} reads globals at {s.image}'s addresses; not moved")
            else:
                moved.append(dataclasses.replace(s, image=args.image))
        chosen = moved
    images = {}
    rust = harness.RustChecker(build=not args.no_build)
    rng_seed = args.seed if args.seed is not None else int(time.time())
    total_bad = 0
    try:
        for s in chosen:
            if s.image not in images:
                o = oracle.for_image(s.image)
                images[s.image] = (o.images[-1], o)
            img, o = images[s.image]
            out = harness.check(s, o, rust, img.resolve(s.name), random.Random(f"{rng_seed}:{s.name}"),
                                args.cases)
            ok = out.passed == out.cases and out.cases > 0
            total_bad += 0 if ok else 1
            status = "ok  " if ok else "FAIL"
            print(f"{status} {s.name:<28} {out.passed}/{out.cases}" +
                  (f"  ({out.guest_faults} cases the original couldn't run)" if out.guest_faults else ""))
            for f in out.failures:
                print("     " + f)
    finally:
        rust.close()
    print(f"seed {rng_seed}: {len(chosen) - total_bad} of {len(chosen)} functions match the original")
    return 1 if total_bad else 0


def is_float_spec(s) -> bool:
    """Whether a function takes or returns floats: the results that could come out
    differently on another CPU, operating system or C library."""
    import harness
    float_args = (harness.F32, harness.F64, harness.Floats, harness.FloatBuffer, harness.OutFloats)
    if s.golden is not None:
        return s.golden
    return isinstance(s.ret, (harness.RetF32, harness.RetF64)) or any(isinstance(a, float_args) for a in s.args)


def cmd_golden(args) -> int:
    """Writes engine/tools/decomp_check/golden/<area>.txt: one checker request per
    line with the original's reply, as the oracle gives it. `cargo test -p
    decomp_check` replays them through the Rust ports on whatever machine it runs
    on, with no game files and no emulator: Windows, Linux, macOS, ARM.

    Line format: `<request>\t<reply>\t<rules>`; the reply is `ok <tokens>` or
    `err <exit>`, the rules `ulps=N floats=I,J` (which reply tokens are buffers of
    floats; any NaN matches any NaN, as in the checker)."""
    import harness
    import oracle
    specs = [s for s in load_specs().values()
             if (args.all or is_float_spec(s)) and (not args.names or s.name in args.names)]
    oracles = {}
    files: dict[str, list[str]] = {}
    for s in specs:
        if s.image not in oracles:
            oracles[s.image] = oracle.for_image(s.image)
        o = oracles[s.image]
        address = o.images[-1].resolve(s.name)
        rng = random.Random(f"{GOLDEN_SEED}:{s.name}")
        owners = [None] + [arg for arg in s.args if arg.mutable]
        float_tokens = ",".join(str(i) for i, arg in enumerate(owners) if arg is not None and arg.floats)
        rules = f"ulps={s.float_ulps}" + (f" floats={float_tokens}" if float_tokens else "")
        lines = files.setdefault(s.rust.split("::")[0], [])
        lines.append(f"# {s.name} ({s.rust})")
        written = 0
        wanted = min(args.cases, s.golden_cases or args.cases)
        for values in s.cases(rng):
            if written >= wanted:
                break
            try:
                reply = "ok " + " ".join(harness.guest_answer(o, s, address, values))
            except oracle.GuestError as e:
                reply = f"err {e.exit_name}"
            except oracle.GuestFault:
                continue
            request = " ".join([s.name] + [arg.token(v) for arg, v in zip(s.args, values)])
            lines.append(f"{request}\t{reply}\t{rules}")
            written += 1
        print(f"{s.name:<28} {written} cases")
    GOLDEN.mkdir(parents=True, exist_ok=True)
    header = ("# The original game's answers, recorded by `python3 tools/decomp/decomp.py golden`\n"
              "# (seed {seed}) and replayed by `cargo test -p decomp_check`. Results of the\n"
              "# game's functions on random inputs; no game files. Format: tools/decomp/decomp.py.\n")
    for area, lines in sorted(files.items()):
        path = GOLDEN / f"{area}.txt"
        path.write_text(header.format(seed=GOLDEN_SEED) + "\n".join(lines) + "\n", encoding="utf-8")
        print(f"wrote {path.relative_to(image.ROOT)} ({path.stat().st_size // 1024} KB)")
    return 0


def cmd_status(_args) -> int:
    specs = load_specs()
    ported = ported_in_rust()
    img = image.load("default")
    named = sum(1 for a in img.names if img.function_at(a) and img.function_at(a)[0] == a)
    print(f"default.xex: {len(img.functions):,} functions with unwind data, {named:,} named in the symbol map, "
          f"{len(ported)} ported to Rust ({len([n for n in ported if n in specs])} with a check).\n")
    areas: dict[str, list[str]] = {}
    for name, (path, _addr) in sorted(ported.items()):
        areas.setdefault(path, []).append(name + ("" if name in specs else " (no check yet)"))
    for path, names in sorted(areas.items()):
        print(f"{path}: {len(names)}")
        print("   " + ", ".join(names))
    missing = sorted(set(specs) - set(ported))
    if missing:
        print("\nSpecs without a Rust port (or without its `/// Original:` line): " + ", ".join(missing))
    return 0


def cmd_asm(_args) -> int:
    import disasm
    img = image.load("default")
    out_dir = image.cache_dir()
    with open(out_dir / "default.asm", "w", encoding="utf-8") as f:
        for lo, hi in img.code_ranges():
            for line in disasm.disassemble(img, lo, hi):
                if line.startswith("  ") and not line.startswith("            "):
                    parts = line.split(None, 2)
                    f.write(f"{parts[0].lower()}:\t{parts[2] if len(parts) > 2 else ''}\n")
    with open(out_dir / "default.pe.names", "w", encoding="utf-8") as f:
        for address, name in sorted(img.names.items()):
            f.write(f"{address:08X} {name}\n")
    print(f"Wrote {out_dir / 'default.asm'} and default.pe.names (for PPCX_IMG={out_dir / 'default.pe'})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("setup")
    p = sub.add_parser("dis")
    p.add_argument("what")
    p.add_argument("--count", type=int, default=0, help="instructions (default: the whole function)")
    p.add_argument("--image", default="default")
    for name in ("calls", "callers"):
        p = sub.add_parser(name)
        p.add_argument("what")
        p.add_argument("--image", default="default")
    p = sub.add_parser("call")
    p.add_argument("what")
    p.add_argument("args", nargs="*")
    p.add_argument("--image", default="default")
    p = sub.add_parser("check")
    p.add_argument("names", nargs="*")
    p.add_argument("--cases", type=int, default=2000)
    p.add_argument("--seed", type=int)
    p.add_argument("--area", help="only specs whose Rust path starts with this, e.g. shared::info")
    p.add_argument("--no-build", action="store_true", help="don't rebuild decomp_check first")
    p.add_argument("--image", help="check the same functions in another program (by name), e.g. codmp")
    sub.add_parser("status")
    p = sub.add_parser("golden")
    p.add_argument("names", nargs="*")
    p.add_argument("--cases", type=int, default=100, help="cases per function (default 100)")
    p.add_argument("--all", action="store_true", help="also the functions without floats")
    sub.add_parser("asm")
    args = parser.parse_args()
    return {"setup": cmd_setup, "dis": cmd_dis, "call": cmd_call, "check": cmd_check, "calls": cmd_calls, "callers": cmd_callers, "status": cmd_status,
            "golden": cmd_golden, "asm": cmd_asm}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
