"""Checks for iw_pc::net::game_state (both games): what the server writes
into a client's messages. The server's client (`client_t`) is built in the
test heap with the fields the functions read; everything else is zero."""
from __future__ import annotations

import struct
from dataclasses import dataclass

import gen
from harness_x86 import Arg, CallLog, Int, Msg, Region, RetPrinted, RetVoid, StandIn, spec
from specs.net_client import client_state, entity_state, entity_table
from specs.net_delta import CLIENT_STATE_SIZE, LAYOUT as PS_LAYOUT, changed_from, player_state
from specs.net_netchan import GAMES as NET

# Call of Duty 2's sv_debugReliableCmds (a dvar pointer the function reads): pointed at zeros.
DEBUG_DVAR = {"cod2mp": 0x00D52820, "uomp": None}


def _setup(image: str):
    def setup(oracle, values):
        if DEBUG_DVAR[image]:
            oracle.write(DEBUG_DVAR[image], struct.pack("<I", oracle.alloc(0x40, 16)))
    return setup

# Where each game's client_t keeps its reliable commands: the ring (texts at the start of
# each entry), its size, the last command queued, the last acknowledged, the last sent.
CLIENT = {
    "cod2mp": dict(size=0x20818, commands=0x40C, stride=0x408, ring=128, sequence=0x2080C, acknowledge=0x20810,
                   sent=0x20814, at="stack", msg_at="eax"),
    "uomp": dict(size=0x10618, commands=0x40C, stride=0x408, ring=64, sequence=0x1060C, acknowledge=0x10610,
                 sent=0x10614, at="edi", msg_at="eax"),
}
TEXT = gen.LETTERS + gen.DIGITS + b" \"\\%;.:\x92\xe9"


def _i32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


@dataclass
class ServerClient(Arg):
    """A client_t with its reliable commands: the value is (sequence,
    acknowledge, {slot: text}). A b: token the answerer replaces: sequence
    and acknowledge (i32 big-endian), then each slot's text (u16 length,
    bytes). Read back: the last-sent number (i32 big-endian)."""
    image: str = ""
    mutable = True

    def words(self, oracle, value):
        c = CLIENT[self.image]
        sequence, acknowledge, texts = value
        block = bytearray(c["size"])
        struct.pack_into("<ii", block, c["sequence"], _i32(sequence), _i32(acknowledge))
        struct.pack_into("<i", block, c["sent"], 0x5A5A5A5A)
        for slot, text in texts.items():
            at = c["commands"] + slot * c["stride"]
            block[at:at + len(text)] = text
        return [oracle.put(bytes(block), 16)]

    def token(self, value):
        c = CLIENT[self.image]
        sequence, acknowledge, texts = value
        out = bytearray(struct.pack(">ii", _i32(sequence), _i32(acknowledge)))
        for slot in range(c["ring"]):
            text = texts.get(slot, b"")
            text = text[:text.index(0)] if 0 in text else text
            out += struct.pack(">H", len(text)) + text
        return "b:" + out.hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(pointer + CLIENT[self.image]["sent"], 4)[::-1]


def _commands_cases(image: str):
    c = CLIENT[image]

    def cases(rng):
        while True:
            acknowledge = rng.choice([0, 5, rng.getrandbits(31), -1, 0x7FFFFFF0])
            pending = rng.choice([0, 0, 1, 2, 3, 10, c["ring"] - 1, c["ring"], -1, -3])
            sequence = _i32(acknowledge + pending)
            texts = {}
            for k in range(max(0, min(pending, c["ring"])) + 1):
                slot = (sequence - k) % c["ring"]
                n = rng.choice([0, 1, 10, 60, rng.randint(0, 300), 1022, 1023, 1024, c["stride"] - 1])
                texts[slot] = bytes(rng.choice(TEXT) for _ in range(n)) + b"\0"
            used = rng.choice([0, 0, 4, rng.randint(0, 200)])
            maxsize = rng.choice([0x4000, 0x4000, used + rng.randint(0, 40), used + 2000])
            data = bytes(rng.getrandbits(8) for _ in range(used))
            msg = (1 if rng.random() < 0.03 else 0, data, maxsize, used, 0, 0)
            yield [msg, (sequence, acknowledge, texts)]
    return cases


