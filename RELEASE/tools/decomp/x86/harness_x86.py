"""Checks `iw_pc`'s Rust ports against the PC games' own x86 functions.

The same method and the same line protocol as the PowerPC decomp
(`tools/decomp/harness.py`): for each test case the checker
1. calls the original function in the x86 oracle, and
2. asks `pc_check` (engine/tools/pc_check, the Rust ports) the same question,
then compares the result and every buffer the function may have changed.

Line protocol (one request per line, tokens separated by spaces):
    request:  <image>.<function> <arg> <arg> ...      e.g. cod2mp.Q_stricmp s:41 s:61
    reply:    ok <result> <buffer> <buffer> ...      or      err <message>
Tokens (the same as harness.py, so one parser reads both): i:<decimal>;
f:<8 hex> f32 bits; d:<16 hex> f64 bits; s:<hex> a string's bytes (s:- for
NULL); v:<hex> read-only bytes (floats in them big-endian, as harness.py
sends them, although x86 memory is little-endian: the checker converts);
b:<hex> a buffer the function may change (it comes back in the reply, in
order); n: nothing. A game error is `err Com_Error <level>`, the level by
its number in the game's `errorParm_t` (0 fatal, 1 drop ...).

Arguments are listed in the C prototype's order. Each one says where the
original takes it (`at=`): "stack" (cdecl order) or a register ("eax",
"ecx", "edx", "ebx", "esi", "edi"): with whole-program optimization, MSVC
7.1 passed many arguments of the games' internal functions in registers.
"""
from __future__ import annotations

import math
import os
import random
import struct
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

from oracle_x86 import CallResult, GuestError, GuestFault, X86Oracle, x87_to_f32_bits, x87_to_f64_bits
from pe import ROOT

ENGINE = ROOT / "engine"


def f32_bits(v: float) -> int:
    return struct.unpack("<I", struct.pack("<f", v))[0]


def bits_f32(b: int) -> float:
    return struct.unpack("<f", struct.pack("<I", b & 0xFFFFFFFF))[0]


def f64_bits(v: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", v))[0]


def to_f32(v: float) -> float:
    try:
        return bits_f32(f32_bits(v))
    except OverflowError:
        return math.copysign(math.inf, v)


def _swap32(data: bytes) -> bytes:
    """Little-endian floats in guest memory <-> big-endian floats in tokens."""
    return b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))


# --- argument kinds ----------------------------------------------------------------

@dataclass
class Arg:
    """How one argument reaches the guest (where, and as what) and the Rust side (a token)."""
    at: str = "stack"
    mutable = False
    floats = False   # a buffer of f32s, compared as numbers

    def words(self, oracle: X86Oracle, value) -> list[int]:
        """The 32-bit words passed (one, or two for a double on the stack)."""
        raise NotImplementedError

    def token(self, value) -> str:
        raise NotImplementedError

    def read_back(self, oracle: X86Oracle, pointer: int, value) -> bytes:
        return b""


@dataclass
class Int(Arg):
    def words(self, oracle, value):
        return [value & 0xFFFFFFFF]

    def token(self, value):
        return f"i:{value}"


@dataclass
class F32(Arg):
    def words(self, oracle, value):
        return [f32_bits(to_f32(value))]

    def token(self, value):
        return f"f:{f32_bits(to_f32(value)):08x}"


@dataclass
class F64(Arg):
    def words(self, oracle, value):
        bits = f64_bits(value)
        return [bits & 0xFFFFFFFF, bits >> 32]

    def token(self, value):
        return f"d:{f64_bits(value):016x}"


@dataclass
class CStr(Arg):
    """A NUL-terminated string the function only reads (None = a NULL pointer)."""

    def words(self, oracle, value):
        return [0 if value is None else oracle.put_cstr(value)]

    def token(self, value):
        return "s:-" if value is None else f"s:{bytes(value).hex()}"


