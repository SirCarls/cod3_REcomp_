"""Checks for iw_pc::client::download (both games): what the client does with
a download message (svc_download): a block of a file, or the server's
redirect to a web address. The client's download state is in the game's
globals (Call of Duty 2: one block of them and, through a pointer, the menus'
download numbers; United Offensive: the names before clc, the numbers and the
bad-checksum list inside it); its reliable commands are in clc. What the
download does outside them (the file system, the console, the menus' cvars,
the web download, the shell) are stand-ins, recorded in the call log."""
from __future__ import annotations

import struct
from dataclasses import dataclass

import gen
from harness_x86 import Arg, CallLog, Msg, Region, RetPrinted, StandIn, spec
from specs.net_client import Client, L, reliable_ring, send_clc_size

TEXT = gen.LETTERS + gen.DIGITS + b"./_-:@"

# Where each game keeps the download (offsets in the regions the check passes).
DOWNLOAD = {
    "cod2mp": dict(
        # 0x00966DAC: bWWWDlDisconnected, downloadName[256], downloadTempName[256],
        # originalDownloadName[64], (a word), downloadFlags, bWWWDl, download (the file),
        # downloadBlock, downloadSize.
        block=(0x00966DAC, 0x25C), names=0x00966DAC, ui=0x005CF444, ui_size=0x60,
        msg_at="esi", redirect_msg_at="eax",
        stand_ins={0x0043AC10: ("FS_SV_FOpenFileWrite", ((0, "str"),)),
                   0x00422F90: ("FS_Write", ((0, "int"), (1, "int"), (2, "int"))),
                   0x00421FD0: ("FS_FCloseFile", ((0, "int"),)),
                   0x0043AF10: ("FS_SV_Rename", (("ecx", "str"), (0, "str"))),
                   0x00408E80: ("CL_WritePacket", ()),
                   0x0040DE70: ("CL_NextDownload", ()),
                   0x004373A0: ("Dvar_FindVar", ((0, "str"),)),
                   0x00421C30: ("FS_BuildOSPath", (("eax", "str"), (0, "str"), (1, "str"), (2, "int"), (3, "int"))),
                   0x00420AD0: ("Cbuf_AddText", (("eax", "str"),)),
                   0x004B5AD0: ("DL_BeginDownload", ((0, "str"), (1, "str"))),
                   0x004B59C0: ("DL_EndDownload", ())}),
    "uomp": dict(
        # 0x04DC4404: bWWWDlDisconnected, downloadName[256], downloadTempName[256],
        # originalDownloadName[64]; in clc: download, downloadBlock, downloadCount, downloadSize,
        # downloadFlags, bWWWDl, bWWWDlAborting, badChecksumList.
        block=(0x04DC4404, 0x244), names=0x04DC4404, file=0x04DE89A8, number=0x04DE89B0, count=0x04DE89B4,
        size=0x04DE89B8, flags=0x04DE89BC, www=0x04DE8DC0, aborting=0x04DE8DC4, bad=0x04DE91C8,
        msg_at="ecx",
        stand_ins={0x0043F470: ("FS_SV_FOpenFileWrite", ((0, "str"),)),
                   0x0042E290: ("FS_Write", ((0, "int"), (1, "int"), (2, "int"))),
                   0x0042D0D0: ("FS_FCloseFile", ((0, "int"),)),
                   0x0043F7D0: ("FS_SV_Rename", (("ecx", "str"), (0, "str"))),
                   0x0040CC80: ("CL_WritePacket", ()),
                   0x00411B80: ("CL_NextDownload", ()),
                   0x0043D8F0: ("Cvar_FindVar", ((0, "str"),)),
                   0x0042CB20: ("FS_BuildOSPath", (("eax", "str"), (0, "str"), (1, "str"), (2, "int"), (3, "int"))),
                   0x0042BC10: ("Cbuf_AddText", (("eax", "str"),)),
                   0x0043F040: ("DL_BeginDownload", ((0, "str"), (1, "str"))),
                   0x0046B390: ("Sys_OpenURL", (("esi", "str"), (0, "int"))),
                   0x0043DFB0: ("Cvar_SetValue", ((0, "str"), (1, "int"))),
                   0x0043DC50: ("Cvar_Set2", ((0, "str"), (1, "str"), (2, "int")))}),
}
# The debug lines (Com_DPrintf) the function prints only with developer set: not compared.
DEBUG_LINES = (b"CL_ParseDownload: Expected block %d, got %d\n",
               b"CL_ParseDownload: Sending retransmit request to get the missed block\n",
               b"CL_ParseDownload: Sending retransmitt request to get the missed block\n",
               b"Server redirected download: %s\n")