for image, c in CLIENT.items():
    spec(name="SV_UpdateServerCommandsToClient", image=image, rust="net::game_state::write_server_commands",
         args=[Msg(at=c["msg_at"]), ServerClient(image=image, at=c["at"])], ret=RetPrinted(),
         cases=_commands_cases(image), setup=_setup(image))


# --- the snapshot's entity and client lists (SV_EmitPacketEntities, SV_EmitPacketClients) --------
# The server keeps every snapshot's entities and clients in two rings (svs.snapshotEntities,
# svs.snapshotClients; a pointer and a size each); a new entity is coded against its baseline
# (sv.svEntities[n].baseline, at the start of each entry).
SNAPSHOT = {
    "cod2mp": dict(entities=0x00D35720, entities_count=0x00D35710, clients=0x00D35724, clients_count=0x00D35714,
                   entity_size=0xF0, baselines=0x00CD859C, baseline_stride=0x174),
    "uomp": dict(entities=0x04907BE4, entities_count=0x04907BD4, clients=0x04907BE8, clients_count=0x04907BD8,
                 entity_size=0xF4, baselines=0x048A7AA0, baseline_stride=0x180),
}


@dataclass
class RingLists(Arg):
    """A snapshot ring holding the old and the new list: the value is (ring
    size, old first, old states, new first, new states); the ring is built in
    the test heap and the game's pointer and size set. v: token: the old
    count (u32 big-endian), the old states, the new states."""
    image: str = ""
    kind: str = "entities"
    at: str = "none"

    def words(self, oracle, value):
        t = SNAPSHOT[self.image]
        size = t["entity_size"] if self.kind == "entities" else CLIENT_STATE_SIZE
        ring, old_first, old, new_first, new = value
        block = bytearray(ring * size)
        for first, states in ((old_first, old), (new_first, new)):
            for k, state in enumerate(states):
                at = ((first + k) % ring) * size
                block[at:at + size] = state
        oracle.write(t[self.kind], struct.pack("<I", oracle.put(bytes(block), 16)))
        oracle.write(t[self.kind + "_count"], struct.pack("<i", ring))
        return [0]

    def token(self, value):
        _, _, old, _, new = value
        return "v:" + (struct.pack(">I", len(old)) + b"".join(map(bytes, old)) + b"".join(map(bytes, new))).hex()


def _lists(rng, image: str, kind: str):
    """An old and a new sorted list of states: kept ones (changed or not), new ones, gone ones."""
    limit, most = (1023, 40) if kind == "entities" else (64, 20)
    make = (lambda n: entity_state(rng, image, n)) if kind == "entities" else (lambda n: client_state(rng, image, n))
    table = lambda: entity_table(rng, image) if kind == "entities" else "client"
    numbers = sorted(rng.sample(range(limit), rng.choice([0, 1, 3, rng.randint(0, most)])))
    old, new = [], []
    for n in numbers:
        k = rng.random()
        state = make(n)
        if k < 0.25:
            old.append(state)                      # gone
        elif k < 0.5:
            new.append(state)                      # new
        else:
            old.append(state)                      # kept, maybe changed
            changed = changed_from(rng, image, table(), state)
            struct.pack_into("<i", changed, 0, n)
            new.append(changed)
    return old, new


def _emit_cases(image: str, kind: str):
    t = SNAPSHOT[image]

    def cases(rng):
        while True:
            old, new = _lists(rng, image, kind)
            ring = max(1, len(old) + len(new) + rng.choice([0, 1, 5, 100]))
            old_first = rng.choice([0, rng.randint(0, 5000), ring - 1])
            new_first = old_first + len(old) + rng.choice([0, 0, 3])
            if new_first + len(new) - old_first > ring:
                new_first = old_first + len(old)
            baselines = bytearray(1024 * t["baseline_stride"]) if kind == "entities" else None
            if baselines is not None:
                for state in new:
                    if rng.random() < 0.8:
                        n = struct.unpack_from("<i", state, 0)[0]
                        base = entity_state(rng, image, n)
                        at = n * t["baseline_stride"]
                        baselines[at:at + len(base)] = base
            msg = (0, b"", 0x4000, 0, 0, 0)
            lists = (ring, old_first, old, new_first, new)
            if image == "cod2mp":
                head = [msg, old_first, len(old), len(new), new_first]
            else:
                head = [msg, new_first, len(old), old_first, len(new)]
            yield head + [lists] + ([bytes(baselines)] if baselines is not None else [])
    return cases


