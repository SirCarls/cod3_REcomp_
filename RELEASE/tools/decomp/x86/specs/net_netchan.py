"""Checks for iw_pc::net::netchan (both games): the connection layer.

The channel (`netchan_t`) is handed to the Rust side in one layout for both
games (the `Chan` argument kind below), whatever its layout in the guest;
packets the original sends are recorded by the oracle's `sendto` stand-in
and compared whole (`SentPackets`).

The games read a few settings (dvars in Call of Duty 2, cvars in United
Offensive) through pointers that are NULL in the file; each check points
them at zeroed stand-ins (debug prints off, network profiling off), marks
the message Huffman tree as built (nothing here uses it) and gives the IP
socket a non-zero handle so that packets reach `sendto`.
"""
from __future__ import annotations

import random
import struct
from dataclasses import dataclass, field

from harness_x86 import Arg, Int, Msg, RetBool8, RetI32, RetVoid, spec

# --- the games' settings and globals ------------------------------------------------


@dataclass(frozen=True)
class GameNet:
    image: str
    value_offset: int          # where a dvar (CoD2) / cvar (UO) keeps its integer value
    settings: dict             # name -> address of the global pointer to the dvar / cvar
    huffman_built: int         # the "message Huffman tree is built" flag
    ip_socket: int             # the IP socket handle Sys_SendPacket sends with
    max_msglen: int
    chan_size: int
    chan_fields: dict          # field -> offset in netchan_t
    fragment_start_bits: int
    port_at: int               # where netadr_t keeps the port: CoD2 after the IPv4 address, UO at the end
    big_short_pointer: int = 0  # Quake III's `_BigShort` pointer (Swap_Init sets it; NULL in the file),
                                # which NET_AdrToString calls

    def adr_to_guest(self, address: bytes) -> bytes:
        """The canonical address (CoD2's layout: type, ip, port, ipx) as this game stores it."""
        if self.port_at == 8:
            return address
        return address[:8] + address[10:20] + address[8:10]

    def adr_from_guest(self, guest: bytes) -> bytes:
        if self.port_at == 8:
            return guest
        return guest[:8] + guest[18:20] + guest[8:18]


COD2 = GameNet(
    image="cod2mp", value_offset=8,
    settings={"net_profile": 0x00C8FB10, "showpackets": 0x00C84AE8, "showdrop": 0x00C8FB1C,
              "packetDebug": 0x00C8FB18, "net_showprofile": 0x00C8FB20},
    huffman_built=0x019A1B9C, ip_socket=0x00D76AD4, max_msglen=0x20000, chan_size=0x40040,
    chan_fields={"outgoing": 0x0, "sock": 0x4, "dropped": 0x8, "incoming": 0xC, "adr": 0x10, "qport": 0x24,
                 "fragment_sequence": 0x28, "fragment_length": 0x2C, "fragment_buffer": 0x30,
                 "unsent_fragments": 0x20030, "unsent_start": 0x20034, "unsent_length": 0x20038,
                 "unsent_buffer": 0x2003C, "prof": 0x4003C},
    fragment_start_bits=32, port_at=8, big_short_pointer=0x00C93BC4)

UO = GameNet(
    image="uomp", value_offset=0x20,
    settings={"net_profile": 0x04927D30, "showpackets": 0x0491CD88, "showdrop": 0x04927D38,
              "net_qport": 0x0491CD8C, "net_showprofile": 0x04927D3C},
    huffman_built=0x0389FD60, ip_socket=0x009D04F4, max_msglen=0x8000, chan_size=0x10040,
    chan_fields={"sock": 0x0, "dropped": 0x4, "adr": 0x8, "qport": 0x1C, "incoming": 0x20, "outgoing": 0x24,
                 "fragment_sequence": 0x28, "fragment_length": 0x2C, "fragment_buffer": 0x30,
                 "unsent_fragments": 0x8030, "unsent_start": 0x8034, "unsent_length": 0x8038,
                 "unsent_buffer": 0x803C, "prof": 0x1003C},
    fragment_start_bits=16, port_at=0x12, big_short_pointer=0x009C41AC)

