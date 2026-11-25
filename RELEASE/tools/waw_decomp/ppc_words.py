#!/usr/bin/env python3
"""
ppc_words.py - small helpers for reading Xbox 360 (PowerPC) code words, shared by the title-update tools
(tu_strings.py, tu_disasm.py).

    gpr_destinations(word)               the general registers an instruction writes
    jump_table_addresses(words, start, size)
                                         the words right after a `bctr` that are a switch's table of addresses
                                         inside the function, not instructions

Why: an address built with `lis rX, high` is only valid until rX is written again. The tools used to keep every
`lis` they saw until the end of the function, so a later `addi rY, rX, low` with a different rX could be read as a
string address it never was. And a switch's table after its `bctr` holds addresses (0x82......), which decode as
`lwz` instructions; those change with every relink and showed up as changes (Call of Duty 2 TU3's
`info_grenade_hint`, 2026-11-09, was one: a moved table, not a fix).

gpr_destinations covers the instruction forms the Xbox 360 compiler emits for integer code; anything it doesn't
know is taken to write nothing. Calls (`bl`, `bcl`, `bctrl`, `blrl`) count as writing the volatile registers r0 and
r3-r12. The instruction fields follow the PowerPC architecture books' instruction formats. Our own code.
"""

BCTR = 0x4E800420
VOLATILE_GPRS = frozenset((0,) + tuple(range(3, 13)))

# Primary opcodes whose RT field (bits 6-10) is a general register they write.
_WRITES_RT = {7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 58}
# Primary opcodes whose RA field (bits 11-15) is a general register they write: rotates, logical immediates,
# 64-bit rotates and shifts, and the update forms of loads and stores.
_WRITES_RA = {20, 21, 23, 24, 25, 26, 27, 28, 29, 30, 33, 35, 37, 39, 41, 43, 45}

# Opcode 31, by its 10-bit extended opcode (bits 21-30).
_X_LOADS = {20, 21, 23, 53, 55, 84, 87, 119, 279, 311, 341, 343, 373, 375, 532, 534, 790}  # write RT
_X_UPDATE_FORMS = {53, 55, 119, 311, 373, 375, 181, 183, 247, 439}  # also write RA
_X_MOVES_FROM = {19, 83, 339, 371}  # mfcr / mfocrf, mfmsr, mfspr, mftb: write RT
_X_LOGICAL = {24, 26, 27, 28, 58, 60, 124, 284, 316, 412, 444, 476, 536, 539, 792, 794, 824, 922, 954, 986}  # write RA
# Opcode 31 arithmetic (XO form), by its 9-bit extended opcode (bits 22-30): write RT.
_XO_ARITHMETIC = {8, 9, 10, 11, 40, 73, 75, 104, 136, 138, 200, 202, 232, 233, 234, 235, 266, 457, 459, 489, 491}
_XS_SRADI = 413  # sradi, 9-bit extended opcode in bits 21-29: writes RA


def gpr_destinations(word):
    """The set of general registers (0-31) the instruction writes."""
    op = word >> 26
    rt = (word >> 21) & 31
    ra = (word >> 16) & 31
    out = set()
    if op in (16, 18) and word & 1:  # bcl / bl: a call
        return set(VOLATILE_GPRS)
    if op == 19 and word & 1:  # bclrl / bcctrl: a call through LR or CTR
        return set(VOLATILE_GPRS)
    if op == 46:  # lmw: RT through r31
        return set(range(rt, 32))
    if op in _WRITES_RT:
        out.add(rt)
    if op in _WRITES_RA:
        out.add(ra)
    if op == 62 and word & 3 == 1:  # stdu
        out.add(ra)
    if op == 58 and word & 3 == 1:  # ldu
        out.add(ra)
    if op == 31:
        xo10 = (word >> 1) & 0x3FF
        xo9 = (word >> 1) & 0x1FF
        if xo10 in _X_LOADS or xo10 in _X_MOVES_FROM or xo9 in _XO_ARITHMETIC:
            out.add(rt)
        if xo10 in _X_UPDATE_FORMS or xo10 in _X_LOGICAL or (word >> 2) & 0x1FF == _XS_SRADI:
            out.add(ra)
    return out


def jump_table_addresses(words, start, size):
    """Addresses of the words that follow a `bctr` and point inside the function [start, start + size): a switch's
    table of case addresses. `words` is the function's instruction words in order, starting at `start`."""
    table = set()
    in_table = False
    for i, word in enumerate(words):
        address = start + 4 * i
        if in_table and start <= word < start + size and word % 4 == 0:
            table.add(address)
            continue
        in_table = word == BCTR
    return table
