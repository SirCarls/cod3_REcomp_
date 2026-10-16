"""Checks Rust ports against the game's own functions.

A *spec* says how to call one guest function: its arguments, what it
returns, and how to make test cases. For each case the checker
1. calls the guest function in the oracle (the original PowerPC code), and
2. asks `decomp_check` (engine/tools/decomp_check, the Rust ports) the same
   question over a line protocol,
then compares the answers and every buffer the function may have changed.

Line protocol (one request per line, tokens separated by spaces):
    request:  <function> <arg> <arg> ...
    reply:    ok <result> <buffer> <buffer> ...      or      err <message>
Tokens: i:<decimal> integer; f:<8 hex> f32 bits; d:<16 hex> f64 bits;
s:<hex> a string's bytes (s:- for a NULL pointer); b:<hex> a buffer the
function may change (its bytes after the call come back in the reply, in
order); n: nothing (void); v:<hex> bytes the function only reads (a vector,
a structure such as a `netadr_t`);
m:<+0>,<maxsize>,<cursize>,<readcount>,<hex> a network message (`msg_t`)
and the data buffer it points at: the structure's integer fields in decimal,
then the buffer's bytes. The function may change both, so an m: argument
comes back in the reply too, in the same form, in order with the b: ones.
"""
from __future__ import annotations

import inspect
import math
import random
import struct
import subprocess
from dataclasses import dataclass, field
from typing import Callable, Iterable

import image
from image import ROOT
from oracle import CallResult, GuestError, GuestFault, Oracle

ENGINE = ROOT / "engine"


def f32_bits(v: float) -> int:
    return struct.unpack(">I", struct.pack(">f", v))[0]


def bits_f32(b: int) -> float:
    return struct.unpack(">f", struct.pack(">I", b & 0xFFFFFFFF))[0]


def to_f32(v: float) -> float:
    try:
        return bits_f32(f32_bits(v))
    except OverflowError:
        return math.copysign(math.inf, v)


# --- argument kinds --------------------------------------------------------

class Arg:
    """How one argument reaches the guest (register or memory) and the Rust side (a token)."""
    reg = "gpr"
    mutable = False
    floats = False  # a buffer of big-endian f32s, compared as numbers (see _tokens_match)

    def place(self, oracle: Oracle, value) -> int | float:
        raise NotImplementedError

    def token(self, value) -> str:
        raise NotImplementedError

    def read_back(self, oracle: Oracle, pointer: int, value) -> bytes:
        return b""

    def reply_token(self, oracle: Oracle, pointer: int, value) -> str:
        """A mutable argument after the call, as the reply carries it (b:<hex>, or b:- for NULL)."""
        return "b:-" if not pointer else f"b:{self.read_back(oracle, pointer, value).hex()}"


@dataclass
class Int(Arg):
    bits: int = 32

    def place(self, oracle, value):
        return value & ((1 << self.bits) - 1) if value >= 0 else value

    def token(self, value):
        return f"i:{value}"


@dataclass
class F32(Arg):
    reg = "fpr"

    def place(self, oracle, value):
        return to_f32(value)

    def token(self, value):
        return f"f:{f32_bits(value):08x}"


@dataclass
class F64(Arg):
    reg = "fpr"

    def place(self, oracle, value):
        return value

    def token(self, value):
        return f"d:{struct.unpack('>Q', struct.pack('>d', value))[0]:016x}"


@dataclass
class CStr(Arg):
    """A NUL-terminated string the function only reads (None = a NULL pointer)."""

    def place(self, oracle, value):
        return 0 if value is None else oracle.put_cstr(value)

    def token(self, value):
        return "s:-" if value is None else f"s:{bytes(value).hex()}"


@dataclass
class Buffer(Arg):
    """Bytes the function may change; `size` bytes are compared after the call.

    The value is the initial content: a string, which gets its terminator,
    then `fill` bytes up to `size` (so writes past the string show up).
    `terminated=False` for binary data.
    """
    size: int = 0
    fill: int = 0
    terminated: bool = True
    mutable = True

    def initial(self, value) -> bytes:
        data = bytes(value) + (b"\0" if self.terminated else b"")
        return data + bytes([self.fill]) * max(0, self.size - len(data))

    def place(self, oracle, value):
        return oracle.put(self.initial(value), 16)

    def token(self, value):
        return f"b:{self.initial(value).hex()}"

    def read_back(self, oracle, pointer, value):
        return oracle.read(pointer, len(self.initial(value)))


