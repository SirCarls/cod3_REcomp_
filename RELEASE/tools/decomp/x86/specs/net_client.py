"""Checks for iw_pc::client: the client's parsers of the server's messages
(both games), with the client's globals as the game keeps them.

The client's `cl` and the part of `clc` the parsers use are Regions: their
bytes are written into the game's memory before the call and compared whole
after it. The answerer converts them to iw_pc::client::ClientState and back
(engine/tools/pc_check/src/net_client.rs has the same layout); the cases fill
only what ClientState holds, so bytes the port doesn't model stay zero on
both sides.

Messages are built the way the server writes them (specs/net_delta.py's
Wire, put_struct, put_playerstate), against a client some way into a game:
earlier snapshots (some complete, some not, some overwritten), their
entities and clients in the rings, baselines, the times commands left. The
snapshots carry entities and clients over, change, remove and add them.

Cut messages: Call of Duty 2's readers stop at the end, so its messages are
cut anywhere, except where the original would read a structure with its
field count missing: it then copies the word at the offset in the entry in
front of the table (docs/PC_MULTIPLAYER.md, section 12, item 6; the ports
don't write outside the structure). For a client that is 0xDC, two clients
further on in the ring; for a baseline, 0x390038 bytes past the zeroed
entity on the stack it starts from, which isn't there. In a snapshot an
entity's count missing has it copy 0x390038 bytes further on, outside the
client's globals, which the checks don't compare: those cuts stay.
United Offensive's bit readers go on past the end, so its snapshots are cut
only in the entity list, with zeros after the cut (it then reads entity 0
unchanged until it notices, "end of message"); its game states and commands
anywhere.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

import gen
from harness_x86 import (Arg, Buffer, CallLog, CStr, GlobalU32, Int, Msg, Region, Ret, RetI32, RetPrinted, RetStop,
                         RetVoid, StandIn, spec)
from specs.fs_pure import FS, FsWord, SearchPaths, checksum_numbers, random_search_paths
from specs.net_delta import (CLIENT_STATE_SIZE, LAYOUT as PS_LAYOUT, USERCMD, Wire, _i32, f32_word, fields,
                             player_state, put_field, put_playerstate, random_state, usercmd)
from specs.net_msg import RawBytes
from specs.net_netchan import (COD2 as NET_COD2, UO as NET_UO, Address, Chan, SentPackets, fake_settings, random_address,
                               random_chan)

forever = iter(int, 1)
IMAGES = ("cod2mp", "uomp")
HUFFMAN = ("MSG_initHuffman",)
# A guest call that builds a case (coding a message for it): a time limit with room for a busy
# machine, since running out of time there isn't the function's doing.
SETUP_SECONDS = 300.0

# Where the client's state is in each game (the same table is in pc_check's net_client.rs).
L = {
    "cod2mp": dict(
        cl=0x0096B650, cl_size=0x195D94, snap=0x0096B668, old_frame=0x0096DD50, new_snapshots=0x0096DD58,
        gs_offsets=0x0096DD5C, gs_data=0x0096FD5C, gs_chars=0x20000, gs_count=0x0098FD5C,
        pen=0x0098FDA0, pcn=0x0098FDA4, out_packets=0x009D0C30, snapshots=0x009D0DB0, snap_size=0x26D8,
        baselines=0x00A1E8B0, parse_entities=0x00A5A8B0, parse_clients=0x00AD28B0, entity_size=0xF0,
        clc=0x00609FE8, clc_size=0x401B4, client_num=0x00609FE8, connect_count=0x0060A00C,
        checksum_feed=0x0060A114, msg_seq=0x0062A120, cmd_seq=0x0062A124, commands=0x0062A12C, commands_count=128,
        demo_waiting=0x0064A178, outgoing=0x0064A198, realtime=0x0068A520,
        dvars=(0x019A1B64, 0x00B013E4), huffman_flag=0x019A1B9C, systeminfo=0x004139E0, download=0x004140B0,
        msg_at=dict(CL_ParseCommandString="edx", CL_ParseGamestate="eax", CL_ParseSnapshot="eax",
                    CL_ParseServerMessage="eax"),
        # The sending side (CL_WritePacket).
        state=0x00609FE0, chan=0x0064A198, server_id=0x0098FDF8, cmds=0x009CFE2C, cmd_number=0x009D0C2C,
        challenge=0x0060A110, reliable_seq=0x0060A118, reliable_ack=0x0060A11C, reliable_commands=0x0060A120,
        last_sent=0x00609FEC, demo_playing=0x0064A170, packet_dup=0x0096B5F4, no_delta=0x0068A398,
        show_send=0x00609FD4, value_offset=8, default_cmd_at="ebx",
        # CL_PacketEvent.
        packet_msg_at="eax", connectionless=0x0040E750, demo_write=0x0040C060, connected=5,
        last_packet_time=0x00609FF0, demo_recording=0x0064A16C,
        # CL_ConnectionlessPacket.
        server_address=0x00609FF4, connect_time=0x0060A008, server_message=0x0060A010, only_visible=None,
        qport=0x019A1BA0, joining=(3, 4, 5),
        # CL_SetCGameTime.
        server_time=0x0096DD40, old_server_time=0x0096DD44, frame_server_time=0x0096DD48,
        server_time_delta=0x0096DD4C, extrapolated=0x0096DD54, first_demo_frame_skipped=0x0064A17C,
        time_demo=0x0064A174, time_demo_base=0x0064A194, clock_realtime=0x0068A520,
        # The player's commands (CL_FinishMove).
        user_cmd_weapon=0x0098FDC8, user_cmd_offhand=0x0098FDCC, cgame_angles=0x0098FDE0, view_angles=0x0098FDEC,
        oob_stops={0x0040E5D0: "voice", 0x004B34E0: "info", 0x004B4AF0: "status", 0x0040E520: "disconnect",
                   0x004118F0: "update", 0x004B3CF0: "servers", 0x004AB700: "needcdkey", 0x00412160: "newmap"}),
    "uomp": dict(
        cl=0x049581A0, cl_size=0x17BB34, snap=0x049581A4, old_frame=0x0495C6E8, new_snapshots=0x0495C6F0,
        gs_offsets=0x0495C6F4, gs_data=0x0495E6F4, gs_chars=0x5000, gs_count=0x049636F4,
        pen=0x04963738, pcn=0x0496373C, out_packets=0x049643D0, snapshots=0x04964550, snap_size=0x4534,
        baselines=0x049EEBD0, parse_entities=0x04A2BBD0, parse_clients=0x04AA5BD0, entity_size=0xF4,
        clc=0x04DC8860, clc_size=0x20E20, client_num=0x04DC8860, connect_count=0x04DC8884,
        checksum_feed=0x04DC898C, msg_seq=0x04DD899C, cmd_seq=0x04DD89A0, commands=0x04DD89A8, commands_count=64,
        demo_waiting=0x04DE9610, outgoing=0x04DE967C, realtime=0x04AD3E84,
        dvars=(0x0389FCE4, 0x04E1998C, 0x04929080), huffman_flag=0x0389FD60, systeminfo=0x00418B80,
        download=0x00418F90,
        msg_at=dict(CL_ParseCommandString="eax", CL_ParseGamestate="ebx", CL_ParseSnapshot="ecx",
                    CL_ParseServerMessage="stack"),
        state=0x04AD3D60, chan=0x04DE9658, server_id=0x049637A8, cmds=0x049637CC, cmd_number=0x049643CC,
        challenge=0x04DC8988, reliable_seq=0x04DC8994, reliable_ack=0x04DC8998, reliable_commands=0x04DC899C,
        last_sent=0x04DC8864, demo_playing=0x04DE960C, packet_dup=0x04DC8848, no_delta=0x0495818C,
        show_send=0x04958074, value_offset=0x20, default_cmd_at="edi",
        packet_msg_at="ecx", connectionless=0x00412880, demo_write=0x0040FA10, connected=3,
        last_packet_time=0x04DC8868, demo_recording=0x04DE9608,
        server_address=0x04DC886C, connect_time=0x04DC8880, server_message=0x04DC8888, only_visible=0x04DC8990,
        qport=None, joining=(1, 2, 3),
        server_time=0x0495C6D8, old_server_time=0x0495C6DC, frame_server_time=0x0495C6E0,
        server_time_delta=0x0495C6E4, extrapolated=0x0495C6EC, first_demo_frame_skipped=0x04DE9614,
        time_demo=None, time_demo_base=0x04DE962C,
        clock_realtime=0x04AD3E80,   # cls.realtime as the client game's clock sees it (a second clock)
        user_cmd_weapon=0x0496376C, user_cmd_offhand=None, cgame_angles=0x04963790, view_angles=0x0496379C,
        oob_stops={0x004155F0: "info", 0x00415F90: "status", 0x004122C0: "disconnect", 0x00412390: "motd",
                   0x00415A50: "update", 0x00412610: "servers", 0x00470780: "localize", 0x00416FC0: "newmap",
                   # Com_PrintMessage: the console window (user32) behind it.
                   0x004398F0: "print"}),
}
OPS = {  # server message types (iw_pc::net::game_state::server_ops)
    "cod2mp": dict(nop=0, game_state=1, config_string=2, baseline=3, server_command=4, download=5, snapshot=6, eof=7),
    "uomp": dict(nop=1, game_state=2, config_string=3, baseline=4, server_command=5, download=6, snapshot=7, eof=8),
}
MAX_CONFIG_STRINGS = 2048
RING = 2048
SNAPSHOT_WORDS = 7 * 4   # valid, snapFlags, serverTime, messageNum, deltaNum, ping, cmdNum
TAIL = ("num_entities", "num_clients", "pen", "pcn", "scn")   # the five numbers after the player state
TEXT = gen.LETTERS + gen.DIGITS + b" \"\\%;.\x92\xe9"


def _setup(image: str):
    """Points the game's console variables the parsers read (cl_shownet,
    cl_shownuments; United Offensive's cl_running for Con_Close) at an
    all-zero variable, and marks the message Huffman tree as built."""
    lay = L[image]

    def setup(oracle, values):
        zero = oracle.alloc(0x80, 16)
        for pointer in lay["dvars"]:
            oracle.write(pointer, struct.pack("<I", zero))
        oracle.write(lay["huffman_flag"], struct.pack("<I", 1))
    return setup


# --- the client's state -----------------------------------------------------------------

class Client:
    """A `cl` and a `clc` being built, as the game lays them out."""

    def __init__(self, image: str):
        self.image = image
        self.lay = L[image]
        self.cl = bytearray(self.lay["cl_size"])
        self.clc = bytearray(self.lay["clc_size"])

    def _place(self, address: int):
        lay = self.lay
        if lay["cl"] <= address < lay["cl"] + lay["cl_size"]:
            return self.cl, address - lay["cl"]
        return self.clc, address - lay["clc"]

    def put(self, address: int, value: int) -> None:
        block, at = self._place(address)
        struct.pack_into("<i", block, at, _i32(value))

    def get(self, address: int) -> int:
        block, at = self._place(address)
        return struct.unpack_from("<i", block, at)[0]

    def put_bytes(self, address: int, data: bytes) -> None:
        block, at = self._place(address)
        block[at:at + len(data)] = data

    def snapshot(self, index: int) -> int:
        return self.lay["snapshots"] + (index % 32) * self.lay["snap_size"]

    def snapshot_word(self, index: int, name: str) -> int:
        """One of the five numbers after snapshot slot `index`'s player state (TAIL)."""
        ps = PS_LAYOUT[self.image]["size"]
        return self.get(self.snapshot(index) + SNAPSHOT_WORDS + ps + 4 * TAIL.index(name))

    def ring_entity(self, ring: int) -> int:
        return self.lay["parse_entities"] + (ring % RING) * self.lay["entity_size"]

    def ring_client(self, ring: int) -> int:
        return self.lay["parse_clients"] + (ring % RING) * CLIENT_STATE_SIZE


def snapshot_bytes(words7, ps: bytes, tail5) -> bytes:
    """clSnapshot_t: seven numbers, the player state, numEntities, numClients,
    parseEntitiesNum, parseClientsNum, serverCommandNum."""
    return struct.pack("<7i", *map(_i32, words7)) + bytes(ps) + struct.pack("<5i", *map(_i32, tail5))


def entity_table(rng, image: str) -> str:
    return "entity12" if image == "uomp" and rng.random() < 0.3 else "entity"


def entity_state(rng, image: str, number: int) -> bytearray:
    size = L[image]["entity_size"]
    state = random_state(rng, image, entity_table(rng, image), size) if rng.random() < 0.8 else bytearray(size)
    struct.pack_into("<i", state, 0, number)
    return state


def client_state(rng, image: str, number: int) -> bytearray:
    state = random_state(rng, image, "client", CLIENT_STATE_SIZE) if rng.random() < 0.8 \
        else bytearray(CLIENT_STATE_SIZE)
    struct.pack_into("<i", state, 0, number)
    return state


def _numbers(rng, limit: int, most: int) -> list[int]:
    count = rng.choice([0, 1, 2, 3, rng.randint(0, most)])
    return sorted(rng.sample(range(limit), min(count, limit)))


@dataclass
class Kept:
    """A snapshot in the client's history: its entities' and clients' numbers."""
    entities: list[int]
    clients: list[int]
    complete: bool    # valid, and its slot's message number is its own