@dataclass
class Buffer(Arg):
    """Bytes the function may change; all `size` bytes are compared after the call.

    The value is the initial content: a string, which gets its terminator,
    then `fill` bytes up to `size` (so writes past the string show up).
    `terminated=False` for binary data. None = a NULL pointer.
    """
    size: int = 0
    fill: int = 0
    terminated: bool = True
    mutable = True

    def initial(self, value) -> bytes:
        data = bytes(value) + (b"\0" if self.terminated else b"")
        return data + bytes([self.fill]) * max(0, self.size - len(data))

    def words(self, oracle, value):
        return [0 if value is None else oracle.put(self.initial(value), 16)]

    def token(self, value):
        return "b:-" if value is None else f"b:{self.initial(value).hex()}"

    def read_back(self, oracle, pointer, value):
        return b"" if value is None else oracle.read(pointer, len(self.initial(value)))


@dataclass
class Floats(Arg):
    """`count` floats the function only reads (a vec3_t ...)."""
    count: int = 3

    def words(self, oracle, value):
        return [oracle.put(struct.pack(f"<{self.count}f", *value), 16)]

    def token(self, value):
        return "v:" + struct.pack(f">{self.count}f", *value).hex()


@dataclass
class FloatBuffer(Buffer):
    """`count` floats the function reads and changes in place."""
    count: int = 3
    floats = True

    def initial(self, value) -> bytes:   # as the token carries them: big-endian
        return struct.pack(f">{self.count}f", *value)

    def words(self, oracle, value):
        return [oracle.put(_swap32(self.initial(value)), 16)]

    def read_back(self, oracle, pointer, value):
        return _swap32(oracle.read(pointer, 4 * self.count))


@dataclass
class Msg(Arg):
    """A message buffer, the games' `msg_t`, which the function reads and changes.

    In the guest: {int overflowed; byte *data; int maxsize; int cursize;
    int readcount; int bit;} (24 bytes; the same in CoD2 1.3 and UO 1.51)
    and its `data` (maxsize bytes, plus `slack` bytes the original may run
    into: UO's readers don't stop at the end). The value is a tuple
    (overflowed, data, maxsize, cursize, readcount, bit); `data` is padded
    with zeros to maxsize + slack.

    Token (b:, big-endian like harness.py's): overflowed, maxsize, cursize,
    readcount, bit as 5 x i32, then the maxsize + slack data bytes. The data
    pointer itself is not part of it.
    """
    slack: int = 64
    mutable = True

    def _data(self, value) -> bytes:
        _, data, maxsize, _, _, _ = value
        size = maxsize + self.slack
        return bytes(data)[:size] + bytes(max(0, size - len(data)))

    def words(self, oracle, value):
        overflowed, _, maxsize, cursize, readcount, bit = value
        data_ptr = oracle.put(self._data(value), 16)
        return [oracle.put(struct.pack("<iIiiii", overflowed, data_ptr, maxsize, cursize, readcount, bit), 16)]

    def token(self, value):
        overflowed, _, maxsize, cursize, readcount, bit = value
        return "b:" + (struct.pack(">5i", overflowed, maxsize, cursize, readcount, bit) + self._data(value)).hex()

    def read_back(self, oracle, pointer, value):
        overflowed, data_ptr, maxsize, cursize, readcount, bit = struct.unpack("<iIiiii", oracle.read(pointer, 24))
        size = value[2] + self.slack
        return struct.pack(">5i", overflowed, maxsize, cursize, readcount, bit) + oracle.read(data_ptr, size)


@dataclass
class GlobalU32(Arg):
    """A 32-bit global the function reads and may change: written to `address`
    before the call, compared afterwards; the Rust side gets it as a 4-byte
    buffer (big-endian, like harness.py's)."""
    address: int = 0
    at: str = "none"
    mutable = True

    def words(self, oracle, value):
        oracle.write(self.address, struct.pack("<I", value & 0xFFFFFFFF))
        return [self.address]

    def token(self, value):
        return "b:" + struct.pack(">I", value & 0xFFFFFFFF).hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(self.address, 4)[::-1]