for image, t in SNAPSHOT.items():
    entity_ring = RingLists(image=image, kind="entities")
    client_ring = RingLists(image=image, kind="clients")
    baselines = Region(address=t["baselines"], size=1024 * t["baseline_stride"])
    if image == "cod2mp":
        # msg ecx, the old first entity eax; stack: old count, new count, new first.
        entity_args = [Msg(at="ecx"), Int(at="eax"), Int(), Int(), Int()]
        # msg edx, the old first eax, the new first ecx; stack: old count, new count.
        client_args = [Msg(at="edx"), Int(at="eax"), Int(), Int(), Int(at="ecx")]
    else:
        # msg ecx, the new first eax; stack: old count, old first, new count.
        entity_args = [Msg(at="ecx"), Int(at="eax"), Int(), Int(), Int()]
        # msg edx, the new first ecx; stack: old count, old first, new count.
        client_args = [Msg(at="edx"), Int(at="ecx"), Int(), Int(), Int()]
    spec(name="SV_EmitPacketEntities", image=image, rust="net::game_state::write_packet_entities",
         args=entity_args + [entity_ring, baselines], ret=RetVoid(), cases=_emit_cases(image, "entities"))
    spec(name="SV_EmitPacketClients", image=image, rust="net::game_state::write_packet_clients",
         args=client_args + [client_ring], ret=RetVoid(), cases=_emit_cases(image, "clients"))


# --- the whole snapshot (SV_WriteSnapshotToClient) -----------------------------------------------
# client_t's fields the writer reads: the state, the "in the game" word it keeps for the flags,
# deltaMessage, rateDelayed, the frames (32; the player state, then the lists' counts and
# starts), the channel's outgoing sequence; and the server's time, its map-restart bit,
# svs.nextSnapshotEntities and sv_padPackets (a cvar).
WRITER = {
    "cod2mp": dict(client_size=0x6E6C8, active=4, delta=0x20E18, rate=0x20E2C, frames=0x20E34, frame_size=0x26C4,
                   outgoing=0x6E6C4, time=0x00D35704, snap_bit=0x00D35708, next_entities=0x00D35718,
                   padding=(0x00D52888, 8), client_at="ecx", msg_at="eax"),
    "uomp": dict(client_size=0x9B068, active=4, delta=0x10C14, rate=0x10C28, frames=0x10C30, frame_size=0x4520,
                 outgoing=0x9B064, time=0x04907BC4, snap_bit=0x04907BCC, next_entities=0x04907BDC,
                 padding=(0x0491CD34, 0x20), client_at="eax", msg_at="esi"),
}


@dataclass
class SnapshotClientBlock(Arg):
    """A client_t for the snapshot writer: the value is (state, active,
    delta, rate delayed, outgoing, frames: 32 x (player state or None,
    entity count, client count, first entity, first client)). A b: token
    the answerer replaces: the five numbers (i32 big-endian), then per frame
    the four numbers and the player state (u32 length, bytes; 0 for a frame
    the writer doesn't read). Read back: the "in the game" word."""
    image: str = ""
    mutable = True

    def words(self, oracle, value):
        w = WRITER[self.image]
        ps_size = PS_LAYOUT[self.image]["size"]
        state, active, delta, rate, outgoing, frames = value
        block = bytearray(w["client_size"])
        struct.pack_into("<ii", block, 0, state, active)
        struct.pack_into("<i", block, w["delta"], _i32(delta))
        struct.pack_into("<i", block, w["rate"], rate)
        struct.pack_into("<i", block, w["outgoing"], _i32(outgoing))
        for k, (ps, ne, nc, fe, fc) in enumerate(frames):
            at = w["frames"] + k * w["frame_size"]
            if ps is not None:
                block[at:at + ps_size] = ps
            struct.pack_into("<4i", block, at + ps_size, ne, nc, fe, fc)
        return [oracle.put(bytes(block), 16)]

    def token(self, value):
        state, active, delta, rate, outgoing, frames = value
        out = bytearray(struct.pack(">5i", state, active, _i32(delta), rate, _i32(outgoing)))
        for ps, ne, nc, fe, fc in frames:
            ps = bytes(ps) if ps is not None else b""
            out += struct.pack(">4iI", ne, nc, fe, fc, len(ps)) + ps
        return "b:" + out.hex()

    def read_back(self, oracle, pointer, value):
        return oracle.read(pointer + WRITER[self.image]["active"], 4)[::-1]


