"""Checks for iw_pc::client::connect (both games): the `connect` console
command, CL_Connect_f. Its words are the console's (Cmd_Argc / Cmd_Argv's
globals); what it changes is in cls (the state; Call of Duty 2's copy of
net_qport, United Offensive's key catchers), clc (the server's address, the
connect time and count, the server's last message), cls.servername and,
in Call of Duty 2, the menus' block and its key-catcher globals. The rest of
the game (sounds, the local server, the console, name resolution, the CD
key check, the settings, the menus) are stand-ins, recorded in the call log;
the CD key check is the stand-in's answer, never the game's code."""
from __future__ import annotations

import struct
from dataclasses import dataclass

import gen
from harness_x86 import Arg, CallLog, Region, Ret, StandIn, spec
from specs.net_client import GlobalInt, Setting
from specs.net_netchan import GAMES

TEXT = gen.LETTERS + gen.DIGITS + b".:-_"

CONNECT = {
    "cod2mp": dict(
        argc=0x00B1A480, argv1=0x00B17A84, sv_running=(0x00C26108, 8), ui=(0x005CF444, 0xE0),
        cls=(0x00609FE0, 8), clc=(0x00609FF4, 0x11C), server_name=0x0068A410, keys=(0x0096B654, 8),
        net_qport=0x019A1BA0,
        stand_ins={0x0044F940: ("SND_StopSounds", ((0, "int"),)),
                   0x0045C550: ("SV_Frame", ((0, "int"),)),
                   0x0040CD10: ("CL_Disconnect", ()),
                   0x004073B0: ("Con_Close", ()),
                   0x00448D50: ("NET_StringToAdr", (("eax", "str"), ("ebx", "int"))),
                   0x004B3B20: ("CL_CDKeyValidate", ()),
                   0x00539A30: ("UI_CloseAll", ()),
                   0x00414BD0: ("SCR_UpdateScreen", ())}),
    "uomp": dict(
        argc=0x009677C0, argv1=0x00964DC4, sv_running=(0x04927ED4, 0x20), ui=None,
        cls=(0x04AD3D60, 8), clc=(0x04DC886C, 0x11C), server_name=0x04AD3D6C, keys=None, net_qport=None,
        stand_ins={0x00454DD0: ("S_StopAllSounds", ((0, "int"),)),
                   0x0043DC50: ("Cvar_Set2", ((0, "str"), (1, "str"), (2, "int"))),
                   0x00439DA0: ("Com_ShutdownServer", ((0, "str"),)),
                   0x00463220: ("SV_Frame", ((0, "int"),)),
                   0x004109F0: ("CL_Disconnect", ((0, "int"),)),
                   0x0040B490: ("Con_Close", ()),
                   0x0044E420: ("NET_StringToAdr", ((0, "str"), ("ebx", "int"))),
                   0x00416F40: ("CL_CDKeyValidate", ())}),
}

# What the stand-ins answer in the case being run (set by Answers.words).
_answers: dict = {}


@dataclass
class Argv1(Arg):
    """The console's second word: a string a global points at; s: token."""
    pointer: int = 0
    at: str = "none"

    def words(self, oracle, value):
        address = oracle.put_cstr(value)
        oracle.write(self.pointer, struct.pack("<I", address))
        return [address]

    def token(self, value):
        return f"s:{bytes(value).hex()}"


@dataclass
class PointedBlock(Arg):
    """A block a global points at (Call of Duty 2's menus' block), changed by
    the call: the value is its bytes (b: token, read back whole); a game
    without one gets b:- and ignores it."""
    pointer: tuple | None = None
    at: str = "none"
    mutable = True

    def words(self, oracle, value):
        if self.pointer is None:
            return [1]
        address = oracle.put(bytes(value), 16)
        oracle.write(self.pointer[0], struct.pack("<I", address))
        return [address]

    def token(self, value):
        return "b:" + bytes(value).hex()

    def read_back(self, oracle, pointer, value):
        if self.pointer is None:
            return bytes(value)
        return oracle.read(pointer, len(value))


@dataclass
class Answers(Arg):
    """What the stand-ins answer: (the address NET_StringToAdr resolves to,
    canonical 20 bytes, or None: a bad name; whether the CD key passes).
    v: token: a 0 byte, or 1 and the 20 bytes; then the key's 0 or 1."""
    at: str = "none"

    def words(self, oracle, value):
        address, key = value
        _answers.clear()
        _answers.update(address=address, key=key)
        return [0]

    def token(self, value):
        address, key = value
        head = b"\0" if address is None else b"\1" + address
        return "v:" + (head + bytes([key])).hex()


