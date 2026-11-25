#!/usr/bin/env python3
"""[acct2-fh] 2026-11-14 Finest Hour port: the console's language, chosen by the port, in xboxrecomp's kernel.

    python3 xboxrecomp_console_language.py XBOXRECOMP_DIR [--check]

Applies this project's change to the xboxrecomp checkout Finest Hour builds with (FH_DIR/xboxrecomp): two files,
src/kernel/kernel_xbox.c and src/kernel/kernel.h. Each edit replaces one exact piece of text, found exactly once, so a
checkout that differs from the one this was written against (xboxrecomp v0.16.0, f0f8a46, with
fh/patches/xboxrecomp_guest_priority.py's change, which touches other parts of kernel.h) stops with a message instead
of being changed in the wrong place. A checkout that already has the change is left alone. --check says whether the
change is there and changes nothing. fh_gen3.sh runs this before each build, after the other two.

Why (dev/agents/acct2-fh/NOTES.md, 11-14 14:13, read from the Europe v3 default.xbe):
  - the console keeps its language in the EEPROM, and XAPI's XGetLanguage (0x001320BE in Finest Hour) asks the kernel
    for it: ExQueryNonVolatileSetting (ordinal 24) with XC_LANGUAGE (7). It answers 1 (English) for anything above 9;
  - Finest Hour asks once, at start (0x000A4F70, its only caller), and sets its own language from the answer through
    0x00129310: English 0, French 2, Italian 5, Spanish 6; Japanese, German and the others English. The films follow
    it too: the game opens SHELL\\MOVIES\\<LANGUAGE>\\name.bik first, then SHELL\\MOVIES\\name.bik;
  - xboxrecomp's kernel always answered 1, so the port could only ever be in English.

The change: the answer is a value the port sets before the game starts, xbox_kernel_set_language(1..9), English (1)
until it is set, so a game that never sets it is as before. The first time the game asks, the kernel says which
language it answered on stderr (the ask itself is a debug line, off in the port's logs).
"""
import os
import sys

MARK = "[acct2-fh] console language"

XBOX = [
    # 0. fprintf, for the line said once.
    (r"""#include <time.h>
""",
     r"""#include <time.h>
#include <stdio.h>      /* """ + MARK + r""": the line said once */
"""),
    # 1. The answer and its setter, before ExQueryNonVolatileSetting.
    (r"""NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{""",
     r"""/* """ + MARK + r""" (2026-11-14): XC_LANGUAGE's answer. 1 English,
 * 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Chinese,
 * 9 Portuguese, as the console's settings keep it. English until a port sets
 * it, before the game starts (a game reads it at its start). */
static ULONG s_console_language = 1;
static int s_console_language_said;

void xbox_kernel_set_language(ULONG language)
{
    if (language >= 1 && language <= 9)
        s_console_language = language;
}

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{"""),
    # 2. XC_LANGUAGE answers it, and says so once.
    (r"""    case XC_LANGUAGE:
        /* English = 1 */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;""",
     r"""    case XC_LANGUAGE:
        /* """ + MARK + r""": the port's choice (xbox_kernel_set_language),
         * English = 1 until it is set */
        if (!s_console_language_said) {
            s_console_language_said = 1;
            fprintf(stderr, "  [KERNEL] the game asked for the console's language: %lu\n",
                    (unsigned long)s_console_language);
        }
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = s_console_language;"""),
]

HEADER = [
    (r"""/* Override the reported kernel version (ordinal 324). */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe);""",
     r"""/* Override the reported kernel version (ordinal 324). */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe);

/* """ + MARK + r""" (2026-11-14): what ExQueryNonVolatileSetting answers for
 * XC_LANGUAGE (1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian,
 * 7 Korean, 8 Chinese, 9 Portuguese; others are ignored). Set it before the
 * game starts; English (1) until then. */
void xbox_kernel_set_language(ULONG language);"""),
]


def edit_pairs(path, pairs):
    text = open(path, encoding="utf-8", newline="").read()
    if MARK in text:
        return "already has the change"
    for old, new in pairs:
        n = text.count(old)
        if n != 1:
            sys.exit("%s: the text to change was found %d times (expected once):\n%s" % (path, n, old[:300]))
        text = text.replace(old, new)
    return text


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    check = "--check" in sys.argv[2:]
    jobs = []
    for rel, pairs in (("src/kernel/kernel_xbox.c", XBOX), ("src/kernel/kernel.h", HEADER)):
        path = os.path.join(root, *rel.split("/"))
        if not os.path.isfile(path):
            sys.exit("no %s" % path)
        jobs.append((rel, path, edit_pairs(path, pairs)))   # every file checked before any is written
    for rel, path, result in jobs:
        if result == "already has the change":
            print("%s: already has the change" % rel)
        elif check:
            print("%s: the change is not there yet (it applies cleanly)" % rel)
        else:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(result)
            print("%s: changed" % rel)


if __name__ == "__main__":
    main()