@dataclass
class Rings(Arg):
    """The server's two snapshot rings: the value is (entities, clients),
    each a list of states; built in the test heap, the game's pointers and
    sizes set. v: token: the two counts (u32 big-endian), then the states."""
    image: str = ""
    at: str = "none"

    def words(self, oracle, value):
        t = SNAPSHOT[self.image]
        for kind, states in zip(("entities", "clients"), value):
            block = b"".join(bytes(x) for x in states) or bytes(16)
            oracle.write(t[kind], struct.pack("<I", oracle.put(block, 16)))
            oracle.write(t[kind + "_count"], struct.pack("<i", len(states)))
        return [0]

    def token(self, value):
        entities, clients = value
        return "v:" + (struct.pack(">II", len(entities), len(clients)) + b"".join(map(bytes, entities))
                       + b"".join(map(bytes, clients))).hex()


@dataclass
class ServerWord(Arg):
    """A 32-bit server global the writer reads (i: token)."""
    address: int = 0
    at: str = "none"

    def words(self, oracle, value):
        oracle.write(self.address, struct.pack("<i", _i32(value)))
        return [0]

    def token(self, value):
        return f"i:{_i32(value)}"


@dataclass
class PadSetting(Arg):
    """sv_padPackets: a stand-in variable holding the count (i: token)."""
    pointer: int = 0
    offset: int = 8
    at: str = "none"

    def words(self, oracle, value):
        block = bytearray(0x60)
        struct.pack_into("<i", block, self.offset, value)
        oracle.write(self.pointer, struct.pack("<I", oracle.put(bytes(block), 16)))
        return [0]

    def token(self, value):
        return f"i:{value}"


def _writer_cases(image: str):
    t = SNAPSHOT[image]

    def place(ring: list, states: list, rng) -> int:
        first = len(ring)
        ring.extend(states)
        return first

    def cases(rng):
        while True:
            old_entities, new_entities = _lists(rng, image, "entities")
            old_clients, new_clients = _lists(rng, image, "clients")
            entities, clients = [], []
            # Other frames' runs before, between and after (the ring holds every frame's lists).
            filler = lambda kind: [entity_state(rng, image, rng.randint(0, 1022)) if kind == "e"
                                   else client_state(rng, image, rng.randint(0, 63)) for _ in range(rng.choice([0, 0, 2, 7]))]
            entities += filler("e"); clients += filler("c")
            old_fe = place(entities, old_entities, rng); old_fc = place(clients, old_clients, rng)
            entities += filler("e"); clients += filler("c")
            new_fe = place(entities, new_entities, rng); new_fc = place(clients, new_clients, rng)
            entities += filler("e"); clients += filler("c")
            # Starts counted on from earlier laps of the ring, sometimes.
            lap_e = len(entities) * rng.choice([0, 0, 1, 7]); lap_c = len(clients) * rng.choice([0, 0, 1, 3])
            outgoing = rng.choice([1, 2, 40, rng.randint(1, 1 << 20), 0x7FFFFFF0])
            back = rng.choice([0, 1, 1, 2, 5, 28, 29, 30, 31, -1, -3, rng.randint(1, 40), rng.getrandbits(31)])
            delta = rng.choice([_i32(outgoing - back)] * 8 + [0, -1])
            frames = [(None, 0, 0, 0, 0)] * 32
            ps_new = player_state(rng, image)
            ps_old = changed_from(rng, image, "player", ps_new) if rng.random() < 0.7 else player_state(rng, image)
            frames[delta & 31] = (ps_old, len(old_entities), len(old_clients), old_fe + lap_e, old_fc + lap_c)
            frames[outgoing & 31] = (ps_new, len(new_entities), len(new_clients), new_fe + lap_e, new_fc + lap_c)
            next_entities = len(entities) + lap_e + rng.choice([0, 0, 0, 1, len(entities) - old_fe])
            state = rng.choice([4, 4, 4, 1, 2, 3, 0, 5])
            baselines = bytearray(1024 * t["baseline_stride"])
            for st in new_entities:
                if rng.random() < 0.8:
                    n = struct.unpack_from("<i", st, 0)[0]
                    base = entity_state(rng, image, n)
                    baselines[n * t["baseline_stride"]:n * t["baseline_stride"] + len(base)] = base
            msg = (0, b"", 0x20000, 0, 0, 0)
            client = (state, rng.choice([0, 1]), delta, rng.choice([0, 0, 1]), outgoing, frames)
            yield [msg, client, (entities, clients), bytes(baselines), rng.choice([0, rng.randint(0, 1 << 20)]),
                   rng.choice([0, 0, 4, 8, 12]), next_entities, rng.choice([0, 0, 0, 1, 3, -1])]
    return cases