# What the stand-ins answer in the case being run (set by HostAnswers.words).
_answers: dict = {}


def _i32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def os_path(base: bytes, game: bytes, qpath: bytes) -> bytes:
    """The check's FS_BuildOSPath: base/game/qpath, at most 255 characters
    (the answerer builds the same)."""
    return (base + b"/" + game + b"/" + qpath)[:255]


@dataclass
class HostAnswers(Arg):
    """What the stand-ins answer: (the file handle FS_SV_FOpenFileWrite
    gives, DL_BeginDownload's result, ShellExecuteA's, GetForegroundWindow's,
    the home path or None). Also builds the setting fs_homepath points at
    (Call of Duty 2: a string dvar, its text at +8; United Offensive: a cvar,
    its text at +4). v: token: the four numbers (i32 big-endian), then the
    home path (a 0 byte for none, or 1 and the text)."""
    image: str = ""
    at: str = "none"

    def words(self, oracle, value):
        handle, begin, shell, window, home = value
        setting = 0
        if home is not None:
            text = oracle.put(home + b"\0", 4)
            block = bytearray(0x40)
            if self.image == "cod2mp":
                block[6] = 7                      # not an enum (6): the text pointer is the value
                struct.pack_into("<I", block, 8, text)
            else:
                struct.pack_into("<I", block, 4, text)
            setting = oracle.put(bytes(block), 16)
        _answers.clear()
        _answers.update(handle=handle, begin=begin, shell=shell, window=window, setting=setting)
        return [0]

    def token(self, value):
        handle, begin, shell, window, home = value
        tail = b"\0" if home is None else b"\1" + home
        return "v:" + (struct.pack(">4i", handle, begin, shell, window) + tail).hex()


@dataclass
class UiBlock(Arg):
    """Call of Duty 2's menus' download numbers (a block a global points at:
    size at +0x10, count at +0x14, name from +0x1C): the value is its bytes;
    b: token, read back whole."""
    image: str = ""
    at: str = "none"
    mutable = True

    def words(self, oracle, value):
        t = DOWNLOAD[self.image]
        if "ui" not in t:
            return [1]
        pointer = oracle.put(bytes(value), 16)
        oracle.write(t["ui"], struct.pack("<I", pointer))
        self._pointer = pointer
        return [pointer]

    def token(self, value):
        return "b:" + bytes(value).hex()

    def read_back(self, oracle, pointer, value):
        if "ui" not in DOWNLOAD[self.image]:
            return bytes(value)
        return oracle.read(pointer, len(value))


def _write_effect(oracle, buffer, length, handle):
    oracle.calls[-1] = (oracle.calls[-1][0], [oracle.read(buffer & 0xFFFFFFFF, max(length, 0)) if length > 0 else b"",
                                              length, handle])


def _os_path_effect(oracle, base, game, qpath, out, flag):
    oracle.write(out & 0xFFFFFFFF, os_path(base, game, qpath) + b"\0")
    oracle.calls[-1] = (oracle.calls[-1][0], [base, game, qpath, flag])


