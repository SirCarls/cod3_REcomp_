"""Checks for iw_pc::net::oob (both games): packets without a connection.

The tokenizer is checked on its own. Its output is a text buffer (the words,
each followed by a zero) and an array of 512 pointers into it; the pointers
come back as offsets into the buffer (big-endian 32-bit words, entries the
function didn't write keep their fill), so the Rust side can answer with its
own buffer.
"""
from __future__ import annotations

import random
import struct
from dataclasses import dataclass, field

import gen
from harness_x86 import Arg, CStr, GlobalU32, Int, Msg, RetI32, RetVoid, spec

FILL = 0xAB          # what the text buffer holds before the call
ARGV_FILL = 0xEE     # what each argv entry holds before the call
MAX_TOKENS = 512
TEXT_SIZE = 0x2200   # BIG_INFO_STRING + MAX_STRING_TOKENS


@dataclass
class TokenText(Arg):
    """The tokenizer's text buffer: in the test heap (Call of Duty 2 takes it
    as an argument) or at a global (`address`, United Offensive). Compared
    whole after the call."""
    size: int = TEXT_SIZE
    address: int = 0
    mutable = True
    last: int = field(default=0, repr=False)

    def words(self, oracle, value):
        initial = bytes([FILL]) * self.size
        if self.address:
            oracle.write(self.address, initial)
            self.last = self.address
        else:
            self.last = oracle.put(initial, 16)
        return [self.last]

    def token(self, value):
        return "b:" + (bytes([FILL]) * self.size).hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(self.last, self.size)


@dataclass
class TokenArgv(Arg):
    """The 512 word pointers (in the heap, or at a global `address`), read
    back as offsets into `text`'s buffer."""
    text: TokenText = None
    address: int = 0
    mutable = True
    last: int = field(default=0, repr=False)

    def words(self, oracle, value):
        initial = bytes([ARGV_FILL]) * (4 * MAX_TOKENS)
        if self.address:
            oracle.write(self.address, initial)
            self.last = self.address
        else:
            self.last = oracle.put(initial, 16)
        return [self.last]

    def token(self, value):
        return "b:" + (bytes([ARGV_FILL]) * (4 * MAX_TOKENS)).hex()

    def read_back(self, oracle, pointer, value):
        out = bytearray()
        base = self.text.last
        for (word,) in struct.iter_unpack("<I", oracle.read(self.last, 4 * MAX_TOKENS)):
            if base <= word <= base + self.text.size:
                word -= base
            out += struct.pack(">I", word)
        return bytes(out)


# --- lines ----------------------------------------------------------------------------

WORD_CHARS = gen.LETTERS + gen.DIGITS + b"_-.:;,!?#$%&'()*+<=>@[]^`{|}~\\"
CONTROL = bytes(range(1, 0x20)) + b"\x7f"


def _piece(rng: random.Random) -> bytes:
    """One piece of a line: a word, a quoted string, a comment, a separator, an oddity."""
    r = rng.random()
    if r < 0.35:
        return gen.text(rng, 8, WORD_CHARS, min_len=1, high=0.05)
    if r < 0.5:
        inner = gen.text(rng, 10, WORD_CHARS + b" \t/*", high=0.05)
        if rng.random() < 0.3:
            k = rng.randint(0, len(inner))
            inner = inner[:k] + rng.choice([b'\\"', b"\\", b'\\\\"', b'"']) + inner[k:]
        return b'"' + inner + (b'"' if rng.random() < 0.85 else b"")
    if r < 0.6:
        return rng.choice([b" ", b"  ", b"\t", b" \t ", b"\r", b"\n"])
    if r < 0.66:
        return b"//" + gen.text(rng, 8, WORD_CHARS + b" ")
    if r < 0.74:
        body = gen.text(rng, 8, WORD_CHARS + b" */", high=0.05)
        return b"/*" + body + (b"*/" if rng.random() < 0.8 else b"")
    if r < 0.8:
        return rng.choice([b"/*/", b"*/", b"/", b"a/b", b"a//b", b"a/*b*/c", b'a"b"c', b'""', b'" "', b"\\"])
    if r < 0.88:
        return bytes(rng.choice(CONTROL) for _ in range(rng.randint(1, 3)))
    return bytes(rng.choice(gen.HIGH) for _ in range(rng.randint(1, 3)))


def line(rng: random.Random) -> bytes:
    """A line as a packet or the console might hold it (no zero bytes)."""
    r = rng.random()
    if r < 0.04:
        # Around the 512-word limit: one-letter words.
        n = rng.choice([510, 511, 512, 513, 514, 600])
        sep = rng.choice([b" ", b"\t", b"  "])
        out = sep.join(bytes([rng.choice(gen.LETTERS)]) for _ in range(n))
        return out + rng.choice([b"", b" ", b"x", b" //c"])
    if r < 0.08:
        # Long lines.
        out = b""
        while len(out) < rng.randint(900, 2500):
            out += _piece(rng)
        return out
    out = b""
    for _ in range(rng.randint(0, 12)):
        out += _piece(rng)
    return out.replace(b"\0", b"")


def _limit(rng: random.Random) -> int:
    r = rng.random()
    if r < 0.7:
        return 0
    if r < 0.95:
        return rng.randint(1, 6)
    return rng.choice([-1, 512, 513, rng.randint(7, 1000)])


def _tokenize_cases(null: bool):
    def cases(rng):
        while True:
            text = line(rng)
            if null and rng.random() < 0.02:
                text = None
            yield [text, _limit(rng), None, None] + ([0x12345678] if null else [])
    return cases


_cod2_text = TokenText(at="eax")
spec(name="Cmd_TokenizeStringInternal", image="cod2mp", rust="net::oob::Tokens::tokenize",
     args=[CStr(at="ecx"), Int(), _cod2_text, TokenArgv(at="stack", text=_cod2_text)],
     ret=RetI32(), cases=_tokenize_cases(False))

# United Offensive keeps argc, argv and the words in globals; the arguments are the text (eax)
# and the limit ([s1]).
_uo_text = TokenText(at="none", address=0x009655C0)
spec(name="Cmd_TokenizeStringInternal", image="uomp", rust="net::oob::Tokens::tokenize",
     args=[CStr(at="eax"), Int(), _uo_text, TokenArgv(at="none", text=_uo_text, address=0x00964DC0),
           GlobalU32(address=0x009677C0)],
     ret=RetVoid(), cases=_tokenize_cases(True))


# --- sending without a connection ---------------------------------------------------
# NET_OutOfBandPrint / NET_OutOfBandData: the packet the original sends (recorded by the
# oracle's sendto stand-in) against the Rust framing. The settings are the netchan checks'
# stand-ins (specs/net_netchan.py).

from specs.net_netchan import (NA_IP, COD2, UO, Address, BinaryBuffer, Data, SentPackets,  # noqa: E402
                                fake_settings, random_address)
from harness_x86 import Ret, RetBool8, RetStop  # noqa: E402