GAMES = {"cod2mp": COD2, "uomp": UO}

NA_IP = 4


def fake_settings(oracle, net: GameNet, values: dict | None = None) -> None:
    """Points every setting the network code reads at a zeroed stand-in
    (with `values[name]` as its integer), marks the Huffman tree built and
    opens the IP socket."""
    values = values or {}
    for name, pointer in net.settings.items():
        block = bytearray(0x60)
        struct.pack_into("<i", block, net.value_offset, values.get(name, 0))
        oracle.write(pointer, struct.pack("<I", oracle.put(bytes(block), 16)))
    oracle.write(net.huffman_built, struct.pack("<I", 1))
    oracle.write(net.ip_socket, struct.pack("<I", 0x100))
    if net.big_short_pointer:
        # BigShort (cdecl): movzx eax, word [esp+4]; xchg al, ah; ret
        stub = oracle.put(bytes.fromhex("0FB7442404" "86C4" "C3"), 16)
        oracle.write(net.big_short_pointer, struct.pack("<I", stub))


# --- addresses ----------------------------------------------------------------------

def random_address(rng: random.Random) -> bytes:
    """A 20-byte netadr_t in the canonical layout (Call of Duty 2's: type, IPv4
    address, port in network byte order, IPX address): an IPv4 address."""
    ip = bytes(rng.choice([[192, 168, 0, rng.randint(1, 254)], [10, rng.randint(0, 255), 0, 2],
                           [rng.randint(1, 223), rng.randint(0, 255), rng.randint(0, 255), rng.randint(1, 254)]]))
    port = rng.choice([28960, 28961, 28963, 20700, rng.randint(1024, 65535)])
    return struct.pack("<I", NA_IP) + ip + struct.pack(">H", port) + bytes(10)


def sockaddr_of(address: bytes) -> bytes:
    """The sockaddr_in an IPv4 netadr_t becomes (NetadrToSockadr)."""
    return struct.pack("<H", 2) + address[8:10] + address[4:8] + bytes(8)


@dataclass
class Address(Arg):
    """A netadr_t passed by value: five stack words, in the game's layout.
    Token: the 20 bytes in the canonical layout."""
    net: GameNet = None

    def words(self, oracle, value):
        return list(struct.unpack("<5I", self.net.adr_to_guest(value)))

    def token(self, value):
        return "v:" + bytes(value).hex()


@dataclass
class Data(Arg):
    """Bytes the function only reads (a message to send). Token: v:<hex>."""

    def words(self, oracle, value):
        return [oracle.put(bytes(value) + bytes(16), 16)]

    def token(self, value):
        return "v:" + bytes(value).hex()


@dataclass
class SentPackets(Arg):
    """What the call sent (the oracle's `sendto` stand-in records it), as
    a buffer the Rust side fills: for each packet a 4-byte big-endian
    length, the 16-byte destination sockaddr_in, the bytes."""
    at: str = "none"
    mutable = True

    def words(self, oracle, value):
        return [1]  # not passed; non-zero so that the harness reads it back

    def token(self, value):
        return "b:"

    def read_back(self, oracle, pointer, value):
        out = bytearray()
        for to, data in oracle.sent:
            out += struct.pack(">I", len(data)) + to + data
        return bytes(out)


# --- the channel --------------------------------------------------------------------

CHAN_WORDS = ("outgoing", "sock", "dropped", "incoming", "qport", "fragment_sequence", "fragment_length",
              "unsent_fragments", "unsent_start", "unsent_length", "prof")


@dataclass
class ChanState:
    """A netchan_t, game-independent: the numbers, the address (canonical
    layout), the two buffers."""
    numbers: dict
    address: bytes
    fragment_buffer: bytes
    unsent_buffer: bytes