for image, w in WRITER.items():
    t = SNAPSHOT[image]
    spec(name="SV_WriteSnapshotToClient", image=image, rust="net::game_state::write_snapshot_to_client",
         args=[Msg(at=w["msg_at"]), SnapshotClientBlock(image=image, at=w["client_at"]), Rings(image=image),
               Region(address=t["baselines"], size=1024 * t["baseline_stride"]), ServerWord(address=w["time"]),
               ServerWord(address=w["snap_bit"]), ServerWord(address=w["next_entities"]),
               PadSetting(pointer=w["padding"][0], offset=w["padding"][1])],
         ret=RetVoid(), cases=_writer_cases(image))


# --- the game state (SV_SendClientGameState) ---------------------------------------------------
# The whole function: the fragments of a message still going out are sent first (while the slot
# is in use), the client is primed, the game state is written into the server's message buffer
# (Call of Duty 2: 0x20000 bytes of a global block; United Offensive: 0x8000 bytes on its stack)
# and handed to SV_SendMessageToClient; United Offensive then sends one fragment at once when the
# message left some and the timescale setting's integer is 0. The connection's two functions are
# stand-ins: Netchan_TransmitNextFragment sends one of the fragments the case says are left (the
# last one clears unsentFragments); SV_SendMessageToClient records the message (overflowed, its
# bytes) and, when the case says it was too big for one packet, sets unsentFragments with a count
# of its own. The debug lines (Com_DPrintf, printed only with developer set) aren't compared.
SENDER = {
    "cod2mp": dict(size=0x8E700, pure=0x6E6C0, outgoing=0x6E6C4, unsent=0x8E6F4, gamestate=0x2081C,
                   last_command=0x20840, config_strings=0x00CD6598, feed=0x00CD618C, clients=0x00D3570C,
                   # svs.clients' stride: the division the function does gives back the number for any of
                   # 0xB1064..0xB10A3 (the real size is one of them).
                   stride=0xB1064, fragment=0x00447DB0, send=0x0045F370, msg_at="eax", timescale=None),
    "uomp": dict(size=0xA3080, pure=0x9B03C, outgoing=0x9B064, unsent=0xA3070, gamestate=0x1061C,
                 last_command=0x1063C, config_strings=0x048A5A98, feed=0x048A568C, clients=0x04907BD0,
                 stride=0xAB0B4, fragment=0x0044D590, send=0x00465DA0, msg_at="ecx",
                 timescale=(0x04927ECC, 0x20)),   # com_timescale (a cvar pointer) and its integer
}
DEBUG_LINES = (b"SV_SendClientGameState() for %s\n", b"Going from CS_CONNECTED to CS_PRIMED for %s\n",
               b"Sending %i bytes in gamestate to client: %i\n")


