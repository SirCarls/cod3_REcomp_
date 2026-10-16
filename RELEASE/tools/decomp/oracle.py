"""Runs the game's own PowerPC functions, one call at a time: the reference
every Rust port is checked against.

It uses the Unicorn CPU emulator (QEMU's PowerPC code, GPL-2.0; a tool, not
part of the game or the engine) with a PowerPC 970FX model, the closest
QEMU has to the Xbox 360's Xenon: 64-bit PowerPC with a floating point unit
and AltiVec. Like the console, it runs with 32-bit addresses (MSR[SF] = 0),
so the game's sign-extended `lis` addresses work as they do on the 360.

What it can't run (the call stops with a message saying where):
- Xenon's VMX128 vector instructions (the extended AltiVec of the Xbox 360);
- calls into the kernel or XAM (import thunks the console's loader fills),
  except the few the C runtime makes for itself, which have small stand-ins
  here (`KERNEL_STAND_INS`: thread-local storage, the heap, last-error);
- anything that needs the rest of the game to have started (globals the
  game fills at run time are zero unless a test writes them first).

Unicorn 2.1.4 as released can't select a 64-bit PowerPC model (the model
number isn't offset past the 32-bit ones), so floating point instructions
fail. `unicorn_build.py` builds it with that one line fixed; this module
finds the build through `COD3_UNICORN_LIB` or in `work/tools/unicorn/`.
"""
from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass, field

import image
from image import ROOT, GuestImage

UNICORN_DIR = ROOT / "work" / "tools" / "unicorn"


def _point_unicorn_at_fixed_build() -> None:
    """Prefers the build from unicorn_build.py (library and its Python binding)."""
    lib = os.environ.get("COD3_UNICORN_LIB")
    if not lib and (UNICORN_DIR / "build.json").is_file():
        lib = str(UNICORN_DIR)
        sys.path.insert(0, str(UNICORN_DIR / "python"))
    if lib:
        os.environ["LIBUNICORN_PATH"] = lib


_point_unicorn_at_fixed_build()
try:
    from unicorn import (UC_ARCH_PPC, UC_HOOK_BLOCK, UC_HOOK_INTR, UC_HOOK_MEM_INVALID,
                         UC_HOOK_MEM_WRITE, UC_MODE_BIG_ENDIAN, UC_MODE_PPC64, Uc, UcError)
    from unicorn import ppc_const as ppc
except ImportError as e:  # pragma: no cover - depends on the machine
    raise SystemExit("The decomp oracle needs Unicorn: python3 tools/decomp/unicorn_build.py "
                     f"(see docs/DECOMP.md). ({e})") from None

# Guest memory map for a call. The image keeps its own address (0x82000000 for
# default.xex, 0x89000000 for a level DLL); everything else sits where the game
# never puts anything.
HEAP = 0x40000000          # test data (strings, structures); reset for every call
HEAP_SIZE = 0x01000000
STACK = 0x70000000
STACK_SIZE = 0x00100000
RETURN_ADDRESS = 0x7F000000  # the call ends when the function returns here
HOST_FUNCTIONS = 0x7F010000  # addresses of the host-side stand-ins below (one word each)
PCR = 0x7FFE0000           # what r13 points at (the 360's per-processor region)
PCR_SIZE = 0x00010000
# Inside it, a zeroed stand-in for the running thread, which the C runtime reaches
# through r13: +0x100 the current KTHREAD (GetLastError reads its +0x160),
# +0x150 zero ("not at DPC level"). Both are reset before every call.
PCR_THREAD_OFFSET = 0x100
FAKE_KTHREAD = PCR + 0x1000
PAGE = 0x1000

MSR_FP = 1 << 13
MSR_VEC = 1 << 25

INTERRUPTS = {2: "data storage", 3: "instruction storage", 5: "alignment",
              6: "program (an illegal instruction or a trap)", 7: "floating point unavailable",
              73: "vector unit unavailable", 96: "illegal instruction"}


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
    r3: int
    r4: int
    f1: float
    f2: float
    blocks: int
    entered: set[int] = field(default_factory=set)  # functions the call ran, besides the first

    @property
    def u32(self) -> int:
        return self.r3 & 0xFFFFFFFF

    @property
    def i32(self) -> int:
        v = self.r3 & 0xFFFFFFFF
        return v - (1 << 32) if v & 0x80000000 else v

    @property
    def f32(self) -> float:
        return struct.unpack(">f", struct.pack(">f", self.f1))[0]