def history(rng, image: str, ring_gap: bool = True) -> tuple[Client, int, dict[int, Kept]]:
    """A client some way into a game. Returns it, the number of the message
    being read (`clc.serverMessageSequence`) and the snapshots in its slots
    by message number."""
    c = Client(image)
    lay = c.lay
    ps_size = PS_LAYOUT[image]["size"]
    message = rng.choice([1, 2, 5, 40, 300, rng.randint(1, 1 << 20), rng.randint(1, 0x7FFFFF00)])
    last = message - rng.choice([1, 1, 1, 2, 3, 5, 31, 32, 40])     # cl.snap's message
    pen = rng.choice([0, 5, rng.randint(0, 1 << 20), rng.randint(0, 0x7FFF0000)])
    pcn = rng.choice([0, 7, rng.randint(0, 1 << 20)])
    states = [player_state(rng, image) for _ in range(rng.choice([1, 2, 3]))] + [bytes(ps_size)]
    owner: dict[int, tuple[int, Kept]] = {}
    for m in range(max(1, last - 40), last + 1):
        if m != last and rng.random() < 0.5:
            continue
        ents, clients = _numbers(rng, 1024, 12), _numbers(rng, 64, 8)
        valid = 1 if rng.random() < 0.9 else 0
        number = m if rng.random() < 0.95 else m + rng.choice([32, -32, 1])
        start_e, start_c = pen, pcn
        for n in ents:
            c.put_bytes(c.ring_entity(pen), entity_state(rng, image, n))
            pen += 1
        for n in clients:
            c.put_bytes(c.ring_client(pcn), client_state(rng, image, n))
            pcn += 1
        words = [valid, rng.getrandbits(8), rng.randint(0, 1 << 30), number, rng.randint(-1, max(number, -1)),
                 rng.randint(0, 999), rng.getrandbits(16)]
        data = snapshot_bytes(words, rng.choice(states), [len(ents), len(clients), start_e, start_c,
                                                          rng.getrandbits(20)])
        c.put_bytes(c.snapshot(m), data)
        if m == last:
            c.put_bytes(lay["snap"], data)
        owner[m % 32] = (m, Kept(ents, clients, bool(valid) and number == m))
    kept = {m: k for m, k in owner.values()}
    # The rings moved on since (rarely past what a snapshot may refer back to).
    gaps = [0, 0, 0, 3, 100, 1919] + ([1921, 2100] if ring_gap else [])
    c.put(lay["pen"], pen + rng.choice(gaps))
    c.put(lay["pcn"], pcn + rng.choice([0, 0, 0, 2] + ([1921] if ring_gap else [])))
    for n in _numbers(rng, 1024, 20):
        c.put_bytes(lay["baselines"] + n * lay["entity_size"], entity_state(rng, image, n))
    for k in range(32):
        if rng.random() < 0.7:
            c.put_bytes(lay["out_packets"] + 12 * k,
                        struct.pack("<3i", rng.getrandbits(20), rng.randint(0, 1 << 30), rng.randint(0, 1 << 30)))
    c.put(lay["old_frame"], rng.getrandbits(20))
    c.put(lay["new_snapshots"], rng.choice([0, 1]))
    c.put(lay["msg_seq"], message)
    c.put(lay["cmd_seq"], rng.choice([0, rng.randint(0, 1000), rng.getrandbits(31)]))
    c.put(lay["demo_waiting"], rng.choice([0, 1]))
    c.put(lay["outgoing"], rng.choice([0, 1, rng.randint(0, 1 << 20)]))
    return c, message, kept


# --- messages -----------------------------------------------------------------------------

class TracedWire(Wire):
    """A Wire that records, for each thing written, the last byte a reader
    needs for it (-1: no new one) and the part of the message it belongs to,
    so a cut can be placed by what the reader meets first past the end."""

    def __init__(self):
        super().__init__()
        self.zone = "prefix"
        self.needs: list[tuple[int, str]] = []

    def _note(self, before: int) -> None:
        self.needs.append((len(self.data) - 1 if len(self.data) > before else -1, self.zone))

    def bits(self, value, count):
        before = len(self.data)
        super().bits(value, count)
        self._note(before)

    def byte(self, value):
        before = len(self.data)
        super().byte(value)
        self._note(before)

    def short(self, value):
        before = len(self.data)
        super().short(value)
        self._note(before)

    def long(self, value):
        before = len(self.data)
        super().long(value)
        self._note(before)

    def cuts(self, start: int, end: int) -> list[tuple[int, str | None]]:
        """For each length from `start` to `end`, the part the reader meets
        the end of a message cut to that length in (None: none). The bytes
        needed only grow, so one pass does."""
        needed = [(need, zone) for need, zone in self.needs if need >= 0]
        out, i = [], 0
        for p in range(start, end + 1):
            while i < len(needed) and needed[i][0] < p:
                i += 1
            out.append((p, needed[i][1] if i < len(needed) else None))
        return out


def put_entities(w: Wire, rng, image: str, old: list[int]) -> None:
    """A snapshot's entity list against the old snapshot's entity numbers:
    some kept as they are (not named), some changed or removed, new ones;
    then 1023."""
    w.zone = "entities"
    names = {n for n in old if rng.random() < 0.4} | set(_numbers(rng, 1023, 6))
    for n in sorted(names):
        w.bits(n, 10)
        put_delta_struct(w, rng, image, entity_table(rng, image))
    w.bits(1023, 10)


def put_delta_struct(w: Wire, rng, image: str, table: str, head: str | None = None) -> None:
    """A structure as MSG_WriteDeltaStruct sends it after its number (as
    net_delta's put_struct; a field count past the table, which Call of
    Duty 2 takes as an overflow, only rarely). United Offensive's entities
    have the table flag first. `head`: the zone of the part up to the field
    count, when it differs from the fields'."""
    zone = w.zone
    if head is not None:
        w.zone = head
    if image == "uomp" and table.startswith("entity"):
        w.bits(1 if table == "entity12" else 0, 1)
    if rng.random() < 0.08:
        w.bits(1, 1)                      # removed
        w.zone = zone
        return
    w.bits(0, 1)
    if rng.random() < 0.15:
        w.bits(0, 1)                      # unchanged
        w.zone = zone
        return
    w.bits(1, 1)
    entries = fields(image, table)
    n = len(entries)
    k = rng.random()
    count = rng.randint(0, n) if k < 0.8 else n if k < 0.9 else rng.choice([0, 1]) if k < 0.995 \
        else rng.randint(n + 1, 255)
    w.byte(count)
    w.zone = zone
    for _, _, bits in entries[:min(count, n)]:
        put_field(w, rng, image, bits)


def put_clients(w: Wire, rng, image: str, old: list[int]) -> None:
    names = {n for n in old if rng.random() < 0.4} | set(_numbers(rng, 64, 4))
    for n in sorted(names):
        w.zone = "client head"
        w.bits(1, 1)
        w.bits(n, 6)
        w.zone = "client fields"
        put_delta_struct(w, rng, image, "client", head="client head")
    w.zone = "client head"
    w.bits(0, 1)
    w.zone = "after"


def put_snapshot(w: Wire, rng, image: str, message: int, kept: dict[int, Kept]) -> None:
    """A snapshot's body (after its type byte)."""
    w.zone = "header"
    w.long(rng.randint(0, 1 << 30))
    near = [m for m in kept if 0 < message - m < 256]
    k = rng.random()
    if k < 0.15 or not near:
        delta = 0
    elif k < 0.92:
        delta = message - rng.choice(near)
    else:
        delta = rng.randint(1, 255)
    w.byte(delta)
    w.byte(rng.getrandbits(8))
    w.zone = "player"
    put_playerstate(w, rng, image)
    old = kept.get(message - delta) if delta else None
    put_entities(w, rng, image, old.entities if old else [])
    put_clients(w, rng, image, old.clients if old else [])


def framed(rng, image: str, write, cut: bool = False, uo_cut_zones=("entities",)):
    """A msg_t for a parser: some earlier content, then what `write` writes
    (into a TracedWire), then other bytes; maybe cut short (see the module;
    `uo_cut_zones` None: United Offensive's cut anywhere)."""
    w = TracedWire()
    for _ in range(rng.choice([0, 0, 1, 2, 5])):
        if rng.random() < 0.5:
            w.bits(rng.getrandbits(8), rng.randint(1, 8))
        else:
            w.byte(rng.getrandbits(8))
    readcount, bit = len(w.data), w.bit
    write(w)
    end = len(w.data)
    data = bytearray(w.data) + bytes(rng.getrandbits(8) for _ in range(rng.choice([0, 3, 16])))
    cursize = end if rng.random() < 0.8 else len(data)
    if cut:
        cuts = []
        for p, zone in w.cuts(readcount, end):
            if image == "cod2mp" and zone not in ("client head", "baseline head"):
                cuts.append(p)
            elif image == "uomp" and (uo_cut_zones is None or zone in uo_cut_zones):
                cuts.append(p)
        if cuts:
            cursize = rng.choice(cuts)
            if image == "uomp" and uo_cut_zones is not None:
                data = data[:cursize] + bytes(len(data) - cursize)
    maxsize = len(data) + rng.choice([0, 0, 1, 20])
    overflowed = 1 if image == "cod2mp" and rng.random() < 0.03 else 0
    return (overflowed, bytes(data), maxsize, cursize, readcount, bit)


# --- CL_ParseCommandString ------------------------------------------------------------------

def command_body(w: Wire, rng, sequence: int) -> None:
    w.long(sequence)
    length = rng.choice([0, 5, 40, rng.randint(0, 300), 1022, 1023, 1024, 1100])
    w.data += gen.text(rng, length, TEXT, min_len=length) + (b"\0" if rng.random() < 0.95 else b"")


def _command_cases(image: str):
    lay = L[image]

    def cases(rng):
        for _ in forever:
            c = Client(image)
            seq = rng.choice([0, 5, rng.randint(0, 1 << 20), rng.getrandbits(31), -5])
            c.put(lay["cmd_seq"], seq)
            for _ in range(rng.randint(0, 4)):
                c.put_bytes(lay["commands"] + rng.randrange(lay["commands_count"]) * 0x400,
                            gen.text(rng, 60, TEXT, min_len=1))
            step = rng.choice([1, 1, 2, 0, -1, 200, 64, 128])
            m = framed(rng, image, lambda w: command_body(w, rng, seq + step), cut=rng.random() < 0.1,
                       uo_cut_zones=None)
            yield [m, bytes(c.clc)]
    return cases


# --- CL_ParseGamestate ----------------------------------------------------------------------

def game_state_body(w: Wire, rng, image: str) -> None:
    """A game state's body (after its type byte)."""
    ops = OPS[image]
    w.long(rng.choice([0, 1, rng.randint(0, 1 << 20), -1]))
    k = rng.random()
    big = 0 if k < 0.9 else 1 if k < 0.99 else 2   # long strings; more than there is room for
    parts = rng.choice([0, 1, 3, 10, 40]) if not big else 4 if big == 1 else L[image]["gs_chars"] // 8000 + 2
    # One thing wrong in some messages: a config string number out of range, a wrong part type.
    k = rng.random()
    wrong = "index" if k < 0.04 else "type" if k < 0.08 else None
    wrong_at = rng.randrange(parts) if parts else -1
    for part in range(parts):
        k = rng.random()
        if wrong == "type" and part == wrong_at:
            w.byte(rng.choice([ops["nop"], ops["snapshot"], 0xFF, 9]))
        elif k < 0.6 or big:
            w.byte(ops["config_string"])
            index = rng.choice([0, 1, 2, rng.randint(0, 2047), 2047]) if not (wrong == "index" and part == wrong_at) \
                else rng.choice([2048, -1, 30000])
            w.short(index)
            length = rng.choice([0, 1, 10, 80, rng.randint(0, 400)]) if not big else rng.choice([8191, 8000, 6000])
            w.data += gen.text(rng, length, TEXT, min_len=length) + b"\0"
        else:
            w.zone = "baseline"
            w.byte(ops["baseline"])
            w.bits(rng.choice([0, 1, 1023, rng.randint(0, 1023)]), 10)
            put_delta_struct(w, rng, image, entity_table(rng, image), head="baseline head")
            w.zone = "game state"
    w.byte(ops["eof"])
    w.long(rng.randint(-1, 63))
    w.long(rng.getrandbits(32))


def used_client(rng, image: str) -> Client:
    """A client in a game, with config strings and the connection's numbers set."""
    c, _, _ = history(rng, image)
    lay = c.lay
    count = 1
    for _ in range(rng.randint(0, 5)):
        text = gen.text(rng, 40, TEXT, min_len=1)
        c.put(lay["gs_offsets"] + 4 * rng.randrange(MAX_CONFIG_STRINGS), count)
        c.put_bytes(lay["gs_data"] + count, text)
        count += len(text) + 1
    c.put(lay["gs_count"], count)
    c.put(lay["client_num"], rng.randint(0, 63))
    c.put(lay["checksum_feed"], rng.getrandbits(31))
    c.put(lay["connect_count"], rng.randint(0, 9))
    # The client game's clock (cl: a game state clears it), the demo numbers (clc: kept).
    for name in ("server_time", "old_server_time", "frame_server_time", "server_time_delta", "extrapolated",
                 "first_demo_frame_skipped", "time_demo_base") + (("time_demo",) if lay["time_demo"] else ()):
        c.put(lay[name], _i32(rng.getrandbits(32)) if rng.random() < 0.7 else 0)
    return c


def _game_state_cases(image: str):
    def cases(rng):
        for _ in forever:
            c = used_client(rng, image) if rng.random() < 0.6 else Client(image)
            m = framed(rng, image, lambda w: game_state_body(w, rng, image), cut=rng.random() < 0.1,
                       uo_cut_zones=None)
            yield [m, bytes(c.cl), bytes(c.clc)]
    return cases


# --- CL_ParsePacketEntities / CL_ParsePacketClients -------------------------------------------

@dataclass
class SnapshotRef(Int):
    """A pointer to cl.snapshots[value] (None: NULL); the answerer gets the index (-1: NULL)."""
    image: str = ""

    def words(self, oracle, value):
        lay = L[self.image]
        return [0 if value is None else lay["snapshots"] + value * lay["snap_size"]]

    def token(self, value):
        return f"i:{-1 if value is None else value}"