@dataclass
class Floats(Arg):
    """`count` floats in memory the function only reads (a vec2_t, vec3_t ...; big-endian)."""
    count: int = 3

    def place(self, oracle, value):
        return oracle.put(struct.pack(f">{self.count}f", *value), 16)

    def token(self, value):
        return "v:" + struct.pack(f">{self.count}f", *value).hex()


@dataclass
class FloatBuffer(Buffer):
    """`count` floats the function reads and changes in place."""
    count: int = 3
    floats = True

    def initial(self, value) -> bytes:
        return struct.pack(f">{self.count}f", *value)


@dataclass
class OutFloats(Buffer):
    """`count` floats the function writes (None = a NULL pointer, where it allows one).

    Filled with 0xCD bytes first, so a float the function forgot to write shows up.
    """
    count: int = 3
    floats = True

    def initial(self, value) -> bytes:
        return bytes([0xCD]) * (4 * self.count)

    def place(self, oracle, value):
        return 0 if value is None else oracle.put(self.initial(value), 16)

    def token(self, value):
        return "b:-" if value is None else f"b:{self.initial(value).hex()}"

    def read_back(self, oracle, pointer, value):
        return b"" if value is None else oracle.read(pointer, 4 * self.count)


@dataclass
class GlobalU32(Arg):
    """A 32-bit global the function reads and may change (a generator's seed ...).

    It isn't passed in a register: the checker writes it to `address` before
    the call and compares it afterwards; the Rust side gets it as a 4-byte buffer.
    """
    address: int = 0
    reg = "none"
    mutable = True

    def place(self, oracle, value):
        oracle.write(self.address, struct.pack(">I", value & 0xFFFFFFFF))
        return self.address

    def token(self, value):
        return "b:" + struct.pack(">I", value & 0xFFFFFFFF).hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(self.address, 4)


@dataclass
class GlobalU8(GlobalU32):
    """An 8-bit global the function reads and may change (a status flag ...); a 1-byte buffer on the Rust side."""

    def place(self, oracle, value):
        oracle.write(self.address, bytes([value & 0xFF]))
        return self.address

    def token(self, value):
        return f"b:{value & 0xFF:02x}"

    def read_back(self, oracle, pointer, value):
        return oracle.read(self.address, 1)


@dataclass
class Bytes(Arg):
    """A structure the function only reads, by pointer; the value is its bytes, as the guest keeps them."""

    def place(self, oracle, value):
        return oracle.put(bytes(value), 16)

    def token(self, value):
        return f"v:{bytes(value).hex()}"


@dataclass
class BytesInRegisters(Bytes):
    """A small structure passed by value, which the Xbox 360 compiler puts in registers.

    It takes one register per 8 bytes, big-endian, and the last one is filled
    from the top: a 20-byte `netadr_t` is r3 (bytes 0-7), r4 (8-15) and the
    upper half of r5 (16-19), as Netchan_Process passes one to NET_AdrToString.
    """
    reg = "gprs"

    def place(self, oracle, value):
        data = bytes(value) + bytes(-len(value) % 8)
        return [int.from_bytes(data[i:i + 8], "big") for i in range(0, len(data), 8)]


@dataclass
class Message:
    """A test case's network message: a `msg_t` and the buffer its `data` points at."""
    data: bytes             # the whole buffer; its length is the buffer's real size
    cursize: int = 0        # +0xC: how many bytes are written
    readcount: int = 0      # +0x10: where the next read starts
    maxsize: int | None = None  # +8: the size the game believes the buffer has (None: len(data))
    overflowed: int = 0     # +0: zeroed by MSG_Init; no message function reads it

    @property
    def size(self) -> int:
        return len(self.data) if self.maxsize is None else self.maxsize

    def token(self) -> str:
        return f"m:{self.overflowed},{self.size},{self.cursize},{self.readcount},{bytes(self.data).hex()}"