def oob_text(rng: random.Random, limit: int, over: bool) -> bytes:
    """What servers and clients print: commands and info strings, sometimes huge
    (up to `limit` characters; one more when `over`, which the original refuses)."""
    r = rng.random()
    if r < 0.5:
        out = rng.choice([b"infoResponse\n", b"statusResponse\n", b"challengeResponse ", b"print\n",
                          b"error\nEXE_SERVER_IS_DIFFERENT_VER\x151.3\n", b"getinfo xxx", b"getchallenge",
                          b"connectResponse", b"disconnect", b""])
        return out + line(rng).replace(b"%", b"")
    if r < 0.9:
        return gen.text(rng, 300, WORD_CHARS + b" \n\\\"", high=0.03).replace(b"%", b"")
    n = rng.choice([limit - 1, limit, rng.randint(1000, limit)] + ([limit + 1, limit + 10] if over else []))
    return (b"\\key\\value" * (n // 10 + 1))[:n]


def _print_cases(limit: int, uo: bool):
    def cases(rng):
        while True:
            # United Offensive's vsprintf overruns its buffer past the limit (the game
            # crashes): only Call of Duty 2's refusal is checked beyond it.
            text = oob_text(rng, limit, over=not uo)
            sock = rng.choice([0, 1])
            if uo:
                # The original formats: either "%s" and the text, or the text itself as the format.
                if rng.random() < 0.5:
                    yield [sock, random_address(rng), b"%s", text, text, None]
                else:
                    yield [sock, random_address(rng), text, b"unused", text, None]
            else:
                yield [sock, random_address(rng), text, None]
    return cases


def _oob_setup(net):
    def setup(oracle, values):
        fake_settings(oracle, net)
    return setup


spec(name="NET_OutOfBandPrint", image="cod2mp", rust="net::oob::out_of_band_print",
     args=[Int(), Address(net=COD2), CStr(at="eax"), SentPackets()], ret=RetBool8(),
     cases=_print_cases(0x1FFFB, False), setup=_oob_setup(COD2))
spec(name="NET_OutOfBandPrint", image="uomp", rust="net::oob::out_of_band_print",
     args=[Int(at="ecx"), Address(net=UO), CStr(), CStr(), CStr(at="none"), SentPackets()], ret=RetVoid(),
     cases=_print_cases(0x7FFB, True), setup=_oob_setup(UO))


def connect_text(rng: random.Random) -> bytes:
    """A `connect "<userinfo>"` line as clients send it, or other data."""
    keys = [b"name", b"rate", b"snaps", b"cl_punkbuster", b"protocol", b"challenge", b"qport", b"cl_voice",
            b"cl_anonymous", b"cg_predictItems", b"cl_hwid", b"protocol_cod2x"]
    info = b"".join(b"\\" + rng.choice(keys) + b"\\" + gen.text(rng, 20, gen.LETTERS + gen.DIGITS + b" ._",
                                                               high=0.05)
                    for _ in range(rng.randint(0, 25)))
    if rng.random() < 0.8:
        return b'connect "' + info + b'"'
    return bytes(rng.getrandbits(8) for _ in range(rng.choice([0, 1, 7, 8, 9, rng.randint(0, 2000)])))


@dataclass
class SentDecompressed(SentPackets):
    """United Offensive: what NET_OutOfBandData sent, decompressed again with the
    game's own adaptive Huff_Decompress(msg, 12) (0x00441D40), so that the Rust
    framing (before compression, which is helper pc-msg's) can be compared."""

    def read_back(self, oracle, pointer, value):
        sent = list(oracle.sent)
        out = bytearray()
        for to, data in sent:
            size = 0x10000
            buffer = oracle.put(data + bytes(size + 64 - len(data)), 16)
            msg = oracle.put(struct.pack("<iIiiii", 0, buffer, size, len(data), 0, 0), 16)
            oracle.call(0x00441D40, stack=[msg, 12], fpu="pc53")
            cursize = struct.unpack("<i", oracle.read(msg + 12, 4))[0]
            plain = oracle.read(buffer, cursize)
            out += struct.pack(">I", len(plain)) + to + plain
        return bytes(out)


def _data_cases(limit: int):
    def cases(rng):
        while True:
            data = connect_text(rng)
            if rng.random() < 0.01:
                data = bytes(rng.getrandbits(8) for _ in range(64)) * (limit // 64)
            yield [rng.choice([0, 1]), random_address(rng), data, len(data), None]
    return cases


spec(name="NET_OutOfBandData", image="cod2mp", rust="net::oob::out_of_band_data",
     args=[Int(), Address(net=COD2), Data(at="eax"), Int(at="ecx"), SentPackets()], ret=RetBool8(),
     cases=_data_cases(0x8000), setup=_oob_setup(COD2))
spec(name="NET_OutOfBandData", image="uomp", rust="net::oob::out_of_band_data",
     args=[Int(), Address(net=UO), Data(at="edx"), Int(), SentDecompressed()], ret=RetVoid(),
     cases=_data_cases(0x4000), setup=_oob_setup(UO))


# --- SV_ConnectionlessPacket: which handler runs ------------------------------------
# The original runs until it reaches one of the handlers (the end points below), which
# names its decision; "return" when it returns without one (an ignored packet).

DISPATCH_STOPS = {
    # CoD2: the branch after each command's compare (before SV_UpdateLastTimeMasterServer-
    # Communicated, which resolves the master server's name, and the handler).
    "cod2mp": {0x0045B4A5: "pb", 0x0045B71C: "status", 0x0045B773: "voice", 0x0045B7B5: "info",
               0x0045B80C: "challenge", 0x0045B863: "connect", 0x0045B8F4: "ipAuthorize", 0x0045B94B: "rcon"},
    # UO 0x00439D20 is Com_DPrintf (prints only with `developer` on; not named in names/, where
    # that name would make it a print exit for every check): "bad connectionless packet".
    "uomp": {0x0046242D: "pb", 0x00461190: "status", 0x004616F0: "info", 0x00458C50: "challenge",
             0x004BD500: "connect", 0x00459590: "ipAuthorize", 0x004620D0: "rcon", 0x00439D20: "unknown"},
}
UO_HUFF_COMPRESS = 0x00441FA0


@dataclass
class ConnectionlessMsg(Arg):
    """The packet as the server's msg_t holds it (cursize = the packet's length,
    `maxsize` 0x4000). United Offensive's `connect` packets are compressed first
    with the game's own adaptive Huff_Compress(msg, 12), as its clients send them;
    the Rust side gets the packet before compression (token v:<hex>)."""
    uo: bool = False

    def words(self, oracle, value):
        packet = bytes(value)
        size = 0x4000
        buffer = oracle.put(packet + bytes(size + 64 - len(packet)), 16)
        msg = oracle.put(struct.pack("<iIiiii", 0, buffer, size, len(packet), 0, 0), 16)
        if self.uo and packet[4:11] == b"connect":
            oracle.call(UO_HUFF_COMPRESS, stack=[msg, 12], fpu="pc53")
        return [msg]

    def token(self, value):
        return "v:" + bytes(value).hex()


def connectionless_packet(rng: random.Random, uo: bool) -> bytes:
    commands = [b"getstatus", b"getinfo", b"getchallenge", b"connect", b"ipAuthorize", b"rcon", b"v",
                b"disconnect", b"getservers", b"echo", b"print", b"infoResponse", b"PB_SV", b"pb_cl", b"pB_",
                b"pb", b"p", b"", b"GETINFO", b"GetChallenge", b"Connect", b"RCON", b"V", b"vv", b"info",
                b"getinfox", b"connect\"", b"\"getinfo\"", b"/*c*/getinfo", b"//getinfo"]
    r = rng.random()
    if r < 0.3:
        body = connect_text(rng)
    else:
        body = rng.choice(commands)
        if rng.random() < 0.7:
            body += rng.choice([b" ", b"\t", b"\n", b"\0", b"", b"  "]) + line(rng)[:200]
        if rng.random() < 0.1:
            body = rng.choice([b" ", b"\t\t", b"\n", b"\"", b"\x92", b"\xe9"]) + body
    return b"\xff\xff\xff\xff" + body


def _dispatch_setup(net):
    def setup(oracle, values):
        fake_settings(oracle, net)
        # sv_maxclients and the other settings SV_ConnectionlessPacket reads (zero: no
        # client slots to search for PunkBuster's packets, sv_packet_info off).
        for pointer in DISPATCH_SETTINGS[net.image]:
            block = oracle.put(bytes(0x60), 16)
            oracle.write(pointer, struct.pack("<I", block))
    return setup


DISPATCH_SETTINGS = {
    "cod2mp": (0x00D52810, 0x00D52858, 0x00C22F00),   # sv_maxclients, sv_packet_info, dedicated
    "uomp": (0x0491CCD0, 0x0491CD08),                  # sv_maxclients, sv_packet_info
}


def _dispatch_cases(uo: bool):
    def cases(rng):
        while True:
            yield [random_address(rng), connectionless_packet(rng, uo)]
    return cases


spec(name="SV_ConnectionlessPacket", image="cod2mp", rust="net::oob::server_request",
     args=[Address(net=COD2), ConnectionlessMsg()], ret=RetStop(), stops=DISPATCH_STOPS["cod2mp"],
     cases=_dispatch_cases(False), setup=_dispatch_setup(COD2))
spec(name="SV_ConnectionlessPacket", image="uomp", rust="net::oob::server_request",
     args=[Address(net=UO), ConnectionlessMsg(uo=True)], ret=RetStop(), stops=DISPATCH_STOPS["uomp"],
     cases=_dispatch_cases(True), setup=_dispatch_setup(UO))


# --- getinfo / getstatus: SVC_Info, SVC_Status --------------------------------------
# The originals read the server's settings (dvars in Call of Duty 2, cvars in United
# Offensive) through pointers, look some up by name in the settings hash table, walk the
# list of settings for the server info string, and read the client slots. `ServerWorld`
# builds all of that in the guest from one random description (a dict), and hands the same
# description to the Rust side (token: for each item, a 1-byte name length, the name, a
# 4-byte big-endian value length, the value). The answer is the packet the original sends.

CLIENTS_AREA = 0x50000000          # an extra mapped region for the client slots (64 x ~0.7 MB)
CLIENTS_AREA_SIZE = 0x03000000

DVAR_TYPE = {"bool": 0, "int": 5, "enum": 6, "string": 7}


@dataclass(frozen=True)
class SettingsLayout:
    image: str
    hash_function: int         # generateHashValue (name in eax), the settings hash table's index
    hash_table: int
    hash_next: int             # offset of the next setting in a hash chain
    list_head: int             # the list of every setting (for the server info string)
    list_next: int
    flags_at: int
    client_stride: int
    clients_pointer: int       # svs.clients
    argc: int
    argv: int


COD2_SETTINGS = SettingsLayout("cod2mp", 0x00435A70, 0x00C5C5D0, 0x20, 0x00C5C5C4, 0x1C, 4, 0xB1064,
                               0x00D3570C, 0x00B1A480, 0x00B17A80)
UO_SETTINGS = SettingsLayout("uomp", 0x0043D860, 0x009987A0, 0x28, 0x04927E98, 0x24, 0x10, 0xAB0B4,
                             0x04907BD0, 0x009677C0, 0x00964DC0)

# Settings read through a pointer: name -> (pointer, kind).
DIRECT_SETTINGS = {
    "cod2mp": {"sv_privateClients": (0x00D5280C, "int"), "sv_maxclients": (0x00D52810, "int"),
               "sv_hostname": (0x00D52814, "string"), "mapname": (0x00D52844, "string"),
               "g_gametype": (0x00D5283C, "string"), "sv_pure": (0x00D356B8, "bool"),
               "sv_minPing": (0x00D5286C, "int"), "sv_maxPing": (0x00CD610C, "int"),
               "sv_allowAnonymous": (0x00D5281C, "bool"), "sv_disableClientConsole": (0x00D52864, "bool"),
               "dedicated": (0x00C22F00, "int"), "sv_voice": (0x00D52868, "bool"),
               "sv_punkbuster": (0x00D528A0, "int")},
    "uomp": {"sv_privateClients": (0x0491CCCC, "int"), "sv_maxclients": (0x0491CCD0, "int"),
             "sv_hostname": (0x0491CCD4, "string"), "mapname": (0x0491CCF8, "string"),
             "g_gametype": (0x0491CCF4, "string"), "sv_pure": (0x04907BAC, "int"),
             "sv_minPing": (0x0491CD18, "int"), "sv_maxPing": (0x048A5670, "int"),
             "sv_allowAnonymous": (0x0491CCDC, "int"), "sv_disableClientConsole": (0x0491CD10, "int"),
             "dedicated": (0x049290A4, "int"), "sv_punkbuster": (0x0491CD48, "int"),
             "fs_basegame": (0x0492D0C0, "string")},
}
COD2_SERVER_IWDS = 0x019A1B74      # fs_numServerIwds
COD2_GAME_RUNNING = 0x00CD6104     # non-zero: SVC_Status reads scores from the game's clients
COD2_GAME_CLIENTS = 0x0193A780     # level.clients: score at +0x26B8, 0x28A4 bytes each
UO_GAME_VM = 0x0389FDBC            # gvm: SVC_Status asks it for each score (VM_Call(gvm, 22, slot))
OFFICIAL_CHECK = {"cod2mp": 0x0043B9A0, "uomp": 0x004403A0}   # FS_iwIwd and UO's equivalent
CLIENT_FIELDS = {"cod2mp": {"ping": 0x6E6B4, "name": 0x20C48}, "uomp": {"ping": 0x9B030, "name": 0x10A44}}


def _atoi(text: bytes) -> int:
    i, n = 0, len(text)
    while i < n and text[i] in b" \t\n\v\f\r":
        i += 1
    sign = text[i:i + 1]
    if sign in (b"-", b"+"):
        i += 1
    total = 0
    while i < n and 0x30 <= text[i] <= 0x39:
        total = (total * 10 + text[i] - 0x30) & 0xFFFFFFFF
        i += 1
    if sign == b"-":
        total = -total & 0xFFFFFFFF
    return total - (1 << 32) if total & 0x80000000 else total


def _atof(text: bytes) -> float:
    try:
        return float(text.decode("latin-1").strip() or "0")
    except ValueError:
        return 0.0


class ServerWorld:
    """Builds a server's settings and client slots in the guest."""

    def __init__(self, oracle, layout: SettingsLayout):
        self.o, self.lay = oracle, layout
        self.cod2 = layout.image == "cod2mp"
        self.listed = []

    def text(self, value: bytes) -> int:
        return self.o.put_cstr(bytes(value))

    def setting(self, name: str, kind: str, value) -> int:
        """A dvar/cvar in the heap. `value`: bool/int number, bytes, or (index, [names]) for an enum."""
        block = bytearray(0x60)
        struct.pack_into("<I", block, 0, self.text(name.encode()))
        if self.cod2:
            struct.pack_into("<B", block, 6, DVAR_TYPE[kind])
            if kind == "bool":
                struct.pack_into("<B", block, 8, value & 0xFF)
            elif kind == "int":
                struct.pack_into("<i", block, 8, value)
            elif kind == "string":
                struct.pack_into("<I", block, 8, self.text(value))
            else:
                index, names = value
                table = self.o.put(b"".join(struct.pack("<I", self.text(n)) for n in names) + bytes(4), 4)
                struct.pack_into("<iiI", block, 0x10, 0, len(names), table)
                struct.pack_into("<i", block, 8, index)
        else:
            text = value if isinstance(value, bytes) else str(value).encode()
            struct.pack_into("<I", block, 4, self.text(text))
            struct.pack_into("<f", block, 0x1C, _atof(text))
            struct.pack_into("<i", block, 0x20, _atoi(text))
        return self.o.put(bytes(block), 16)

    def by_name(self, name: str, pointer: int) -> None:
        name_at = self.text(name.encode())
        index = self.o.call(self.lay.hash_function, {"eax": name_at}).u32
        slot = self.lay.hash_table + 4 * index
        self.o.write(pointer + self.lay.hash_next, self.o.read(slot, 4))
        self.o.write(slot, struct.pack("<I", pointer))

    def server_info_list(self, pointers: list[int]) -> None:
        """The settings list Dvar_InfoString/Cvar_InfoString walks, in this order (all marked server info)."""
        head = 0
        for pointer in reversed(pointers):
            self.o.write(pointer + self.lay.list_next, struct.pack("<I", head))
            self.o.write(pointer + self.lay.flags_at, struct.pack("<H" if self.cod2 else "<I", 4))
            head = pointer
        self.o.write(self.lay.list_head, struct.pack("<I", head))

    def clients(self, slots: list[dict]) -> None:
        if CLIENTS_AREA not in getattr(self.o, "_extra_regions", set()):
            self.o.uc.mem_map(CLIENTS_AREA, CLIENTS_AREA_SIZE)
            self.o._extra_regions = getattr(self.o, "_extra_regions", set()) | {CLIENTS_AREA}
        fields = CLIENT_FIELDS[self.lay.image]
        for i, slot in enumerate(slots):
            base = CLIENTS_AREA + i * self.lay.client_stride
            self.o.write(base, struct.pack("<i", slot["state"]))
            self.o.write(base + fields["ping"], struct.pack("<i", slot["ping"]))
            self.o.write(base + fields["name"], slot["name"][:31] + b"\0")
        self.o.write(self.lay.clients_pointer, struct.pack("<I", CLIENTS_AREA))

    def args(self, words: list[bytes]) -> None:
        """Cmd_Argc / Cmd_Argv as the request left them."""
        self.o.write(self.lay.argc, struct.pack("<i", len(words)))
        for i, word in enumerate(words):
            self.o.write(self.lay.argv + 4 * i, struct.pack("<I", self.text(word)))

    def official(self, name: bytes, basegame: bytes) -> bool:
        main = self.text(b"main")
        registers = {"edi": self.text(name), "ebx": main}
        if self.cod2:
            return self.o.call(OFFICIAL_CHECK["cod2mp"], registers, fpu="pc24").u32 != 0
        return self.o.call(OFFICIAL_CHECK["uomp"], registers, [self.text(basegame)], fpu="pc53").u32 != 0


def _tlv(items: list[tuple[str, bytes]]) -> bytes:
    out = bytearray()
    for name, value in items:
        out += bytes([len(name)]) + name.encode() + struct.pack(">I", len(value)) + value
    return bytes(out)


SAFE = gen.LETTERS + gen.DIGITS + b" _-.:"


def _text_value(rng: random.Random, empty: float = 0.2) -> bytes:
    if rng.random() < empty:
        return b""
    r = rng.random()
    if r < 0.85:
        return gen.text(rng, 20, SAFE, min_len=1, high=0.02)
    if r < 0.95:
        return gen.text(rng, 20, SAFE + b"\\;\"%", min_len=1)   # refused or cleaned by Info_SetValueForKey
    return gen.text(rng, 400, SAFE, min_len=200)                 # long: the info string fills up


def _number_setting(rng: random.Random, cod2: bool):
    """(kind, value) for a by-name number setting, or None for a missing one."""
    r = rng.random()
    if r < 0.2:
        return None
    if r < 0.55 and cod2:
        return ("int", rng.choice([0, 1, 2, -1, rng.randint(-5, 100)]))
    return ("string", rng.choice([b"0", b"1", b"2", b"", b" 3", b"-1", b"1x", b"x1", b"007", b"99999999999",
                                  b"+4", b"\t5"]))


def _setting_token(setting) -> bytes:
    if setting is None:
        return b"-"
    kind, value = setting
    return b"n:%d" % value if kind == "int" else b"t:" + value


IWD_NAMES = [b"main/iw_00", b"main/iw_07", b"main/iw_15", b"main/iw_24", b"main/iw_25", b"main/iw_7",
             b"main/localized_english_iw00", b"main/localized_german_iw03", b"mods/iw_00", b"main/zz_mod",
             b"main/mp_pak0", b"main/pak0", b"main/pakuo01", b"uo/pak0", b"uo/pakuo00", b"uo/pakuo03",
             b"uo/mp_pak0", b"uo/mp_bin", b"main/mp_bin", b"main/IW_01", b"uo/localized_english_pak0",
             b"uo/zz_mod", b"uo/pakuo0a"]


def _server_info(rng: random.Random) -> list[tuple[bytes, bytes]]:
    """The server info settings: distinct names (whatever their case), short enough that
    the info string is simply their pairs in order."""
    pairs, seen, length = [], set(), 0
    for _ in range(rng.choice([0, 1, 3, 8, 30])):
        key = gen.text(rng, 10, gen.LETTERS + b"_", min_len=1)
        value = gen.text(rng, 20, SAFE, min_len=1)
        if key.lower() in seen or key.lower() == b"sv_keywords" or length + len(key) + len(value) + 2 > 900:
            continue
        seen.add(key.lower())
        length += len(key) + len(value) + 2
        pairs.append((key, value))
    return pairs


def random_server(rng: random.Random, cod2: bool) -> dict:
    max_clients = rng.choice([0, 1, 8, 16, 20, rng.randint(1, 64)])
    private = rng.choice([0, 0, 2, max_clients, max_clients + 1, rng.randint(0, max_clients)])
    states = [rng.choice([0, 0, 1, 2, 3, 4]) for _ in range(max(max_clients, private) if rng.random() < 0.95
                                                         else 0)]
    names = [gen.text(rng, 31, SAFE + b"^\x92\xe9\"", high=0.02) for _ in states]
    server = {
        "argv": [b"getinfo"] + ([gen.text(rng, 16, SAFE + b"\\\"", min_len=0)] if rng.random() < 0.85 else []),
        "sv_hostname": _text_value(rng, 0.1), "mapname": _text_value(rng, 0.05), "g_gametype": _text_value(rng, 0.05),
        "sv_maxclients": max_clients, "sv_privateClients": private, "states": states,
        "pings": [rng.choice([0, 50, 999, rng.randint(0, 2000)]) for _ in states], "names": names,
        "scores": [rng.choice([0, 1, -3, rng.randint(-100, 500)]) for _ in states],
        "sv_pure": rng.choice([0, 0, 1, 1, 2, 256]) if not cod2 else rng.choice([0, 1, 256]),
        "server_iwds": rng.choice([0, 0, 0, 3]),
        "sv_minPing": rng.choice([0, 0, 50, -1]), "sv_maxPing": rng.choice([0, 0, 300]),
        "sv_allowAnonymous": rng.choice([0, 0, 1, 256]), "sv_disableClientConsole": rng.choice([0, 0, 1]),
        "dedicated": rng.choice([None, 0, 1, 2]), "sv_voice": rng.choice([0, 1]),
        "sv_punkbuster": rng.choice([0, 0, 1, -2]),
        "fs_game": rng.choice([None, ("string", b""), ("string", b"mods/pam"), ("string", _text_value(rng)),
                               ("enum", (rng.randint(0, 1), [b"", b"mods/x"])), ("enum", (0, []))]),
        "g_password": rng.choice([None, ("string", b""), ("string", b"secret"), ("enum", (1, [b"", b"pw"]))]),
        "scr_friendlyfire": _number_setting(rng, cod2), "scr_killcam": _number_setting(rng, cod2),
        "g_timeoutsallowed": _number_setting(rng, cod2), "scr_allow_jeeps": _number_setting(rng, cod2),
        "scr_allow_tanks": _number_setting(rng, cod2),
        "referenced": rng.choice([None, ("string", b""),
                                  ("string", b" ".join(rng.choice(IWD_NAMES) for _ in range(rng.randint(1, 6))))]),
        "fs_basegame": rng.choice([b"", b"uo", b"main"]),
        "fs_restrict": rng.choice([None, ("bool", 0), ("bool", 1)] if cod2 else
                                  [None, ("string", b"0"), ("string", b"1"), ("string", b"0.5")]),
        "game_running": rng.choice([0, 1]),
        "info": _server_info(rng),
    }
    if rng.random() < 0.3:
        server["info"].append((b"sv_keywords", rng.choice([b"ctf dm", b"x"])))
    if not cod2:
        # United Offensive's cvars are text: an enum becomes the text it would show.
        for name in ("fs_game", "g_password"):
            setting = server[name]
            if setting is not None and setting[0] == "enum":
                index, names = setting[1]
                server[name] = ("string", names[index] if names else b"")
    return server


@dataclass
class ServerState(Arg):
    """The server's settings and slots (built in the guest by ServerWorld); token: the TLV
    description the Rust answerer reads."""
    layout: SettingsLayout = None
    at: str = "none"
    status: bool = False

    def words(self, oracle, value):
        return [0]

    def build(self, oracle, server: dict) -> list[tuple[str, bytes]]:
        lay = self.layout
        cod2 = lay.image == "cod2mp"
        w = ServerWorld(oracle, lay)
        direct = DIRECT_SETTINGS[lay.image]
        for name, (pointer, kind) in direct.items():
            if name == "dedicated" and server["dedicated"] is None:
                oracle.write(pointer, bytes(4))
                continue
            value = server.get(name, 0)
            if name == "dedicated":
                value = server["dedicated"]
            if name == "fs_basegame":
                value = server["fs_basegame"]
            oracle.write(pointer, struct.pack("<I", w.setting(name, kind, value)))
        if cod2:
            oracle.write(COD2_SERVER_IWDS, struct.pack("<i", server["server_iwds"]))
        listed = []
        for key, value in server["info"]:
            listed.append(w.setting(key.decode(), "string", value))
        w.server_info_list(listed)
        named = {"fs_game": server["fs_game"], "g_password": server["g_password"],
                 "scr_friendlyfire": server["scr_friendlyfire"], "scr_killcam": server["scr_killcam"],
                 "fs_restrict": server["fs_restrict"],
                 ("sv_referencedIwdNames" if cod2 else "sv_referencedPakNames"): server["referenced"]}
        if not cod2:
            named.update({"g_timeoutsallowed": server["g_timeoutsallowed"],
                          "scr_allow_jeeps": server["scr_allow_jeeps"],
                          "scr_allow_tanks": server["scr_allow_tanks"]})
        for name, setting in named.items():
            if setting is not None:
                w.by_name(name, w.setting(name, *setting))
        if not cod2:
            w.by_name("fs_basegame", oracle.read_u32(DIRECT_SETTINGS["uomp"]["fs_basegame"][0]))
        slots = [{"state": s, "ping": p, "name": n} for s, p, n in
                 zip(server["states"], server["pings"], server["names"])]
        w.clients(slots)
        argv = server["argv"] if not self.status else [b"getstatus"] + server["argv"][1:]
        w.args(argv)
        # Scores, where SVC_Status gets them.
        scores = server["scores"] + [0] * 64
        if cod2:
            oracle.write(COD2_GAME_RUNNING, struct.pack("<I", server["game_running"]))
            table = oracle.alloc(64 * 0x28A4, 16)
            for i, score in enumerate(scores[:64]):
                oracle.write(table + i * 0x28A4 + 0x26B8, struct.pack("<i", score))
            oracle.write(COD2_GAME_CLIENTS, struct.pack("<I", table))
        elif server["game_running"]:
            table = oracle.put(b"".join(struct.pack("<i", s) for s in scores[:64]), 16)
            # VM_Call(gvm, 22, slot) calls [gvm + 0x8C](22, slot, ...): mov eax,[esp+8]; mov eax,[eax*4+table]; ret
            stub = oracle.put(bytes.fromhex("8B442408" "8B0485") + struct.pack("<I", table) + b"\xC3", 16)
            vm = oracle.alloc(0x100, 16)
            oracle.write(vm + 0x8C, struct.pack("<I", stub))
            oracle.write(UO_GAME_VM, struct.pack("<I", vm))
        else:
            oracle.write(UO_GAME_VM, bytes(4))
        # Which referenced archives the game counts as its own (asked of the game itself).
        official = b""
        if server["referenced"] is not None and server["referenced"][1]:
            names = server["referenced"][1].split(b" ")
            official = b",".join(b"1" if w.official(n, server["fs_basegame"]) else b"0" for n in names)
        return self.items(server, official, argv)

    def items(self, server: dict, official: bytes, argv: list[bytes]) -> list[tuple[str, bytes]]:
        def text_of(setting):
            if setting is None:
                return b""
            kind, value = setting
            if kind == "enum":
                index, names = value
                return names[index] if names else b""
            return value
        cod2 = self.layout.image == "cod2mp"
        restrict = server["fs_restrict"]
        if restrict is None:
            restricted = 0
        elif cod2:
            restricted = 1 if restrict[1] else 0
        else:
            restricted = 1 if _atof(restrict[1]) != 0.0 else 0
        info = b"".join(b"\\" + k + b"\\" + v for k, v in server["info"])
        slots = max(server["sv_maxclients"], 0)
        players = [(sc, p, n[:31]) for st, sc, p, n in
                   zip(server["states"][:slots], server["scores"], server["pings"], server["names"]) if st >= 2]
        if not server["game_running"]:
            players = [(0, p, n) for _, p, n in players]
        return [
            ("challenge", argv[1] if len(argv) > 1 else b""),
            ("hostname", server["sv_hostname"]), ("mapname", server["mapname"]), ("gametype", server["g_gametype"]),
            ("max_clients", b"%d" % server["sv_maxclients"]), ("private_clients", b"%d" % server["sv_privateClients"]),
            ("states", b",".join(b"%d" % s for s in server["states"])),
            ("pure", b"%d" % server["sv_pure"]), ("server_iwds", b"%d" % server["server_iwds"]),
            ("min_ping", b"%d" % server["sv_minPing"]), ("max_ping", b"%d" % server["sv_maxPing"]),
            ("fs_game", text_of(server["fs_game"])), ("allow_anonymous", b"%d" % server["sv_allowAnonymous"]),
            ("disable_console", b"%d" % server["sv_disableClientConsole"]),
            ("password", text_of(server["g_password"])),
            ("ff", _setting_token(server["scr_friendlyfire"])), ("kc", _setting_token(server["scr_killcam"])),
            ("timeouts", _setting_token(server["g_timeoutsallowed"])),
            ("jeeps", _setting_token(server["scr_allow_jeeps"])), ("tanks", _setting_token(server["scr_allow_tanks"])),
            ("dedicated", b"1" if server["dedicated"] else b"0"), ("voice", b"%d" % server["sv_voice"]),
            ("pb", b"%d" % server["sv_punkbuster"]),
            ("referenced", text_of(server["referenced"])), ("official", official),
            ("server_info", info), ("restricted", b"%d" % restricted),
            ("players", b"\n".join(b"%d %d %s" % (sc, p, n.hex().encode()) for sc, p, n in players)),
        ]

    def token(self, value):
        return "v:" + _tlv(value["items"]).hex()


def _respond_cases(cod2: bool, status: bool, layout: SettingsLayout):
    def cases(rng):
        while True:
            server = random_server(rng, cod2)
            yield [random_address(rng), {"server": server, "items": None}, None]
    return cases


def _respond_setup(net, layout: SettingsLayout, status: bool):
    state = ServerState(layout=layout, status=status)

    def setup(oracle, values):
        fake_settings(oracle, net)
        world = values[1]
        world["items"] = state.build(oracle, world["server"])
    return setup


for _net, _layout, _cod2 in ((COD2, COD2_SETTINGS, True), (UO, UO_SETTINGS, False)):
    for _status, _name in ((False, "SVC_Info"), (True, "SVC_Status")):
        spec(name=_name, image=_net.image, rust=f"net::oob::{'status' if _status else 'info'}_response",
             args=[Address(net=_net), ServerState(layout=_layout, status=_status), SentPackets()], ret=RetVoid(),
             cases=_respond_cases(_cod2, _status, _layout), setup=_respond_setup(_net, _layout, _status))


# --- atol (the C runtime's, used for settings read as numbers) ----------------------

def _atol_cases(rng):
    pieces = [b" ", b"\t", b"\n", b"\v", b"\f", b"\r", b"-", b"+", b"0", b"7", b"123", b"2147483647", b"2147483648",
              b"99999999999", b"x", b".5", b"\xe9", b"\x85", b"\xa0", b"--1", b"+-2", b" - 3"]
    while True:
        yield [b"".join(rng.choice(pieces) for _ in range(rng.randint(0, 5)))]


for _image in ("cod2mp", "uomp"):
    spec(name="atol", image=_image, rust="net::oob::atol", args=[CStr()], ret=RetI32(), cases=_atol_cases)


# --- SV_GetChallenge: the challenge table and the answer ----------------------------
# The table (1024 challenge_t) is compared whole: token, per slot, its bytes with the address
# in the canonical layout. The answer: what was sent (challengeResponse) and whether the
# original went on to ask the authorize server (end point "authorize").

@dataclass(frozen=True)
class ChallengeLayout:
    image: str
    table: int
    stride: int
    time: int                  # svs.time
    clock: int                 # the clock the challenge number is mixed with
    lan_authorize: int         # pointer to net_lanauthorize
    lan_authorize_offset: int
    authorize_address: int     # svs.authorizeAddress (resolved in the setup)
    master_address: int        # the master server's address, as SV_MasterAddress caches it
    is_lan: int                # Sys_IsLANAddress (netadr on the stack)
    authorize_request: int     # SV_AuthorizeRequest (the end point)
    last_master_contact: int = 0
    only_visible: int = 0      # pointer to sv_onlyVisibleClients (UO)


COD2_CHALLENGES = ChallengeLayout("cod2mp", 0x00D3575C, 0x74, 0x00D35704, 0x00D35704, 0x00C8FB14, 8, 0x00D52770,
                                  0x019A6FE8, 0x00467100, 0x00452C80, last_master_contact=0x00D52784)
UO_CHALLENGES = ChallengeLayout("uomp", 0x04907C20, 0x54, 0x04907BC8, 0x04907BC4, 0x04927D34, 0x20, 0x0491CC34,
                                0x009CD930, 0x0046D2C0, 0x00459020, only_visible=0x0491CD24)
CRT_RAND_FIRST_TWO = (41, 18467)   # rand() twice from seed 1 (the oracle's thread data starts each call at 1)

ADDRESS_POOL = [
    struct.pack("<I", NA_IP) + bytes([192, 168, 0, 5]) + struct.pack(">H", 28960) + bytes(10),
    struct.pack("<I", NA_IP) + bytes([192, 168, 0, 5]) + struct.pack(">H", 28961) + bytes(10),
    struct.pack("<I", NA_IP) + bytes([81, 2, 3, 4]) + struct.pack(">H", 28960) + bytes(10),
    struct.pack("<I", NA_IP) + bytes([10, 0, 0, 9]) + struct.pack(">H", 50000) + bytes(10),
    struct.pack("<I", NA_IP) + bytes([127, 0, 0, 1]) + struct.pack(">H", 28960) + bytes(10),
    struct.pack("<I", 2) + bytes(16),                                  # loopback
    struct.pack("<I", 0) + bytes([1, 2, 3, 4]) + bytes(12),            # bot
]


def _canonical_slot(guest: bytes, net) -> bytes:
    return net.adr_from_guest(guest[:20]) + guest[20:]


def _guest_slot(canonical: bytes, net) -> bytes:
    return net.adr_to_guest(canonical[:20]) + canonical[20:]


@dataclass
class ChallengeTableArg(Arg):
    """svs.challenges (a global): value = the canonical bytes of all 1024 slots."""
    layout: ChallengeLayout = None
    net: object = None
    at: str = "none"
    mutable = True

    def words(self, oracle, value):
        lay = self.layout
        guest = b"".join(_guest_slot(value[i * lay.stride:(i + 1) * lay.stride], self.net) for i in range(1024))
        oracle.write(lay.table, guest)
        return [lay.table]

    def token(self, value):
        return "b:" + bytes(value).hex()

    def read_back(self, oracle, pointer, value):
        lay = self.layout
        guest = oracle.read(lay.table, 1024 * lay.stride)
        return b"".join(_canonical_slot(guest[i * lay.stride:(i + 1) * lay.stride], self.net) for i in range(1024))


def random_table(rng: random.Random, lay: ChallengeLayout) -> bytes:
    out = bytearray()
    used = rng.choice([0, 1, 5, 40, 1024])
    base_time = rng.choice([0, 1000, rng.randint(0, 1 << 24)])
    for i in range(1024):
        slot = bytearray(lay.stride)
        if i < used or rng.random() < 0.02:
            address = rng.choice(ADDRESS_POOL + [random_address(rng)])
            slot[:20] = address
            struct.pack_into("<iiiiii", slot, 0x14, rng.getrandbits(31), base_time + rng.randint(-5000, 5000),
                             rng.randint(0, 1 << 20), base_time + rng.randint(-20000, 100),
                             rng.randint(0, 3), rng.choice([0, 0, 0, 1]))
            for k in range(0x2C, lay.stride):
                slot[k] = rng.getrandbits(8) if rng.random() < 0.3 else 0
        else:
            struct.pack_into("<i", slot, 0x18, rng.choice([0, 0, base_time - 1]))
        out += slot
    return bytes(out)


@dataclass
class ChallengeWorld(Arg):
    """The other state SV_GetChallenge reads; token: the inputs the Rust side needs (TLV)."""
    layout: ChallengeLayout = None
    net: object = None
    at: str = "none"

    def words(self, oracle, value):
        return [0]

    def token(self, value):
        return "v:" + _tlv(value["items"]).hex()


def _challenge_setup(net, lay: ChallengeLayout):
    cod2 = lay.image == "cod2mp"

    def setup(oracle, values):
        fake_settings(oracle, net)
        from_address, world = values[0], values[2]
        w = world["world"]
        oracle.write(lay.time, struct.pack("<i", w["time"]))
        oracle.write(lay.clock, struct.pack("<i", w["clock"]))
        block = bytearray(0x60)
        struct.pack_into("<i", block, lay.lan_authorize_offset, w["lan_authorize"])
        oracle.write(lay.lan_authorize, struct.pack("<I", oracle.put(bytes(block), 16)))
        oracle.write(lay.authorize_address, net.adr_to_guest(
            struct.pack("<I", NA_IP) + bytes([63, 146, 124, 10]) + struct.pack(">H", 20700) + bytes(10)))
        oracle.write(lay.master_address, net.adr_to_guest(w["master"]))
        if cod2:
            oracle.write(lay.last_master_contact, struct.pack("<i", w["last_master"]))
        else:
            block = bytearray(0x60)
            struct.pack_into("<i", block, 0x20, w["only_visible"])
            oracle.write(lay.only_visible, struct.pack("<I", oracle.put(bytes(block), 16)))
        ServerWorld(oracle, COD2_SETTINGS if cod2 else UO_SETTINGS).args(w["argv"])
        from_lan = oracle.call(lay.is_lan, stack=list(struct.unpack("<5I", net.adr_to_guest(from_address))),
                               fpu="pc53").u32 & 0xFF
        clock = w["clock"]
        first, second = CRT_RAND_FIRST_TWO
        world["items"] = [
            ("time", b"%d" % w["time"]), ("challenge", b"%d" % (((first << 16) ^ second) ^ clock)),
            ("lan_authorize", b"%d" % (w["lan_authorize"] & (0xFF if cod2 else 0xFFFFFFFF))),
            ("from_lan", b"%d" % (1 if from_lan else 0)), ("last_master", b"%d" % w["last_master"]),
            ("from_master", b"1" if _same_address(from_address, w["master"]) else b"0"),
            ("guid", w["argv"][2] if len(w["argv"]) == 3 else b"-"), ("guid_given", b"1" if len(w["argv"]) == 3 else b"0"),
            ("only_visible", b"%d" % w["only_visible"]),
        ]
    return setup


def _same_address(a: bytes, b: bytes) -> bool:
    """NET_CompareAdr on canonical addresses."""
    kind = struct.unpack_from("<I", a)[0]
    if kind != struct.unpack_from("<I", b)[0]:
        return False
    if kind == NA_IP:
        return a[4:10] == b[4:10]
    if kind == 5:
        return a[8:20] == b[8:20]
    return True


def _challenge_cases(lay: ChallengeLayout):
    def cases(rng):
        while True:
            table = random_table(rng, lay)
            time = rng.choice([0, 7001, 1_200_001, rng.randint(0, 1 << 24)])
            if rng.random() < 0.5:
                # Somebody already asking: time relative to their first request.
                slot = rng.randrange(1024)
                first = struct.unpack_from("<i", table, slot * lay.stride + 0x20)[0]
                time = first + rng.choice([0, 4999, 5000, 5001, 6999, 7000, 7001, 100000])
            from_address = rng.choice(ADDRESS_POOL + [random_address(rng)])
            world = {
                "time": time, "clock": rng.choice([time, rng.getrandbits(31)]),
                "lan_authorize": rng.choice([0, 0, 1, 256]),
                # The master server's cached address is an IPv4 one (type 0 would make
                # SV_MasterAddress resolve the name again).
                "master": rng.choice(([from_address] if from_address[:4] == struct.pack("<I", NA_IP) else [])
                                     + ADDRESS_POOL[:3]),
                "last_master": rng.choice([0, time - 1_200_001, time - 1_200_000, time]),
                "argv": rng.choice([[b"getchallenge"], [b"getchallenge", b"0"],
                                    [b"getchallenge", b"0", gen.text(rng, 40, gen.DIGITS + b"abcdef")],
                                    [b"getchallenge", b"0", b"x", b"y"]]),
                "only_visible": rng.choice([0, 0, 1, 32]),
            }
            if lay.clock == lay.time:
                world["clock"] = time   # Call of Duty 2 mixes the number with svs.time itself
            yield [from_address, table, {"world": world, "items": None}, None]
    return cases


for _net, _lay in ((COD2, COD2_CHALLENGES), (UO, UO_CHALLENGES)):
    spec(name="SV_GetChallenge", image=_net.image, rust="net::oob::get_challenge",
         args=[Address(net=_net), ChallengeTableArg(layout=_lay, net=_net), ChallengeWorld(layout=_lay, net=_net),
               SentPackets()],
         ret=RetStop(), stops={_lay.authorize_request: "authorize"},
         cases=_challenge_cases(_lay), setup=_challenge_setup(_net, _lay))


# --- SV_DirectConnect: the decision -------------------------------------------------
# The original runs up to the point where it takes a slot (end points "reconnect" and
# "new", with the slot number read from a register there) or returns after a refusal
# (the reply it sent is compared, and the challenge table: first ping, connected).

@dataclass(frozen=True)
class ConnectLayout:
    image: str
    stops: dict                # address -> label
    slot_register: dict        # label -> (register, "index" | "pointer")
    settings: dict             # name -> (pointer, kind)
    password: int              # sv_privatePassword (text)
    punkbuster_hook: int       # PunkBuster's connect check pointer (NULL: PunkBuster off)
    client_fields: dict        # address, qport, last_connect


COD2_CONNECT = ConnectLayout(
    "cod2mp", {0x0045430D: "reconnect", 0x004543EB: "new"}, {"reconnect": ("ebp", "pointer"), "new": ("eax", "index")},
    {"sv_maxclients": (0x00D52810, "int"), "sv_privateClients": (0x00D5280C, "int"),
     "sv_reconnectlimit": (0x00D52828, "int"), "sv_minPing": (0x00D5286C, "int"), "sv_maxPing": (0x00CD610C, "int")},
    0x00D52874, 0x005DD9CC, {"address": 0x6E6D4, "qport": 0x6E6E8, "last_connect": 0x20E24})
UO_CONNECT = ConnectLayout(
    "uomp", {0x0045A180: "reconnect", 0x0045A266: "new"}, {"reconnect": ("ebx", "pointer"), "new": ("ecx", "index")},
    {"sv_maxclients": (0x0491CCD0, "int"), "sv_privateClients": (0x0491CCCC, "int"),
     "sv_reconnectlimit": (0x0491CCE0, "int"), "sv_minPing": (0x0491CD18, "float"), "sv_maxPing": (0x048A5670, "float")},
    0x0491CD20, 0x005CF08C, {"address": 0x9B048, "qport": 0x9B05C, "last_connect": 0x10C20})


class RetConnect(RetStop):
    """The end point reached and, for a slot, its number: "new 3", "reconnect 0", "return"."""

    def __init__(self, layout: ConnectLayout, stride: int):
        self.layout, self.stride = layout, stride

    def from_guest(self, oracle, result):
        from oracle_x86 import REGISTERS
        label = result.stopped or "return"
        if result.stopped:
            register, kind = self.layout.slot_register[label]
            value = oracle.uc.reg_read(REGISTERS[register])
            slot = value if kind == "index" else (value - CLIENTS_AREA) // self.stride
            label = f"{label} {slot}"
        return "s:" + label.encode().hex()


def _userinfo(rng: random.Random, protocol: int, challenge: int, qport: int) -> bytes:
    pairs = [(b"name", gen.text(rng, 12, SAFE, min_len=1)), (b"rate", b"25000")]
    r = rng.random()
    pairs.append((b"protocol", b"%d" % protocol if r < 0.85 else rng.choice([b"6", b"118", b"22", b"120", b"", b" 118x"])))
    if rng.random() < 0.95:
        pairs.append((b"challenge", b"%d" % challenge))
    pairs.append((b"qport", b"%d" % qport))
    if rng.random() < 0.3:
        pairs.append((b"password", rng.choice([b"secret", b"Secret", b"", b"x"])))
    rng.shuffle(pairs)
    return b"".join(b"\\" + k + b"\\" + v for k, v in pairs)


def _connect_setup(net, lay: ConnectLayout, chal: ChallengeLayout, settings: SettingsLayout):
    cod2 = lay.image == "cod2mp"

    def setup(oracle, values):
        fake_settings(oracle, net)
        from_address, world = values[0], values[2]
        w = world["world"]
        sw = ServerWorld(oracle, settings)
        for name, (pointer, kind) in lay.settings.items():
            value = w[name]
            block = bytearray(0x60)
            if cod2:
                struct.pack_into("<i", block, 8, int(value))
            else:
                struct.pack_into("<I", block, 4, sw.text(str(value).encode()))
                struct.pack_into("<f", block, 0x1C, float(value))
                struct.pack_into("<i", block, 0x20, int(value))
            oracle.write(pointer, struct.pack("<I", oracle.put(bytes(block), 16)))
        block = bytearray(0x60)
        struct.pack_into("<I", block, 8 if cod2 else 4, sw.text(w["password"]))
        oracle.write(lay.password, struct.pack("<I", oracle.put(bytes(block), 16)))
        oracle.write(lay.punkbuster_hook, bytes(4))
        oracle.write(chal.time, struct.pack("<i", w["time"]))
        # Client slots.
        if CLIENTS_AREA not in getattr(oracle, "_extra_regions", set()):
            oracle.uc.mem_map(CLIENTS_AREA, CLIENTS_AREA_SIZE)
            oracle._extra_regions = getattr(oracle, "_extra_regions", set()) | {CLIENTS_AREA}
        f = lay.client_fields
        for i, cl in enumerate(w["clients"]):
            base = CLIENTS_AREA + i * settings.client_stride
            oracle.write(base, struct.pack("<i", cl["state"]))
            oracle.write(base + f["address"], net.adr_to_guest(cl["address"]))
            oracle.write(base + f["qport"], struct.pack("<i", cl["qport"]))
            oracle.write(base + f["last_connect"], struct.pack("<i", cl["last_connect"]))
        oracle.write(settings.clients_pointer, struct.pack("<I", CLIENTS_AREA))
        sw.args([b"connect"] + ([w["userinfo"]] if w["userinfo"] is not None else []))
        from_lan = oracle.call(chal.is_lan, stack=list(struct.unpack("<5I", net.adr_to_guest(from_address))),
                               fpu="pc53").u32 & 0xFF
        clients = b"".join(struct.pack(">iii", cl["state"], cl["qport"], cl["last_connect"]) + cl["address"]
                           for cl in w["clients"])
        world["items"] = [
            ("userinfo", w["userinfo"] or b""), ("time", b"%d" % w["time"]),
            ("reconnect_limit", b"%d" % w["sv_reconnectlimit"]), ("max_clients", b"%d" % w["sv_maxclients"]),
            ("private_clients", b"%d" % w["sv_privateClients"]), ("password", w["password"]),
            ("min_ping", repr(float(w["sv_minPing"])).encode()), ("max_ping", repr(float(w["sv_maxPing"])).encode()),
            ("from_lan", b"%d" % (1 if from_lan else 0)), ("clients", clients),
        ]
    return setup


def _connect_cases(net, lay: ConnectLayout, chal: ChallengeLayout):
    cod2 = lay.image == "cod2mp"
    protocol = 118 if cod2 else 22

    def cases(rng):
        while True:
            table = bytearray(random_table(rng, chal))
            from_address = rng.choice(ADDRESS_POOL + [random_address(rng)])
            time = rng.choice([1000, rng.randint(0, 1 << 24)])
            # The challenge the client sends: often one the table has for its address.
            challenge = rng.getrandbits(31)
            if rng.random() < 0.7:
                slot = rng.randrange(rng.choice([1, 5, 1024]))
                table[slot * chal.stride:slot * chal.stride + 20] = from_address
                challenge = struct.unpack_from("<i", table, slot * chal.stride + 0x14)[0]
                if rng.random() < 0.5:
                    struct.pack_into("<i", table, slot * chal.stride + 0x24, 0)          # first ping not yet known
                struct.pack_into("<i", table, slot * chal.stride + 0x1C, time - rng.choice([0, 30, 50, 300, 1000]))
            qport = rng.choice([rng.getrandbits(16), 1234])
            max_clients = rng.choice([0, 1, 4, 8, 20])
            clients = []
            for i in range(max_clients):
                address = rng.choice([from_address, from_address, rng.choice(ADDRESS_POOL)]) if rng.random() < 0.4 \
                    else random_address(rng)
                clients.append({"state": rng.choice([0, 0, 1, 2, 4]), "address": address,
                                "qport": rng.choice([qport, rng.getrandbits(16)]),
                                "last_connect": time - rng.choice([0, 1000, 4999, 5000, 5001, 100000])})
            world = {
                "time": time, "clients": clients, "sv_maxclients": max_clients,
                "sv_privateClients": rng.choice([0, 0, 1, 2, max_clients]),
                "sv_reconnectlimit": rng.choice([0, 3, 5]),
                "sv_minPing": rng.choice([0, 0, 50, 100] + ([0.5, 49.5] if not cod2 else [])),
                "sv_maxPing": rng.choice([0, 0, 200, 40] + ([299.5] if not cod2 else [])),
                "password": rng.choice([b"", b"secret"]),
                "userinfo": _userinfo(rng, protocol, challenge, qport) if rng.random() < 0.97 else None,
            }
            yield [from_address, bytes(table), {"world": world, "items": None}, None]
    return cases


for _net, _lay, _chal, _settings in ((COD2, COD2_CONNECT, COD2_CHALLENGES, COD2_SETTINGS),
                                     (UO, UO_CONNECT, UO_CHALLENGES, UO_SETTINGS)):
    spec(name="SV_DirectConnect", image=_net.image, rust="net::oob::direct_connect",
         args=[Address(net=_net), ChallengeTableArg(layout=_chal, net=_net), ChallengeWorld(layout=_chal, net=_net),
               SentPackets()],
         ret=RetConnect(_lay, _settings.client_stride), stops=_lay.stops,
         cases=_connect_cases(_net, _lay, _chal), setup=_connect_setup(_net, _lay, _chal, _settings))


# --- the client side: CL_SetServerInfo, CL_ServerInfoPacket's protocol test ---------

ENTRY_SIZE = {"cod2mp": 0x94, "uomp": 0x98}   # serverInfo_t (at least what CL_SetServerInfo writes)
INFO_KEYS = [b"clients", b"hostname", b"mapname", b"sv_maxclients", b"game", b"gametype", b"nettype", b"minPing",
             b"maxping", b"sv_allowAnonymous", b"con_disabled", b"pswrd", b"pure", b"ff", b"kc", b"hw", b"mod",
             b"voice", b"pb", b"timeoutsallowed", b"timeoutsAllowed", b"jps", b"tnk", b"protocol", b"challenge"]


def client_info(rng: random.Random) -> bytes:
    pairs = []
    for key in rng.sample(INFO_KEYS, rng.randint(0, len(INFO_KEYS))):
        r = rng.random()
        if r < 0.5:
            value = b"%d" % rng.choice([0, 1, 2, 5, 22, 118, 255, 256, 300, -1, 65535, 70000, rng.randint(-1 << 31, 1 << 31)])
        elif r < 0.6:
            value = rng.choice([b" 7", b"x", b"", b"12abc", b"-0", b"+3"])
        else:
            value = gen.text(rng, 40, SAFE)
        pairs.append((gen.case_variant(rng, key) if rng.random() < 0.2 else key, value))
    return b"".join(b"\\" + k + b"\\" + v for k, v in pairs)


def _set_server_info_cases(size: int):
    def cases(rng):
        while True:
            info = client_info(rng) if rng.random() < 0.95 else None
            yield [bytes(rng.getrandbits(8) for _ in range(size)), info, rng.choice([0, 50, 999, 65535, 40000])]
    return cases


for _image in ("cod2mp", "uomp"):
    spec(name="CL_SetServerInfo", image=_image, rust="net::oob::set_server_info",
         args=[BinaryBuffer(at="esi"), CStr(at="edi"), Int()], ret=RetVoid(),
         cases=_set_server_info_cases(ENTRY_SIZE[_image]))


CLIENT_ACCEPT = {"cod2mp": 0x004B3575, "uomp": 0x0041569F}


@dataclass
class ReadMsg(Msg):
    """A message the original only reads from (what its readers do to readcount is
    helper pc-msg's check): token v:, not compared."""
    mutable = False

    def token(self, value):
        return "v:" + super().token(value)[2:]


@dataclass
class DebugProtocol(Arg):
    """The `debug_protocol` setting (registered by name, or missing); token: its text,
    then its float value (as UO keeps it) after a zero byte."""
    layout: SettingsLayout = None
    at: str = "none"

    def words(self, oracle, value):
        return [0]

    def token(self, value):
        text = value if value is not None else b""
        return "v:" + (text + b"\0" + struct.pack(">f", _atof(text))).hex()


def _info_packet_setup(net, layout: SettingsLayout):
    cod2 = layout.image == "cod2mp"

    def setup(oracle, values):
        fake_settings(oracle, net)
        debug = values[2]
        if debug is not None:
            w = ServerWorld(oracle, layout)
            w.by_name("debug_protocol", w.setting("debug_protocol", "string", debug))
    return setup


def _info_packet_cases(cod2: bool):
    protocol = 118 if cod2 else 22

    def cases(rng):
        while True:
            info = client_info(rng)
            r = rng.random()
            if r < 0.6:
                info += b"\\protocol\\" + rng.choice([b"%d" % protocol, b"%d" % protocol, b"120", b"6", b"22", b"118",
                                                      b"", b" 118", b"118.5"])
            debug = rng.choice([None, None, b"", b"120", b"118", b"22", b"22.0", b"22.5", b" 22", b"x"])
            yield [random_address(rng), (0, info + b"\0", 0x4000, len(info) + 1, 0, 0), debug]
    return cases


for _net, _layout, _cod2 in ((COD2, COD2_SETTINGS, True), (UO, UO_SETTINGS, False)):
    spec(name="CL_ServerInfoPacket", image=_net.image, rust="net::oob::info_response_accepted",
         args=[Address(net=_net), ReadMsg(), DebugProtocol(layout=_layout)], ret=RetStop(),
         stops={CLIENT_ACCEPT[_net.image]: "accepted"},
         cases=_info_packet_cases(_cod2), setup=_info_packet_setup(_net, _layout))


# --- NET_CompareAdr, NET_CompareBaseAdr ---------------------------------------------
# Both return 0 for "the same" (else a difference); the port says same or not.

class RetSame(Ret):
    """1 when the original returned 0 ("the same"), else 0."""

    def from_guest(self, oracle, result):
        return f"i:{1 if result.u32 == 0 else 0}"


@dataclass
class AddressPointer(Arg):
    """A pointer to a netadr_t in the game's layout; token: the canonical 20 bytes."""
    net: object = None

    def words(self, oracle, value):
        return [oracle.put(self.net.adr_to_guest(value), 4)]

    def token(self, value):
        return "v:" + bytes(value).hex()


def _compare_cases(rng):
    while True:
        a = rng.choice(ADDRESS_POOL + [random_address(rng)])
        r = rng.random()
        if r < 0.3:
            b = a
        elif r < 0.6:
            b = bytearray(a)
            b[rng.choice([4, 7, 8, 9, 12, 19])] ^= 1 << rng.randrange(8)   # one bit of the ip, port or ipx
            b = bytes(b)
        elif r < 0.7:
            kind = rng.choice([0, 1, 2, 3, 4, 5, 6])
            a = struct.pack("<I", kind) + a[4:]
            b = struct.pack("<I", rng.choice([kind, kind, rng.randrange(7)])) + bytes(rng.getrandbits(8) for _ in range(16))
        else:
            b = rng.choice(ADDRESS_POOL + [random_address(rng)])
        yield [a, b]


for _net, _compare, _base in ((COD2, 0x00448650, 0x004485B0), (UO, 0x0044DEC0, 0x0044DD50)):
    spec(name="NET_CompareAdrSigned", image=_net.image, rust="net::oob::same_address",
         args=[AddressPointer(at="eax", net=_net), AddressPointer(at="edx", net=_net)], ret=RetSame(),
         cases=_compare_cases)
    spec(name="NET_CompareBaseAdrSigned", image=_net.image, rust="net::oob::same_base_address",
         args=[AddressPointer(at="eax", net=_net), AddressPointer(at="edx", net=_net)], ret=RetSame(),
         cases=_compare_cases)
