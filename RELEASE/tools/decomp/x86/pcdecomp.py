#!/usr/bin/env python3
"""The x86 decomp's command line: the PC games' executables (Call of Duty 2,
United Offensive, Call of Duty), checked against the Rust crate `iw_pc`.
See tools/decomp/x86/README.md and docs/DECOMP.md (the method is the same).

    pcdecomp.py setup                          find the images, check the tools
    pcdecomp.py dis NAME|ADDR --image I        readable disassembly of one function
    pcdecomp.py call NAME|ADDR ARG... --image I
                                               run one original function. ARG: [where=]kind:value,
                                               where = stack (default), eax, ecx, edx, ebx, esi, edi;
                                               kind: i:5, f:1.5, s:text, x:64 (64 zero bytes)
    pcdecomp.py check [NAME...] --image I|all  compare Rust ports with the originals
                                               (--cases N, --seed S, --against cod3)
    pcdecomp.py find TEXT --image I            strings containing TEXT and the functions using them
    pcdecomp.py status                         what is ported and checked

Images: cod2mp (CoD2MP_s.exe 1.3), uomp (CoDUOMP.exe 1.51), codmp (CoDMP.exe
1.5), uo_game_mp (uo_game_mp_x86.dll 1.51), cod2_gfx_mp (gfx_d3d_mp_x86_s.dll,
for reading only). The files come from IW_PC_INPUT or dev/decomp-input/pc/.
"""
from __future__ import annotations

import argparse
import importlib
import random
import re
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True  # no __pycache__ next to the sources (tools/decomp is shared)
sys.path.insert(0, str(HERE))
sys.path.insert(1, str(HERE.parent))  # tools/decomp: gen.py (random strings) is shared

import pe  # noqa: E402

CRATE = pe.ROOT / "engine" / "crates" / "iw_pc" / "src"
ORIGINAL_LINE = re.compile(r"^\s*///.*?\bOriginal:\s*`([^`]+)`\s*at\s+(0x[0-9A-Fa-f]{8})\s*\(([^)]*)\)")


def load_specs() -> dict:
    import harness_x86
    for path in sorted((HERE / "specs").glob("*.py")):
        if path.name != "__init__.py":
            importlib.import_module(f"specs.{path.stem}")
    return harness_x86.SPECS


def ported_in_rust() -> dict[tuple[str, str], tuple[str, str]]:
    """(executable title, original name) -> (Rust file, address), from the `/// Original:` lines."""
    found = {}
    for path in sorted(CRATE.rglob("*.rs")):
        for line in path.read_text(encoding="utf-8").splitlines():
            m = ORIGINAL_LINE.match(line)
            if m:
                found[(m.group(3).strip(), m.group(1))] = (path.relative_to(CRATE).as_posix(), m.group(2))
    return found


def cmd_setup(_args) -> int:
    found = []
    for key, known in pe.KNOWN_IMAGES.items():
        try:
            img = pe.load(key)
        except SystemExit as e:
            print(f"  {key:<12} missing ({str(e).splitlines()[0]})")
            continue
        found.append(img)
        code = sum(hi - lo for lo, hi in img.code_ranges())
        print(f"  {key:<12} {known.file_name:<22} at 0x{img.base:08X}, {len(img.data) / 1e6:5.1f} MB mapped, "
              f"{code / 1e6:4.2f} MB code, {len(img.imports)} imports, {len(img.names)} names")
    import oracle_x86
    if found:
        o = oracle_x86.X86Oracle(found[0])
        # A tiny probe: fld1; fld1; faddp; fdiv of 3 at 24-bit precision (what Direct3D 9 sets).
        probe = oracle_x86.HEAP + 0x100
        o.write(probe, bytes.fromhex("D9E8D9E8DEC1D835") + struct.pack("<I", probe + 0x20) + b"\xC3")
        o.write(probe + 0x20, struct.pack("<f", 3.0))
        r = o.call(probe, fpu="pc24")
        got = oracle_x86.x87_to_f64_bits(r.st0)
        ok = got == struct.unpack("<Q", struct.pack("<d", struct.unpack("<f", struct.pack("<f", 2 / 3))[0]))[0]
        print(f"  oracle       {'OK' if ok else 'WRONG'} (Unicorn x86, x87 precision control honored: {ok})")
    try:
        import capstone  # noqa: F401
        print("  capstone     OK")
    except ImportError:
        print("  capstone     missing: pip install capstone (needed for `dis` and `find`)")
    return 0


def cmd_dis(args) -> int:
    import disasm_x86
    img = pe.load(args.image)
    start = img.resolve(args.what)
    end = None
    if args.count:
        end = start + args.count
    print(f"== {img.describe(start)} (0x{start:08X}) in {img.title}")
    print("\n".join(disasm_x86.disassemble(img, start, end)))
    return 0