@dataclass
class GameStateClientBlock(Arg):
    """A client_t for the game state: the value is (state, fragments left,
    outgoing sequence, last client command, client number, fragments the
    message leaves, sequence, acknowledge, {slot: text}). Also points
    svs.clients so that the client has that number. A b: token the answerer
    replaces: the eight numbers (i32 big-endian), then each command slot's
    text (u16 length, bytes). Read back: state, pureAuthentic,
    gamestateMessageNum, unsentFragments, reliableSent (i32 big-endian)."""
    image: str = ""
    at: str = "stack"
    mutable = True

    def words(self, oracle, value):
        s, c = SENDER[self.image], CLIENT[self.image]
        state, fragments, outgoing, last_command, number, after, sequence, acknowledge, texts = value
        block = bytearray(s["size"])
        struct.pack_into("<i", block, 0, state)
        struct.pack_into("<i", block, s["unsent"], 1 if fragments > 0 else 0)
        struct.pack_into("<i", block, s["outgoing"], _i32(outgoing))
        struct.pack_into("<i", block, s["last_command"], _i32(last_command))
        for at in (s["pure"], s["gamestate"], c["sent"]):
            struct.pack_into("<i", block, at, 0x5A5A5A5A)
        struct.pack_into("<ii", block, c["sequence"], _i32(sequence), _i32(acknowledge))
        for slot, text in texts.items():
            at = c["commands"] + slot * c["stride"]
            block[at:at + len(text)] = text
        pointer = oracle.put(bytes(block), 16)
        oracle.write(s["clients"], struct.pack("<I", (pointer - number * s["stride"]) & 0xFFFFFFFF))
        oracle.game_state_case = {"client": pointer, "left": fragments, "after": after}
        return [pointer]

    def token(self, value):
        c = CLIENT[self.image]
        *numbers, texts = value
        out = bytearray(struct.pack(">8i", *(_i32(n) for n in numbers)))
        for slot in range(c["ring"]):
            text = texts.get(slot, b"")
            text = text[:text.index(0)] if 0 in text else text
            out += struct.pack(">H", len(text)) + text
        return "b:" + out.hex()

    def read_back(self, oracle, pointer, value):
        s, c = SENDER[self.image], CLIENT[self.image]
        words = [struct.unpack("<i", oracle.read(pointer + at, 4))[0]
                 for at in (0, s["pure"], s["gamestate"], s["unsent"], c["sent"])]
        return struct.pack(">5i", *words)


@dataclass
class ConfigStrings(Arg):
    """sv.configstrings: the value is {index: text}; every other entry
    points at an empty string, as the server keeps them. v: token: per
    string its index (u16 big-endian), its length (u32 big-endian) and the
    text."""
    image: str = ""
    at: str = "none"

    def words(self, oracle, value):
        empty = oracle.put(b"\0", 4)
        table = [empty] * 2048
        for index, text in value.items():
            table[index] = oracle.put(text + b"\0", 4)
        oracle.write(SENDER[self.image]["config_strings"], struct.pack("<2048I", *table))
        return [0]

    def token(self, value):
        out = bytearray()
        for index, text in sorted(value.items()):
            out += struct.pack(">HI", index, len(text)) + text
        return "v:" + out.hex()


@dataclass
class CvarInteger(Arg):
    """A cvar's integer the function reads through the game's pointer to it
    (none: the game has no such read, the answerer gets the value as it is).
    i: token."""
    setting: tuple | None = None
    at: str = "none"

    def words(self, oracle, value):
        if self.setting:
            pointer, offset = self.setting
            block = bytearray(0x40)
            struct.pack_into("<i", block, offset, value)
            oracle.write(pointer, struct.pack("<I", oracle.put(bytes(block), 16)))
        return [0]

    def token(self, value):
        return f"i:{value}"


class RetWarning(RetPrinted):
    """RetPrinted without the function's own debug lines: the first warning
    (a command or config string too long), p:- for none."""

    def from_guest(self, oracle, result):
        prints = [p for p in result.prints if p not in DEBUG_LINES]
        if not prints:
            return "p:-"
        return f"p:{prints[0].removeprefix(bytes([0x15])).hex()}"


