#!/usr/bin/env python3
"""Builds the Unicorn CPU emulator that the decomp oracle needs, into work/tools/unicorn/.

    python3 tools/decomp/unicorn_build.py [--yes]

Why a build of our own: four bugs in Unicorn 2.1.4 (QEMU 5) make it run the
game's code differently from an Xbox 360. Both are fixed here, on the pinned
release; nothing else is changed:

1. It can't select a 64-bit PowerPC CPU model (in
   qemu/target/ppc/translate_init.inc.c the PPC64 model number isn't offset
   past the PPC32 table), so it runs a 32-bit model and every floating point
   instruction of the game fails.
2. `fctiw`/`fctiwz`/`fctid`... give 0x80000000 for every value out of range
   (qemu/target/ppc/fpu_helper.c). The PowerPC architecture says only NaN
   does; other values saturate (+3e9 -> 0x7FFFFFFF). The game's angle code
   (AngleNormalize180 ...) depends on it. Later QEMU versions fixed it too.
3. `or rX,rX,rX` with any register is a "pause" that halts the emulated CPU
   (qemu/target/ppc/translate.c, gen_pause). The Xbox 360 compiler marks
   `__try` blocks with `mr r8,r8` and similar, which the console runs as
   no-ops; here they stay no-ops.
4. The single-precision fused multiply-adds (`fmadds`, `fmsubs`, `fnmadds`,
   `fnmsubs`) are computed as a double FMA and then rounded again to single
   (qemu/target/ppc/translate/fp-impl.inc.c), which can be off by one in the
   last bit. The architecture rounds once, to single. Here the double FMA
   runs in round-to-odd mode before the rounding to single, which gives the
   once-rounded result exactly (53 bits >= 24 + 2).

What it does (it asks first, unless --yes):
1. downloads Unicorn 2.1.4 (github.com/unicorn-engine/unicorn, GPL-2.0, commit
   pinned below) with git into work/tools/unicorn-src/;
2. applies the four fixes and builds only its PowerPC part with CMake;
3. copies the library and Unicorn's own Python binding (plain Python) into
   work/tools/unicorn/, where tools/decomp/oracle.py finds them.

Needs git, CMake and a C compiler (the same ones the setup tool checks for).
Nothing is installed anywhere else; delete work/tools/unicorn* to remove it.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "work" / "tools"
SOURCE = TOOLS / "unicorn-src"
TARGET = TOOLS / "unicorn"
REPO = "https://github.com/unicorn-engine/unicorn"
TAG = "2.1.4"
COMMIT = "8028ec436f2d9376525352dd38ed9ed6b9f6be10"
# (file, code as released, code as fixed). Bump FIXES_VERSION when this list changes.
FIXES_VERSION = 4
FIXES = [
    ("qemu/target/ppc/translate_init.inc.c",
     """    } else if (uc->cpu_model + UC_CPU_PPC32_7457A_V1_2 + 1 >= ARRAY_SIZE(ppc_cpus)) {
        free(cpu);
        return NULL;
    }