# msg_t: +0 overflowed, +4 data, +8 maxsize, +0xC cursize, +0x10 readcount (20 bytes,
# MSG_Init at 0x824521D8 and SV_SendClientSnapshot's inline set-up zero exactly these).
MSG_FIELDS = struct.Struct(">iIiii")
# Where the data buffer goes, after the structure. The bytes in between are a guard:
# a function that wrote past the 20 bytes would mean the layout above is wrong.
MSG_DATA_OFFSET = 32
MSG_GUARD = bytes([0xA5]) * (MSG_DATA_OFFSET - MSG_FIELDS.size)


@dataclass
class Msg(Arg):
    """A network message (`msg_t`) and its data buffer, which the function may change.

    The value is a `Message`. Its token (m:) carries the structure's integer
    fields and the buffer's bytes; after the call both are read back for the
    reply, so a port must leave the same fields and the same bytes.
    """
    mutable = True

    def place(self, oracle, value):
        data = bytes(value.data)
        address = oracle.alloc(MSG_DATA_OFFSET + len(data), 16)
        oracle.write(address, MSG_FIELDS.pack(value.overflowed, address + MSG_DATA_OFFSET, value.size,
                                              value.cursize, value.readcount) + MSG_GUARD + data)
        return address

    def token(self, value):
        return value.token()

    def reply_token(self, oracle, pointer, value):
        overflowed, data, maxsize, cursize, readcount = MSG_FIELDS.unpack(oracle.read(pointer, MSG_FIELDS.size))
        if data != pointer + MSG_DATA_OFFSET:
            return "m:the-data-pointer-changed"
        if oracle.read(pointer + MSG_FIELDS.size, len(MSG_GUARD)) != MSG_GUARD:
            return "m:wrote-past-the-msg_t"
        return Message(oracle.read(data, len(value.data)), cursize, readcount, maxsize, overflowed).token()


# --- results -----------------------------------------------------------------

class Ret:
    def from_guest(self, oracle: Oracle, result: CallResult) -> str:
        raise NotImplementedError


class RetVoid(Ret):
    def from_guest(self, oracle, result):
        return "n:"


class RetI32(Ret):
    def from_guest(self, oracle, result):
        return f"i:{result.i32}"


class RetU32(Ret):
    def from_guest(self, oracle, result):
        return f"i:{result.u32}"


class RetBool(Ret):
    """A C++ `bool`: only the low byte of r3 counts (i:0 or i:1)."""

    def from_guest(self, oracle, result):
        return f"i:{int(result.u32 & 0xFF != 0)}"


class RetF32(Ret):
    def from_guest(self, oracle, result):
        return f"f:{f32_bits(result.f1):08x}"


class RetF64(Ret):
    def from_guest(self, oracle, result):
        return f"d:{struct.unpack('>Q', struct.pack('>d', result.f1))[0]:016x}"


class RetCStr(Ret):
    """A pointer to a string (s:- when NULL)."""

    def from_guest(self, oracle, result):
        return "s:-" if result.u32 == 0 else f"s:{oracle.read_cstr(result.u32).hex()}"


# --- specs -------------------------------------------------------------------

@dataclass
class Spec:
    name: str                       # the name in the symbol map, also the name decomp_check answers to
    args: list[Arg]
    ret: Ret
    cases: Callable[[random.Random], Iterable[list]]
    rust: str = ""                  # the Rust function, for the status table
    image: str = "default"
    float_ulps: int = 0             # allowed difference for f32 results and buffers of floats
    setup: Callable[[Oracle, list], None] | None = None  # writes guest globals before the call
    notes: str = ""
    # Functions of the game's startup that this one depends on, run before every call
    # (Swap_Init fills the byte-order function pointers NET_AdrToString calls). Found
    # in whichever program is checked (`startup_address`), so `check --image` keeps them.
    startup: tuple[str, ...] = ()
    # Golden answers (`decomp.py golden`): None records them when the function
    # takes or returns floats; True also for one whose floats are inside a
    # structure (a message); False never (a case that carries the game's own
    # data, such as a table it reads, can't be published).
    golden: bool | None = None
    golden_cases: int | None = None  # fewer than `golden --cases` for big cases