def _parse_call_arg(o, text: str):
    where, _, rest = text.rpartition("=") if "=" in text.split(":")[0] else ("stack", "", text)
    kind, _, value = rest.partition(":")
    if kind == "i":
        return where, [int(value, 0) & 0xFFFFFFFF]
    if kind == "f":
        return where, [struct.unpack("<I", struct.pack("<f", float(value)))[0]]
    if kind == "s":
        return where, [o.put_cstr(value.encode("latin-1"))]
    if kind == "x":
        return where, [o.put(bytes(int(value, 0)))]
    raise SystemExit(f"unknown argument {text!r} (use i:, f:, s:, x:, optionally after eax= ...)")


def cmd_call(args) -> int:
    import oracle_x86
    img = pe.load(args.image)
    o = oracle_x86.X86Oracle(img, trace_blocks=True)
    registers, stack = {}, []
    for a in args.args:
        where, words = _parse_call_arg(o, a)
        if where == "stack":
            stack += words
        else:
            registers[where] = words[0]
    address = img.resolve(args.what)
    try:
        r = o.call(address, registers, stack, args.pops, args.fpu)
    except oracle_x86.GuestError as e:
        print(f"game error: {e.exit_name}({e.code}, {e.message!r})")
        return 0
    except oracle_x86.GuestFault as e:
        print(f"fault: {e}")
        return 1
    text = ""
    try:
        if oracle_x86.HEAP <= r.u32 < oracle_x86.HEAP + oracle_x86.HEAP_SIZE or img.contains(r.u32):
            text = f'  -> "{o.read_cstr(r.u32, 200).decode("latin-1")}"'
    except oracle_x86.GuestFault:
        pass
    print(f"eax = 0x{r.u32:08X} ({r.i32}){text}\nedx = 0x{r.edx & 0xFFFFFFFF:08X}")
    if r.st0 is not None:
        print(f"st0 = {oracle_x86.x87_value(r.st0)} (x87 stack depth {r.fpu_depth}; as f32 bits "
              f"0x{oracle_x86.x87_to_f32_bits(r.st0):08X})")
    if r.prints:
        print("printed: " + "; ".join(repr(p) for p in r.prints))
    if r.inexact:
        print("ran x87 instructions the oracle doesn't compute bit-exactly: " + ", ".join(sorted(r.inexact)))
    if r.clobbered:
        print("changed callee-saved registers: " + ", ".join(r.clobbered))
    if r.entered:
        print("called: " + ", ".join(sorted(img.describe(a) for a in r.entered)))
    print(f"{r.blocks} blocks")
    return 0


def cmd_check(args) -> int:
    import harness_x86
    import oracle_x86
    specs = load_specs()
    chosen = [s for s in specs.values()
              if (args.image == "all" or s.image == args.image) and (not args.names or s.name in args.names)
              and (not args.against or s.cod3)]
    if not chosen:
        print("No specs match.")
        return 1
    oracles = {}
    checker = harness_x86.CppChecker if args.impl == "cpp" else harness_x86.RustChecker
    rust = checker(build=not args.no_build)
    seed = args.seed if args.seed is not None else int(time.time())
    bad = 0
    try:
        for s in chosen:
            key = (s.image, s.prepare)
            if key not in oracles:
                img = pe.load(s.image)
                o = oracle_x86.X86Oracle(img)
                for name in s.prepare:
                    started = time.time()
                    o.run_and_keep(img.resolve(name), fpu=harness_x86.GAME_FPU[s.image])
                    print(f"     ({s.image}: ran {name} once, {time.time() - started:.1f} s)")
                oracles[key] = (img, o)
            img, o = oracles[key]
            started = time.time()
            out = harness_x86.check(s, o, rust, img.resolve(s.name), random.Random(f"{seed}:{s.name}"),
                                    args.cases, request_prefix=args.against)
            ok = out.passed == out.cases and out.cases > 0
            exact = not out.inexact
            bad += 0 if ok else 1
            status = ("ok  " if exact else "ok~ ") if ok else ("FAIL" if not args.against else "DIFF")
            label = s.request_name if not args.against else f"{args.against}.{s.cod3} vs {s.request_name}"
            extra = []
            if out.errors:
                extra.append(f"{out.errors} game errors")
            if out.guest_faults:
                extra.append(f"{out.guest_faults} cases the original couldn't run")
            if out.inexact:
                extra.append("not bit-exact in the oracle: ran " + ", ".join(sorted(out.inexact)))
            print(f"{status} {label:<34} {out.passed}/{out.cases}  ({time.time() - started:.1f} s"
                  + "".join(f"; {e}" for e in extra) + ")")
            for f in out.failures:
                print("     " + f)
    finally:
        rust.close()
    what = "behave like the original" if args.against else "match the original"
    print(f"seed {seed}: {len(chosen) - bad} of {len(chosen)} functions {what}")
    return 1 if bad and not args.against else 0


