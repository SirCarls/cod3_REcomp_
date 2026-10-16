"""Runs the PC games' own x86 functions, one call at a time: the reference
every `iw_pc` port is checked against.

It uses the Unicorn CPU emulator (QEMU's x86 code, GPL-2.0; a tool, not part
of the game or the engine), stock 2.1.4, in 32-bit protected mode. The image
is mapped at its preferred base (no relocation needed), every import slot
points at a stop address, and a call is set up like the game's compiler
(MSVC 7.1) would: arguments on the stack and/or in registers as the spec
says, a return address that ends the call, the x87 control word the spec
asks for.

Calls are independent: every page written during a call (the image's data,
the test heap, the stack) is put back before the next one.

What it can't do (the call stops with a message saying where):
- imports (Windows, DirectX ...) other than the few stand-ins below;
- anything that needs the rest of the game to have started (globals are as
  in the file: zero for most, unless a spec writes them);
- bit-exact x87 transcendental instructions: QEMU 5 computes `fsin`, `fcos`,
  `fsincos`, `fptan`, `fpatan`, `f2xm1`, `fyl2x`, `fyl2xp1`, `fprem` and
  `fprem1` through the host's double-precision C library, not the way an x87
  does. A call that runs one is marked (`CallResult.inexact`), and the
  checker never reports it as a bit-exact match.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from fractions import Fraction

try:
    from unicorn import (UC_ARCH_X86, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_INVALID,
                         UC_HOOK_MEM_WRITE, UC_MODE_32, Uc, UcError)
    from unicorn import x86_const as R
except ImportError:  # pragma: no cover - depends on the machine
    raise SystemExit("The x86 oracle needs Unicorn 2.1.4: pip install unicorn==2.1.4") from None

from pe import PEImage

PAGE = 0x1000
# Guest memory around the image(s): places none of the games' images use
# (executables at 0x400000, game DLLs at 0x20000000, the renderer at 0x10000000).
HEAP = 0x40000000          # test data (strings, structures); reset for every call
HEAP_SIZE = 0x01000000
STACK = 0x70000000
STACK_SIZE = 0x00100000
STACK_TOP = STACK + STACK_SIZE - 0x1000
IMPORT_STOPS = 0x7E000000  # every import slot points at its own address here
RETURN_ADDRESS = 0x7F000000  # the call ends when the function returns here
TEB = 0x7FFD0000           # a zeroed thread environment block (fs:)
GDT = 0x7FFE0000           # one data segment for fs
THREAD_DATA = 0x7FFC0000   # stand-in per-thread data the game reaches through TlsGetValue
THREAD_DATA_SIZE = 0x10000

REGISTERS = {"eax": R.UC_X86_REG_EAX, "ecx": R.UC_X86_REG_ECX, "edx": R.UC_X86_REG_EDX,
             "ebx": R.UC_X86_REG_EBX, "esi": R.UC_X86_REG_ESI, "edi": R.UC_X86_REG_EDI,
             "ebp": R.UC_X86_REG_EBP}
CALLEE_SAVED = ("ebx", "esi", "edi", "ebp")

# x87 control words: all exceptions masked, round to nearest, and the precision control.
FPU_CONTROL = {"pc24": 0x007F, "pc53": 0x027F, "pc64": 0x037F}

# Instructions QEMU 5 doesn't compute like an x87 (two-byte opcodes D9 xx).
INEXACT_X87 = {0xF0: "f2xm1", 0xF1: "fyl2x", 0xF2: "fptan", 0xF3: "fpatan", 0xF5: "fprem1",
               0xF8: "fprem", 0xF9: "fyl2xp1", 0xFB: "fsincos", 0xFE: "fsin", 0xFF: "fcos"}

INTERRUPTS = {0: "divide error", 1: "debug", 3: "int3 (a breakpoint or padding)", 4: "overflow",
              5: "bound", 6: "invalid opcode", 13: "general protection", 14: "page fault", 16: "x87 error"}


class GuestFault(Exception):
    """The guest function did something the oracle can't run."""