@dataclass
class Chan(Arg):
    """A `netchan_t` the function reads and changes, laid out in the guest
    as the game has it. Token (the same for both games): the numbers of
    CHAN_WORDS as big-endian 32-bit words, the 20-byte address, the
    fragment buffer, the unsent buffer (max_msglen bytes each)."""
    net: GameNet = None
    mutable = True
    last: int = field(default=0, repr=False)

    def guest_bytes(self, value: ChanState) -> bytes:
        f = self.net.chan_fields
        out = bytearray(self.net.chan_size)
        for name in CHAN_WORDS:
            struct.pack_into("<i", out, f[name], value.numbers[name])
        out[f["adr"]:f["adr"] + 20] = self.net.adr_to_guest(value.address)
        out[f["fragment_buffer"]:f["fragment_buffer"] + self.net.max_msglen] = value.fragment_buffer
        out[f["unsent_buffer"]:f["unsent_buffer"] + self.net.max_msglen] = value.unsent_buffer
        return bytes(out)

    def canonical(self, guest: bytes) -> bytes:
        f = self.net.chan_fields
        words = b"".join(struct.pack(">i", struct.unpack_from("<i", guest, f[name])[0]) for name in CHAN_WORDS)
        return (words + self.net.adr_from_guest(guest[f["adr"]:f["adr"] + 20])
                + guest[f["fragment_buffer"]:f["fragment_buffer"] + self.net.max_msglen]
                + guest[f["unsent_buffer"]:f["unsent_buffer"] + self.net.max_msglen])

    def words(self, oracle, value):
        self.last = oracle.put(self.guest_bytes(value), 16)
        return [self.last]

    def token(self, value):
        return "b:" + self.canonical(self.guest_bytes(value)).hex()

    def read_back(self, oracle, pointer, value):
        return self.canonical(oracle.read(pointer, self.net.chan_size))