# --- results -----------------------------------------------------------------------

class Ret:
    on_x87 = False  # the result is in st(0)

    def from_guest(self, oracle: X86Oracle, result: CallResult) -> str:
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


class RetBool8(Ret):
    """A `bool`/`char` result: only `al` is set."""

    def from_guest(self, oracle, result):
        return f"i:{result.eax & 0xFF}"


class RetF32(Ret):
    """A float in st(0), as the caller's `fstp dword ptr` stores it."""
    on_x87 = True

    def from_guest(self, oracle, result):
        return f"f:{x87_to_f32_bits(result.st0):08x}"


class RetF64(Ret):
    on_x87 = True

    def from_guest(self, oracle, result):
        return f"d:{x87_to_f64_bits(result.st0):016x}"


class RetCStr(Ret):
    """A pointer to a string (s:- when NULL)."""

    def from_guest(self, oracle, result):
        return "s:-" if result.u32 == 0 else f"s:{oracle.read_cstr(result.u32).hex()}"


class RetStop(Ret):
    """For a function that decides which other function runs (Spec.stops):
    the label of the end point reached, or "return" when it returned
    without reaching one, as a string token."""

    def from_guest(self, oracle, result):
        return "s:" + (result.stopped or "return").encode().hex()


class RetPrinted(Ret):
    """For a function that prints a warning (Com_Printf) instead of failing:
    the format of the first message, without its 0x15 marker (p:-, nothing
    printed). The function's own result is ignored."""

    def from_guest(self, oracle, result):
        if not result.prints:
            return "p:-"
        return f"p:{result.prints[0].removeprefix(bytes([0x15])).hex()}"


# --- specs -------------------------------------------------------------------------

@dataclass
class Spec:
    name: str                       # the function's name in the image (names/<image>.tsv)
    image: str                      # cod2mp, uomp ...
    args: list[Arg]
    ret: Ret
    cases: Callable[[random.Random], Iterable[list]]
    rust: str = ""                  # the Rust function, for the status table
    callee_pops: int = 0            # bytes the function's `ret N` removes
    fpu: str = ""                   # the x87 precision control; "" = what the game runs with (GAME_FPU)
    float_ulps: int = 0
    setup: Callable[[X86Oracle, list], None] | None = None
    cod3: str = ""                  # cod3_game's port of the same function, if there is one (see --against)
    notes: str = ""
    prepare: tuple[str, ...] = ()   # the game's own functions (no arguments) run once before the cases,
                                    # their globals kept (X86Oracle.keep_state): ("MSG_initHuffman",)
    stops: dict[int, str] = field(default_factory=dict)  # end points (address: label) for a function that
                                    # decides what runs next: the call ends at the first one reached (RetStop)

    @property
    def request_name(self) -> str:
        return f"{self.image}.{self.name}"


SPECS: dict[str, Spec] = {}

# The x87 precision each game runs its frame with (dev/agents/cod2-uo-decomp/x86-oracle.md):
# CoD2MP_s.exe's Com_Frame sets 24 bits every frame (client and dedicated server, same
# executable); United Offensive and Call of Duty keep the C runtime's 53-bit default.
GAME_FPU = {"cod2mp": "pc24", "cod2_gfx_mp": "pc24", "uomp": "pc53", "codmp": "pc53", "uo_game_mp": "pc53"}


def spec(**kwargs) -> Spec:
    s = Spec(**kwargs)
    if s.request_name in SPECS:
        raise ValueError(f"two specs for {s.request_name}")
    SPECS[s.request_name] = s
    return s


# --- running -----------------------------------------------------------------------

@dataclass
class Outcome:
    spec: Spec
    cases: int = 0
    passed: int = 0
    failures: list[str] = field(default_factory=list)
    guest_faults: int = 0
    inexact: set[str] = field(default_factory=set)
    errors: int = 0     # cases where the original stopped with the game's error