def stand_ins(image: str) -> dict:
    out = {}
    for address, (label, args) in DOWNLOAD[image]["stand_ins"].items():
        if label == "FS_SV_FOpenFileWrite":
            out[address] = StandIn(label, args, result=lambda *a: _answers["handle"])
        elif label == "FS_Write":
            out[address] = StandIn(label, args, effect=_write_effect)
        elif label in ("Dvar_FindVar", "Cvar_FindVar"):
            out[address] = StandIn(label, args, result=lambda *a: _answers["setting"])
        elif label == "FS_BuildOSPath":
            out[address] = StandIn(label, args, effect=_os_path_effect)
        elif label == "DL_BeginDownload":
            out[address] = StandIn(label, args, result=lambda *a: _answers["begin"])
        else:
            out[address] = StandIn(label, args)
    return out


def import_stand_ins(image: str) -> dict:
    if image != "cod2mp":
        return {}
    return {"shell32.dll!ShellExecuteA": StandIn("ShellExecuteA", ((1, "str"), (2, "str"), (5, "int")),
                                                 result=lambda *a: _answers["shell"], pops=24),
            "user32.dll!GetForegroundWindow": StandIn("GetForegroundWindow", (), result=lambda: _answers["window"]),
            "user32.dll!ShowWindow": StandIn("ShowWindow", ((0, "int"), (1, "int")), result=1, pops=8)}


class RetWarning(RetPrinted):
    """RetPrinted without the debug lines: the first warning, p:- for none."""

    def from_guest(self, oracle, result):
        prints = [p for p in result.prints if p not in DEBUG_LINES]
        if not prints:
            return "p:-"
        return f"p:{prints[0].removeprefix(bytes([0x15])).hex()}"


def _name(rng, size: int, empty: float = 0.3) -> bytes:
    """A name buffer: often empty, with old bytes after the terminator."""
    out = bytearray(rng.getrandbits(8) if rng.random() < 0.3 else 0 for _ in range(size))
    if rng.random() < empty:
        out[0] = 0
        return bytes(out)
    n = min(size - 1, rng.choice([1, 5, 20, rng.randint(1, size - 1), size - 1]))
    out[:n] = gen.text(rng, n, TEXT, min_len=n)
    out[n] = 0
    return bytes(out)


def _message(rng, block_expected: int) -> tuple:
    """A download message after its type byte: a block (its number near the
    one expected, block 0 with a size, sometimes negative with an error
    text), or a redirect; sometimes cut short."""
    data = bytearray(bytes(rng.getrandbits(8) for _ in range(rng.choice([0, 0, 1, 7]))))
    readcount = len(data)
    k = rng.random()
    if k < 0.25:
        data += struct.pack("<h", -1)
        url = gen.text(rng, rng.choice([0, 10, 60, 300, rng.randint(0, 1100)]), TEXT)
        data += url + b"\0"
        data += struct.pack("<i", _i32(rng.choice([0, 1, 12345, rng.getrandbits(31), -1])))
        data += struct.pack("<i", rng.choice([0, 0, 1, 2, 3, rng.getrandbits(8)]))
    else:
        block = rng.choice([block_expected] * 6 + [0, block_expected + 1, block_expected + 3, block_expected - 1,
                                                   rng.randint(-5, 0x7FFF)])
        block = max(-0x8000, min(0x7FFF, block))
        data += struct.pack("<h", block)
        if block == 0:
            size = rng.choice([0, 1, 4096, rng.getrandbits(31), -1, -5])
            data += struct.pack("<i", size)
            if size < 0:
                data += gen.text(rng, rng.choice([0, 20, 200]), TEXT) + b"\0"
        length = rng.choice([0, 0, 1, 100, 2048, rng.randint(1, 2048), -1, -3])
        data += struct.pack("<h", length)
        if length > 0:
            data += bytes(rng.getrandbits(8) for _ in range(length if rng.random() < 0.9 else rng.randint(0, length)))
    cursize = len(data)
    if rng.random() < 0.1:
        cursize = rng.randint(readcount, cursize)
    return (0, bytes(data), 0x4000, cursize, readcount, 0)