class RetPrints(Ret):
    """Everything the call printed (Com_Printf), as formats: p: and their
    bytes with a 0 byte between them; p:- for nothing."""

    def from_guest(self, oracle, result):
        if not result.prints:
            return "p:-"
        return "p:" + b"\0".join(p.removeprefix(bytes([0x15])) for p in result.prints).hex()


def _resolve_effect(image: str):
    net = GAMES[image]

    def effect(oracle, name, out):
        address = _answers["address"]
        if address is not None:
            oracle.write(out & 0xFFFFFFFF, net.adr_to_guest(address))
        oracle.calls[-1] = (oracle.calls[-1][0], [name])
    return effect


def stand_ins(image: str) -> dict:
    out = {}
    for address, (label, args) in CONNECT[image]["stand_ins"].items():
        if label == "NET_StringToAdr":
            out[address] = StandIn(label, args, result=lambda *a: int(_answers["address"] is not None),
                                   effect=_resolve_effect(image))
        elif label == "CL_CDKeyValidate":
            out[address] = StandIn(label, args, result=lambda *a: _answers["key"])
        else:
            out[address] = StandIn(label, args)
    return out


def _setup(image: str):
    big_short = GAMES[image].big_short_pointer

    def setup(oracle, values):
        # BigShort (cdecl), as Swap_Init leaves the pointer: movzx eax, word [esp+4]; xchg al, ah; ret
        stub = oracle.put(bytes.fromhex("0FB7442404" "86C4" "C3"), 16)
        oracle.write(big_short, struct.pack("<I", stub))
    return setup


def _server_name(rng) -> bytes:
    k = rng.random()
    if k < 0.2:
        return b"localhost"
    if k < 0.25:
        return rng.choice([b"LOCALHOST", b"localhost:28960", b"localhos", b"localhostx", b""])
    if k < 0.3:
        return gen.text(rng, rng.randint(250, 300), TEXT, min_len=250)
    if k < 0.65:
        ip = ".".join(str(rng.randint(0, 255)) for _ in range(4)).encode()
        return ip + (b":" + str(rng.randint(0, 65535)).encode() if rng.random() < 0.5 else b"")
    return gen.text(rng, rng.randint(1, 40), TEXT, min_len=1)


def _address(rng) -> bytes:
    """A resolved netadr_t, canonical: mostly IPv4, sometimes loopback or a
    bot; the port often 0 (none given)."""
    kind = rng.choice([4, 4, 4, 4, 2, 0, 3, 1])
    ip = bytes(rng.getrandbits(8) for _ in range(4))
    port = 0 if rng.random() < 0.4 else rng.getrandbits(16)
    return struct.pack("<I", kind) + ip + struct.pack(">H", port) + bytes(rng.getrandbits(8) if rng.random() < 0.2 else 0
                                                                            for _ in range(10))


def _cases(image: str):
    t = CONNECT[image]
    net = GAMES[image]

    def cases(rng):
        while True:
            argc = 2 if rng.random() < 0.9 else rng.choice([0, 1, 3, 4])
            server = _server_name(rng)
            running = rng.choice([0, 1])
            ui = bytes(rng.getrandbits(8) for _ in range(t["ui"][1])) if t["ui"] else b""
            cls = bytes(rng.getrandbits(8) for _ in range(t["cls"][1]))
            clc = bytearray(rng.getrandbits(8) if rng.random() < 0.5 else 0 for _ in range(t["clc"][1]))
            clc[0:20] = net.adr_to_guest(_address(rng))
            name = bytearray(rng.getrandbits(8) if rng.random() < 0.3 else 0 for _ in range(256))
            keys = bytes(rng.getrandbits(8) for _ in range(t["keys"][1])) if t["keys"] else b""
            address = None if rng.random() < 0.15 else _address(rng)
            key = 1 if rng.random() < 0.8 else 0
            case = [argc, server, running, ui, cls, bytes(clc), bytes(name)]
            if t["keys"]:
                case += [keys, rng.choice([0, 0x1234, rng.getrandbits(16), rng.getrandbits(31)])]
            yield case + [(address, key), None]
    return cases


for image, t in CONNECT.items():
    args = [GlobalInt(address=t["argc"]), Argv1(pointer=t["argv1"]),
            Setting(pointer=t["sv_running"][0], offset=t["sv_running"][1]), PointedBlock(pointer=t["ui"]),
            Region(address=t["cls"][0], size=t["cls"][1]), Region(address=t["clc"][0], size=t["clc"][1]),
            Region(address=t["server_name"], size=256)]
    if t["keys"]:
        args += [Region(address=t["keys"][0], size=t["keys"][1]), GlobalInt(address=t["net_qport"])]
    args += [Answers(), CallLog()]
    spec(name="CL_Connect_f", image=image, rust="client::connect::connect_command", args=args, ret=RetPrints(),
         stand_ins=stand_ins(image), setup=_setup(image), cases=_cases(image))