def _fragment(image: str):
    unsent = SENDER[image]["unsent"]

    def effect(oracle):
        case = oracle.game_state_case
        case["left"] -= 1
        if case["left"] <= 0:
            oracle.write(case["client"] + unsent, struct.pack("<i", 0))
    return effect


def _send(image: str):
    unsent = SENDER[image]["unsent"]

    def effect(oracle, msg):
        overflowed, data, _maxsize, cursize = struct.unpack("<iIii", oracle.read(msg & 0xFFFFFFFF, 16))
        oracle.calls[-1] = (oracle.calls[-1][0], [overflowed, oracle.read(data, cursize) if cursize > 0 else b""])
        case = oracle.game_state_case
        if case["after"] > 0:
            case["left"] = case["after"]
            oracle.write(case["client"] + unsent, struct.pack("<i", 1))
    return effect


def _game_state_setup(image: str):
    commands = _setup(image)

    def setup(oracle, values):
        commands(oracle, values)
        oracle.write(NET[image].huffman_built, struct.pack("<i", 1))   # the tree is built (no MSG_initHuffman)
    return setup


def _game_state_cases(image: str):
    c, t = CLIENT[image], SNAPSHOT[image]
    stride = t["baseline_stride"]

    def cases(rng):
        while True:
            acknowledge = rng.choice([0, 5, rng.getrandbits(31), -1])
            pending = rng.choice([0, 0, 1, 2, 3, 10, c["ring"] - 1, c["ring"], -1])
            sequence = _i32(acknowledge + pending)
            texts = {}
            for k in range(max(0, min(pending, c["ring"])) + 1):
                n = rng.choice([0, 1, 10, 60, rng.randint(0, 300), 1023, 1024])
                texts[(sequence - k) % c["ring"]] = bytes(rng.choice(TEXT) for _ in range(n)) + b"\0"
            # Config strings: a few, many, or long ones that fill the buffer (and one past
            # BIG_INFO_STRING now and then: sent empty after a warning).
            k = rng.random()
            count = rng.choice([0, 1, 3, 12, 60, 300]) if k < 0.9 else rng.randint(2, 8)
            strings = {}
            for index in rng.sample(range(2048), count):
                if k < 0.9:
                    n = rng.choice([1, 2, 10, 40, rng.randint(1, 400)]) if rng.random() < 0.995 else 8192
                else:
                    n = rng.choice([4000, 6000, 8191, 8192])
                strings[index] = gen.text(rng, n, TEXT, min_len=n)
            baselines = bytearray(1024 * stride)
            for index in rng.sample(range(1024), rng.choice([0, 0, 1, 4, 30, 150])):
                number = index if rng.random() < 0.95 else rng.choice([0, rng.randint(1, 1023)])
                base = entity_state(rng, image, number)
                baselines[index * stride:index * stride + len(base)] = base
            client = (rng.choice([2, 2, 2, 2, 1, 0, 3, 4]), rng.choice([0, 0, 0, 0, 1, 2, 5]),
                      rng.choice([1, 2, rng.randint(1, 1 << 20), 0x7FFFFFF0, -1]),
                      rng.choice([0, 1, rng.getrandbits(31), -1]), rng.randrange(64),
                      rng.choice([0, 0, 0, 1, 3]), sequence, acknowledge, texts)
            yield [client, strings, bytes(baselines), _i32(rng.getrandbits(32)), rng.choice([1, 1, 0, 2, -1]), None]
    return cases


for image, s in SENDER.items():
    spec(name="SV_SendClientGameState", image=image, rust="net::game_state::send_client_game_state",
         args=[GameStateClientBlock(image=image), ConfigStrings(image=image),
               Region(address=SNAPSHOT[image]["baselines"], size=1024 * SNAPSHOT[image]["baseline_stride"]),
               ServerWord(address=s["feed"]), CvarInteger(setting=s["timescale"]), CallLog()],
         ret=RetWarning(),
         stand_ins={s["fragment"]: StandIn("Netchan_TransmitNextFragment", effect=_fragment(image)),
                    s["send"]: StandIn("SV_SendMessageToClient", args=((s["msg_at"], "int"),), effect=_send(image))},
         cases=_game_state_cases(image), setup=_game_state_setup(image))