def _cases(image: str, redirect_only: bool = False):
    t = DOWNLOAD[image]
    lay = L[image]

    def cases(rng):
        while True:
            c = Client(image)
            reliable_ring(c, rng, image)
            if rng.random() < 0.8:
                # Mostly room in the ring (more than its size unacknowledged is the commands' error).
                acknowledge = c.get(lay["reliable_ack"])
                c.put(lay["reliable_seq"], _i32(acknowledge + rng.randint(0, lay["commands_count"] - 4)))
            expected = rng.choice([0, 0, 1, 2, 7, rng.randint(0, 0x7FFF), -1])
            file = rng.choice([0, 0, 5])
            www = 1 if rng.random() < 0.15 else 0
            block = bytearray(t["block"][1])
            struct.pack_into("<i", block, 0, rng.choice([0, 0, 1]))
            block[4:4 + 256] = _name(rng, 256)
            block[0x104:0x104 + 256] = _name(rng, 256)
            block[0x204:0x204 + 64] = _name(rng, 64)
            ui = bytearray(0x60)
            numbers = dict(size=_i32(rng.choice([0, 1000, rng.getrandbits(31)])), count=rng.choice([0, 100, 70000]),
                           flags=rng.choice([0, 1, 2, 3]))
            if image == "cod2mp":
                struct.pack_into("<iiiii", block, 0x248, numbers["flags"], www, file, expected, numbers["size"])
                struct.pack_into("<ii", ui, 0x10, numbers["size"], numbers["count"])
                ui[0x1C:0x1C + 16] = _name(rng, 16, empty=0.5)
            else:
                for key, value in (("file", file), ("number", expected), ("count", numbers["count"]),
                                   ("size", numbers["size"]), ("flags", numbers["flags"]), ("www", www),
                                   ("aborting", rng.choice([0, 0, 1]))):
                    c.put(t[key], value)
                if rng.random() < 0.5:
                    # (A redirect keeps the name as the original name first: that is what it looks for.)
                    names = [bytes(block[4:4 + 63]).split(b"\0")[0]] if rng.random() < 0.5 else []
                    names += [gen.text(rng, 12, TEXT, min_len=1) for _ in range(rng.randint(0, 3))]
                    rng.shuffle(names)
                    c.put_bytes(t["bad"], b"".join(b"@" + n for n in names)[:1000] + b"\0")
            message = _message(rng, expected)
            if redirect_only:
                # The redirect function itself: the message after the block number.
                overflowed, data, maxsize, cursize, readcount, bit = message
                if cursize < readcount + 2 or struct.unpack_from("<h", data, readcount)[0] != -1:
                    continue
                message = (overflowed, data, maxsize, cursize, readcount + 2, bit)
            home = None if rng.random() < 0.2 else gen.text(rng, rng.choice([0, 10, 40]), TEXT)
            answers = (rng.choice([5, 5, 0]) if not file else 5, rng.choice([1, 1, 0]), rng.choice([42, 42, 0]),
                       rng.choice([0x1234, 0]), home)
            yield [message, bytes(c.clc[:send_clc_size(image)]), bytes(block), bytes(ui), answers, None]
    return cases


for image, t in DOWNLOAD.items():
    lay = L[image]
    args = [Msg(at=t["msg_at"]), Region(address=lay["clc"], size=send_clc_size(image)),
            Region(address=t["block"][0], size=t["block"][1]), UiBlock(image=image), HostAnswers(image=image),
            CallLog()]
    spec(name="CL_ParseDownload", image=image, rust="client::download::parse_download", args=args,
         ret=RetWarning(), stand_ins=stand_ins(image), import_stand_ins=import_stand_ins(image),
         cases=_cases(image))
    if "redirect_msg_at" in t:
        spec(name="CL_ParseDownloadRedirect", image=image, rust="client::download::parse_redirect",
             args=[Msg(at=t["redirect_msg_at"])] + args[1:], ret=RetWarning(), stand_ins=stand_ins(image),
             import_stand_ins=import_stand_ins(image), cases=_cases(image, redirect_only=True))