def _packet_cases(image: str, clients: bool):
    lay = L[image]

    def cases(rng):
        for _ in forever:
            c, message, kept = history(rng, image, ring_gap=False)
            complete = [m for m in kept if kept[m].complete]
            old = rng.choice(complete) if complete and rng.random() < 0.85 else None
            if old is not None:   # the old snapshot's entries within reach (CL_ParseSnapshot checks it)
                for ring, start, count in ((lay["pen"], "pen", "num_entities"), (lay["pcn"], "pcn", "num_clients")):
                    first = c.snapshot_word(old, start)
                    if not 0 <= _i32(c.get(ring) - first) <= 1920:
                        c.put(ring, first + c.snapshot_word(old, count) + rng.randint(0, 50))
            entry = kept[old] if old is not None else Kept([], [], False)

            def write(w):
                if clients:
                    put_clients(w, rng, image, entry.clients)
                else:
                    put_entities(w, rng, image, entry.entities)
            m = framed(rng, image, write, cut=rng.random() < 0.15)
            new = bytes(rng.getrandbits(8) for _ in range(lay["snap_size"])) if rng.random() < 0.2 \
                else bytes(lay["snap_size"])
            yield [m, None if old is None else old % 32, new, bytes(c.cl)]
    return cases


# --- CL_ParseSnapshot -----------------------------------------------------------------------

def _snapshot_cases(image: str):
    def cases(rng):
        for _ in forever:
            c, message, kept = history(rng, image)
            m = framed(rng, image, lambda w: put_snapshot(w, rng, image, message, kept), cut=rng.random() < 0.12)
            yield [m, bytes(c.cl), bytes(c.clc), rng.randint(0, 1 << 30)]
    return cases


# --- CL_ParseServerMessage ------------------------------------------------------------------

@dataclass
class CodedMsg(Msg):
    """A message as CL_ParseServerMessage gets it: `readcount` bytes of
    header, then the rest Huffman-coded. The value holds the rest as plain
    bytes: the original's own MSG_WriteBitsCompress codes it in the guest
    (its port is checked to code the same) and the answerer codes it with
    the port. A read-only `v:` token (the header words, then the plain
    data, as Msg's); not compared: the function reads it only."""
    mutable = False

    def words(self, oracle, value):
        overflowed, data, maxsize, cursize, readcount, bit = value
        plain = bytes(data[readcount:cursize])
        source = oracle.put(plain + bytes(64), 16)
        coded = oracle.alloc(4 * len(plain) + 64, 16)
        compress = oracle.images[0].resolve("MSG_WriteBitsCompress")
        length = oracle.call(compress, {"eax": source, "ebx": coded, "ecx": len(plain)},
                             max_seconds=SETUP_SECONDS).i32
        buffer = bytes(data[:readcount]) + oracle.read(coded, length)
        data_ptr = oracle.put(buffer + bytes(self.slack), 16)
        msg = struct.pack("<iIiiii", overflowed, data_ptr, len(buffer) + 16, len(buffer), readcount, bit)
        return [oracle.put(msg, 16)]

    def token(self, value):
        return "v:" + super().token(value)[2:]


def fits_message(image: str, plain_length: int) -> bool:
    """A server message's bytes fit the game's message buffer (`max_msglen`,
    with room for the few bytes the padding bits decode into). The client
    decompresses into a buffer that big (United Offensive's 0x8000 bytes on
    CL_ParseServerMessage's stack, Call of Duty 2's 0x20000 a global) without
    a limit, so a longer message writes past it: servers never send one (their
    own buffer is as big), and the checks never give one. Only big game
    states come near it."""
    return plain_length + 64 <= NET[image].max_msglen


def _server_message_cases(image: str):
    ops = OPS[image]

    def cases(rng):
        for _ in forever:
            c, message, kept = history(rng, image)
            w = TracedWire()
            header = rng.choice([0, 4, 4, 9])
            w.data += bytes(rng.getrandbits(8) for _ in range(header))
            snapshots = 0
            for _ in range(rng.choice([0, 1, 1, 2, 3])):
                k = rng.random()
                if 0.5 <= k < 0.86 and snapshots:
                    # One snapshot a message, as servers send: a second one would be written
                    # against snapshots the first has replaced, so the reader would read on out
                    # of step with the writer, through random bits, which in United Offensive
                    # can be a HUD element index of 30 or 31 (its reader then reads outside the
                    # player state: an input the checks never give, see specs/net_delta.py).
                    k = 0.4
                w.bit = 0   # each part starts on a byte of its own (its type is read as a raw byte)
                if k < 0.35:
                    w.byte(ops["server_command"])
                    command_body(w, rng, c.get(c.lay["cmd_seq"]) + rng.choice([1, 2, 0]))
                elif k < 0.5:
                    w.byte(ops["nop"])
                elif k < 0.86:
                    w.byte(ops["snapshot"])
                    put_snapshot(w, rng, image, message, kept)
                    snapshots += 1
                elif k < 0.94:
                    w.byte(ops["game_state"])
                    game_state_body(w, rng, image)
                elif k < 0.98:
                    w.byte(ops["download"])
                else:
                    w.byte(rng.choice([0xFE, 9, 0xFF]))
            if rng.random() < 0.9:
                w.bit = 0
                w.byte(ops["eof"])
            if not fits_message(image, len(w.data) - header):
                continue
            m = (0, bytes(w.data), len(w.data), len(w.data), header, 0)
            yield [m, bytes(c.cl), bytes(c.clc), rng.randint(0, 1 << 30)]
    return cases


# --- the sending side: MSG_SetDefaultUserCmd, CL_WritePacket ----------------------------------

NET = {"cod2mp": NET_COD2, "uomp": NET_UO}
SPECIAL_FLOATS = [0.0, -0.0, 1.5, -2.0, 0.25, -1e-30, float("inf"), float("-inf"), float("nan")]


def default_cmd_state(rng, image: str) -> bytearray:
    """A player state for MSG_SetDefaultUserCmd: the flag that turns on the
    lean and stance buttons often set, the words and floats they come from
    at their edges, view angles anywhere."""
    ps = player_state(rng, image) if rng.random() < 0.5 else bytearray(PS_LAYOUT[image]["size"])
    cod2 = image == "cod2mp"
    flags = struct.unpack_from("<i", ps, 0xC)[0]
    if rng.random() < 0.6:
        flags |= 0x800000 if cod2 else 0x80000
    if not cod2 and rng.random() < 0.4:
        flags |= 0x10000
    struct.pack_into("<i", ps, 0xC, _i32(flags))
    struct.pack_into("<i", ps, 0xA0 if cod2 else 0x84, _i32(rng.choice([0, 4, 8, 12, 0x20, 0x40, 0x60, rng.getrandbits(32)])))
    for offset in ((0x4C, 0xDC) if cod2 else (0x44, 0xE0)):
        struct.pack_into("<i", ps, offset, f32_word(rng.choice(SPECIAL_FLOATS + [rng.uniform(-50, 50)])))
    for i in range(2):
        angle = rng.choice([0.0, 359.99, -180.0, 90.0, 1e10, -1e30, float("nan"), rng.uniform(-720, 720),
                            rng.uniform(-1e6, 1e6)])
        struct.pack_into("<i", ps, 0xE8 + 4 * i, f32_word(angle))
        struct.pack_into("<i", ps, (0x54 if cod2 else 0x4C) + 4 * i, rng.choice([0, rng.randint(0, 65535),
                                                                                  _i32(rng.getrandbits(32))]))
    return ps


def _default_cmd_cases(image: str):
    size = USERCMD[image]["size"]

    def cases(rng):
        for _ in forever:
            yield [bytes(default_cmd_state(rng, image)), bytes(rng.getrandbits(8) for _ in range(size))]
    return cases


@dataclass
class GlobalChan(Chan):
    """A netchan_t at a fixed address (the client's clc.netchan): Chan's
    token and layout, written there before the call and read back from there."""
    address: int = 0
    at: str = "none"

    def words(self, oracle, value):
        oracle.write(self.address, self.guest_bytes(value))
        return [self.address]

    def read_back(self, oracle, pointer, value):
        return self.canonical(oracle.read(self.address, self.net.chan_size))


@dataclass
class Setting(Arg):
    """A console variable the function reads (an integer, i: token): a
    stand-in variable holding the value, its pointer written to `pointer`."""
    pointer: int = 0
    offset: int = 8
    at: str = "none"

    def words(self, oracle, value):
        block = bytearray(0x60)
        struct.pack_into("<i", block, self.offset, value)
        address = oracle.put(bytes(block), 16)
        oracle.write(self.pointer, struct.pack("<I", address))
        return [address]

    def token(self, value):
        return f"i:{value}"


def _send_setup(image: str):
    """The parsers' stand-ins, a zero cl_showSend, the network settings
    (United Offensive's packets carry the net_qport cvar: the channel's)."""
    lay, net = L[image], NET[image]
    parse_setup = _setup(image)

    def setup(oracle, values):
        parse_setup(oracle, values)
        zero = oracle.alloc(0x80, 16)
        oracle.write(lay["show_send"], struct.pack("<I", zero))
        chan = values[3]
        fake_settings(oracle, net, {"net_qport": chan.numbers["qport"]} if image == "uomp" else None)
    return setup


def send_clc_size(image: str) -> int:
    """The part of clc before the channel."""
    return L[image]["chan"] - L[image]["clc"]


def _write_packet_cases(image: str):
    lay, net = L[image], NET[image]
    size = USERCMD[image]["size"]
    ring = lay["commands_count"]
    states = [1, 2, 3, 5, 6, 8, 9, 9, 9] if image == "cod2mp" else [3, 4, 5, 6, 7, 8, 9, 9, 9]

    def cases(rng):
        for _ in forever:
            c, message, kept = history(rng, image)
            # The latest player commands, and where the earlier packets left off.
            number = rng.choice([0, 5, 200, rng.randint(0, 1 << 20), rng.getrandbits(31)])
            c.put(lay["cmd_number"], number)
            time = rng.randint(0, 1 << 30)
            for k in range(40):
                cmd = usercmd(rng, image)
                if rng.random() < 0.8:
                    struct.pack_into("<i", cmd, 0, _i32(time - k * rng.choice([1, 16, 50, 300])))
                c.put_bytes(lay["cmds"] + ((number - k) % 128) * size, bytes(cmd))
            for k in range(32):
                c.put(lay["out_packets"] + 12 * k, number - rng.choice([0, 1, 2, 3, 5, 31, 32, 33, 40, -2]))
            c.put(lay["server_id"], rng.choice([0, 1, rng.getrandbits(8), _i32(rng.getrandbits(32))]))
            if rng.random() < 0.6:   # just read a snapshot: its message is the last one received
                c.put(lay["msg_seq"], c.get(lay["snap"] + 12))
            # The connection: reliable commands not yet acknowledged, the last server command.
            c.put(lay["challenge"], _i32(rng.getrandbits(32)))
            c.put(lay["checksum_feed"], _i32(rng.getrandbits(32)))
            sequence = rng.choice([0, 3, rng.randint(0, 1000), rng.getrandbits(31)])
            pending = rng.choice([0, 0, 1, 1, 2, 3, 5, 8, -1, -3]) if rng.random() < 0.97 else ring + 3
            c.put(lay["reliable_seq"], sequence)
            c.put(lay["reliable_ack"], sequence - pending)
            for k in range(max(0, min(pending, ring))):
                n = sequence - k
                length = rng.choice([0, 3, 20, 100, rng.randint(0, 400), 1023]) if pending < ring else 10
                c.put_bytes(lay["reliable_commands"] + (n % ring) * 0x400,
                            gen.text(rng, length, TEXT, min_len=length) + b"\0")
            command = c.get(lay["cmd_seq"])
            c.put_bytes(lay["commands"] + (command % ring) * 0x400,
                        gen.text(rng, rng.choice([0, 5, 40, 200]), TEXT) + b"\0")
            c.put(lay["demo_playing"], 1 if rng.random() < 0.03 else 0)
            c.put(lay["last_sent"], rng.getrandbits(20))
            chan = random_chan(rng, net, sock=0)
            chan.numbers["unsent_fragments"] = 0 if rng.random() < 0.9 else 1
            state = rng.choice(states)
            dup = rng.choice([0, 0, 1, 2, 5])
            yield [state, bytes(c.cl), bytes(c.clc[:send_clc_size(image)]), chan, rng.randint(0, 1 << 30), dup,
                   1 if rng.random() < 0.1 else 0, None]
    return cases


# --- CL_PacketEvent ---------------------------------------------------------------------------

def scramble(data: bytes, key: int, text: bytes) -> bytes:
    """The netchan's XOR scrambling (iw_pc::net::netchan::scramble, checked
    against the four originals): the key runs through the text's characters."""
    text = bytes(text).split(b"\0")[0]
    out, index = bytearray(data), 0
    for i in range(len(out)):
        if index >= len(text):
            index = 0
        c = text[index] if index < len(text) else 0
        key = (key ^ (c << (i & 1))) & 0xFF
        out[i] ^= key
        index += 1
    return bytes(out)


@dataclass
class ServerPacket(Arg):
    """A packet as CL_PacketEvent gets it, in a msg_t. The value is a dict:
    `raw` (the whole packet as it is), or `sequence`, `acknowledge`, `body`
    (the server message's plain bytes), `key` and `text` (the scrambling):
    the body is Huffman-coded by the original's MSG_WriteBitsCompress (its
    port is checked to code the same), scrambled, and put after the two
    numbers. The packet built is kept for the token (Msg's layout: header
    words, then the data; a b: token, compared after the call)."""
    slack: int = 64
    mutable = True
    last: bytes = b""

    def packet(self, oracle, value) -> bytes:
        if "raw" in value:
            return bytes(value["raw"])
        plain = bytes(value["body"])
        source = oracle.put(plain + bytes(64), 16)
        coded = oracle.alloc(4 * len(plain) + 64, 16)
        compress = oracle.images[0].resolve("MSG_WriteBitsCompress")
        length = oracle.call(compress, {"eax": source, "ebx": coded, "ecx": len(plain)},
                             max_seconds=SETUP_SECONDS).i32
        body = scramble(oracle.read(coded, length), value["key"], value["text"])
        return struct.pack("<ii", _i32(value["sequence"]), _i32(value["acknowledge"])) + body

    def words(self, oracle, value):
        self.last = self.packet(oracle, value)
        data = self.last + bytes(self.slack)
        data_ptr = oracle.put(data, 16)
        return [oracle.put(struct.pack("<iIiiii", 0, data_ptr, len(self.last), len(self.last), 0, 0), 16)]

    def token(self, value):
        size = len(self.last)
        return "b:" + (struct.pack(">5i", 0, size, size, 0, 0) + self.last + bytes(self.slack)).hex()

    def read_back(self, oracle, pointer, value):
        overflowed, data_ptr, maxsize, cursize, readcount, bit = struct.unpack("<iIiiii", oracle.read(pointer, 24))
        return (struct.pack(">5i", overflowed, maxsize, cursize, readcount, bit)
                + oracle.read(data_ptr, len(self.last) + self.slack))