def _ulp_distance(a: int, b: int) -> int:
    def ordered(x: int) -> int:
        return x if x < 0x80000000 else 0x80000000 - x
    return abs(ordered(a) - ordered(b))


def _floats_match(a: int, b: int, ulps: int) -> bool:
    """Two f32 bit patterns are the same number (within `ulps`); any NaN matches any NaN."""
    fa, fb = bits_f32(a), bits_f32(b)
    if math.isnan(fa) and math.isnan(fb):
        return True
    return a == b or _ulp_distance(a, b) <= ulps


def _tokens_match(expected: str, actual: str, s: Spec, arg: Arg | None) -> bool:
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
        return len(e) % 4 == 0 and all(
            _floats_match(struct.unpack(">I", e[i:i + 4])[0], struct.unpack(">I", a[i:i + 4])[0], s.float_ulps)
            for i in range(0, len(e), 4))
    return False


def guest_call(oracle: X86Oracle, s: Spec, address: int, values: list) -> tuple[CallResult, list[int]]:
    """Places the arguments as the spec says and calls the original."""
    oracle.reset()
    registers: dict[str, int] = {}
    stack: list[int] = []
    pointers = []
    for arg, value in zip(s.args, values):
        words = arg.words(oracle, value)
        pointers.append(words[0])
        if arg.at == "stack":
            stack.extend(words)
        elif arg.at != "none":
            if len(words) != 1 or arg.at in registers:
                raise ValueError(f"{s.request_name}: argument in {arg.at} doesn't fit")
            registers[arg.at] = words[0]
    if s.setup:
        s.setup(oracle, values)
    result = oracle.call(address, registers, stack, s.callee_pops, s.fpu or GAME_FPU[s.image], stops=s.stops)
    if result.stopped is not None:
        return result, pointers
    expected_depth = 1 if s.ret.on_x87 else 0
    if result.fpu_depth != expected_depth:
        raise GuestFault(f"the x87 stack holds {result.fpu_depth} values after the call, "
                         f"the spec's result kind says {expected_depth}")
    if result.clobbered:
        raise GuestFault(f"the function changed {', '.join(result.clobbered)} (callee-saved): "
                         "a register argument is missing from the spec")
    return result, pointers


def guest_answer(oracle: X86Oracle, s: Spec, address: int, values: list) -> tuple[list[str], CallResult]:
    """The original function's answer as reply tokens: the result, then every mutable buffer."""
    result, pointers = guest_call(oracle, s, address, values)
    tokens = [s.ret.from_guest(oracle, result)]
    for arg, value, pointer in zip(s.args, values, pointers):
        if arg.mutable:
            tokens.append("b:-" if not pointer else f"b:{arg.read_back(oracle, pointer, value).hex()}")
    return tokens, result


def target_dir() -> Path:
    return Path(os.environ.get("CARGO_TARGET_DIR") or ENGINE / "target")


class RustChecker:
    """Talks to pc_check (engine/tools/pc_check) over its stdin and stdout."""

    def __init__(self, build: bool = True):
        if build:
            subprocess.run(["cargo", "build", "--release", "--quiet", "-p", "pc_check"], cwd=ENGINE, check=True)
        release = target_dir() / "release"
        exe = next((p for p in (release / "pc_check.exe", release / "pc_check") if p.is_file()), None)
        if exe is None:
            raise SystemExit(f"pc_check isn't built in {release}: cargo build --release -p pc_check (in engine/)")
        self._start(exe)

    def _start(self, exe: Path) -> None:
        self.proc = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                     bufsize=1)

    def ask(self, line: str) -> list[str]:
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        reply = self.proc.stdout.readline().strip()
        if not reply:
            raise RuntimeError("pc_check stopped answering")
        return reply.split(" ")

    def close(self) -> None:
        if self.proc.stdin:
            self.proc.stdin.close()
        self.proc.wait(timeout=10)


