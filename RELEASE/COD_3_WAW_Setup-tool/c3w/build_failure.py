"""[acct2-bro] 2026-11-22 What a failed compile or link of the game says to the person building it (owner 23:48: "The
setup process really needs to be robust and not confusing or easy to mess up").

The user reports of 2026-11-23: the game's link stopped on an error in the project's own code, and the person built
again, then again with one thread, each time 20 minutes, because the message said only "Command failed ... See the log
above". A compile or link error in the game's or the SDK's code is the setup tool's to fix, not the PC's: rebuilding,
fewer threads or reinstalling the tools gives the same error. ours_hint() says so, and what to do instead (the bug
report the Log page makes). Errors that are the PC's (a full disk, too little memory, a file another program holds or
an antivirus took) say that instead, so nobody is told to wait for an update that won't help.
"""
from __future__ import annotations

import re
from typing import Iterable

# Output lines of a compile or link that failed on the code itself (clang, lld-link, ld.lld, ninja, CMake).
_OURS = (
    re.compile(r"^FAILED: .*\.(?:obj|o|exe|dll|so|lib|a)\b"),
    re.compile(r"\berror: (?:undefined (?:symbol|reference)|use of undeclared|unknown type name|no member named|"
               r"no matching|cannot initialize|expected |redefinition|duplicate symbol|call to )"),
    re.compile(r"\b(?:lld-link|ld\.lld|ld): error: "),
    re.compile(r"\.(?:c|cc|cpp|h|hpp|inc):\d+:\d+: (?:fatal )?error: "),
    re.compile(r"^CMake Error at .*CMakeLists\.txt:\d+"),
)
# The PC's side: these win over the patterns above (a compiler killed for memory also prints "FAILED:").
_PCS = (
    ("no space left on device", "the disk is full: free some space on the drive the work folder is on"),
    ("not enough space on the disk", "the disk is full: free some space on the drive the work folder is on"),
    ("out of memory", "the PC ran out of memory: close other programs, or build with fewer threads"),
    ("cannot allocate memory", "the PC ran out of memory: close other programs, or build with fewer threads"),
    ("std::bad_alloc", "the PC ran out of memory: close other programs, or build with fewer threads"),
    ("being used by another process", "another program holds a file of the build: close it (an editor, a file "
     "viewer, another copy of the setup tool) and build again"),
    ("access is denied", "Windows refused a file of the build: an antivirus may have taken it (restore it from the "
     "quarantine, or add the work folder to its exclusions) and build again"),
    ("permission denied", "the build may not write a file it needs: check the work folder's permissions and build "
     "again"),
)
_SIGNALS = re.compile(r"(?:killed|terminated) by signal|\bsignal 9\b|exit code 137\b|clang.*: error: unable to "
                      r"execute command")
# [acct2-bro] 2026-11-23 Lines that only name symbols or files (a linker's undefined-symbol listing, a compiler's
# "In file included from"): words in them are names, not what happened. The 2026-11-23 test's link error listed
# "typeinfo for std::bad_alloc" and was called the PC running out of memory.
_NAMES = re.compile(r"^\s*>>> |\bundefined (?:symbol|reference)\b|\breferenced by\b|\b(?:typeinfo|vtable) for |"
                    r"^\s*In file included from |^\s*note: ")


def ours_hint(lines: Iterable[str], version: str = "") -> str:
    """A hint for a failed compile or link ("" when the output doesn't look like one)."""
    lines = list(lines)
    low = "\n".join(line for line in lines if not _NAMES.search(line)).lower()   # [acct2-bro] 2026-11-23
    for needle, what in _PCS:
        if needle in low:
            return f"What to do: {what}."
    if _SIGNALS.search(low):
        return ("What to do: a compiler was stopped from outside (most often the PC ran out of memory): close other "
                "programs, or build with fewer threads, and build again.")
    if not any(p.search(line) for line in lines for p in _OURS):
        return ""
    which = f" {version}" if version else ""
    return (f"What this is: an error in the code this version of the setup tool{which} builds, not a problem with "
            "your PC. Building again, with fewer threads, or reinstalling the tools will stop at the same place. "
            "What to do: on the Log page, press \"Make bug report...\" and send the file it makes; the fix comes in "
            "an update. If you changed files in the project folder yourself, put them back first.")


__all__ = ["ours_hint"]