class RetEvent(Ret):
    """CL_PacketEvent's result: the stop reached (CL_ConnectionlessPacket,
    the parser's, the demo writer), else Call of Duty 2's bool (i:) or
    United Offensive's "return"."""

    def __init__(self, image: str):
        self.cod2 = image == "cod2mp"

    def from_guest(self, oracle, result):
        if result.stopped:
            return "s:" + result.stopped.encode().hex()
        return f"i:{result.eax & 0xFF}" if self.cod2 else "s:" + b"return".hex()


def _packet_event_setup(image: str):
    lay, net = L[image], NET[image]
    parse_setup = _setup(image)

    def setup(oracle, values):
        parse_setup(oracle, values)
        fake_settings(oracle, net)
    return setup


def _packet_event_cases(image: str):
    lay, net = L[image], NET[image]
    ring = lay["commands_count"]
    ops = OPS[image]

    def cases(rng):
        for _ in forever:
            c, message, kept = history(rng, image)
            chan = random_chan(rng, net, sock=0)
            chan.numbers.update(unsent_fragments=0, fragment_length=0)
            server = chan.address
            incoming = message - rng.choice([1, 1, 1, 2, 5])
            chan.numbers["incoming"] = incoming
            c.put(lay["challenge"], _i32(rng.getrandbits(32)))
            reliable = rng.choice([0, 3, rng.randint(0, 1000), rng.getrandbits(31)])
            c.put(lay["reliable_seq"], reliable)
            c.put(lay["reliable_ack"], reliable - rng.randint(0, 5))
            for k in range(rng.randint(0, 6)):
                n = reliable - k
                c.put_bytes(lay["reliable_commands"] + (n % ring) * 0x400,
                            gen.text(rng, rng.choice([0, 5, 40, 300]), TEXT) + b"\0")
            c.put(lay["demo_recording"], 1 if rng.random() < 0.04 else 0)
            state = lay["connected"] + rng.choice([0, 1, 2, 3]) if rng.random() < 0.93 else lay["connected"] - 1
            sender = server if rng.random() < 0.95 else random_address(rng)
            k = rng.random()
            if k < 0.05:
                value = {"raw": b"\xff\xff\xff\xff" + gen.text(rng, 30, TEXT)}
            elif k < 0.09:
                value = {"raw": bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 3)))}
            elif k < 0.13 and image == "cod2mp":
                value = {"raw": struct.pack("<i", message) + bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 3)))}
            else:
                sequence = message if rng.random() < 0.9 else rng.choice([incoming, incoming - 3, message + 40])
                acknowledge = reliable - (rng.choice([0] * 6 + [1, 2, ring - 1, ring]) if rng.random() < 0.95
                                          else rng.choice([ring + 1, 500]))
                w = TracedWire()
                snapshots = 0
                for _ in range(rng.choice([0, 1, 1, 2])):
                    q = rng.random()
                    if 0.4 <= q < 0.9 and snapshots:
                        q = 0.3   # one snapshot a message (see _server_message_cases)
                    w.bit = 0
                    if q < 0.4:
                        w.byte(ops["server_command"])
                        command_body(w, rng, c.get(lay["cmd_seq"]) + rng.choice([1, 2, 0]))
                    elif q < 0.9:
                        w.byte(ops["snapshot"])
                        put_snapshot(w, rng, image, message, kept)
                        snapshots += 1
                    elif q < 0.95:
                        w.byte(ops["game_state"])
                        game_state_body(w, rng, image)
                    else:
                        w.byte(ops["nop"])
                if rng.random() < 0.95:
                    w.bit = 0
                    w.byte(ops["eof"])
                if not fits_message(image, len(w.data)):
                    continue
                slot = lay["reliable_commands"] + (acknowledge % ring) * 0x400 - lay["clc"]
                text = bytes(c.clc[slot:slot + 0x400])
                key = (c.get(lay["challenge"]) ^ sequence) & 0xFF
                value = {"sequence": sequence, "acknowledge": acknowledge, "body": bytes(w.data), "key": key,
                         "text": text}
            yield [value, sender, 0, state, bytes(c.cl), bytes(c.clc[:send_clc_size(image)]), chan,
                   rng.randint(0, 1 << 30)]
    return cases


# --- CL_ConnectionlessPacket -----------------------------------------------------------------

@dataclass
class GlobalInt(Arg):
    """An integer the function reads from a global (`address`; None: the
    game has no such global and the answerer gets the value as it is)."""
    address: int | None = None
    at: str = "none"

    def words(self, oracle, value):
        if self.address is not None:
            oracle.write(self.address, struct.pack("<i", _i32(value)))
        return [self.address or 0]

    def token(self, value):
        return f"i:{value}"


def case_mix(rng, word: bytes) -> bytes:
    return gen.case_variant(rng, word) if rng.random() < 0.3 else word


def connectionless_text(rng, image: str, lay) -> bytes:
    """A packet's text: the commands a client takes, with their words, cases
    and edges; some unknown."""
    cod2 = image == "cod2mp"
    words = [b"challengeResponse", b"connectResponse", b"infoResponse", b"statusResponse", b"disconnect",
             b"echo", b"keyAuthorize", b"print", b"error", b"updateResponse", b"getserversResponse",
             b"needcdkey", b"loadingnewmap"] + ([b"v"] if cod2 else [b"motd"])
    k = rng.random()
    if k < 0.08:
        first = rng.choice([b"", b"hello", b"challenge", b"connectResponses", b"getservers", b"needcd",
                            b"getserversResponseX", b"needcdkeyplease", b"\"" + b"print" + b"\""])
    else:
        first = case_mix(rng, rng.choice(words))
        if first.lower().startswith(b"getserversresponse") and rng.random() < 0.3:
            first += gen.text(rng, 8, gen.LETTERS)
    line = first
    lower = first.lower()
    if lower == b"challengeresponse":
        line += b" " + rng.choice([b"0", b"-1", b"12345", str(rng.getrandbits(31)).encode(), b"abc", b"99999999999"])
        if rng.random() < 0.5:
            line += b" " + rng.choice([b"0", b"1", b"7", b"x"])
    elif lower == b"echo":
        line += rng.choice([b"", b" hi", b" \"two words\"", b" " + gen.text(rng, 60, TEXT)])
    elif rng.random() < 0.3:
        line += b" " + gen.text(rng, 30, TEXT)
    text = line
    if lower in (b"print", b"error") or rng.random() < 0.2:
        text += b"\n" + gen.text(rng, rng.choice([0, 20, 300, 2000]), TEXT + b"\n")
    if lower == b"loadingnewmap":
        text += b"\n" + rng.choice([b"mp_toujane", b"mp_" + gen.text(rng, 90, gen.LETTERS)]) + b"\n" + \
            gen.text(rng, 20, TEXT)
    return b"\xff\xff\xff\xff" + text


def _connectionless_cases(image: str):
    lay, net = L[image], NET[image]
    connecting, challenging, connected = lay["joining"]

    def cases(rng):
        for _ in forever:
            c = Client(image)
            server = random_address(rng)
            c.put_bytes(lay["server_address"], net.adr_to_guest(server))
            c.put(lay["challenge"], _i32(rng.getrandbits(32)))
            c.put(lay["connect_count"], rng.randint(0, 9))
            c.put(lay["connect_time"], rng.randint(-99999, 1 << 20))
            c.put_bytes(lay["server_message"], gen.text(rng, 40, TEXT) + b"\0")
            if lay["only_visible"] is not None:
                c.put(lay["only_visible"], rng.choice([0, 0, 1]))
            c.put(lay["last_packet_time"], rng.getrandbits(20))
            c.put(lay["last_sent"], rng.getrandbits(20))
            state = rng.choice([connecting, connecting, challenging, challenging, connected, connected + 2, 0, 1, 9])
            k = rng.random()
            if k < 0.6:
                sender = server
            elif k < 0.75:   # the same host, another port: the same base address
                sender = server[:8] + struct.pack(">H", rng.randint(1024, 65535)) + server[10:]
            else:
                sender = random_address(rng)
            packet = connectionless_text(rng, image, lay)
            while packet[4:7].lower() == b"pb_":
                packet = connectionless_text(rng, image, lay)
            cursize = len(packet) if rng.random() < 0.97 else rng.randint(0, 3)
            msg = (rng.choice([0, 0, 1]), packet, len(packet) + rng.choice([0, 16]), cursize,
                   rng.choice([0, 4, 7]), rng.choice([0, 9]))
            chan = random_chan(rng, net, sock=0)
            yield [sender, msg, 0, state, bytes(c.clc[:send_clc_size(image)]), chan, rng.randint(0, 1 << 30),
                   rng.choice([0, 0x1234, rng.getrandbits(16)]) if lay["qport"] is not None else 0, None]
    return cases



# --- CL_CheckForResend, Sys_IsLANAddress -------------------------------------------------

RESEND = {
    "cod2mp": dict(
        # cls.qport: the copy of net_qport CL_Connect takes (the connect text's qport).
        qport=0x00609FE4,
        lan_authorize=0x00C8FB14, lan_authorize_offset=8,          # net_lanauthorize (a bool dvar: the byte)
        local_count=0x00D75938, local_table=0x00D758F8,            # numIP, localIP[16][4]
        authorize=0x0040D130,                                      # CL_RequestAuthorization
        cd_key=0x005CEFB4, key_hash=0x0040CFC0,                    # cl_cdkey, CL_BuildMd5StrFromCDKey
        info_string=0x0043A9D0, dvar_list=0x00C5C5C4, name_dvar=0x0096B624,
        modified_flags=0x00C5C580, punkbuster_hook=0x005DD860),
    "uomp": dict(
        qport=None,                                                # the net_qport cvar, found by name
        lan_authorize=0x04927D34, lan_authorize_offset=0x20,
        local_count=0x009CF358, local_table=0x009CF318,
        authorize=0x00410BB0,
        info_string=0x0043ED30, cvar_list=0x04927E98, cvar_hash=0x009987A0, hash_value=0x0043D860,
        modified_flags=0x04927E9C, punkbuster_hook=0x005CEF20),
}
MAX_LOCAL_ADDRESSES = 16


@dataclass
class LocalAddresses(Arg):
    """The machine's own IPv4 addresses (Sys_IsLANAddress's localIP table and
    count, globals): a list of 4-byte addresses, v: token."""
    image: str = "cod2mp"
    at: str = "none"

    def words(self, oracle, value):
        r = RESEND[self.image]
        oracle.write(r["local_table"], b"".join(bytes(a) for a in value).ljust(4 * MAX_LOCAL_ADDRESSES, b"\0"))
        oracle.write(r["local_count"], struct.pack("<i", len(value)))
        return [r["local_table"]]

    def token(self, value):
        return "v:" + b"".join(bytes(a) for a in value).hex()


@dataclass
class UoCvarValue(Arg):
    """United Offensive's net_qport, which CL_CheckForResend looks up by
    name (Cvar_FindVar) and reads as its float value: a stand-in cvar
    hooked into the cvar hash table (the guest's generateHashValue picks
    the bucket), or none at all (value None: 0). i: token."""
    name: bytes = b"net_qport"
    at: str = "none"

    def words(self, oracle, value):
        if value is None:
            return [0]
        r = RESEND["uomp"]
        name = oracle.put(self.name + b"\0", 16)
        bucket = oracle.call(r["hash_value"], {"eax": name}).u32
        cvar = bytearray(0x30)   # name, string, ..., value (float) +0x1C, integer +0x20, hashNext +0x28
        struct.pack_into("<I", cvar, 0, name)
        struct.pack_into("<I", cvar, 4, oracle.put(str(value).encode() + b"\0", 16))
        struct.pack_into("<fi", cvar, 0x1C, float(value), value)
        oracle.write(r["cvar_hash"] + 4 * bucket, struct.pack("<I", oracle.put(bytes(cvar), 16)))
        return [0]

    def token(self, value):
        return f"i:{value or 0}"


@dataclass
class GuestUserinfo(Arg):
    """The player's settings as the game builds them for `connect`
    (Call of Duty 2's Dvar_InfoString(2), United Offensive's
    Cvar_InfoString(2)), from stand-in variables: Call of Duty 2's from its
    name dvar (the value's `vars`: the name, no other dvars), United
    Offensive's from a list of cvars (`vars`: (name, string, flags)
    tuples). The info string the original builds from them (a call before
    the case's) is the token the answerer gets (s:).

    An info string that ends up 1023 or 1024 characters long with the
    three pairs CL_CheckForResend adds (`pairs`: their lengths, in order)
    overflows the original's 1032-byte packet buffer into its saved frame
    pointer (`connect "` and `"` around it): the longest variable is then
    cut until it doesn't (iw_pc::client::check_for_resend)."""
    image: str = "cod2mp"
    at: str = "none"
    last: bytes = b""

    def _build(self, oracle, variables) -> int:
        r = RESEND[self.image]
        if self.image == "cod2mp":
            oracle.write(r["dvar_list"], struct.pack("<I", 0))
            dvar = bytearray(0x40)
            struct.pack_into("<I", dvar, 8, oracle.put(bytes(variables) + b"\0", 16))
            oracle.write(r["name_dvar"], struct.pack("<I", oracle.put(bytes(dvar), 16)))
            return oracle.call(r["info_string"], stack=[2]).u32
        head = 0
        for name, string, flags in reversed(variables):   # the list's first cvar is the first one added
            cvar = bytearray(0x30)   # name, string, ..., flags +0x10, next +0x24
            struct.pack_into("<II", cvar, 0, oracle.put(name + b"\0", 16), oracle.put(string + b"\0", 16))
            struct.pack_into("<I", cvar, 0x10, flags)
            struct.pack_into("<I", cvar, 0x24, head)
            head = oracle.put(bytes(cvar), 16)
        oracle.write(r["cvar_list"], struct.pack("<I", head))
        return oracle.call(r["info_string"], {"edi": 2}).u32

    @staticmethod
    def _final_length(length: int, pairs) -> int:
        length = min(length, 1023)   # strncpy(info, userinfo, 1023)
        for pair in pairs:
            if length + pair <= 1024:
                length += pair
        return length

    def words(self, oracle, value):
        variables = value["vars"]
        while True:
            pointer = self._build(oracle, variables)
            self.last = oracle.read_cstr(pointer, 0x2000)
            if self._final_length(len(self.last), value["pairs"]) <= 1022:
                return [pointer]
            if self.image == "cod2mp":
                variables = variables[:-40]
            else:
                k = max(range(len(variables)), key=lambda i: len(variables[i][1]))
                name, string, flags = variables[k]
                variables = variables[:k] + [(name, string[:-40], flags)] + variables[k + 1:]

    def token(self, value):
        return "s:" + self.last.hex()