SPECS: dict[str, Spec] = {}


def spec(**kwargs) -> Spec:
    s = Spec(**kwargs)
    if s.name in SPECS:
        raise ValueError(f"two specs for {s.name}")
    SPECS[s.name] = s
    return s


# --- running -----------------------------------------------------------------

@dataclass
class Outcome:
    spec: Spec
    cases: int = 0
    passed: int = 0
    failures: list[str] = field(default_factory=list)
    guest_faults: int = 0


def _ulp_distance(a: int, b: int) -> int:
    """Distance between two f32 bit patterns in units in the last place."""
    def ordered(x: int) -> int:
        return x if x < 0x80000000 else 0x80000000 - x
    return abs(ordered(a) - ordered(b))


def _floats_match(a: int, b: int, ulps: int) -> bool:
    """Two f32 bit patterns are the same number (within `ulps`).

    Any NaN matches any NaN: the PowerPC makes 0x7FC00000 where an x86 makes
    0xFFC00000 for the same invalid operation, and no game logic reads a NaN's bits.
    """
    fa, fb = bits_f32(a), bits_f32(b)
    if math.isnan(fa) and math.isnan(fb):
        return True
    return a == b or _ulp_distance(a, b) <= ulps


def _tokens_match(expected: str, actual: str, s: Spec, arg: Arg | None) -> bool:
    """One reply token. `arg` is the argument a buffer token belongs to (None for the result)."""
    if expected == actual:
        return True
    if expected[:2] != actual[:2]:
        return False
    if expected.startswith("f:"):
        return _floats_match(int(expected[2:], 16), int(actual[2:], 16), s.float_ulps)
    if expected.startswith("d:"):
        e, a = (struct.unpack(">d", bytes.fromhex(t[2:]))[0] for t in (expected, actual))
        return math.isnan(e) and math.isnan(a)
    if arg is not None and arg.floats and expected.startswith("b:") and len(expected) == len(actual):
        e, a = bytes.fromhex(expected[2:]), bytes.fromhex(actual[2:])
        if len(e) % 4:
            return False
        return all(_floats_match(struct.unpack(">I", e[i:i + 4])[0], struct.unpack(">I", a[i:i + 4])[0],
                                 s.float_ulps) for i in range(0, len(e), 4))
    return False


_STARTUP_ADDRESSES: dict[tuple[str, str], int] = {}
# How many bytes of the campaign's function identify the same function in another program.
STARTUP_OPENING = 24


def startup_address(oracle: Oracle, name: str) -> int:
    """Where one of the game's startup functions is in the program the oracle runs.

    By name when the program's symbol map has one; otherwise the one place in
    its code that starts with the same instructions as the campaign's function
    of that name (Swap_Init has no name in the multiplayer program's map yet;
    its first instructions use no addresses, so they are the same in both).
    """
    program = next(img for img in oracle.images if img.name in image.PROGRAMS)
    key = (program.name, name)
    if key not in _STARTUP_ADDRESSES:
        if name in program.addresses:
            _STARTUP_ADDRESSES[key] = program.addresses[name]
        else:
            campaign = image.load("default")
            opening = campaign.read(campaign.resolve(name), STARTUP_OPENING)
            hits, at = [], program.data.find(opening)
            while at >= 0:
                if at % 4 == 0 and program.is_code(program.base + at):
                    hits.append(program.base + at)
                at = program.data.find(opening, at + 1)
            if len(hits) != 1:
                raise GuestFault(f"{name} has no name in {program.name}'s symbol map, and the campaign "
                                 f"function's first instructions are found in {len(hits)} places")
            _STARTUP_ADDRESSES[key] = hits[0]
    return _STARTUP_ADDRESSES[key]