#else""",
     """    } else if (uc->cpu_model + UC_CPU_PPC32_7457A_V1_2 + 1 >= ARRAY_SIZE(ppc_cpus)) {
        free(cpu);
        return NULL;
    } else {
        /* cod3 decomp oracle: PPC64 model numbers start after the PPC32 ones */
        uc->cpu_model += UC_CPU_PPC32_7457A_V1_2 + 1;
    }
#else"""),
    ("qemu/target/ppc/fpu_helper.c",
     """        if (status & float_flag_invalid) {                             \\
            float_invalid_cvt(env, 1, GETPC(), float64_classify(arg)); \\
            ret = nanval;                                              \\
        }                                                              \\""",
     """        if (status & float_flag_invalid) {                             \\
            float_invalid_cvt(env, 1, GETPC(), float64_classify(arg)); \\
            /* cod3 decomp oracle: only NaN gives nanval; the rest */  \\
            /* saturates, as softfloat returned it (Power ISA)      */  \\
            if (float64_is_any_nan(arg)) {                             \\
                ret = nanval;                                          \\
            }                                                          \\
        }                                                              \\"""),
    ("qemu/target/ppc/translate.c",
     """         * mdoio(29), mdoom(30), and all currently undefined.
         */
        gen_pause(ctx);""",
     """         * mdoio(29), mdoom(30), and all currently undefined.
         * cod3 decomp oracle: not paused; the Xbox 360 compiler uses
         * mr r8,r8 and friends as __try markers, run as no-ops.
         */"""),
    ("qemu/target/ppc/translate/fp-impl.inc.c",
     """_GEN_FLOAT_ACB(name##s, name, 0x3B, op2, 1, set_fprf, type);""",
     """_GEN_FLOAT_ACB(name##s, name##_odd, 0x3B, op2, 1, set_fprf, type);"""),
    ("qemu/target/ppc/helper.h",
     """DEF_HELPER_4(fnmsub, i64, env, i64, i64, i64)
""",
     """DEF_HELPER_4(fnmsub, i64, env, i64, i64, i64)
DEF_HELPER_4(fmadd_odd, i64, env, i64, i64, i64)
DEF_HELPER_4(fmsub_odd, i64, env, i64, i64, i64)
DEF_HELPER_4(fnmadd_odd, i64, env, i64, i64, i64)
DEF_HELPER_4(fnmsub_odd, i64, env, i64, i64, i64)
"""),
    ("qemu/target/ppc/fpu_helper.c",
     """FPU_FMADD(fnmsub, NMSUB_FLGS)
""",
     """FPU_FMADD(fnmsub, NMSUB_FLGS)

/*
 * cod3 decomp oracle: the single forms (fmadds ...) round once, to single.
 * The double result in round-to-odd, then frsp, gives exactly that.
 */
#define FPU_FMADD_ODD(op, madd_flags)                                    \\
uint64_t helper_##op##_odd(CPUPPCState *env, uint64_t arg1,              \\
                           uint64_t arg2, uint64_t arg3)                 \\
{                                                                        \\
    int mode = get_float_rounding_mode(&env->fp_status);                 \\
    uint32_t flags;                                                      \\
    float64 ret;                                                         \\
    set_float_rounding_mode(float_round_to_odd, &env->fp_status);        \\
    ret = float64_muladd(arg1, arg2, arg3, madd_flags, &env->fp_status); \\
    set_float_rounding_mode(mode, &env->fp_status);                      \\
    flags = get_float_exception_flags(&env->fp_status);                  \\
    if (flags) {                                                         \\
        if (flags & float_flag_invalid) {                                \\
            float64_maddsub_update_excp(env, arg1, arg2, arg3,           \\
                                        madd_flags, GETPC());            \\
        }                                                                \\
        do_float_check_status(env, GETPC());                             \\
    }                                                                    \\
    return ret;                                                          \\
}

FPU_FMADD_ODD(fmadd, MADD_FLGS)
FPU_FMADD_ODD(fnmadd, NMADD_FLGS)
FPU_FMADD_ODD(fmsub, MSUB_FLGS)
FPU_FMADD_ODD(fnmsub, NMSUB_FLGS)
"""),
] + [
    # Unicorn builds the 32- and 64-bit targets from the same files and renames every helper
    # per target in these headers; the new helpers need the same.
    (f"qemu/{target}.h",
     f"""#define helper_fnmsub helper_fnmsub_{target}
""",
     f"""#define helper_fnmsub helper_fnmsub_{target}
#define helper_fmadd_odd helper_fmadd_odd_{target}
#define helper_fmsub_odd helper_fmsub_odd_{target}
#define helper_fnmadd_odd helper_fnmadd_odd_{target}
#define helper_fnmsub_odd helper_fnmsub_odd_{target}
""") for target in ("ppc", "ppc64")
]
LIB_NAMES = ["libunicorn.so.2", "libunicorn.so", "unicorn.dll", "libunicorn.dll", "libunicorn.2.dylib",
             "libunicorn.dylib"]


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print("  $ " + " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def approve(assume_yes: bool) -> bool:
    print("The decomp oracle needs a fixed build of the Unicorn CPU emulator.\n"
          f"  Download: {REPO} at {TAG} (commit {COMMIT[:12]}), GPL-2.0, about 60 MB with git\n"
          f"  Source goes to:  {SOURCE}\n"
          f"  Build result to: {TARGET}\n"
          "  Uses: git, CMake, a C compiler. Nothing goes outside the project folder.")
    if assume_yes:
        return True
    try:
        return input("Download and build it? [y/N] ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--yes", action="store_true", help="don't ask before downloading")
    args = parser.parse_args()
    for tool in ("git", "cmake"):
        if not shutil.which(tool):
            print(f"{tool} is missing. The setup tool's dependency check installs it (or see README).")
            return 1
    stamp = TARGET / "build.json"
    if stamp.is_file() and json.loads(stamp.read_text()).get("fixes") == FIXES_VERSION:
        print(f"Already built: {TARGET}")
        return 0
    if not approve(args.yes):
        print("Nothing downloaded.")
        return 1
    TOOLS.mkdir(parents=True, exist_ok=True)
    if SOURCE.exists():
        shutil.rmtree(SOURCE)
    run(["git", "clone", "--quiet", "--depth", "1", "--branch", TAG, REPO, str(SOURCE)])
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=SOURCE, capture_output=True, text=True,
                          check=True).stdout.strip()
    if head != COMMIT:
        print(f"The download is commit {head}, not the pinned {COMMIT}. Stopping.")
        return 1
    for file_name, released, fixed in FIXES:
        fix_path = SOURCE / file_name
        text = fix_path.read_text(encoding="utf-8")
        if text.count(released) != 1:
            print(f"The code a fix changes isn't in {file_name} as expected. Stopping.")
            return 1
        fix_path.write_text(text.replace(released, fixed), encoding="utf-8")
    build = SOURCE / "build"
    configure = ["cmake", "-S", str(SOURCE), "-B", str(build), "-DUNICORN_ARCH=ppc",
                 "-DCMAKE_BUILD_TYPE=Release", "-DUNICORN_BUILD_TESTS=OFF"]
    if shutil.which("ninja"):
        configure += ["-G", "Ninja"]
    run(configure)
    run(["cmake", "--build", str(build), "--config", "Release", "--parallel", str(os.cpu_count() or 2)])
    if TARGET.exists():
        shutil.rmtree(TARGET)
    TARGET.mkdir(parents=True)
    copied = []
    for found in list(build.rglob("*unicorn*")):
        if found.is_file() and found.name in LIB_NAMES:
            shutil.copy2(found, TARGET / found.name)
            copied.append(found.name)
    if not copied:
        print(f"The build finished but no Unicorn library was found under {build}.")
        return 1
    if "libunicorn.dll" in copied and "unicorn.dll" not in copied:
        # A MinGW build names it libunicorn.dll; Unicorn's Python binding looks for unicorn.dll.
        shutil.copy2(TARGET / "libunicorn.dll", TARGET / "unicorn.dll")
        copied.append("unicorn.dll")
    shutil.copytree(SOURCE / "bindings" / "python" / "unicorn", TARGET / "python" / "unicorn",
                    ignore=shutil.ignore_patterns("__pycache__"))
    stamp.write_text(json.dumps({"repo": REPO, "tag": TAG, "commit": COMMIT, "fixes": FIXES_VERSION,
                                 "fixed_files": [f for f, _, _ in FIXES], "libraries": copied}, indent=1))
    print(f"Done: {', '.join(copied)} and the Python binding in {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