@dataclass
class GuestKeyHash(Arg):
    """Call of Duty 2's `getchallenge` carries a hash of the player's key,
    which the port doesn't compute (iw_pc::client::ResendSettings): the
    value is a stand-in key put in cl_cdkey, and the token the hash the
    original's CL_BuildMd5StrFromCDKey makes of it (called before the
    case's call). United Offensive sends no key: the value as it is."""
    image: str = "cod2mp"
    at: str = "none"
    last: bytes = b""

    def words(self, oracle, value):
        if self.image != "cod2mp":
            self.last = bytes(value)
            return [0]
        r = RESEND["cod2mp"]
        oracle.write(r["cd_key"], bytes(value)[:32].ljust(33, b"\0"))
        out = oracle.alloc(0x100, 16)
        oracle.call(r["key_hash"], stack=[out])
        self.last = oracle.read_cstr(out, 0x100)
        return [0]

    def token(self, value):
        return "s:" + self.last.hex()


def lan_ip(rng) -> list[int]:
    return rng.choice([[10, rng.randint(0, 255), rng.randint(0, 255), rng.randint(1, 254)],
                       [127, 0, 0, 1], [169, 254, rng.randint(0, 255), 9], [169, 253, 1, 1],
                       [172, rng.randint(14, 33), 0, 5], [192, 168, rng.randint(0, 255), 7],
                       [192, 169, 1, 1], [191, 168, 1, 1]])


def local_addresses(rng, near: list[int] | None = None) -> list[list[int]]:
    n = rng.choice([0, 0, 1, 1, 2, 3, MAX_LOCAL_ADDRESSES])
    out = [[rng.randint(1, 223), rng.randint(0, 255), rng.randint(0, 255), rng.randint(1, 254)] for _ in range(n)]
    if near and out and rng.random() < 0.5:   # the same /24 as the address checked, or nearly
        k = rng.randrange(n)
        out[k] = list(near[:3]) + [rng.randint(1, 254)]
        if rng.random() < 0.3:
            out[k][rng.randrange(3)] ^= 1
    return out


def checked_address(rng) -> tuple[bytes, list[int]]:
    """A canonical netadr_t for Sys_IsLANAddress: every type, IPv4 ones on
    and off the private ranges."""
    kind = rng.choice([4, 4, 4, 4, 4, 0, 1, 2, 3, 5, 6])
    ip = lan_ip(rng) if rng.random() < 0.5 else [rng.randint(1, 223), rng.randint(0, 255), rng.randint(0, 255),
                                                 rng.randint(1, 254)]
    return (struct.pack("<I", kind) + bytes(ip) + struct.pack(">H", rng.randint(1, 65535))
            + bytes(rng.getrandbits(8) for _ in range(10))), ip


def _lan_cases(image: str):
    def cases(rng):
        for _ in forever:
            address, ip = checked_address(rng)
            yield [address, local_addresses(rng, ip)]
    return cases


def userinfo_value(rng, image: str):
    """Call of Duty 2: a player name (some too long for the string, some
    with the characters an info string refuses); United Offensive: cvars."""
    text = TEXT.replace(b"\\", b"").replace(b";", b"").replace(b"\"", b"")
    if image == "cod2mp":
        length = rng.choice([0, 5, 15, 40, rng.randint(0, 1100), rng.randint(960, 1030), rng.randint(990, 1017),
                             rng.randint(1000, 1017)])
        name = gen.text(rng, length, text, min_len=length)
        if rng.random() < 0.05:
            name += rng.choice([b"\\", b";", b"\""])
        return name
    cvars = []
    for _ in range(rng.choice([0, 1, 3, 8, 20])):
        name = gen.text(rng, rng.randint(1, 12), gen.LETTERS, min_len=1)
        length = rng.choice([0, 4, 20, rng.randint(0, 300)])
        cvars.append((name, gen.text(rng, length, text, min_len=length), rng.choice([2, 2, 2, 3, 0, 1])))
    if rng.random() < 0.5:   # one long one: the string near or past its 1024 characters
        length = rng.choice([rng.randint(900, 1030), rng.randint(960, 1010)])
        cvars.append((b"name", gen.text(rng, length, text, min_len=length), 2))
    return cvars


def _resend_setup(image: str):
    """The parsers' stand-ins, the network settings, and no PunkBuster."""
    net, r = NET[image], RESEND[image]
    parse_setup = _setup(image)

    def setup(oracle, values):
        parse_setup(oracle, values)
        fake_settings(oracle, net)
        oracle.write(r["punkbuster_hook"], struct.pack("<I", 0))
    return setup


def _resend_cases(image: str):
    lay = L[image]
    connecting, challenging, connected = lay["joining"]
    wait = 2000 if image == "cod2mp" else 3000

    def cases(rng):
        for _ in forever:
            c = Client(image)
            ip = lan_ip(rng) if rng.random() < 0.5 else [rng.randint(1, 223), rng.randint(0, 255),
                                                         rng.randint(0, 255), rng.randint(1, 254)]
            server = struct.pack("<I", 4) + bytes(ip) + struct.pack(">H", rng.choice([28960, rng.randint(1, 65535)])) \
                + bytes(10)
            c.put_bytes(lay["server_address"], NET[image].adr_to_guest(server))
            c.put(lay["challenge"], rng.choice([0, -1, _i32(rng.getrandbits(32))]))
            c.put(lay["connect_count"], rng.choice([0, 1, 5, rng.getrandbits(31), -1]))
            realtime = rng.choice([rng.randint(0, 1 << 30), rng.randint(0, 5000), _i32(rng.getrandbits(32))])
            since = rng.choice([wait, wait + 1, rng.randint(wait, 3 * wait), 99999 + realtime] * 3
                               + [wait - 1, 0, rng.randint(0, 3 * wait), _i32(rng.getrandbits(32))])
            c.put(lay["connect_time"], _i32(realtime - since))
            c.put(lay["demo_playing"], 1 if rng.random() < 0.05 else 0)
            c.put(lay["last_sent"], rng.getrandbits(20))
            c.put(lay["last_packet_time"], rng.getrandbits(20))
            state = rng.choice([connecting] * 8 + [challenging] * 10 + [connected, 0, connecting - 1, 9])
            qport = rng.choice([0, 0x1234, rng.getrandbits(16), rng.randint(-5, 5)])
            if image == "uomp" and rng.random() < 0.15:
                qport = None   # no net_qport cvar: 0
            key = gen.text(rng, rng.choice([0, 20, 32, 32, 40]), gen.LETTERS + gen.DIGITS + b" -_\t")
            challenge = c.get(lay["challenge"])
            pairs = [len(b"\\protocol\\") + len(str(118 if image == "cod2mp" else 22)),
                     len(b"\\challenge\\") + len(str(challenge)), len(b"\\qport\\") + len(str(qport or 0))]
            if image == "uomp":
                pairs[1], pairs[2] = pairs[2], pairs[1]
            userinfo = {"vars": userinfo_value(rng, image), "pairs": pairs}
            yield [state, bytes(c.clc[:send_clc_size(image)]), _i32(realtime), qport, userinfo,
                   key, 1 if rng.random() < 0.25 else 0, local_addresses(rng, ip),
                   rng.choice([0, 2, 0xFFFFFFFF, rng.getrandbits(32)]), None]
    return cases


# --- CL_SystemInfoChanged --------------------------------------------------------------------

SYSTEM_INFO_CALLS = {
    "cod2mp": dict(server_running=0x00C26108, running_offset=8,
                   cheats=0x00439220, loaded=0x0043C550, referenced=0x0043C810, find=0x004373A0,
                   set=0x00438CF0, register=0x00437DE0,
                   archives=(b"sv_iwds", b"sv_iwdNames", b"sv_referencedIwds", b"sv_referencedIwdNames")),
    "uomp": dict(server_running=0x04927ED4, running_offset=0x20,
                 cheats=0x0043E260, loaded=0x00441190, referenced=0x00441450, find=0x0043D8F0, set=0x0043DC50,
                 archives=(b"sv_paks", b"sv_pakNames", b"sv_referencedPaks", b"sv_referencedPakNames")),
}
_existing = {"names": set(), "dvar": 0}   # this case's existing settings (ExistingSettings)


def _find_setting(name: bytes) -> int:
    return _existing["dvar"] if name.lower() in _existing["names"] else 0


def system_info_stand_ins(image: str) -> dict:
    """The console variables and the file system CL_SystemInfoChanged
    calls, as stand-ins: Call of Duty 2's Dvar_FindVar finds the case's
    existing settings (ExistingSettings: a zeroed stand-in dvar, so a found
    sv_pure reads as off), United Offensive's Cvar_FindVar (sv_pure) none."""
    c = SYSTEM_INFO_CALLS[image]
    if image == "cod2mp":
        return {c["cheats"]: StandIn("cheats"),
                c["loaded"]: StandIn("loaded", (("ecx", "str"), (0, "str"))),
                c["referenced"]: StandIn("referenced", (("ecx", "str"), ("eax", "str"))),
                c["find"]: StandIn("find", ((0, "str"),), result=_find_setting),
                c["set"]: StandIn("set", ((0, "str"), (1, "int"))),
                c["register"]: StandIn("register", ((0, "str"), (1, "int"), (2, "int"), (3, "str"), (4, "int"),
                                                    (5, "int")))}
    return {c["cheats"]: StandIn("cheats"),
            c["loaded"]: StandIn("loaded", ((0, "str"), (1, "str"))),
            c["referenced"]: StandIn("referenced", (("eax", "str"), ("ecx", "str"))),
            c["find"]: StandIn("find", ((0, "str"),)),
            c["set"]: StandIn("set", ((0, "str"), (1, "str"), (2, "int")))}


class ExistingSettings(Arg):
    """The settings the client already has (Call of Duty 2's Dvar_FindVar
    finds them, ignoring case): a list of names, v: token (each name and a
    0 byte)."""
    at = "none"

    def words(self, oracle, value):
        _existing["names"] = {bytes(n).lower() for n in value}
        _existing["dvar"] = oracle.alloc(0x40, 16)
        return [0]

    def token(self, value):
        return "v:" + b"".join(bytes(n) + b"\0" for n in value).hex()


def system_info_text(rng, image: str) -> tuple[bytes, list[bytes]]:
    """A system info string as servers send it (their keys, numbers, archive
    lists), with edges: leading, doubled and trailing backslashes, empty
    values, a last key without one, keys in other cases; some too long."""
    archives = SYSTEM_INFO_CALLS[image]["archives"]
    numbers = lambda: b" ".join(str(_i32(rng.getrandbits(32))).encode() for _ in range(rng.randint(0, 6)))
    names = lambda: b" ".join(b"main/iw_" + str(rng.randint(0, 15)).zfill(2).encode() for _ in range(rng.randint(0, 6)))
    pairs = [(b"sv_serverid", rng.choice([str(rng.randint(0, 999999)).encode(), b"-5", b" 42x", b"", b"abc",
                                         str(rng.getrandbits(40)).encode()])),
             (b"sv_cheats", rng.choice([b"0", b"1", b"", b"2", b" 0", b"x"])),
             (archives[0], numbers()), (archives[1], names()), (archives[2], numbers()), (archives[3], names()),
             (b"sv_pure", rng.choice([b"0", b"1"])), (b"g_gametype", rng.choice([b"tdm", b"dm", b"sd"])),
             (b"sv_voice", b"0"), (b"cl_wwwDownload", b"1")]
    for _ in range(rng.choice([0, 1, 3, 10])):
        pairs.append((gen.text(rng, rng.randint(1, 16), gen.LETTERS + gen.DIGITS + b"_", min_len=1),
                      gen.text(rng, rng.choice([0, 3, 20, 200]), TEXT.replace(b"\\", b""))))
    rng.shuffle(pairs)
    pairs = [p for p in pairs if rng.random() < 0.9]
    pairs = [(gen.case_variant(rng, k) if rng.random() < 0.1 else k, v) for k, v in pairs]
    text = b"".join(b"\\" + k + b"\\" + v for k, v in pairs)
    k = rng.random()
    if k < 0.05:
        text = text[1:]
    elif k < 0.1:
        text += b"\\"
    elif k < 0.15:
        text += b"\\lastkey"
    elif k < 0.2:
        text = text.replace(b"\\", b"\\\\", 1)
    elif k < 0.23:
        text = b""
    elif k < 0.27:   # near and past Info_ValueForKey's 8192 characters
        text += b"\\pad\\" + b"p" * (rng.choice([8180, 8186, 8187, 8200]) - len(text) - 5)
    existing = [k for k, _ in pairs if rng.random() < 0.3] + [b"sv_PURE"] * (rng.random() < 0.2)
    return text, existing