class GuestError(Exception):
    """The guest function reached one of the game's error exits (Com_Error ...).

    That is a result, not a failure: the port must report the same error.
    """

    def __init__(self, exit_name: str, code: int, message: bytes):
        super().__init__(f"{exit_name}({code}, {message!r})")
        self.exit_name, self.code, self.message = exit_name, code, message


@dataclass
class CallResult:
    eax: int
    edx: int
    st0: tuple[int, int] | None          # (64-bit significand, 16-bit sign+exponent), when the x87 holds a result
    fpu_depth: int
    blocks: int
    prints: list[bytes] = field(default_factory=list)  # formats passed to Com_Printf-like functions
    entered: set[int] = field(default_factory=set)
    inexact: set[str] = field(default_factory=set)     # x87 instructions QEMU doesn't compute exactly
    clobbered: list[str] = field(default_factory=list)  # callee-saved registers the function changed
    stopped: str | None = None   # the end point (call(stops=...)) the call reached, instead of returning

    @property
    def u32(self) -> int:
        return self.eax & 0xFFFFFFFF

    @property
    def i32(self) -> int:
        v = self.u32
        return v - (1 << 32) if v & 0x80000000 else v


# --- the x87's 80-bit numbers ----------------------------------------------------

def x87_value(st: tuple[int, int]) -> Fraction | str:
    """The exact value of an 80-bit register: a Fraction, or 'nan', 'inf', '-inf'."""
    significand, se = st
    sign = -1 if se & 0x8000 else 1
    exponent = se & 0x7FFF
    if exponent == 0x7FFF:
        if significand & 0x7FFFFFFFFFFFFFFF:
            return "nan"
        return "inf" if sign > 0 else "-inf"
    if exponent == 0:
        exponent = 1  # denormal
    return sign * Fraction(significand) * Fraction(2) ** (exponent - 16383 - 63)


def round_to_format(st: tuple[int, int], mantissa_bits: int, min_exp: int, max_exp: int) -> tuple[int, int, int]:
    """Rounds an 80-bit value to a binary format, to nearest even, in one step (no double rounding).

    Returns (sign, biased exponent, fraction) of the target format; handles
    subnormals, overflow to infinity, NaN.
    """
    significand, se = st
    sign = 1 if se & 0x8000 else 0
    exponent = se & 0x7FFF
    max_biased = 2 * max_exp + 1
    if exponent == 0x7FFF:
        if significand & 0x7FFFFFFFFFFFFFFF:
            return sign, max_biased, (significand >> (64 - mantissa_bits)) & ((1 << (mantissa_bits - 1)) - 1) | (
                1 << (mantissa_bits - 2))
        return sign, max_biased, 0
    if significand == 0:
        return sign, 0, 0
    e = (exponent if exponent else 1) - 16383  # value = significand * 2^(e - 63)
    # Normalize so the top bit of the 64-bit significand is set.
    while significand < (1 << 63):
        significand <<= 1
        e -= 1
    bias = max_exp
    target_e = max(e, min_exp)          # subnormals keep min_exp and lose bits
    shift = 63 - (mantissa_bits - 1) + (target_e - e)
    if shift >= 128:
        kept, rest, half = 0, significand, 1 << 127
    else:
        kept = significand >> shift
        rest = significand & ((1 << shift) - 1)
        half = 1 << (shift - 1)
    if rest > half or (rest == half and kept & 1):
        kept += 1
    if kept >= (1 << mantissa_bits):    # rounding carried into a new bit
        kept >>= 1
        target_e += 1
    if kept < (1 << (mantissa_bits - 1)):  # subnormal (or zero)
        return sign, 0, kept
    if target_e > max_exp:
        return sign, max_biased, 0
    return sign, target_e + bias, kept & ((1 << (mantissa_bits - 1)) - 1)