def _round_up(n: int) -> int:
    return (n + PAGE - 1) & ~(PAGE - 1)


def _f64_bits(value: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def _bits_f64(bits: int) -> float:
    return struct.unpack("<d", struct.pack("<Q", bits & 0xFFFFFFFFFFFFFFFF))[0]


# Kernel calls the C runtime makes on its own (per-thread data for printf and
# friends): name -> (oracle, r3..r6) -> r3. Anything else stops the call.
def _tls_get(o: "Oracle", index, *_):
    return o._tls.get(index, 0)


def _tls_set(o: "Oracle", index, value, *_):
    o._tls[index] = value
    return 1


def _tls_alloc(o: "Oracle", *_):
    o._tls_next += 1
    return o._tls_next


def _heap_alloc(o: "Oracle", heap, flags, size, *_):
    address = o.alloc(max(size, 1), 16)
    o.write(address, bytes(size))  # zeroed, like HEAP_ZERO_MEMORY; harmless otherwise
    return address


KERNEL_STAND_INS = {
    "KeTlsGetValue": _tls_get,
    "KeTlsSetValue": _tls_set,
    "KeTlsAlloc": _tls_alloc,
    "RtlAllocateHeap": _heap_alloc,
    "RtlFreeHeap": lambda o, *_: 1,
    "RtlNtStatusToDosError": lambda o, status, *_: 0 if status == 0 else 317,  # ERROR_MR_MID_NOT_FOUND
    "KeTlsFree": lambda o, *_: 1,
    # One thread, so locks are no-ops.
    "RtlInitializeCriticalSection": lambda o, *_: 0,
    "RtlInitializeCriticalSectionAndSpinCount": lambda o, *_: 0,
    "RtlEnterCriticalSection": lambda o, *_: 0,
    "RtlLeaveCriticalSection": lambda o, *_: 0,
    "RtlTryEnterCriticalSection": lambda o, *_: 1,
    "RtlDeleteCriticalSection": lambda o, *_: 0,
}


# Stand-ins the game reaches through a function pointer instead of an import:
# name -> (oracle, r3..r6) -> r3. Each gets an address in the HOST_FUNCTIONS
# page; PROGRAM_HOOKS stores those addresses into the program's pointers.
def _demonware_allocate(o: "Oracle", size, *_):
    address = o.alloc(max(size, 1), 16)
    o.write(address, bytes(size))
    return address


HOST_STAND_INS = {
    "demonware_allocate": _demonware_allocate,
    "demonware_free": lambda o, *_: 0,
}

# Function pointers the game fills in at start, as startup would leave them.
# The multiplayer program's Demonware library allocates through two hooks the
# game installs (bdMemory: allocate 0x821AD880 calls through 0x82A97E94, free
# 0x821AD8D8 through 0x82A97E98); without them every bdBitBuffer is empty.
PROGRAM_HOOKS = {
    "codmp": {0x82A97E94: "demonware_allocate", 0x82A97E98: "demonware_free"},
}


# The C runtime's per-thread data, as its startup (`_mtinit`: 0x82352998 in
# default.xex, 0x82322CE8 in codmp_xenonf.xex) leaves it: printf and friends
# reach it through `_getptd`. Seeded before every call, since reset() puts the
# image's data back. The multiplayer program's addresses were found by matching
# its C runtime to the campaign's instruction by instruction
# (tools/name_mp_functions.py, `pass_same_as_campaign`).
CRT_THREAD_STATE = {
    "default": {
        "tls_index": 0x825C6E38,         # __tlsindex: a TLS slot holding the "FlsGetValue" pointer
        "fls_index": 0x825C6E34,         # __flsindex: the slot holding the per-thread data pointer
        "fls_get_value": 0x8291F434,     # gpFlsGetValue = the KeTlsGetValue thunk
        "fls_set_value": 0x8291F438,     # gpFlsSetValue = the KeTlsSetValue thunk
        "except_action_table": 0x825C7520,  # what _initptd stores at +0x5C
    },
    "codmp": {
        "tls_index": 0x825CC348,
        "fls_index": 0x825CC344,
        "fls_get_value": 0x82A9B55C,
        "fls_set_value": 0x82A9B560,
        "except_action_table": 0x825CCA38,
    },
}
CRT_PTD = PCR + 0x2000  # the per-thread data block (0xC4 bytes), inside the zeroed PCR area
CRT_TLS_SLOT, CRT_FLS_SLOT = 1, 2


def for_image(name: str) -> "Oracle":
    """An oracle for one image: a program on its own, a level DLL next to the campaign program."""
    img = image.load(name)
    return Oracle(img) if name in image.PROGRAMS else Oracle(image.load("default"), img)


class Oracle:
    def __init__(self, *images: GuestImage):
        if not images:
            raise ValueError("Oracle needs at least one image")
        self.images = list(images)
        self.uc = Uc(UC_ARCH_PPC, UC_MODE_PPC64 | UC_MODE_BIG_ENDIAN)
        self.uc.ctl_set_cpu_model(ppc.UC_CPU_PPC64_970FX_V3_1)
        for img in self.images:
            self.uc.mem_map(img.base, _round_up(len(img.data)))
            self.uc.mem_write(img.base, img.data)
        self.uc.mem_map(HEAP, HEAP_SIZE)
        self.uc.mem_map(STACK, STACK_SIZE)
        self.uc.mem_map(RETURN_ADDRESS, PAGE)
        self.uc.mem_map(HOST_FUNCTIONS, PAGE)
        self._host_names = list(HOST_STAND_INS)
        self.uc.mem_map(PCR, PCR_SIZE)
        self.uc.reg_write(ppc.UC_PPC_REG_MSR, MSR_FP | MSR_VEC)  # 32-bit addressing: SF stays 0
        self._dirty: set[int] = set()
        self._code_written = False
        self._replaced: dict = {}
        self._fault: str | None = None
        self._entered: set[int] = set()
        self._blocks = 0
        self._thunks: dict[int, str] = {}
        self._starts: set[int] = set()
        for img in self.images:
            self._thunks.update(img.imports)
            self._starts.update(f[0] for f in img.functions)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._on_bad_memory)
        self.uc.hook_add(UC_HOOK_INTR, self._on_interrupt)
        self.uc.hook_add(UC_HOOK_BLOCK, self._on_block)
        self._heap_next = HEAP
        # The game's error exits: a call that reaches one ends with GuestError.
        self._exits: dict[int, str] = {}
        for img in self.images:
            for name in ("Com_Error",):
                if name in img.addresses:
                    self._exits[img.addresses[name]] = name
        self._error: tuple[str, int, int] | None = None
        self._check_fixed_unicorn()

    # --- setup checks -----------------------------------------------------
    def _check_fixed_unicorn(self) -> None:
        """A stock Unicorn 2.1.4 runs a 32-bit model here and every float instruction fails."""
        probe = RETURN_ADDRESS + 0x100
        self.uc.mem_write(probe, bytes.fromhex("FC01102A"))  # fadd f0, f1, f2
        self._fault = None
        try:
            self.uc.emu_start(probe, probe + 4)
        except UcError:
            pass
        if self._fault:
            raise SystemExit(
                "This Unicorn can't run PowerPC floating point code (the stock 2.1.4 release picks a "
                "32-bit CPU). Build the fixed one: python3 tools/decomp/unicorn_build.py")

    # --- hooks ------------------------------------------------------------
    def _on_write(self, uc, access, address, size, value, user):
        self._dirty.add(address & ~(PAGE - 1))
        if size > 1:
            self._dirty.add((address + size - 1) & ~(PAGE - 1))

    def _on_bad_memory(self, uc, access, address, size, value, user):
        pc = uc.reg_read(ppc.UC_PPC_REG_PC)
        self._fault = f"touched unmapped memory at 0x{address:08X} ({size} bytes) in {self.describe(pc)}"
        return False

    def _on_interrupt(self, uc, intno, user):
        pc = uc.reg_read(ppc.UC_PPC_REG_PC)
        what = INTERRUPTS.get(intno, f"interrupt {intno}")
        word = self.read_u32(pc - 4) if self._mapped(pc - 4) else 0
        self._fault = f"{what} near {self.describe(pc - 4)} (instruction 0x{word:08X})"
        uc.emu_stop()

    def _on_block(self, uc, address, size, user):
        self._blocks += 1
        if address in self._exits:
            self._error = (self._exits[address], uc.reg_read(ppc.UC_PPC_REG_3), uc.reg_read(ppc.UC_PPC_REG_4))
            uc.emu_stop()
        elif address in self._replaced:
            args = [uc.reg_read(ppc.UC_PPC_REG_3 + i) & 0xFFFFFFFF for i in range(6)]
            uc.reg_write(ppc.UC_PPC_REG_3, (self._replaced[address](self, *args) or 0) & 0xFFFFFFFF)
            uc.reg_write(ppc.UC_PPC_REG_PC, uc.reg_read(ppc.UC_PPC_REG_LR))
        elif HOST_FUNCTIONS <= address < HOST_FUNCTIONS + 4 * len(self._host_names):
            stand_in = HOST_STAND_INS[self._host_names[(address - HOST_FUNCTIONS) // 4]]
            args = [uc.reg_read(ppc.UC_PPC_REG_3 + i) & 0xFFFFFFFF for i in range(4)]
            uc.reg_write(ppc.UC_PPC_REG_3, stand_in(self, *args) & 0xFFFFFFFF)
            uc.reg_write(ppc.UC_PPC_REG_PC, uc.reg_read(ppc.UC_PPC_REG_LR))
        elif address in self._thunks:
            name = self._thunks[address].removeprefix("__imp__")
            stand_in = KERNEL_STAND_INS.get(name)
            if stand_in is None:
                self._fault = f"calls {self._thunks[address]} (a kernel/XAM import) from the game"
                uc.emu_stop()
                return
            args = [uc.reg_read(ppc.UC_PPC_REG_3 + i) & 0xFFFFFFFF for i in range(4)]
            uc.reg_write(ppc.UC_PPC_REG_3, stand_in(self, *args) & 0xFFFFFFFF)
            uc.reg_write(ppc.UC_PPC_REG_PC, uc.reg_read(ppc.UC_PPC_REG_LR))
        elif address in self._starts:
            self._entered.add(address)

    # --- memory -----------------------------------------------------------
    def _mapped(self, address: int) -> bool:
        return any(img.contains(address) for img in self.images) or HEAP <= address < HEAP + HEAP_SIZE

    def describe(self, address: int) -> str:
        for img in self.images:
            if img.contains(address):
                return img.describe(address)
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

    def put_cstr(self, text: bytes) -> int:
        return self.put(bytes(text) + b"\0", 4)

    def write(self, address: int, data: bytes) -> None:
        """Writes guest memory from the test; reset() undoes it like the guest's own writes.

        A write into a program's code (a check replacing a function the one
        under test calls) also drops the emulator's translations of that
        code, now and again when reset() puts the original bytes back:
        Unicorn keeps running the old translation otherwise.
        """
        self.uc.mem_write(address, bytes(data))
        for page in range(address & ~(PAGE - 1), address + len(data), PAGE):
            self._dirty.add(page)
        if any(img.contains(address) and img.is_code(address) for img in self.images):
            self.uc.ctl_remove_cache(address, address + len(data))
            self._code_written = True

    def read(self, address: int, size: int) -> bytes:
        return bytes(self.uc.mem_read(address, size))

    def replace(self, address: int, stand_in) -> None:
        """Until reset(), a call to the game function at `address` runs
        `stand_in(oracle, r3, ..., r8)` instead, whose result goes to r3.

        For checking a function on its own when it calls one that isn't
        ported yet, or one the emulator can't run (VMX128 code): the check's
        Rust side then uses the same stand-in.
        """
        self._replaced[address] = stand_in

    def read_u32(self, address: int) -> int:
        return struct.unpack(">I", self.read(address, 4))[0]

    def read_cstr(self, address: int, limit: int = 1 << 16) -> bytes:
        out = bytearray()
        while len(out) < limit:
            chunk = self.read(address + len(out), 64)
            end = chunk.find(b"\0")
            if end >= 0:
                return bytes(out + chunk[:end])
            out += chunk
        raise GuestFault(f"no string terminator within {limit} bytes at 0x{address:08X}")

    def reset(self) -> None:
        """Undo every write of the previous call: image data back to the file's, test memory to zero."""
        for page in self._dirty:
            for img in self.images:
                if img.contains(page):
                    self.uc.mem_write(page, img.read(page, PAGE))
                    break
            else:
                self.uc.mem_write(page, bytes(PAGE))
        self._dirty.clear()
        self._heap_next = HEAP
        self._replaced.clear()
        if self._code_written:
            self.uc.ctl_flush_tb()
            self._code_written = False

    def _seed_hooks(self) -> None:
        for img in self.images:
            for pointer, name in PROGRAM_HOOKS.get(img.name, {}).items():
                address = HOST_FUNCTIONS + 4 * self._host_names.index(name)
                self.write(pointer, struct.pack(">I", address))

    def _seed_crt(self) -> None:
        program = next((img for img in self.images if img.name in CRT_THREAD_STATE), None)
        if program is None:
            return
        c = CRT_THREAD_STATE[program.name]
        get_thunk = program.addresses.get("__imp__KeTlsGetValue")
        set_thunk = program.addresses.get("__imp__KeTlsSetValue")
        if get_thunk is None or set_thunk is None:
            return  # no symbol map: the C runtime's per-thread data stays unset
        for name, value in (("tls_index", CRT_TLS_SLOT), ("fls_index", CRT_FLS_SLOT),
                            ("fls_get_value", get_thunk), ("fls_set_value", set_thunk)):
            self.write(c[name], struct.pack(">I", value))
        self.uc.mem_write(CRT_PTD, bytes(0xC4))
        self.uc.mem_write(CRT_PTD + 0x04, struct.pack(">I", 0xFFFFFFFF))  # _thandle
        self.uc.mem_write(CRT_PTD + 0x14, struct.pack(">I", 1))           # _holdrand
        self.uc.mem_write(CRT_PTD + 0x5C, struct.pack(">I", c["except_action_table"]))
        self._tls = {CRT_TLS_SLOT: get_thunk, CRT_FLS_SLOT: CRT_PTD}
        self._tls_next = CRT_FLS_SLOT

    # --- calls ------------------------------------------------------------
    def call(self, function: int, gprs: list[int] = (), fprs: list[float] = (),
             max_instructions: int = 20_000_000) -> CallResult:
        """Calls a guest function with integer arguments in r3.. and floating point ones in f1..

        Memory set up with put()/write() before the call stays until reset().
        """
        if len(gprs) > 8 or len(fprs) > 13:
            raise ValueError("too many register arguments")
        for i in range(32):
            self.uc.reg_write(ppc.UC_PPC_REG_0 + i, 0)
            self.uc.reg_write(ppc.UC_PPC_REG_FPR0 + i, 0)
        for i, v in enumerate(gprs):
            self.uc.reg_write(ppc.UC_PPC_REG_3 + i, v & 0xFFFFFFFFFFFFFFFF)
        for i, v in enumerate(fprs):
            self.uc.reg_write(ppc.UC_PPC_REG_FPR1 + i, _f64_bits(v))
        self.uc.reg_write(ppc.UC_PPC_REG_1, STACK + STACK_SIZE - 0x400)
        self.uc.reg_write(ppc.UC_PPC_REG_13, PCR)
        self.uc.mem_write(PCR, bytes(PCR_SIZE))
        self.uc.mem_write(PCR + PCR_THREAD_OFFSET, struct.pack(">I", FAKE_KTHREAD))
        self.uc.reg_write(ppc.UC_PPC_REG_LR, RETURN_ADDRESS)
        self.uc.reg_write(ppc.UC_PPC_REG_CR, 0)
        self.uc.reg_write(ppc.UC_PPC_REG_XER, 0)
        self.uc.reg_write(ppc.UC_PPC_REG_FPSCR, 0)
        self._fault, self._error, self._blocks, self._entered = None, None, 0, set()
        self._tls, self._tls_next = {}, 0
        self._seed_crt()
        self._seed_hooks()
        try:
            self.uc.emu_start(function, RETURN_ADDRESS, count=max_instructions)
        except UcError as e:
            if not self._fault:
                pc = self.uc.reg_read(ppc.UC_PPC_REG_PC)
                self._fault = f"{e} at {self.describe(pc)}"
        pc = self.uc.reg_read(ppc.UC_PPC_REG_PC)
        if self._error:
            name, code, message = self._error
            try:
                text = self.read_cstr(message & 0xFFFFFFFF, 512)
            except Exception:
                text = b"?"
            raise GuestError(name, code & 0xFFFFFFFF, text)
        if self._fault:
            raise GuestFault(self._fault)
        if pc != RETURN_ADDRESS:
            raise GuestFault(f"didn't return within {max_instructions:,} instructions "
                             f"(stopped in {self.describe(pc)})")
        return CallResult(
            r3=self.uc.reg_read(ppc.UC_PPC_REG_3), r4=self.uc.reg_read(ppc.UC_PPC_REG_4),
            f1=_bits_f64(self.uc.reg_read(ppc.UC_PPC_REG_FPR1)),
            f2=_bits_f64(self.uc.reg_read(ppc.UC_PPC_REG_FPR2)),
            blocks=self._blocks, entered=self._entered - {function})