def _system_info_cases(image: str):
    lay = L[image]

    def cases(rng):
        for _ in forever:
            c = Client(image)
            text, existing = system_info_text(rng, image)
            at = rng.choice([0, 1, 300, 5000])
            c.put(lay["gs_offsets"] + 4 * 1, at)
            c.put_bytes(lay["gs_data"] + at, text + b"\0")
            c.put(lay["gs_count"], at + len(text) + 1)
            c.put(lay["server_id"], _i32(rng.getrandbits(32)))
            c.put(lay["demo_playing"], 1 if rng.random() < 0.05 else 0)
            yield [bytes(c.cl), bytes(c.clc[:send_clc_size(image)]), 1 if rng.random() < 0.1 else 0, existing, None]
    return cases


# --- CL_SetCGameTime (and CL_FirstSnapshot, CL_AdjustTimeDelta) --------------------------------

CLOCK = {
    "cod2mp": dict(
        # cl_paused, sv_paused (as Dvar_RegisterBool leaves them: 0x00434308, 0x004342BC).
        server_name=0x0068A410, cl_paused=(0x00C260F8, 8), sv_paused=(0x00C27284, 8),
        server_running=(0x00C26108, 8), freeze_demo=(0x0096B604, 8), show_time_delta=0x0096B608,
        timescale=0x00C2610C,                       # com_timescale's value, a global float
        time_nudge=None, time_demo=None,            # no cl_timeNudge; clc.timeDemo is in clc
        active_action=(0x0068A3BC, 8),              # cl_activeAction: a string dvar, its text at +8
        read_demo=0x0040C5B0, time_demo_frame=0x00402F90,
        cbuf=0x00420AD0, clear_action=0x00438900),
    "uomp": dict(
        # cl_paused, sv_paused (as Cvar_Get leaves them: 0x0043BF94, 0x0043BF7B).
        server_name=0x04AD3D6C, cl_paused=(0x04927EC4, 0x20), sv_paused=(0x04929068, 0x20),
        server_running=(0x04927ED4, 0x20), freeze_demo=(0x04DF96AC, 0x20), show_time_delta=0x04DF96B0,
        timescale=(0x04927ECC, 0x1C),               # the timescale cvar's float value
        time_nudge=(0x04E19948, 0x20), time_demo=(0x04DF96A0, 0x20),
        active_action=(0x04AD3CE8, 4),              # the activeAction cvar: its text at +4
        read_demo=0x00410060, time_demo_frame=0x00405770,
        cbuf=0x0042BC10, clear_action=0x0043DC50),
}


@dataclass
class GlobalCStr(Arg):
    """A string kept in a global array (`size` bytes, zeros after it); s: token."""
    address: int = 0
    size: int = 256
    at: str = "none"

    def words(self, oracle, value):
        oracle.write(self.address, bytes(value)[:self.size - 1].ljust(self.size, b"\0"))
        return [self.address]

    def token(self, value):
        return f"s:{bytes(value)[:self.size - 1].hex()}"


@dataclass
class FloatSetting(Arg):
    """A float the function reads, given as its bits: at a global
    (`pointer` None) or in a stand-in variable (`pointer`, `offset`); f: token."""
    address: int = 0
    pointer: int | None = None
    offset: int = 0
    at: str = "none"

    def words(self, oracle, value):
        if self.pointer is None:
            oracle.write(self.address, struct.pack("<I", value))
            return [self.address]
        block = bytearray(0x60)
        struct.pack_into("<I", block, self.offset, value)
        address = oracle.put(bytes(block), 16)
        oracle.write(self.pointer, struct.pack("<I", address))
        return [address]

    def token(self, value):
        return f"f:{value:08x}"


@dataclass
class TextSetting(Arg):
    """A string setting the function reads: a stand-in variable whose text
    pointer is at `offset`; s: token."""
    pointer: int = 0
    offset: int = 8
    at: str = "none"

    def words(self, oracle, value):
        block = bytearray(0x60)
        struct.pack_into("<I", block, self.offset, oracle.put_cstr(bytes(value)))
        address = oracle.put(bytes(block), 16)
        oracle.write(self.pointer, struct.pack("<I", address))
        return [address]

    def token(self, value):
        return f"s:{bytes(value).hex()}"


def clock_stand_ins(image: str) -> dict:
    """The console's command buffer and the activeAction setting, which
    CL_FirstSnapshot runs and clears."""
    c = CLOCK[image]
    if image == "cod2mp":
        return {c["cbuf"]: StandIn("cbuf", (("eax", "str"),)),
                c["clear_action"]: StandIn("clear", (("eax", "str"), (0, "int")))}
    return {c["cbuf"]: StandIn("cbuf", (("eax", "str"),)),
            c["clear_action"]: StandIn("set", ((0, "str"), (1, "str"), (2, "int")))}


def _clock_setup(image: str):
    c = CLOCK[image]
    parse_setup = _setup(image)

    def setup(oracle, values):
        parse_setup(oracle, values)
        oracle.write(c["show_time_delta"], struct.pack("<I", oracle.alloc(0x80, 16)))
    return setup


FLOAT_BITS = dict(one=0x3F800000, zero=0, minus_zero=0x80000000, half=0x3F000000, nan=0x7FC00000, two=0x40000000)


def _clock_cases(image: str):
    lay = L[image]
    loading, primed, active = (6, 7, 8) if image == "cod2mp" else (4, 5, 6)

    def cases(rng):
        for _ in forever:
            c = Client(image)
            realtime = rng.choice([rng.randint(0, 1 << 24), rng.randint(0, 1 << 30), _i32(rng.getrandbits(32))])
            delta = rng.choice([rng.randint(-5000, 5000), rng.randint(-(1 << 30), 1 << 30), 0])
            server_time = _i32(realtime + delta + rng.choice([0, 3, -3, 50, -50, 4, 5, 6, 200, -200, 600, -600,
                                                              rng.randint(-2000, 2000)]))
            snap_time = _i32(server_time + rng.choice([0, 5, 4, 6, 50, 100, -10, rng.randint(-700, 700)]))
            c.put(lay["snap"], 1 if rng.random() < 0.95 else 0)               # valid
            c.put(lay["snap"] + 4, rng.choice([0, 0, 0, 2, 1, 3]))               # snapFlags
            c.put(lay["snap"] + 8, snap_time)
            between = rng.choice([50, 50, 0, 1, 500, 501, 1000, -50, rng.randint(-100, 2000)])
            c.put(lay["old_frame"], _i32(snap_time - between))
            c.put(lay["new_snapshots"], rng.choice([0, 1, 1]))
            c.put(lay["server_time"], server_time)
            c.put(lay["old_server_time"], _i32(server_time + rng.choice([0, -1, 1, -100, 100, 1000])))
            c.put(lay["frame_server_time"], _i32(snap_time - rng.choice([50, 0, 1, -1, -50, 2000])))
            c.put(lay["server_time_delta"], delta)
            c.put(lay["extrapolated"], rng.choice([0, 0, 1]))
            c.put(lay["demo_playing"], 1 if rng.random() < 0.15 else 0)
            c.put(lay["first_demo_frame_skipped"], rng.choice([0, 1, 1]))
            c.put(lay["time_demo_base"], _i32(rng.getrandbits(32)))
            if image == "cod2mp":
                c.put(lay["time_demo"], 1 if rng.random() < 0.1 else 0)
            state = rng.choice([primed] * 3 + [active] * 6 + [loading, 5 if image == "cod2mp" else 3, 0])
            name = rng.choice([b"192.168.1.10", b"localhost", b"LocalHost", b"localhost:28960", b"", b"local"])
            flag = lambda p: 1 if rng.random() < p else 0
            paused = (flag(0.2), flag(0.2), flag(0.3))
            if rng.random() < 0.05:
                paused = (1, 1, 1)
            timescale = rng.choice([FLOAT_BITS["one"]] * 6 + list(FLOAT_BITS.values()))
            nudge = rng.choice([0, 0, 10, -10, 30, -30, 31, -31, 100, -100])
            action = rng.choice([b"", b"", b"say hi", b"+attack; wait; -attack"])
            yield [state, bytes(c.cl), bytes(c.clc[:send_clc_size(image)]), realtime, name, *paused,
                   flag(0.2), nudge, timescale, flag(0.1), action, None]
    return cases


# --- CL_FinishMove, CL_CreateNewCommands, CL_ReadyToSendPacket ------------------------------------

COMMANDS = {
    "cod2mp": dict(create_cmd=0x00408B00, frame_time=None, old_frame_time=None, frame_msec=None,
                   downloading=0x00966EB0, max_packets=(0x0096B5FC, 8)),
    "uomp": dict(create_cmd=0x0040C900, frame_time=0x04929090, old_frame_time=0x04E1CBD8, frame_msec=0x04E1CBE0,
                 downloading=0x04DC4508, max_packets=(0x04DF969C, 0x20), cvar_set=0x0043DC50),
}
_made = {"cmd": 0}   # this case's command from CL_CreateCmd (MadeCommand)


class Absent32(Arg):
    """A 32-bit global one game has and the other hasn't (here: Call of Duty
    2 has no input timing): a buffer token that must come back unchanged."""
    at = "none"
    mutable = True

    def words(self, oracle, value):
        return [1]

    def token(self, value):
        return "b:" + struct.pack(">I", value & 0xFFFFFFFF).hex()

    def read_back(self, oracle, pointer, value):
        return struct.pack(">I", value & 0xFFFFFFFF)


class MadeCommand(Arg):
    """The command the caller's CL_CreateCmd makes (a stand-in returns it):
    its bytes, v: token."""
    at = "none"

    def words(self, oracle, value):
        _made["cmd"] = oracle.put(bytes(value), 16)
        return [0]

    def token(self, value):
        return "v:" + bytes(value).hex()


def angle_bits(rng) -> int:
    k = rng.random()
    if k < 0.6:
        return f32_word(rng.uniform(-400, 400))
    if k < 0.75:
        return f32_word(rng.choice([0.0, -0.0, 90.0, -90.0, 180.0, 360.0, 359.999, 0.0027466, -0.0027466]))
    if k < 0.9:
        return f32_word(rng.uniform(-1e7, 1e7))
    return rng.choice([0x7F800000, 0xFF800000, 0x7FC00000, 0x7F7FFFFF, 0x00000001, rng.getrandbits(32)])


def put_move_state(c: Client, rng, image: str) -> None:
    """What CL_FinishMove reads: the client game's values, the angles, the times."""
    lay = c.lay
    c.put(lay["user_cmd_weapon"], rng.choice([0, 1, 7, 255, 256, 0x1234, -1]))
    if lay["user_cmd_offhand"]:
        c.put(lay["user_cmd_offhand"], rng.choice([0, 1, 3, 255, 0x100, -1]))
    for i in range(3):
        c.put(lay["cgame_angles"] + 4 * i, _i32(angle_bits(rng) if rng.random() < 0.5 else 0))
        c.put(lay["view_angles"] + 4 * i, _i32(angle_bits(rng)))
    snap_time = _i32(rng.getrandbits(32)) if rng.random() < 0.2 else rng.randint(0, 1 << 30)
    c.put(lay["snap"] + 8, snap_time)
    c.put(lay["server_time"], _i32(snap_time + rng.choice([0, 50, 4999, 5000, 5001, 9000, -100,
                                                            rng.randint(-(1 << 31), (1 << 31) - 1)])))


def _finish_move_cases(image: str):
    size = USERCMD[image]["size"]

    def cases(rng):
        for _ in forever:
            c = Client(image)
            put_move_state(c, rng, image)
            yield [bytes(rng.getrandbits(8) for _ in range(size)), bytes(c.cl)]
    return cases


def _create_commands_cases(image: str):
    lay = L[image]
    size = USERCMD[image]["size"]
    loading, primed, active = (6, 7, 8) if image == "cod2mp" else (4, 5, 6)

    def cases(rng):
        for _ in forever:
            c = Client(image)
            number = rng.choice([0, 5, 127, 128, rng.getrandbits(31), -1, 0x7FFFFFFF])
            c.put(lay["cmd_number"], number)
            for k in range(3):
                c.put_bytes(lay["cmds"] + ((number - k) % 128) * size, bytes(rng.getrandbits(8) for _ in range(size)))
            state = rng.choice([primed, active, active, loading, 5 if image == "cod2mp" else 3, 0, 9])
            frame_time = rng.randint(0, 1 << 30)
            old = _i32(frame_time - rng.choice([0, 16, 50, 199, 200, 201, 5000, -5]))
            yield [state, bytes(c.cl), frame_time, old, rng.randint(0, 300),
                   bytes(rng.getrandbits(8) for _ in range(size)), None]
    return cases


def ready_stand_ins(image: str) -> dict:
    """United Offensive's Cvar_Set2 for cl_maxpackets: the stand-in sets the
    integer the function reads next."""
    if image != "uomp":
        return {}
    pointer, offset = COMMANDS["uomp"]["max_packets"]

    def effect(oracle, name, value, force):
        cvar = struct.unpack("<I", oracle.read(pointer, 4))[0]
        oracle.write(cvar + offset, struct.pack("<i", int(value)))
    return {COMMANDS["uomp"]["cvar_set"]: StandIn("set", ((0, "str"), (1, "str"), (2, "int")), effect=effect)}


def _ready_cases(image: str):
    lay, net = L[image], NET[image]
    loading, primed, active = (6, 7, 8) if image == "cod2mp" else (4, 5, 6)
    quiet = (1, 2) if image == "cod2mp" else (7, 8)

    def cases(rng):
        for _ in forever:
            c = Client(image)
            realtime = rng.randint(0, 1 << 30)
            c.put(lay["last_sent"], _i32(realtime - rng.choice([0, 10, 49, 50, 51, 999, 1000, 1001, 5000, -10])))
            c.put(lay["demo_playing"], 1 if rng.random() < 0.05 else 0)
            chan = random_chan(rng, net, sock=0)
            k = rng.random()
            if k < 0.1:
                chan.address = struct.pack("<I", 2) + bytes(16)   # loopback
            elif k < 0.4:
                chan.address = struct.pack("<I", 4) + bytes(lan_ip(rng)) + struct.pack(">H", 28960) + bytes(10)
            out = chan.numbers["outgoing"]
            for j in range(32):
                c.put(lay["out_packets"] + 12 * j + 8, _i32(realtime - rng.choice([0, 9, 10, 11, 33, 34, 66, 67, 100,
                                                                                   1000, 5000, -5])))
            state = rng.choice([primed, active, active, active, loading, 5 if image == "cod2mp" else 3, *quiet, 0])
            max_packets = rng.choice([30, 30, 15, 100, 1, 14, 101, 1000, 60, 7]) if image == "uomp" \
                else rng.choice([30, 15, 100, 60, 1, 7, 1000])
            yield [state, bytes(c.cl), bytes(c.clc[:send_clc_size(image)]), chan, realtime,
                   rng.choice([0, 0, 0, 0x41]), local_addresses(rng, None), max_packets, None]
    return cases