def x87_to_f32_bits(st: tuple[int, int]) -> int:
    """What `fstp dword ptr` stores for an 80-bit value (round to nearest)."""
    sign, e, frac = round_to_format(st, 24, -126, 127)
    return (sign << 31) | (e << 23) | frac


def x87_to_f64_bits(st: tuple[int, int]) -> int:
    """What `fstp qword ptr` stores for an 80-bit value (round to nearest)."""
    sign, e, frac = round_to_format(st, 53, -1022, 1023)
    return (sign << 63) | (e << 52) | frac


# --- stand-ins for the few imports the games' own helpers make ---------------------
# name -> (handler(oracle, args) -> eax, number of stack arguments (stdcall: the callee pops them)).

def _tls_get_value(o: "X86Oracle", args):
    return o.tls.get(args[0], 0)


def _tls_set_value(o: "X86Oracle", args):
    o.tls[args[0]] = args[1]
    return 1


def _sendto(o: "X86Oracle", args):
    """wsock32 `sendto` (ordinal 20): records (destination sockaddr, data) in
    `o.sent` (emptied at the start of every call) instead of sending, and
    reports every byte sent."""
    _socket, data, length, _flags, to, to_length = args
    o.sent.append((o.read(to, to_length) if to and to_length > 0 else b"",
                   o.read(data, length) if length > 0 else b""))
    return length


IMPORT_STAND_INS = {
    "kernel32.dll!TlsGetValue": (_tls_get_value, 1),
    "kernel32.dll!TlsSetValue": (_tls_set_value, 2),
    "kernel32.dll!GetCurrentThreadId": (lambda o, a: 1, 0),
    "kernel32.dll!GetLastError": (lambda o, a: 0, 0),
    "kernel32.dll!SetLastError": (lambda o, a: 0, 1),
    # One thread, so locks are no-ops.
    "kernel32.dll!EnterCriticalSection": (lambda o, a: 0, 1),
    "kernel32.dll!LeaveCriticalSection": (lambda o, a: 0, 1),
    "kernel32.dll!InterlockedIncrement": (lambda o, a: o._interlocked(a[0], 1), 1),
    "kernel32.dll!InterlockedDecrement": (lambda o, a: o._interlocked(a[0], -1), 1),
    # Packets the games send (a network code check compares them): recorded, never sent.
    "wsock32.dll!#20": (_sendto, 6),
}

# Per-thread data, as the startup code leaves it for the main thread. Seeded before every
# call (reset() puts the file's data back, and the stand-in TLS slots start empty).
#
# "game": the game's own thread block. CoD2MP_s.exe: `Sys_InitMainThread` (0x00462360)
# allocates a TLS index (kept at 0x00D528F0) and stores 0x00D528B0 in it; then
# `Com_InitThreadData(0)` (0x0044AA00, thread number in eax) fills the block: +4 the `va`
# buffers (2 x 1024 bytes and a counter), +8 the `Com_Error` jump buffer, +0xC 24 bytes.
#
# "crt": the C runtime's per-thread data (`_getptd`, which `tolower` and friends call). Its
# startup keeps a TLS index in `__flsindex` and calls TlsGetValue/TlsSetValue through two
# pointers (`gpFlsGetValue`, `gpFlsSetValue`); `_getptd` itself would allocate the block.
# Here the block is a zeroed stand-in with the fields `_getptd` sets (+0 thread id, +4 -1,
# +0x14 the `rand` seed 1, +0x54 `&__initialmbcinfo`) and +0x64 the locale the process
# starts with (`__ptlocinfo`'s value in the file: the "C" locale).
THREAD_STATE = {
    "cod2mp": {
        "game": {"index_at": 0x00D528F0, "block": 0x00D528B0,
                 "words": {0x00D528B4: 0x00C93BD0, 0x00D528B8: 0x00C93B30, 0x00D528BC: 0x00C94BD8}},
        "crt": {"fls_index": 0x005CD390, "fls_get": 0x005DEE08, "fls_set": 0x005DEE0C,
                "ptlocinfo": 0x005CD20C, "mbcinfo": 0x005CD668},
    },
    "uomp": {
        "crt": {"fls_index": 0x005C33E8, "fls_get": 0x005D0264, "fls_set": 0x005D0268,
                "ptlocinfo": 0x005C3264, "mbcinfo": 0x005C36B8},
    },
}
CRT_PTD_SIZE = 0x8C
GAME_TLS_INDEX, CRT_TLS_INDEX = 1, 2