CPP = ENGINE / "cpp"


def cpp_build_dir() -> Path:
    return Path(os.environ.get("IW_CPP_BUILD_DIR") or CPP / "build")


class CppChecker(RustChecker):
    """Talks to pc_check_cpp (engine/cpp/pc_check), the C++ ports' answerer:
    the same protocol, built with CMake (Release) into IW_CPP_BUILD_DIR or
    engine/cpp/build."""

    def __init__(self, build: bool = True):
        out = cpp_build_dir()
        if build:
            if not (out / "CMakeCache.txt").is_file():
                generator = ["-G", "Ninja"] if shutil.which("ninja") else []
                subprocess.run(["cmake", "-S", str(CPP), "-B", str(out), "-DCMAKE_BUILD_TYPE=Release", *generator],
                               check=True, stdout=subprocess.DEVNULL)
            subprocess.run(["cmake", "--build", str(out), "--config", "Release", "--target", "pc_check_cpp"],
                           check=True, stdout=subprocess.DEVNULL)
        candidates = [out / "pc_check_cpp", out / "pc_check_cpp.exe", out / "Release" / "pc_check_cpp.exe"]
        exe = next((p for p in candidates if p.is_file()), None)
        if exe is None:
            raise SystemExit(f"pc_check_cpp isn't built in {out}: cmake -S engine/cpp -B {out} && cmake --build {out}")
        self._start(exe)


def _short(line: str, keep: int = 96) -> str:
    """A request or reply with long tokens shortened, for failure reports."""
    out = []
    for token in line.split(" "):
        if len(token) > keep:
            token = f"{token[:keep]}...({(len(token) - 2) // 2} bytes)"
        out.append(token)
    return " ".join(out)


def _difference(request: str, expected: list[str], actual: list[str]) -> str:
    return (f"{_short(request)}\n      original: {_short(' '.join(expected))}"
            f"\n      rust:     {_short(' '.join(actual))}")


def check(s: Spec, oracle: X86Oracle, rust: RustChecker, address: int, rng: random.Random,
          max_cases: int, max_failures: int = 5, request_prefix: str | None = None) -> Outcome:
    """Runs up to `max_cases` cases. `request_prefix` replaces the image in the
    request name (`cod3` asks cod3_game's port of the same function)."""
    out = Outcome(s)
    name = s.request_name if request_prefix is None else f"{request_prefix}.{s.name}"
    for values in s.cases(rng):
        if out.cases >= max_cases:
            break
        out.cases += 1
        try:
            expected, result = guest_answer(oracle, s, address, values)
            out.inexact |= result.inexact
        except GuestError as e:
            expected = ["err", e.exit_name, str(e.code)]
            out.errors += 1
        except GuestFault as e:
            out.guest_faults += 1
            if len(out.failures) < max_failures:
                out.failures.append(f"original function couldn't run for {_short(repr(values), 300)}: {e}")
            continue
        request = " ".join([name] + [arg.token(v) for arg, v in zip(s.args, values)])
        reply = rust.ask(request)
        if expected[0] == "err":
            if reply[:3] == expected:
                out.passed += 1
            elif len(out.failures) < max_failures:
                out.failures.append(_difference(request, expected, reply))
            continue
        if reply[0] != "ok":
            if len(out.failures) < max_failures:
                out.failures.append(f"Rust: {' '.join(reply[1:])}  <- {_short(request)}\n"
                                    f"      original: {_short(' '.join(expected))}")
            continue
        actual = reply[1:]
        owners = [None] + [arg for arg in s.args if arg.mutable]
        same = len(actual) == len(expected) and all(
            _tokens_match(e, a, s, owner) for e, a, owner in zip(expected, actual, owners))
        if same:
            out.passed += 1
        elif len(out.failures) < max_failures:
            out.failures.append(_difference(request, expected, actual))
    return out