def random_chan(rng: random.Random, net: GameNet, sock: int | None = None) -> ChanState:
    """A channel in a state the game can reach (or close to it)."""
    big = net.max_msglen
    incoming = rng.choice([0, 1, 2, rng.randint(0, 100), rng.randint(0, 1 << 20), 0x7FFFFFF0])
    outgoing = rng.choice([1, 2, rng.randint(1, 100), rng.randint(1, 1 << 20), 0x7FFFFFFE])
    fragment_length = rng.choice([0, 0, 1300, 2600, 1300 * rng.randint(0, big // 1300)])
    unsent_length = rng.choice([0, 1300, 2600, 1301, rng.randint(1300, big)])
    unsent_start = rng.choice([0, min(unsent_length, 1300 * rng.randint(0, unsent_length // 1300 + 1)),
                               rng.randint(0, unsent_length)])
    numbers = {
        "outgoing": outgoing,
        "sock": rng.choice([0, 1]) if sock is None else sock,
        "dropped": rng.randint(0, 5),
        "incoming": incoming,
        "qport": rng.choice([rng.randint(0, 0xFFFF), rng.randint(-1 << 31, (1 << 31) - 1)]),
        "fragment_sequence": rng.choice([incoming, incoming + 1, rng.randint(0, 1 << 20)]),
        "fragment_length": fragment_length,
        "unsent_fragments": rng.choice([0, 1]),
        "unsent_start": unsent_start,
        "unsent_length": unsent_length,
        "prof": 0,
    }
    # Buffers: a recognizable pattern, so that a stray or missing write shows.
    fragment = bytes(rng.getrandbits(8) for _ in range(64)) * (big // 64)
    unsent = bytes(rng.getrandbits(8) for _ in range(64)) * (big // 64)
    return ChanState(numbers, random_address(rng), fragment, unsent)


def chan_with_net_qport(net: GameNet):
    """United Offensive's client packets carry the `net_qport` cvar; the game
    sets up its channel with that same value (net_qport is init-only), so
    the stand-in cvar holds the channel's qport."""
    def setup(oracle, values):
        chan = next(v for v in values if isinstance(v, ChanState))
        fake_settings(oracle, net, {"net_qport": chan.numbers["qport"]})
    return setup


def plain_setup(net: GameNet):
    def setup(oracle, values):
        fake_settings(oracle, net)
    return setup


# --- Netchan_Setup ------------------------------------------------------------------

def _setup_cases(net: GameNet):
    def cases(rng):
        while True:
            chan = random_chan(rng, net)
            sock = rng.choice([0, 1])
            qport = rng.choice([rng.randint(0, 0xFFFF), rng.randint(-1 << 31, (1 << 31) - 1)])
            yield [chan, sock, random_address(rng), qport]
    return cases


for _net, _addr in ((COD2, 0x00447D50), (UO, 0x0044D530)):
    spec(name="Netchan_Setup", image=_net.image, rust="net::netchan::Netchan::setup",
         args=[Chan(at="edx", net=_net), Int(), Address(net=_net), Int()], ret=RetVoid(),
         cases=_setup_cases(_net), setup=plain_setup(_net))


# --- Netchan_Transmit, Netchan_TransmitNextFragment ---------------------------------

def message(rng: random.Random, net: GameNet) -> bytes:
    r = rng.random()
    if r < 0.45:
        n = rng.randint(0, 1299)
    elif r < 0.6:
        n = rng.choice([1299, 1300, 1301, 2599, 2600, 2601, 3900])
    elif r < 0.9:
        n = rng.randint(1300, 12000)
    else:
        n = rng.choice([net.max_msglen, net.max_msglen - 1, rng.randint(1300, net.max_msglen)])
    seed = rng.getrandbits(32)
    return bytes((seed + i * 7 + (i >> 8)) & 0xFF for i in range(n))


def _transmit_cases(net: GameNet):
    def cases(rng):
        while True:
            chan = random_chan(rng, net)
            data = message(rng, net)
            length = len(data)
            if rng.random() < 0.03:
                length = net.max_msglen + rng.choice([1, 2, 1000])   # the game's error
                data = data + bytes(length - len(data))
            yield [chan, length, data, None]
    return cases


def _next_fragment_cases(net: GameNet):
    def cases(rng):
        while True:
            chan = random_chan(rng, net)
            # Mostly the states Netchan_Transmit leaves: a message being sent in pieces.
            length = rng.choice([1300, 2600, 1301, 3899, rng.randint(1300, net.max_msglen)])
            start = min(length, 1300 * rng.randint(0, length // 1300 + 1))
            chan.numbers.update(unsent_fragments=1, unsent_length=length, unsent_start=start)
            if rng.random() < 0.1:
                chan.numbers["unsent_start"] = rng.randint(0, length)
            yield [chan, None]
    return cases


spec(name="Netchan_Transmit", image="cod2mp", rust="net::netchan::Netchan::transmit",
     args=[Chan(at="ebx", net=COD2), Int(at="eax"), Data(), SentPackets()], ret=RetBool8(),
     cases=_transmit_cases(COD2), setup=plain_setup(COD2))
spec(name="Netchan_Transmit", image="uomp", rust="net::netchan::Netchan::transmit",
     args=[Chan(net=UO), Int(at="ecx"), Data(), SentPackets()], ret=RetVoid(),
     cases=_transmit_cases(UO), setup=chan_with_net_qport(UO))
spec(name="Netchan_TransmitNextFragment", image="cod2mp", rust="net::netchan::Netchan::transmit_next_fragment",
     args=[Chan(net=COD2), SentPackets()], ret=RetBool8(),
     cases=_next_fragment_cases(COD2), setup=plain_setup(COD2))
spec(name="Netchan_TransmitNextFragment", image="uomp", rust="net::netchan::Netchan::transmit_next_fragment",
     args=[Chan(net=UO), SentPackets()], ret=RetVoid(),
     cases=_next_fragment_cases(UO), setup=chan_with_net_qport(UO))


# --- Netchan_Process ----------------------------------------------------------------

def _process_cases(net: GameNet):
    def cases(rng):
        while True:
            chan = random_chan(rng, net)
            n = chan.numbers
            incoming = n["incoming"]
            r = rng.random()
            if r < 0.3:
                sequence = rng.choice([incoming + 1, incoming + 1, incoming + 2, incoming + rng.randint(3, 50),
                                       incoming, incoming - 1, incoming - rng.randint(2, 50), 0, -5])
                fragmented = False
            else:
                fragmented = True
                sequence = rng.choice([n["fragment_sequence"], n["fragment_sequence"], incoming + 1,
                                       incoming, incoming - 3, rng.randint(0, 1 << 21)])
            sequence &= 0x7FFFFFFF
            header = struct.pack("<I", sequence | (0x80000000 if fragmented else 0))
            if n["sock"] == 1:
                header += struct.pack("<H", rng.getrandbits(16))
            body = bytes(rng.getrandbits(8) for _ in range(rng.choice([0, 1, 5, 40, 300])))
            if fragmented:
                start = rng.choice([n["fragment_length"], n["fragment_length"], 0, 1300,
                                    n["fragment_length"] + rng.choice([-1300, 1, 1300]), rng.getrandbits(16)])
                length = rng.choice([1300, 1300, rng.randint(0, 1299), 0, -1, -1300, 1301, 2000])
                if length > 0 and rng.random() < 0.9:
                    body = bytes(rng.getrandbits(8) for _ in range(64)) * (length // 64 + 1)
                    body = body[:length + rng.choice([0, 0, 0, 3, -1])]
                if net.fragment_start_bits == 32:
                    header += struct.pack("<i", start if -(1 << 31) <= start < (1 << 31) else 0)
                else:
                    header += struct.pack("<H", start & 0xFFFF)
                header += struct.pack("<h", max(-32768, min(32767, length)))
            packet = header + body
            k = rng.random()
            if k < 0.05:
                packet = packet[:rng.randint(0, len(packet))]   # cut short
            maxsize = rng.choice([net.max_msglen, net.max_msglen, 16384, 1400, rng.randint(1, 5000)])
            maxsize = max(maxsize, len(packet))
            data = packet + bytes(rng.getrandbits(8) for _ in range(rng.choice([0, 8])))
            overflowed = rng.choice([0, 0, 1])
            yield [chan, (overflowed, data, maxsize, len(packet), rng.choice([0, 3]), rng.choice([0, 9]))]
    return cases


spec(name="Netchan_Process", image="cod2mp", rust="net::netchan::Netchan::process",
     args=[Chan(at="eax", net=COD2), Msg()], ret=RetI32(), cases=_process_cases(COD2), setup=plain_setup(COD2))
spec(name="Netchan_Process", image="uomp", rust="net::netchan::Netchan::process",
     args=[Chan(at="eax", net=UO), Msg()], ret=RetI32(), cases=_process_cases(UO), setup=plain_setup(UO))


# --- the XOR scrambling -------------------------------------------------------------
# Every scrambler takes the data and a length; the key and the text come from the client
# structure (server side) or the client's globals (client side). The `KeyState` argument
# writes them where the game reads them; its token is the key's numbers (big-endian 32-bit
# words) followed by the text. The text is put in the slot the game picks with the
# acknowledged command's number (the other slots are empty), so a wrong slot shows.

@dataclass(frozen=True)
class KeyLayout:
    """Where a scrambler reads its key and its text."""
    numbers: tuple            # (name, offset or address) for each number of the key, in token order
    text: int                 # the text's offset (server: in client_t) or address (client: global)
    index: tuple = ()         # (name, offset or address) of the command number picking the text slot
    slots: int = 1            # how many slots (the number is taken modulo this)
    stride: int = 0           # bytes between slots
    client_size: int = 0      # server side: the client_t's size (0 = globals)


@dataclass
class KeyState(Arg):
    layout: KeyLayout = None

    def words(self, oracle, value):
        numbers, index, text = value
        lay = self.layout
        base = oracle.alloc(lay.client_size, 16) if lay.client_size else 0
        for (_, where), number in zip(lay.numbers, numbers):
            oracle.write(base + where, struct.pack("<i", number))
        slot = 0
        if lay.index:
            oracle.write(base + lay.index[1], struct.pack("<i", index))
            slot = index % lay.slots
        oracle.write(base + lay.text + slot * lay.stride, bytes(text) + b"\0")
        return [base]

    def token(self, value):
        numbers, _, text = value
        return "v:" + b"".join(struct.pack(">i", n) for n in numbers).hex() + bytes(text).hex()


def key_text(rng: random.Random) -> bytes:
    r = rng.random()
    if r < 0.1:
        return b""
    if r < 0.2:
        return bytes(rng.randint(1, 255) for _ in range(rng.randint(1, 1023)))
    words = [b"cs", b"d", b"print", b"\"%s\"", b"score", b"\x92", b"\xe9t\xe9", b"%", b"cp", b"@", b"1023"]
    out = b" ".join(rng.choice(words) for _ in range(rng.randint(1, 12)))
    return out[:1023]


def _scramble_cases(count: int):
    def cases(rng):
        while True:
            numbers = [rng.choice([rng.getrandbits(32) - (1 << 31), rng.randint(0, 300), -1, 0])
                       for _ in range(count)]
            index = rng.choice([rng.randint(0, 300), rng.randint(0, 1 << 30), 0, 63, 64, 127, 128])
            n = rng.choice([0, 1, 2, 3, 17, 100, 1300, rng.randint(0, 4000), -1])
            data = bytes(rng.getrandbits(8) for _ in range(max(n, 0) + rng.choice([0, 4])))
            yield [data, (numbers, index, key_text(rng)), n]
    return cases


def _data_arg(at: str) -> Arg:
    return BinaryBuffer(at=at)


@dataclass
class BinaryBuffer(Arg):
    """Bytes the function changes in place (no terminator added); all of them compared."""
    mutable = True

    def words(self, oracle, value):
        return [oracle.put(bytes(value) + bytes(16), 16)]

    def token(self, value):
        return "b:" + bytes(value).hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(pointer, len(value))


COD2_CLIENT = 0xB1064   # sizeof(client_t), CoD2 1.3
UO_CLIENT = 0xAB0B4     # sizeof(client_t), UO 1.51
SCRAMBLERS = [
    # image, name, address, data at, key at, layout
    ("cod2mp", "SV_Netchan_Encode", "eax", "stack",
     KeyLayout(numbers=(("challenge", 0x20820), ("outgoingSequence", 0x6E6C4)), text=0x20844,
               client_size=COD2_CLIENT)),
    ("cod2mp", "SV_Netchan_Decode", "stack", "ecx",
     KeyLayout(numbers=(("challenge", 0x20820), ("serverId", 0xAE710), ("messageAcknowledge", 0x20818)),
               text=0x40C, index=("reliableAcknowledge", 0x20810), slots=128, stride=0x408,
               client_size=COD2_CLIENT)),
    ("cod2mp", "CL_Netchan_Encode", "edx", "none",
     KeyLayout(numbers=(("challenge", 0x0060A110), ("serverId", 0x0098FDF8), ("serverMessageSequence", 0x0062A120)),
               text=0x0062A12C, index=("serverCommandSequence", 0x0062A124), slots=128, stride=1024)),
    ("cod2mp", "CL_Netchan_Decode", "stack", "none",
     KeyLayout(numbers=(("challenge", 0x0060A110), ("serverMessageSequence", 0x0062A120)),
               text=0x0060A120, index=("reliableAcknowledge", 0x0060A11C), slots=128, stride=1024)),
    ("uomp", "SV_Netchan_Encode", "eax", "stack",
     KeyLayout(numbers=(("challenge", 0x10620), ("outgoingSequence", 0x9B064)), text=0x10640,
               client_size=UO_CLIENT)),
    ("uomp", "SV_Netchan_Decode", "stack", "ecx",
     KeyLayout(numbers=(("challenge", 0x10620), ("serverId", 0xAB08C), ("messageAcknowledge", 0x10618)),
               text=0x40C, index=("reliableAcknowledge", 0x10610), slots=64, stride=0x408,
               client_size=UO_CLIENT)),
    ("uomp", "CL_Netchan_Encode", "edx", "none",
     KeyLayout(numbers=(("challenge", 0x04DC8988), ("serverId", 0x049637A8), ("serverMessageSequence", 0x04DD899C)),
               text=0x04DD89A8, index=("serverCommandSequence", 0x04DD89A0), slots=64, stride=1024)),
    ("uomp", "CL_Netchan_Decode", "stack", "none",
     KeyLayout(numbers=(("challenge", 0x04DC8988), ("serverMessageSequence", 0x04DD899C)),
               text=0x04DC899C, index=("reliableAcknowledge", 0x04DC8998), slots=64, stride=1024)),
]

for _image, _name, _data_at, _key_at, _layout in SCRAMBLERS:
    spec(name=_name, image=_image, rust="net::netchan::scramble",
         args=[_data_arg(_data_at), KeyState(at=_key_at, layout=_layout), Int()], ret=RetVoid(),
         cases=_scramble_cases(len(_layout.numbers)))


# --- SV_SendMessageToClient: header, Huffman, scrambling, netchan -------------------
# The original runs whole (the game's own MSG_initHuffman builds its tree once, `prepare`).
# The Rust side gets the coded body the game's own MSG_WriteBitsCompress makes of the same
# message (computed in the setup): the check covers the order of the steps, the scrambling
# key and text, the channel and the bookkeeping, not the Huffman coder (helper pc-msg's).

@dataclass(frozen=True)
class SendLayout:
    image: str
    compress: int              # MSG_WriteBitsCompress (from eax, to ebx, length ecx) -> length
    challenge: int             # client_t offsets
    last_command: int
    netchan: int
    frames: int                # frames[0].messageSent; messageAcked +4, messageSize +8
    frame_stride: int
    next_snapshot: int
    client_size: int
    svs_time: int


SEND_LAYOUTS = {
    "cod2mp": SendLayout("cod2mp", 0x00444000, 0x20820, 0x20844, 0x6E6C4, 0x234EC, 0x26C4, 0x20E28, 0xB1064, 0x00D35704),
    "uomp": SendLayout("uomp", 0x00449780, 0x10620, 0x10640, 0x9B040, 0x15144, 0x4520, 0x10C24, 0xAB0B4, 0x04907BC8),
}
SEND_CLIENT = 0x50000000           # the client_t, in the extra region the SVC_* checks map
SEND_REGION_SIZE = 0x03000000


@dataclass
class SendClient(Arg):
    """The client_t (at SEND_CLIENT): its channel, challenge, last command, frames.
    Token: the channel (Chan's), then a TLV with the rest and the coded body; read back:
    the channel, the 32 frames' (sent, acked, size), nextSnapshotTime."""
    net: GameNet = None
    layout: SendLayout = None
    mutable = True

    def words(self, oracle, value):
        lay = self.layout
        if SEND_CLIENT not in getattr(oracle, "_extra_regions", set()):
            oracle.uc.mem_map(SEND_CLIENT, SEND_REGION_SIZE)
            oracle._extra_regions = getattr(oracle, "_extra_regions", set()) | {SEND_CLIENT}
        oracle.write(SEND_CLIENT + lay.netchan, Chan(net=self.net).guest_bytes(value["chan"]))
        oracle.write(SEND_CLIENT + lay.challenge, struct.pack("<i", value["challenge"]))
        oracle.write(SEND_CLIENT + lay.last_command, value["text"] + b"\0")
        oracle.write(lay.svs_time, struct.pack("<i", value["time"]))
        return [SEND_CLIENT]

    def token(self, value):
        chan = Chan(net=self.net)
        extra = b"".join(bytes([len(k)]) + k.encode() + struct.pack(">I", len(v)) + v for k, v in [
            ("challenge", b"%d" % value["challenge"]), ("text", value["text"]), ("time", b"%d" % value["time"]),
            ("body", value["body"])])
        return "b:" + (chan.canonical(chan.guest_bytes(value["chan"])) + struct.pack(">I", len(extra)) + extra).hex()

    def read_back(self, oracle, pointer, value):
        lay = self.layout
        chan = Chan(net=self.net)
        guest = oracle.read(SEND_CLIENT + lay.netchan, self.net.chan_size)
        frames = b""
        for i in range(32):
            sent, acked, size = struct.unpack("<iii", oracle.read(SEND_CLIENT + lay.frames + i * lay.frame_stride, 12))
            frames += struct.pack(">iii", sent, acked, size)
        next_snapshot = struct.unpack("<i", oracle.read(SEND_CLIENT + lay.next_snapshot, 4))[0]
        return chan.canonical(guest) + frames + struct.pack(">i", next_snapshot)


def _send_setup(net: GameNet, lay: SendLayout):
    def setup(oracle, values):
        fake_settings(oracle, net)
        client, msg = values[0], values[1]
        data = bytes(msg[1])
        cursize = msg[3]
        src = oracle.put(data[4:cursize] + bytes(16), 16)
        out = oracle.alloc(0x40000, 16)
        n = oracle.call(lay.compress, {"eax": src, "ebx": out, "ecx": cursize - 4},
                        fpu=GAME_FPU_OF[net.image]).u32
        client["body"] = oracle.read(out, n)
    return setup


GAME_FPU_OF = {"cod2mp": "pc24", "uomp": "pc53"}


def _send_cases(net: GameNet):
    def cases(rng):
        while True:
            chan = random_chan(rng, net, sock=1)
            chan.address = bytes(struct.pack("<I", NA_IP) + bytes([rng.choice([10, 192]), 168, 0, 7])
                                 + struct.pack(">H", 28960) + bytes(10))   # a LAN client: no rate limit path
            chan.numbers["unsent_fragments"] = 0
            r = rng.random()
            n = rng.choice([4, 5, 20, 200, 1000, 1290, 1400, 3000, rng.randint(4, 9000)])
            body = bytes(rng.choice([rng.getrandbits(8), 0, 0, 1, 255]) for _ in range(n - 4))
            data = struct.pack("<i", rng.getrandbits(31)) + body
            client = {"chan": chan, "challenge": rng.getrandbits(32) - (1 << 31), "text": key_text(rng),
                      "time": rng.randint(0, 1 << 24), "body": b""}
            yield [client, (0, data, len(data) + 64, len(data), 0, 0), None]
    return cases


@dataclass
class ReadOnlyMsg(Msg):
    """The message, only read (token v:)."""
    mutable = False

    def token(self, value):
        return "v:" + super().token(value)[2:]


for _net, _reg in ((COD2, "eax"), (UO, "ecx")):
    spec(name="SV_SendMessageToClient", image=_net.image, rust="net::netchan::send_server_message",
         args=[SendClient(at="esi", net=_net, layout=SEND_LAYOUTS[_net.image]), ReadOnlyMsg(at=_reg), SentPackets()],
         ret=RetVoid(), cases=_send_cases(_net), setup=_send_setup(_net, SEND_LAYOUTS[_net.image]),
         prepare=("MSG_initHuffman",))