# The game's functions a call stops at: name -> what they are.
# "error": the game's fatal/drop error (never returns): the call ends with GuestError.
# "print": console output: recorded and skipped (cdecl, returns nothing useful).
GAME_EXITS = {"Com_Error": "error", "Com_Printf": "print", "Com_DPrintf": "print"}


def _round_up(n: int) -> int:
    return (n + PAGE - 1) & ~(PAGE - 1)


def _gdt_entry(base: int, limit: int, access: int, flags: int) -> bytes:
    return struct.pack("<HHBBBB", limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF, access,
                       (flags << 4) | ((limit >> 16) & 0xF), (base >> 24) & 0xFF)


class X86Oracle:
    def __init__(self, image: PEImage, *more: PEImage, trace_blocks: bool = False):
        self.images = [image, *more]
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self._import_stops: dict[int, str] = {}
        self._baseline: dict[int, bytes] = {}
        for img in self.images:
            data = bytearray(img.data)
            # Every import slot gets its own stop address (the file holds name pointers there).
            for slot, name in sorted(img.imports.items()):
                stop = IMPORT_STOPS + 4 * len(self._import_stops)
                self._import_stops[stop] = name
                struct.pack_into("<I", data, slot - img.base, stop)
            img_data = bytes(data)
            self._baseline[img.base] = img_data
            uc.mem_map(img.base, _round_up(len(img_data)))
            uc.mem_write(img.base, img_data)
        for base, size in ((HEAP, HEAP_SIZE), (STACK, STACK_SIZE), (IMPORT_STOPS, 0x10000),
                           (RETURN_ADDRESS, PAGE), (TEB, PAGE), (GDT, PAGE), (THREAD_DATA, THREAD_DATA_SIZE)):
            uc.mem_map(base, size)
        uc.mem_write(IMPORT_STOPS, b"\xCC" * 0x10000)
        self._setup_fs()
        self._dirty: set[int] = set()
        self._fault: str | None = None
        self._error: tuple[str, int, int] | None = None
        self._prints: list[bytes] = []
        self._inexact: set[str] = set()
        self._entered: set[int] = set()
        self._blocks = 0
        self._heap_next = HEAP
        self.tls: dict[int, int] = {}
        self.sent: list[tuple[bytes, bytes]] = []   # packets sent during the last call (see _sendto)
        self._end_hooks: set[int] = set()
        self._ends: dict[int, str] = {}
        self._ended: str | None = None
        self._readonly = [(s.start, s.end) for img in self.images for s in img.sections
                          if not s.flags & 0x80000000]
        self._write_hook = uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        uc.hook_add(UC_HOOK_MEM_INVALID, self._on_bad_memory)
        uc.hook_add(UC_HOOK_INTR, self._on_interrupt)
        uc.hook_add(UC_HOOK_CODE, self._on_import, begin=IMPORT_STOPS, end=IMPORT_STOPS + 0xFFFF)
        self._exits: dict[int, tuple[str, str]] = {}
        for img in self.images:
            for name, kind in GAME_EXITS.items():
                if name in img.addresses:
                    address = img.addresses[name]
                    self._exits[address] = (name, kind)
                    uc.hook_add(UC_HOOK_CODE, self._on_exit, begin=address, end=address)
        for img in self.images:
            for lo, hi in img.code_ranges():
                code = img.read(lo, hi - lo)
                i = code.find(b"\xD9")
                while 0 <= i < len(code) - 1:
                    if code[i + 1] in INEXACT_X87:
                        uc.hook_add(UC_HOOK_CODE, self._on_inexact, user_data=INEXACT_X87[code[i + 1]],
                                    begin=lo + i, end=lo + i)
                    i = code.find(b"\xD9", i + 1)
        if trace_blocks:
            self._starts = {a for img in self.images for a in img.names if img.is_code(a)}
            uc.hook_add(UC_HOOK_BLOCK, self._on_block)

    # --- setup ---------------------------------------------------------------
    def _setup_fs(self) -> None:
        """fs: points at a zeroed thread environment block, as on Windows.

        Once a GDT is loaded, the other data segments must come from it too
        (a flat 32-bit one), or Unicorn treats the stack as 16-bit.
        """
        gdt = (bytes(8)
               + _gdt_entry(TEB, 0xFFF, 0xF2, 0x4)       # 0x08: fs, read/write, ring 3, byte-granular
               + _gdt_entry(0, 0xFFFFF, 0x92, 0xC))      # 0x10: flat 4 GB data, ring 0, 32-bit
        self.uc.mem_write(GDT, gdt)
        self.uc.reg_write(R.UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        self.uc.reg_write(R.UC_X86_REG_FS, 0x08 | 3)
        for segment in (R.UC_X86_REG_SS, R.UC_X86_REG_DS, R.UC_X86_REG_ES):
            self.uc.reg_write(segment, 0x10)

    def _stop_for(self, import_name: str) -> int:
        return next(stop for stop, name in self._import_stops.items() if name == import_name)

    def _seed_threads(self) -> None:
        """The main thread's data as the startup code leaves it (see THREAD_STATE)."""
        self.tls = {}
        self.uc.mem_write(THREAD_DATA, bytes(THREAD_DATA_SIZE))
        for img in self.images:
            state = THREAD_STATE.get(img.key, {})
            game = state.get("game")
            if game:
                self.write(game["index_at"], struct.pack("<I", GAME_TLS_INDEX))
                self.tls[GAME_TLS_INDEX] = game["block"]
                for address, value in game["words"].items():
                    self.write(address, struct.pack("<I", value))
            crt = state.get("crt")
            if crt:
                ptd = THREAD_DATA
                self.write(crt["fls_index"], struct.pack("<I", CRT_TLS_INDEX))
                self.write(crt["fls_get"], struct.pack("<I", self._stop_for("kernel32.dll!TlsGetValue")))
                self.write(crt["fls_set"], struct.pack("<I", self._stop_for("kernel32.dll!TlsSetValue")))
                block = bytearray(CRT_PTD_SIZE)
                struct.pack_into("<iiI", block, 0, 1, -1, 0)
                struct.pack_into("<I", block, 0x14, 1)
                struct.pack_into("<I", block, 0x54, crt["mbcinfo"])
                struct.pack_into("<I", block, 0x64, img.u32(crt["ptlocinfo"]))
                self.uc.mem_write(ptd, bytes(block))
                self.tls[CRT_TLS_INDEX] = ptd

    def _reset_teb(self) -> None:
        teb = bytearray(PAGE)
        struct.pack_into("<IIII", teb, 0, 0xFFFFFFFF, STACK + STACK_SIZE, STACK, 0)  # SEH chain end, stack
        struct.pack_into("<I", teb, 0x18, TEB)  # its own address
        self.uc.mem_write(TEB, bytes(teb))

    # --- hooks ---------------------------------------------------------------
    def _on_write(self, uc, access, address, size, value, user):
        page = address & ~(PAGE - 1)
        self._dirty.add(page)
        last = (address + size - 1) & ~(PAGE - 1)
        if last != page:
            self._dirty.add(last)
        for lo, hi in self._readonly:
            if lo <= address < hi:
                pc = uc.reg_read(R.UC_X86_REG_EIP)
                self._fault = f"wrote to read-only memory at 0x{address:08X} in {self.describe(pc)}"
                uc.emu_stop()
                break

    def _on_bad_memory(self, uc, access, address, size, value, user):
        pc = uc.reg_read(R.UC_X86_REG_EIP)
        self._fault = f"touched unmapped memory at 0x{address:08X} ({size} bytes) in {self.describe(pc)}"
        return False

    def _on_interrupt(self, uc, intno, user):
        pc = uc.reg_read(R.UC_X86_REG_EIP)
        self._fault = f"{INTERRUPTS.get(intno, f'interrupt {intno}')} near {self.describe(pc)}"
        uc.emu_stop()

    def _on_inexact(self, uc, address, size, mnemonic):
        self._inexact.add(mnemonic)

    def _on_block(self, uc, address, size, user):
        self._blocks += 1
        if address in self._starts:
            self._entered.add(address)

    def _stack_args(self, count: int) -> list[int]:
        esp = self.uc.reg_read(R.UC_X86_REG_ESP)
        return [struct.unpack("<I", self.read(esp + 4 + 4 * i, 4))[0] for i in range(count)]

    def _return(self, eax: int, pop_args: int = 0) -> None:
        esp = self.uc.reg_read(R.UC_X86_REG_ESP)
        ret = struct.unpack("<I", self.read(esp, 4))[0]
        self.uc.reg_write(R.UC_X86_REG_EAX, eax & 0xFFFFFFFF)
        self.uc.reg_write(R.UC_X86_REG_ESP, esp + 4 + 4 * pop_args)
        self.uc.reg_write(R.UC_X86_REG_EIP, ret)

    def _on_import(self, uc, address, size, user):
        name = self._import_stops.get(address, f"0x{address:08X}")
        stand_in = IMPORT_STAND_INS.get(name)
        if stand_in is None:
            self._fault = f"import {name}"
            uc.emu_stop()
            return
        handler, nargs = stand_in
        self._return(handler(self, self._stack_args(nargs)), nargs)

    def _on_exit(self, uc, address, size, user):
        name, kind = self._exits[address]
        args = self._stack_args(2)
        if kind == "error":
            self._error = (name, args[0], args[1])
            uc.emu_stop()
        elif kind == "print":
            try:
                self._prints.append(self.read_cstr(args[0], 1024))
            except GuestFault:
                self._prints.append(b"?")
            self._return(0)

    def _on_end(self, uc, address, size, user):
        if address in self._ends:
            self._ended = self._ends[address]
            uc.emu_stop()

    def _interlocked(self, address: int, delta: int) -> int:
        value = (struct.unpack("<i", self.read(address, 4))[0] + delta) & 0xFFFFFFFF
        self.write(address, struct.pack("<I", value))
        return value

    # --- memory --------------------------------------------------------------
    def describe(self, address: int) -> str:
        for img in self.images:
            if img.contains(address):
                return img.describe(address)
        if address in self._import_stops:
            return f"import {self._import_stops[address]}"
        return f"0x{address:08X}"

    def alloc(self, size: int, align: int = 16) -> int:
        address = (self._heap_next + align - 1) & ~(align - 1)
        if address + size > HEAP + HEAP_SIZE:
            raise GuestFault("test heap is full")
        self._heap_next = address + max(size, 1)
        return address

    def put(self, data: bytes, align: int = 16) -> int:
        address = self.alloc(len(data), align)
        self.write(address, data)
        return address

    def put_cstr(self, text: bytes, zeros_after: int = 64) -> int:
        """A string and its terminator, then `zeros_after` zero bytes nothing else
        uses: a function that reads past the terminator (a filter's `?` at the end
        of a name) sees zeros, as a port reading past the end of a slice does."""
        address = self.put(bytes(text) + b"\0", 4)
        self.alloc(zeros_after, 1)
        return address

    def write(self, address: int, data: bytes) -> None:
        """Writes guest memory from the test; reset() undoes it like the guest's own writes."""
        self.uc.mem_write(address, bytes(data))
        for page in range(address & ~(PAGE - 1), address + len(data), PAGE):
            self._dirty.add(page)

    def read(self, address: int, size: int) -> bytes:
        try:
            return bytes(self.uc.mem_read(address, size))
        except UcError:
            raise GuestFault(f"can't read {size} bytes at 0x{address:08X}") from None

    def read_u32(self, address: int) -> int:
        return struct.unpack("<I", self.read(address, 4))[0]

    def read_cstr(self, address: int, limit: int = 1 << 16) -> bytes:
        out = bytearray()
        while len(out) < limit:
            chunk = self.read(address + len(out), 64)
            end = chunk.find(b"\0")
            if end >= 0:
                return bytes(out + chunk[:end])
            out += chunk
        raise GuestFault(f"no string terminator within {limit} bytes at 0x{address:08X}")

    def run_and_keep(self, function: int, fpu: str = "pc53", max_seconds: float = 900.0) -> CallResult:
        """Runs a set-up function of the game (no arguments) and keeps what it did, as
        keep_state() does, without tracking its writes page by page: a function like
        `MSG_initHuffman` writes memory about 10^8 times, and a Python hook on each
        write would take many minutes. The images are read back whole afterwards."""
        self.reset()
        self.uc.hook_del(self._write_hook)
        try:
            result = self.call(function, fpu=fpu, max_seconds=max_seconds)
        finally:
            self._write_hook = self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        for img in self.images:
            self._baseline[img.base] = bytearray(self.read(img.base, len(self._baseline[img.base])))
        # Whatever it left on the stack or the test heap goes back to zero.
        self.uc.mem_write(STACK, bytes(STACK_SIZE))
        self.uc.mem_write(HEAP, bytes(HEAP_SIZE))
        self._dirty.clear()
        self._heap_next = HEAP
        return result

    def keep_state(self) -> None:
        """Makes the images' memory as it is now the state reset() returns to.

        For globals a group of functions needs set up once by the game's own
        start-up function (the message Huffman tree that `MSG_initHuffman`
        builds): call that function, then keep_state(). Memory outside the
        images (the test heap, the stack) still goes back to zero.
        """
        for page in self._dirty:
            for base, data in self._baseline.items():
                if base <= page < base + len(data):
                    if not isinstance(data, bytearray):
                        data = self._baseline[base] = bytearray(data)
                    data[page - base:page - base + PAGE] = self.read(page, PAGE)
                    break
        self.reset()

    def reset(self) -> None:
        """Undoes every write of the previous call: image data back to the file's, the rest to zero."""
        for page in self._dirty:
            for base, data in self._baseline.items():
                if base <= page < base + len(data):
                    self.uc.mem_write(page, bytes(data[page - base:page - base + PAGE]))  # kept state: a bytearray
                    break
            else:
                self.uc.mem_write(page, bytes(PAGE))
        self._dirty.clear()
        self._heap_next = HEAP

    # --- calls ---------------------------------------------------------------
    def call(self, function: int, registers: dict[str, int] | None = None, stack: list[int] = (),
             callee_pops: int = 0, fpu: str = "pc53", max_seconds: float = 20.0,
             stops: dict[int, str] | None = None) -> CallResult:
        """Calls a guest function.

        `registers`: argument registers by name (eax, ecx, edx, ebx, esi, edi);
        `stack`: 32-bit stack words, first argument first (cdecl order: the
        first is closest to the return address); `callee_pops`: how many
        bytes the function's `ret N` removes (0 for caller cleanup).
        `stops`: addresses (with a label) where the call ends as soon as it
        reaches one, for a function that decides which other function to run:
        the result's `stopped` names the one reached (registers and the stack
        are then left as they are, unchecked).
        Memory set up with put()/write() before the call stays until reset().
        """
        registers = registers or {}
        uc = self.uc
        self._ends = dict(stops or {})
        for address in self._ends.keys() - self._end_hooks:
            uc.hook_add(UC_HOOK_CODE, self._on_end, begin=address, end=address)
            self._end_hooks.add(address)
        self._ended = None
        self.sent = []
        esp = STACK_TOP
        for word in reversed(list(stack)):
            esp -= 4
            uc.mem_write(esp, struct.pack("<I", word & 0xFFFFFFFF))
        esp -= 4
        uc.mem_write(esp, struct.pack("<I", RETURN_ADDRESS))
        self._dirty.update({esp & ~(PAGE - 1), STACK_TOP - PAGE})
        sentinels = {}
        for k, name in enumerate(("eax", "ecx", "edx", "ebx", "esi", "edi", "ebp")):
            value = registers.get(name)
            if value is None:
                value = 0xC0DE0000 + k  # recognizable garbage for registers that aren't arguments
                sentinels[name] = value
            uc.reg_write(REGISTERS[name], value & 0xFFFFFFFF)
        uc.reg_write(R.UC_X86_REG_ESP, esp)
        uc.reg_write(R.UC_X86_REG_EFLAGS, 0x202)  # direction flag clear, as the ABI requires
        uc.reg_write(R.UC_X86_REG_FPCW, FPU_CONTROL[fpu])
        uc.reg_write(R.UC_X86_REG_FPSW, 0)
        uc.reg_write(R.UC_X86_REG_FPTAG, 0xFFFF)  # an empty x87 stack, as at any call
        uc.reg_write(R.UC_X86_REG_MXCSR, 0x1F80)
        self._reset_teb()
        self._seed_threads()
        self._fault, self._error, self._blocks = None, None, 0
        self._prints, self._inexact, self._entered = [], set(), set()
        try:
            # A time limit, not an instruction count: Unicorn counts instructions with a
            # hook on every one, which makes it check every code hook on every instruction
            # (the x87 detection adds hundreds): 10 to 15 times slower.
            uc.emu_start(function, RETURN_ADDRESS, timeout=int(max_seconds * 1_000_000))
        except UcError as e:
            if not self._fault:
                self._fault = f"{e} at {self.describe(uc.reg_read(R.UC_X86_REG_EIP))}"
        pc = uc.reg_read(R.UC_X86_REG_EIP)
        if self._error:
            name, code, message = self._error
            try:
                text = self.read_cstr(message, 512)
            except GuestFault:
                text = b"?"
            raise GuestError(name, code, text)
        if self._fault:
            raise GuestFault(self._fault)
        if self._ended is not None:
            return CallResult(eax=uc.reg_read(R.UC_X86_REG_EAX), edx=uc.reg_read(R.UC_X86_REG_EDX), st0=None,
                              fpu_depth=0, blocks=self._blocks, prints=list(self._prints),
                              entered=self._entered - {function}, inexact=set(self._inexact),
                              stopped=self._ended)
        if pc != RETURN_ADDRESS:
            raise GuestFault(f"didn't return within {max_seconds:g} seconds "
                             f"(stopped in {self.describe(pc)})")
        after = uc.reg_read(R.UC_X86_REG_ESP)
        if after != esp + 4 + callee_pops:
            raise GuestFault(f"the function removed {after - esp - 4} bytes of arguments, the spec says "
                             f"{callee_pops}: its calling convention is wrong")
        status = uc.reg_read(R.UC_X86_REG_FPSW)
        tags = uc.reg_read(R.UC_X86_REG_FPTAG)
        depth = sum(1 for i in range(8) if (tags >> (2 * i)) & 3 != 3)
        top = (status >> 11) & 7
        st0 = None
        if depth:
            st0 = tuple(uc.reg_read(R.UC_X86_REG_FP0 + top))
        clobbered = [name for name in CALLEE_SAVED
                     if name in sentinels and uc.reg_read(REGISTERS[name]) != sentinels[name]]
        return CallResult(eax=uc.reg_read(R.UC_X86_REG_EAX), edx=uc.reg_read(R.UC_X86_REG_EDX), st0=st0,
                          fpu_depth=depth, blocks=self._blocks, prints=list(self._prints),
                          entered=self._entered - {function}, inexact=set(self._inexact), clobbered=clobbered)