def _adjust_cases(image: str):
    clock = _clock_cases(image)

    def cases(rng):
        for values in clock(rng):
            state, cl, clc, realtime, *_rest = values
            timescale = values[10]
            yield [cl, clc, realtime, timescale]
    return cases


def _first_snapshot_cases(image: str):
    clock = _clock_cases(image)

    def cases(rng):
        for values in clock(rng):
            state, cl, clc, realtime = values[:4]
            yield [state, cl, clc, realtime, values[12], None]
    return cases

# --- the client's reliable commands (CL_AddReliableCommand, CL_SendPureChecksums) ---------------

def reliable_ring(c: Client, rng, image: str) -> None:
    """The ring's two numbers, from empty to past full (the original errs
    only when more than the ring's size is unacknowledged), and old texts in
    the slots (the copy leaves the bytes after its terminator)."""
    lay = L[image]
    ring = lay["commands_count"]
    acknowledge = rng.choice([0, 1, 127, rng.getrandbits(31), -1, 0x7FFFFFF0, -0x80000000])
    gap = rng.choice([0, 1, 2, ring - 1, ring, ring, ring + 1, ring + 2, rng.randint(0, ring), -1, -ring,
                      rng.getrandbits(31)])
    sequence = _i32(acknowledge + gap)
    c.put(lay["reliable_ack"], _i32(acknowledge))
    c.put(lay["reliable_seq"], sequence)
    for k in range(rng.choice([0, 1, 3])):
        slot = (sequence + 1 - k) % ring
        c.put_bytes(lay["reliable_commands"] + 1024 * slot, bytes(rng.choice(TEXT) for _ in range(rng.randint(0, 1023))))


def reliable_command_text(rng) -> bytes:
    k = rng.random()
    if k < 0.05:
        return b""
    if k < 0.15:
        return bytes(rng.choice(TEXT) for _ in range(rng.choice([1023, 1024, 1025, rng.randint(1024, 3000)])))
    return bytes(rng.choice(TEXT) for _ in range(rng.randint(1, 200)))


def _add_reliable_cases(image: str):
    def cases(rng):
        for _ in forever:
            c = Client(image)
            reliable_ring(c, rng, image)
            yield [reliable_command_text(rng), bytes(c.clc[:send_clc_size(image)])]
    return cases


def _send_pure_cases(image: str):
    def cases(rng):
        for _ in forever:
            c = Client(image)
            reliable_ring(c, rng, image)
            feed, fake = checksum_numbers(rng)
            yield [random_search_paths(rng, image), feed, fake, bytes(c.clc[:send_clc_size(image)])]
    return cases

# --- the client game's server commands (CL_GetServerCommand, CL_ConfigstringModified) -------------

SERVER_COMMANDS = {
    "cod2mp": dict(last_executed=0x0062A128, tokens_text=0x00B18280, tokens_argv=0x00B17A80, tokens_argc=0x00B1A480,
                   big=0x005DF300, show=(0x0068A39C, 8), restart=0x00401AEB,
                   notify=0x00403C70, systeminfo=0x004139E0, stricmp=0x0058BEED, localize=0x004AB560,
                   localize_at="esi", reason=0x00539880, notify_lists=0x006017B0),
    "uomp": dict(last_executed=0x04DD89A4, tokens_text=0x009655C0, tokens_argv=0x00964DC0, tokens_argc=0x009677C0,
                 big=0x005D06E0, show=(0x04958194, 0x20), restart=0x00401937,
                 notify=0x00409340, systeminfo=0x00418B80, stricmp=None, localize=0x00470590, localize_at="eax",
                 reason=None, notify_lists=None),
}
BIG = 0x2000


@dataclass
class TokenWords(Arg):
    """The tokenizer's words after the call (Cmd_Argc, Cmd_Argv): argc
    (u32, big-endian), then each word's length (u32) and bytes."""
    image: str = ""
    at: str = "none"
    mutable = True

    def words(self, oracle, value):
        return [1]

    def token(self, value):
        return "b:"

    def read_back(self, oracle, pointer, value):
        t = SERVER_COMMANDS[self.image]
        argc = struct.unpack("<I", oracle.read(t["tokens_argc"], 4))[0]
        out = bytearray(struct.pack(">I", argc))
        for i in range(min(argc, 512)):
            word = oracle.read_cstr(struct.unpack("<I", oracle.read(t["tokens_argv"] + 4 * i, 4))[0])
            out += struct.pack(">I", len(word)) + word
        return bytes(out)


@dataclass
class TokenWord(Arg):
    """Word `index` of the caller's tokenizer (Cmd_Argv), placed in the
    test heap with argv pointing at it."""
    image: str = ""
    index: int = 1
    at: str = "none"

    def words(self, oracle, value):
        oracle.write(SERVER_COMMANDS[self.image]["tokens_argv"] + 4 * self.index,
                     struct.pack("<I", oracle.put_cstr(bytes(value))))
        return [0]

    def token(self, value):
        return f"s:{bytes(value).hex()}"


class RetCommand(Ret):
    """CL_GetServerCommand's result: true or false, or "restart" (a string
    token) when it reached the map restart's last call (a stop)."""

    def from_guest(self, oracle, result):
        if result.stopped is not None:
            return "s:" + result.stopped.encode().hex()
        return f"i:{result.i32}"


def server_command_stand_ins(image: str, get: bool) -> dict:
    t = SERVER_COMMANDS[image]
    out = {t["systeminfo"]: StandIn("systeminfo")}
    if not get:
        return out
    out[t["notify"]] = StandIn("notify")
    # The disconnect's messages: the localizer and the reason's formatter give back their
    # input (only the error's level is compared).
    out[t["localize"]] = StandIn("localize", ((t["localize_at"], "int"),), result=lambda p: p)
    if t["reason"]:
        out[t["reason"]] = StandIn("reason", (("ecx", "int"), (0, "int")), result=lambda text, fmt: fmt)
    if t["stricmp"]:
        out[t["stricmp"]] = StandIn("stricmp", ((0, "str"), (1, "str")),
                                    result=lambda a, b: 0 if a.lower() == b.lower() else 1)
    return out


def _server_command_setup(image: str):
    t = SERVER_COMMANDS[image]
    base = _setup(image)

    def setup(oracle, values):
        base(oracle, values)
        zero = oracle.alloc(0x80, 16)
        oracle.write(t["show"][0], struct.pack("<I", zero))
        if image == "cod2mp":
            # The console's notify lists (the map restart clears the third inline): none.
            oracle.write(t["notify_lists"], struct.pack("<I", oracle.alloc(0xB00, 16)))
        else:
            for pointer in (0x04E3CDB4, 0x04E3CDB8):
                oracle.write(pointer, struct.pack("<I", oracle.alloc(0x40, 16)))
            oracle.write(0x04E3CDC4, struct.pack("<I", 0))
    return setup


def put_game_state(c: Client, strings: dict[int, bytes]) -> None:
    """Config strings packed as the game keeps them: from offset 1 in index order."""
    lay = c.lay
    at = 1
    for i in sorted(strings):
        text = strings[i]
        if not text:
            continue
        c.put(lay["gs_offsets"] + 4 * i, at)
        c.put_bytes(lay["gs_data"] + at, text + b"\0")
        at += len(text) + 1
    c.put(lay["gs_count"], at)


def random_game_state(rng, image: str) -> dict[int, bytes]:
    limit = L[image]["gs_chars"]
    strings = {}
    k = rng.random()
    if k < 0.15:   # near the limit
        total = 1
        i = 0
        target = limit - rng.choice([1, 2, 10, 300, 1100])
        while total < target and i < MAX_CONFIG_STRINGS:
            n = min(rng.randint(1, 900), target - total - 1)
            if n <= 0:
                break
            strings[i] = bytes(rng.choice(TEXT[:-3]) for _ in range(n))
            total += n + 1
            i += rng.choice([1, 1, 2])
        return strings
    for i in rng.sample(range(MAX_CONFIG_STRINGS), rng.choice([0, 1, 3, 20, 60])) + [0, 1, 2]:
        if rng.random() < 0.8:
            strings[i] = bytes(rng.choice(TEXT[:-3]) for _ in range(rng.choice([1, 5, 30, rng.randint(1, 300)])))
    return strings


def config_string_text(rng, strings: dict[int, bytes], index: int) -> bytes:
    k = rng.random()
    if k < 0.2 and index in strings:
        return strings[index]                      # unchanged
    if k < 0.3:
        return b""
    return bytes(rng.choice(TEXT[:-3]) for _ in range(rng.choice([1, 4, 40, rng.randint(1, 900)])))


def config_index(rng) -> int:
    return rng.choice([0, 1, 1, 2, 5, 2047, rng.randint(0, 2047), rng.randint(0, 2047), 2048, -1, 99999])


def _server_command_text(rng, image: str, strings: dict[int, bytes], big_length: int) -> bytes:
    k = rng.random()
    word = lambda n: bytes(rng.choice(gen.LETTERS + gen.DIGITS) for _ in range(n))
    if k < 0.3:
        index = config_index(rng)
        text = config_string_text(rng, strings, index)
        quoted = b'"' + text.replace(b'"', b"") + b'"' if rng.random() < 0.7 else text.replace(b'"', b"")
        return rng.choice([b"d", b"d", b"dx"]) + b" " + str(index).encode() + b" " + quoted
    if k < 0.5:
        letter = rng.choice([b"x", b"y", b"z", b"z"])
        index = config_index(rng) if letter == b"x" else rng.randint(0, 2047)
        part = config_string_text(rng, strings, index)
        if letter != b"x" and rng.random() < 0.2:
            part = bytes(rng.choice(gen.LETTERS) for _ in range(max(0, BIG - big_length - rng.choice([0, 1, 2, 3]))))[:1000]
        if letter != b"x" and big_length == 0:
            # The part starts the big config string: never as a `z` command, which the original
            # would run again and again (forever, when it adds nothing).
            part = rng.choice([b"q", b"d ", b"e "]) + part
        return letter + b" " + str(index).encode() + b' "' + part.replace(b'"', b"") + b'"'
    if k < 0.62:
        return rng.choice([b"w", b'w "EXE_KICKED"', b'w "kicked by PunkBuster" PB', b"w reason pb",
                           b"w a b c", b'w ""', b"wx 5", b"w " + word(8)])
    if k < 0.7:
        return rng.choice([b"B", b"n", b"n 1234", b"Bogus", b"B x"])
    return rng.choice([b"", b" ", b"print \"hello\"", b"e 5", b"cp 1 2", b"b 0 \"x\"", b"\xe9t", b"A",
                       b"{", b"//d 1 x", b"\"d 1 x\"", word(1) + b" " + word(rng.randint(0, 20))])


def _get_server_command_cases(image: str):
    lay = L[image]
    ring = lay["commands_count"]

    def cases(rng):
        for _ in forever:
            c = Client(image)
            strings = random_game_state(rng, image)
            put_game_state(c, strings)
            size = USERCMD[image]["size"]
            c.put_bytes(lay["cmds"], bytes(rng.getrandbits(8) for _ in range(128 * size)))
            sequence = rng.choice([1, 5, 200, rng.randint(1, 1 << 30), 0x7FFFFFF0])
            c.put(lay["cmd_seq"], sequence)
            c.put(lay["demo_playing"], 1 if rng.random() < 0.1 else 0)
            big = bytearray(BIG)
            k = rng.random()
            if k < 0.5:
                text = b"d " + str(config_index(rng)).encode() + b" " + config_string_text(rng, strings, 3)
                big[:len(text)] = text
            elif k < 0.6:
                n = rng.choice([BIG - 1, BIG - 2, BIG - 40, rng.randint(1000, BIG - 1)])
                text = b"d 7 " + bytes(rng.choice(gen.LETTERS) for _ in range(n - 4))
                big[:len(text)] = text
            big_length = big.index(0) if 0 in big else BIG
            for k in range(rng.choice([1, 2, 5])):
                slot = (sequence - k) % ring
                text = _server_command_text(rng, image, strings, big_length)[:1023]
                c.put_bytes(lay["commands"] + 1024 * slot, text + b"\0")
            number = sequence - rng.choice([0] * 8 + [1, 2, 4, ring - 1, ring, ring + 1, -1, -5])
            yield [number, bytes(c.cl), bytes(c.clc[:send_clc_size(image)]), bytes(big), None, None]
    return cases


def _configstring_cases(image: str):
    def cases(rng):
        for _ in forever:
            c = Client(image)
            strings = random_game_state(rng, image)
            put_game_state(c, strings)
            index = config_index(rng)
            word = rng.choice([str(index).encode(), str(index).encode(), b"", b"x", b" 7", b"0x10", b"+3",
                               str(index).encode() + b"abc"])
            text = config_string_text(rng, strings, index)
            argc = rng.choice([3, 3, 3, 2, 1, 0, 5])
            yield [word, text, argc, bytes(c.cl), None]
    return cases

# --- time-outs and the player's settings (CL_CheckTimeout, CL_CheckUserinfo) ---------------------

TIMEOUT = {
    "cod2mp": dict(count=0x0096B664, timeout=(0x0068A3B0, 8), connect_timeout=(0x0068A3A4, 8),
                   info_string=0x0043A9D0, info_string_at=0),
    "uomp": dict(count=0x049581A0, timeout=(0x04AD3CDC, 0x1C), connect_timeout=None,
                 info_string=0x0043ED30, info_string_at="edi"),
}
_userinfo = {"text": 0}   # where this case's settings text is (UserinfoText)


