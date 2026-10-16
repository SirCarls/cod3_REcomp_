#!/usr/bin/env python3
"""
install_xenon.py - add the Xbox 360 (Xenon) PowerPC language to a Ghidra install:
"PowerPC:BE:64:Xenon-32addr" = Power ISA 64-bit, AltiVec, VMX128, 32-bit addresses.

    python3 tools/waw_decomp/ghidra_xenon/install_xenon.py <ghidra folder>

Writes vmx128.sinc, ppc_isa_xenon.sinc and ppc_64_isa_altivec_xenon_be.slaspec into
Ghidra/Processors/PowerPC/data/languages, adds the language to ppc.ldefs (once) and
compiles it with Ghidra's own sleigh compiler. Re-running is safe.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANG_ID = "PowerPC:BE:64:Xenon-32addr"
ENTRY = """  <language processor="PowerPC"
            endian="big"
            size="32"
            variant="Xenon-VMX128-64-32addr"
            version="1.6"
            slafile="ppc_64_isa_altivec_xenon_be.sla"
            processorspec="ppc_64.pspec"
            manualindexfile="../manuals/PowerISA.idx"
            id="PowerPC:BE:64:Xenon-32addr">
    <description>Xbox 360 Xenon: Power ISA Big Endian w/Altivec, VMX128 and 32-bit Addressing</description>
    <truncate_space space="ram" size="4"/>
    <compiler name="default" spec="ppc_64_32.cspec" id="default"/>
    <external_name tool="DWARF.register.mapping.file" name="ppc.dwarf"/>
  </language>
"""


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    ghidra = os.path.abspath(sys.argv[1])
    langs = os.path.join(ghidra, "Ghidra", "Processors", "PowerPC", "data", "languages")
    if not os.path.isfile(os.path.join(langs, "ppc_isa.sinc")):
        sys.exit(f"error: not a Ghidra install (no PowerPC languages in {langs})")
    subprocess.run([sys.executable, os.path.join(HERE, "gen_vmx128.py"), langs], check=True)
    ldefs = os.path.join(langs, "ppc.ldefs")
    with open(ldefs, encoding="utf-8") as f:
        text = f.read()
    if LANG_ID not in text:
        text = re.sub(r"</language_definitions>", ENTRY + "</language_definitions>", text)
        with open(ldefs, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"added {LANG_ID} to ppc.ldefs")
    sleigh = os.path.join(ghidra, "support", "sleigh.bat" if os.name == "nt" else "sleigh")
    r = subprocess.run([sleigh, "ppc_64_isa_altivec_xenon_be.slaspec", "ppc_64_isa_altivec_xenon_be.sla"],
                       cwd=langs, capture_output=True, text=True)
    errors = [l for l in (r.stdout + r.stderr).splitlines() if "ERROR" in l or "Error" in l]
    if r.returncode != 0 or errors or not os.path.isfile(os.path.join(langs, "ppc_64_isa_altivec_xenon_be.sla")):
        sys.exit("sleigh failed:\n" + "\n".join(errors or [r.stdout[-2000:], r.stderr[-2000:]]))
    print(f"compiled ppc_64_isa_altivec_xenon_be.sla; language {LANG_ID} is ready")


if __name__ == "__main__":
    main()