def guest_answer(oracle: Oracle, s: Spec, address: int, values: list) -> list[str]:
    """The original function's answer as reply tokens: the result, then every mutable buffer."""
    oracle.reset()
    for name in s.startup:
        oracle.call(startup_address(oracle, name))
    gprs, fprs, pointers = [], [], []
    used_gprs = 0  # how many of gprs are real arguments, not the slots floats leave
    for arg, value in zip(s.args, values):
        placed = arg.place(oracle, value)
        if arg.reg == "fpr":
            # The Xbox 360's calling convention gives every argument a slot in
            # both register files: a float in f1 also uses up r4's place, so
            # an integer after it comes in r5 (MSGBB_ReadCoordDelta's pointer).
            fprs.append(placed)
            gprs.append(0)
        elif arg.reg == "gpr":
            gprs.append(placed)
            used_gprs = len(gprs)
        elif arg.reg == "gprs":  # a structure passed by value, in several registers
            gprs.extend(placed)
            used_gprs = len(gprs)
        pointers.append(placed)
    del gprs[used_gprs:]  # trailing float slots carry nothing
    if s.setup:
        # A setup that takes a third argument also gets where each argument
        # was placed (the guest addresses of buffers, for pointers to them).
        if len(inspect.signature(s.setup).parameters) >= 3:
            s.setup(oracle, values, pointers)
        else:
            s.setup(oracle, values)
    result = oracle.call(address, gprs, fprs)
    tokens = [s.ret.from_guest(oracle, result)]
    for arg, value, pointer in zip(s.args, values, pointers):
        if arg.mutable:
            tokens.append(arg.reply_token(oracle, pointer, value))
    return tokens


class RustChecker:
    """Talks to engine/target/release/decomp_check over its stdin and stdout."""

    def __init__(self, build: bool = True):
        if build:
            subprocess.run(["cargo", "build", "--release", "--quiet", "-p", "decomp_check"], cwd=ENGINE,
                           check=True)
        release = ENGINE / "target" / "release"
        exe = next((p for p in (release / "decomp_check.exe", release / "decomp_check") if p.is_file()), None)
        if exe is None:
            raise SystemExit("decomp_check isn't built: cargo build --release -p decomp_check (in engine/)")
        self.proc = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                     bufsize=1)

    def ask(self, line: str) -> list[str]:
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline().strip()
        if not reply:
            raise RuntimeError("decomp_check stopped answering")
        return reply.split(" ")

    def close(self) -> None:
        if self.proc.stdin:
            self.proc.stdin.close()
        self.proc.wait(timeout=10)


def check(s: Spec, oracle: Oracle, rust: RustChecker, address: int, rng: random.Random,
          max_cases: int, max_failures: int = 5) -> Outcome:
    out = Outcome(s)
    for values in s.cases(rng):
        if out.cases >= max_cases:
            break
        out.cases += 1
        try:
            expected = guest_answer(oracle, s, address, values)
        except GuestError as e:
            # The original stops with the game's error; the port must refuse the same input.
            expected = ["err", e.exit_name]
        except GuestFault as e:
            out.guest_faults += 1
            if len(out.failures) < max_failures:
                out.failures.append(f"original function couldn't run for {values!r}: {e}")
            continue
        request = " ".join([s.name] + [arg.token(v) for arg, v in zip(s.args, values)])
        reply = rust.ask(request)
        if expected[0] == "err":
            if reply[:2] == expected:
                out.passed += 1
            elif len(out.failures) < max_failures:
                out.failures.append(f"{request}\n      original: {' '.join(expected)}\n      rust:     {' '.join(reply)}")
            continue
        if reply[0] != "ok":
            if len(out.failures) < max_failures:
                out.failures.append(f"Rust: {' '.join(reply[1:])}  <- {request}")
            continue
        actual = reply[1:]
        owners = [None] + [arg for arg in s.args if arg.mutable]
        same = len(actual) == len(expected) and all(
            _tokens_match(e, a, s, owner) for e, a, owner in zip(expected, actual, owners))
        if same:
            out.passed += 1
        elif len(out.failures) < max_failures:
            out.failures.append(f"{request}\n      original: {' '.join(expected)}\n      rust:     {' '.join(actual)}")
    return out