class UserinfoText(Arg):
    """The player's settings as an info string, which the stand-in for
    Dvar_InfoString / Cvar_InfoString returns: s: token."""
    at = "none"

    def words(self, oracle, value):
        _userinfo["text"] = oracle.put_cstr(bytes(value))
        return [0]

    def token(self, value):
        return f"s:{bytes(value).hex()}"


def userinfo_stand_ins(image: str) -> dict:
    t = TIMEOUT[image]
    return {t["info_string"]: StandIn("infostring", ((t["info_string_at"], "int"),), result=lambda flag: _userinfo["text"])}


TIMEOUT_FLOATS = [0x43480000, 0x43480000, 0x3F800000, 0, 0x80000000, 0x7FC00000, 0x7F800000, 0xFF800000,
                  0xBF800000, 0x00000001, 0x7F7FFFFF, 0x447A0000, 0x3DCCCCCD]


def _timeout_cases(image: str):
    lay = L[image]
    t = TIMEOUT[image]

    def cases(rng):
        for _ in forever:
            c = Client(image)
            realtime = rng.choice([rng.randint(0, 1 << 30), rng.randint(0, 1000), -rng.randint(0, 1000)])
            quiet = rng.choice([0, 1, 199999, 200000, 200001, 200500, 1000, 999, 1001, 5000, 100, 101, -5,
                                rng.getrandbits(31), -rng.getrandbits(31)])
            c.put(lay["last_packet_time"], _i32(realtime - quiet))
            if rng.random() < 0.1:
                c.put(lay["last_packet_time"], rng.choice([0, -1, 1]))
            c.put(t["count"], rng.choice([0, 0, 1, 4, 5, 6, 100, -1, 0x7FFFFFFF]))
            state = rng.randint(0, 9)
            flag = lambda: rng.choice([0, 0, 0, 1, 2, -1])
            timeout = rng.choice(TIMEOUT_FLOATS + [rng.getrandbits(32)])
            connect = rng.choice(TIMEOUT_FLOATS + [rng.getrandbits(32)])
            yield [state, bytes(c.cl), bytes(c.clc[:send_clc_size(image)]), realtime, flag(), flag(), timeout,
                   connect]
    return cases


def _userinfo_cases(image: str):
    def cases(rng):
        for _ in forever:
            c = Client(image)
            reliable_ring(c, rng, image)
            state = rng.randint(0, 9)
            flags = rng.choice([0, 2, 2, 3, 0x7FFFFFFF, 1, 0x402, -1])
            n = rng.choice([0, 10, 60, rng.randint(0, 300), 1012, 1013, 1014, rng.randint(900, 1100)])
            info = bytes(rng.choice(TEXT) for _ in range(n))
            yield [state, rng.choice([0, 0, 0, 1, -1]), flags, info, bytes(c.clc[:send_clc_size(image)]), None]
    return cases

for image in IMAGES:
    lay = L[image]
    at = lay["msg_at"]
    setup = _setup(image)
    cl_region = Region(address=lay["cl"], size=lay["cl_size"])
    clc_region = Region(address=lay["clc"], size=lay["clc_size"])
    realtime = GlobalU32(address=lay["realtime"])
    spec(name="CL_ParseCommandString", image=image, rust="client::parse_command_string",
         args=[Msg(at=at["CL_ParseCommandString"]), clc_region], ret=RetVoid(),
         cases=_command_cases(image), setup=setup)
    spec(name="CL_ParseGamestate", image=image, rust="client::parse_game_state",
         args=[Msg(at=at["CL_ParseGamestate"]), cl_region, clc_region], ret=RetStop(),
         stops={lay["systeminfo"]: "systeminfo"}, cases=_game_state_cases(image), setup=setup)
    for name, rust, clients in (("CL_ParsePacketEntities", "client::parse_packet_entities", False),
                                ("CL_ParsePacketClients", "client::parse_packet_clients", True)):
        spec(name=name, image=image, rust=rust,
             args=[Msg(), SnapshotRef(image=image), Buffer(size=lay["snap_size"], terminated=False), cl_region],
             ret=RetVoid(), cases=_packet_cases(image, clients), setup=setup)
    spec(name="CL_ParseSnapshot", image=image, rust="client::parse_snapshot",
         args=[Msg(at=at["CL_ParseSnapshot"]), cl_region, clc_region, realtime], ret=RetVoid(),
         cases=_snapshot_cases(image), setup=setup)
    spec(name="MSG_SetDefaultUserCmd", image=image, rust="net::delta::set_default_usercmd",
         args=[RawBytes(), Buffer(size=USERCMD[image]["size"], terminated=False, at=lay["default_cmd_at"])],
         ret=RetVoid(), cases=_default_cmd_cases(image))
    spec(name="CL_WritePacket", image=image, rust="client::write_packet",
         args=[GlobalU32(address=lay["state"]), cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               GlobalChan(net=NET[image], address=lay["chan"]), realtime,
               Setting(pointer=lay["packet_dup"], offset=lay["value_offset"]),
               Setting(pointer=lay["no_delta"], offset=lay["value_offset"]), SentPackets()],
         ret=RetVoid(), prepare=HUFFMAN, cases=_write_packet_cases(image), setup=_send_setup(image))
    spec(name="CL_PacketEvent", image=image, rust="client::packet_event",
         args=[ServerPacket(at=lay["packet_msg_at"]), Address(net=NET[image]), Int(),
               GlobalU32(address=lay["state"]), cl_region,
               Region(address=lay["clc"], size=send_clc_size(image)),
               GlobalChan(net=NET[image], address=lay["chan"]), realtime],
         ret=RetEvent(image), prepare=HUFFMAN,
         stops={lay["connectionless"]: "connectionless", lay["demo_write"]: "demo",
                lay["systeminfo"]: "systeminfo", lay["download"]: "download"},
         cases=_packet_event_cases(image), setup=_packet_event_setup(image))
    spec(name="CL_ConnectionlessPacket", image=image, rust="client::connectionless_packet",
         args=[Address(net=NET[image]), Msg(), Int(), GlobalU32(address=lay["state"]),
               Region(address=lay["clc"], size=send_clc_size(image)),
               GlobalChan(net=NET[image], address=lay["chan"]), realtime, GlobalInt(address=lay["qport"]),
               SentPackets()],
         ret=RetEvent(image), stops=lay["oob_stops"], cases=_connectionless_cases(image),
         setup=_packet_event_setup(image))
    r = RESEND[image]
    spec(name="Sys_IsLANAddress", image=image, rust="net::oob::is_lan_address",
         args=[Address(net=NET[image]), LocalAddresses(image=image)], ret=RetI32(), cases=_lan_cases(image))
    spec(name="CL_CheckForResend", image=image, rust="client::check_for_resend",
         args=[GlobalU32(address=lay["state"]), Region(address=lay["clc"], size=send_clc_size(image)), realtime,
               GlobalInt(address=r["qport"]) if image == "cod2mp" else UoCvarValue(),
               GuestUserinfo(image=image), GuestKeyHash(image=image),
               Setting(pointer=r["lan_authorize"], offset=r["lan_authorize_offset"]), LocalAddresses(image=image),
               GlobalU32(address=r["modified_flags"]), SentPackets()],
         ret=RetStop(), stops={r["authorize"]: "authorize"}, cases=_resend_cases(image), setup=_resend_setup(image))
    c = SYSTEM_INFO_CALLS[image]
    spec(name="CL_SystemInfoChanged", image=image, rust="client::system_info_changed",
         args=[cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               Setting(pointer=c["server_running"], offset=c["running_offset"]), ExistingSettings(), CallLog()],
         ret=RetVoid(), stand_ins=system_info_stand_ins(image), cases=_system_info_cases(image), setup=setup)
    k = CLOCK[image]
    setting = lambda pair: Setting(pointer=pair[0], offset=pair[1])
    spec(name="CL_SetCGameTime", image=image, rust="client::set_cgame_time",
         args=[GlobalU32(address=lay["state"]), cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               GlobalU32(address=lay["clock_realtime"]), GlobalCStr(address=k["server_name"]),
               setting(k["cl_paused"]), setting(k["sv_paused"]),
               setting(k["server_running"]), setting(k["freeze_demo"]),
               setting(k["time_nudge"]) if k["time_nudge"] else GlobalInt(),
               FloatSetting(address=k["timescale"]) if image == "cod2mp"
               else FloatSetting(pointer=k["timescale"][0], offset=k["timescale"][1]),
               setting(k["time_demo"]) if k["time_demo"] else GlobalInt(),
               TextSetting(pointer=k["active_action"][0], offset=k["active_action"][1]), CallLog()],
         ret=RetStop(), stops={k["read_demo"]: "readdemo", k["time_demo_frame"]: "timedemo"},
         stand_ins=clock_stand_ins(image), cases=_clock_cases(image), setup=_clock_setup(image))
    spec(name="CL_AdjustTimeDelta", image=image, rust="client::adjust_time_delta",
         args=[cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               GlobalU32(address=lay["clock_realtime"]),
               FloatSetting(address=k["timescale"]) if image == "cod2mp"
               else FloatSetting(pointer=k["timescale"][0], offset=k["timescale"][1])],
         ret=RetVoid(), cases=_adjust_cases(image), setup=_clock_setup(image))
    spec(name="CL_FirstSnapshot", image=image, rust="client::first_snapshot",
         args=[GlobalU32(address=lay["state"]), cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               GlobalU32(address=lay["clock_realtime"]),
               TextSetting(pointer=k["active_action"][0], offset=k["active_action"][1]), CallLog()],
         ret=RetVoid(), stand_ins=clock_stand_ins(image), cases=_first_snapshot_cases(image),
         setup=_clock_setup(image))
    m = COMMANDS[image]
    spec(name="CL_FinishMove", image=image, rust="client::finish_move",
         args=[Buffer(size=USERCMD[image]["size"], terminated=False, at="eax"), cl_region], ret=RetVoid(),
         cases=_finish_move_cases(image))
    timing = (lambda a: GlobalU32(address=a)) if image == "uomp" else (lambda a: Absent32())
    spec(name="CL_CreateNewCommands", image=image, rust="client::create_new_commands",
         args=[GlobalU32(address=lay["state"]), cl_region, GlobalInt(address=m["frame_time"]),
               timing(m["old_frame_time"]), timing(m["frame_msec"]), MadeCommand(), CallLog()],
         ret=RetVoid(), stand_ins={m["create_cmd"]: StandIn("create", result=lambda: _made["cmd"])},
         cases=_create_commands_cases(image), setup=setup)
    spec(name="CL_ReadyToSendPacket", image=image, rust="client::ready_to_send_packet",
         args=[GlobalU32(address=lay["state"]), cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               GlobalChan(net=NET[image], address=lay["chan"]), realtime, GlobalInt(address=m["downloading"]),
               LocalAddresses(image=image), Setting(pointer=m["max_packets"][0], offset=m["max_packets"][1]),
               CallLog()],
         ret=RetI32(), stand_ins=ready_stand_ins(image), cases=_ready_cases(image), setup=_packet_event_setup(image))
    spec(name="CL_AddReliableCommand", image=image, rust="client::add_reliable_command",
         args=[CStr(), Region(address=lay["clc"], size=send_clc_size(image))], ret=RetPrinted(),
         cases=_add_reliable_cases(image))
    spec(name="CL_SendPureChecksums", image=image, rust="client::send_pure_checksums",
         args=[SearchPaths(image=image), FsWord(address=FS[image]["feed"]), FsWord(address=FS[image]["fake"]),
               Region(address=lay["clc"], size=send_clc_size(image))], ret=RetVoid(),
         cases=_send_pure_cases(image))
    t = SERVER_COMMANDS[image]
    spec(name="CL_GetServerCommand", image=image, rust="client::get_server_command",
         args=[Int(at="eax"), cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               Region(address=t["big"], size=BIG), TokenWords(image=image), CallLog()],
         ret=RetCommand(), stops={t["restart"]: "restart"}, stand_ins=server_command_stand_ins(image, True),
         cases=_get_server_command_cases(image), setup=_server_command_setup(image))
    spec(name="CL_ConfigstringModified", image=image, rust="client::configstring_modified",
         args=[TokenWord(image=image, index=1), TokenWord(image=image, index=2),
               GlobalInt(address=t["tokens_argc"]), cl_region, CallLog()],
         ret=RetVoid(), stand_ins=server_command_stand_ins(image, False), cases=_configstring_cases(image),
         setup=setup)
    o = TIMEOUT[image]
    spec(name="CL_CheckTimeout", image=image, rust="client::check_timeout",
         args=[GlobalU32(address=lay["state"]), cl_region, Region(address=lay["clc"], size=send_clc_size(image)),
               realtime, setting(k["cl_paused"]), setting(k["sv_paused"]),
               FloatSetting(pointer=o["timeout"][0], offset=o["timeout"][1]),
               FloatSetting(pointer=o["connect_timeout"][0], offset=o["connect_timeout"][1])
               if o["connect_timeout"] else GlobalInt()],
         ret=RetVoid(), cases=_timeout_cases(image))
    spec(name="CL_CheckUserinfo", image=image, rust="client::check_userinfo",
         args=[GlobalU32(address=lay["state"]), setting(k["cl_paused"]), GlobalU32(address=RESEND[image]["modified_flags"]),
               UserinfoText(), Region(address=lay["clc"], size=send_clc_size(image)), CallLog()],
         ret=RetPrinted(), stand_ins=userinfo_stand_ins(image), cases=_userinfo_cases(image))
    spec(name="CL_ParseServerMessage", image=image, rust="client::parse_server_message",
         args=[CodedMsg(at=at["CL_ParseServerMessage"]), cl_region, clc_region, realtime], ret=RetStop(),
         stops={lay["systeminfo"]: "systeminfo", lay["download"]: "download"}, prepare=HUFFMAN,
         cases=_server_message_cases(image), setup=setup)