def _function_start(img, address: int) -> int:
    """A guess at the start of the function holding `address`: after the int3/nop padding before it."""
    named = [a for a in img.names if a <= address and img.is_code(a)]
    best = max(named) if named else 0
    if best and address - best < img.lengths.get(best, 0):
        return best
    a = address
    while a > address - 0x4000:
        if a % 16 == 0 and img.read(a - 1, 1) in (b"\xCC", b"\x90", b"\xC3"):
            # Padding (or a return) right before a 16-byte boundary: a function starts here.
            return a
        a -= 1
    return address


def cmd_find(args) -> int:
    import disasm_x86
    img = pe.load(args.image)
    needle = args.text.encode("latin-1")
    shown = 0
    for s in img.sections:
        if s.flags & pe.SCN_CODE:
            continue
        raw = img.read(s.start, s.size)
        i = raw.find(needle)
        while i >= 0 and shown < 40:
            start = raw.rfind(b"\0", 0, i) + 1
            address = s.start + start
            text = img.cstr(address, 300) or b""
            refs = disasm_x86.references_to(img, address, kinds=("push", "mov", "lea", "cmp"))
            where = ", ".join(f"{img.describe(_function_start(img, r))} (0x{_function_start(img, r):08X}) "
                              f"at 0x{r:08X}" for r in refs[:8])
            print(f"0x{address:08X} {text.decode('latin-1')!r}\n    used by: {where or '-'}")
            shown += 1
            i = raw.find(needle, i + 1)
    return 0


def cmd_refs(args) -> int:
    import disasm_x86
    img = pe.load(args.image)
    target = img.resolve(args.what)
    for r in disasm_x86.references_to(img, target):
        start = _function_start(img, r)
        print(f"0x{r:08X} in {img.describe(start)} (0x{start:08X})")
    return 0


def cmd_status(_args) -> int:
    specs = load_specs()
    ported = ported_in_rust()
    for key, known in pe.KNOWN_IMAGES.items():
        mine = sorted((name, where) for (title, name), where in ported.items() if title == known.title)
        if not mine:
            continue
        checked = [n for n, _ in mine if f"{key}.{n}" in specs]
        print(f"{known.title}: {len(mine)} functions ported to Rust, {len(checked)} with a check")
        by_file: dict[str, list[str]] = {}
        for name, (path, address) in mine:
            by_file.setdefault(path, []).append(f"{name}@{address}" + ("" if f"{key}.{name}" in specs
                                                                       else " (no check yet)"))
        for path, names in sorted(by_file.items()):
            print(f"  {path}: " + ", ".join(names))
    missing = sorted(n for n, s in specs.items()
                     if (pe.KNOWN_IMAGES[s.image].title, s.name) not in ported)
    if missing:
        print("\nSpecs without a Rust port (or without its `/// Original:` line): " + ", ".join(missing))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("setup")
    p = sub.add_parser("dis")
    p.add_argument("what")
    p.add_argument("--count", type=lambda t: int(t, 0), default=0, help="bytes (default: the whole function)")
    p.add_argument("--image", default="cod2mp")
    p = sub.add_parser("call")
    p.add_argument("what")
    p.add_argument("args", nargs="*")
    p.add_argument("--image", default="cod2mp")
    p.add_argument("--fpu", default="pc53", choices=["pc24", "pc53", "pc64"])
    p.add_argument("--pops", type=int, default=0, help="bytes of arguments the function's `ret N` removes")
    p = sub.add_parser("check")
    p.add_argument("names", nargs="*")
    p.add_argument("--image", default="all")
    p.add_argument("--cases", type=int, default=2000)
    p.add_argument("--seed", type=int)
    p.add_argument("--against", choices=["cod3"],
                   help="run another game's port (cod3_game) against these originals, to see what they share")
    p.add_argument("--no-build", action="store_true", help="don't rebuild pc_check first")
    p.add_argument("--impl", choices=["rust", "cpp"], default="rust",
                   help="which ports answer: the Rust crate (pc_check) or the C++ version (engine/cpp, pc_check_cpp)")
    p = sub.add_parser("find")
    p.add_argument("text")
    p.add_argument("--image", default="cod2mp")
    p = sub.add_parser("refs")
    p.add_argument("what")
    p.add_argument("--image", default="cod2mp")
    sub.add_parser("status")
    args = parser.parse_args()
    return {"setup": cmd_setup, "dis": cmd_dis, "call": cmd_call, "check": cmd_check, "find": cmd_find,
            "refs": cmd_refs, "status": cmd_status}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
