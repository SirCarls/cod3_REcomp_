#!/usr/bin/env python3
"""Call of Duty 3 sound banks ("NSLB": .wbk .wde .wfr .wes .wit .wen .wul), read only.

Treyarch's sound library "NSL" (on top of FMOD Ex 4.04 in the Xbox 360 game) keeps a level's or
a briefing's sounds in one bank file. The layout below was read from the game's own loader
(default.xex: 0x8211CDA8 reads the header, 0x82120678 byte-swaps it and turns offsets into
pointers) and checked against the retail banks; see docs/COD3_AUDIO.md.

    python3 nslb.py BANK...                 summary of each bank, every check run
    python3 nslb.py BANK --list             one line per wave: name, category, codec, rate, ...
    python3 nslb.py BANK --json OUT.json    everything, for other tools
    python3 nslb.py BANK --extract DIR      each wave as the RIFF file stored in the bank
                                            (game data: keep it private)

All integers in the file are little-endian. Exit status 1 when a check fails.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Dict, List, Optional

MAGIC = b"NSLB"
HEADER_FIELDS = (  # offset, name: the 16 u32 words the game byte-swaps (0x82120678)
    (0x00, "magic"), (0x04, "version"), (0x08, "header_size"), (0x0C, "unknown_0c"),
    (0x10, "wave_count"), (0x14, "unknown_14"), (0x18, "name_table_offset"), (0x1C, "wave_table_offset"),
    (0x20, "format_area_size"), (0x24, "format_area_offset"), (0x28, "names_size"), (0x2C, "names_offset"),
    (0x30, "resident_size"), (0x34, "resident_offset"), (0x38, "stream_size"), (0x3C, "stream_offset"),
)
# Byte 4 of a wave's format record: the codec, and what NSL asks FMOD for (0x8211A250:
# FMOD_CREATESOUNDEXINFO.format and .suggestedsoundtype). NSL is cross-platform: the Xbox 360
# banks only use XMA.
CODECS = {
    0x11: "PCM16 (raw)", 0x15: "PCM16 (raw)", 0x20: "IMA ADPCM", 0x21: "VAG (PS2 ADPCM)",
    0x30: "XMA", 0x32: "MPEG", 0x33: "MPEG", 0x35: "ATRAC3",
}
# What the code is known to read from the parameter block (bit number = NSL parameter index).
PARAMS = {
    0: "volume",               # NSL rolloff 0x82119F10 (near volume, default 1); game reads it (index 0)
    1: "pitch",                # game reads it (index 1); NSL scales the frequency by it
    6: "volume_at_max_distance",  # rolloff: the far end of the curve (default 0)
    7: "priority_quiet",       # voice priority (0x8211C690) when the gain at the listener is <= param 9
    8: "priority_loud",        # ... when the gain is >= param 10 (linear between)
    9: "priority_gain_low",
    10: "priority_gain_high",
    25: "min_distance",        # rolloff and the game (index 0x19); default 1
    26: "max_distance",        # rolloff and the game (index 0x1A); default 100
}


@dataclass
class Wave:
    index: int
    name: str
    category: str
    format_offset: int        # in the format area
    data_offset: int          # in the file
    data_size: int
    samples: int
    rate: int
    rate2: int
    codec: int
    streamed: bool            # format byte 5 bit 0: played from the file, not loaded with the bank
    looped: bool              # format byte 5 bit 1
    channel_mask: int         # format byte 6: one bit per channel; 0 = mono
    flags7: int               # format byte 7: bit 0 = 3D (cleared by the game for multichannel waves)
    sound_id: int             # format +0x0C
    params: Dict[int, float] = field(default_factory=dict)
    riff: Dict[str, object] = field(default_factory=dict)

    @property
    def channels(self) -> int:
        return bin(self.channel_mask).count("1") or 1

    @property
    def is_3d(self) -> bool:
        return bool(self.flags7 & 1) and self.channel_mask == 0


@dataclass
class Bank:
    path: str
    size: int
    header: Dict[str, int]
    name: str
    categories: List[str]
    waves: List[Wave]
    problems: List[str]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def _cstr(b: bytes, o: int) -> str:
    e = b.find(b"\0", o)
    return b[o:e if e >= 0 else len(b)].decode("latin-1")


def parse_riff(b: bytes) -> Dict[str, object]:
    """The RIFF/WAVE file a wave is stored as: its format chunk (XMA's XMAWAVEFORMAT, or PCM),
    and the size of each chunk."""
    out: Dict[str, object] = {}
    if len(b) < 12 or b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        return {"error": "no RIFF/WAVE header"}
    out["riff_size"] = _u32(b, 4) + 8
    o = 12
    chunks = {}
    while o + 8 <= len(b):
        cid, size = b[o:o + 4].decode("latin-1"), _u32(b, o + 4)
        chunks[cid] = (o + 8, size)
        o += 8 + size + (size & 1)
    out["chunks"] = {k: v[1] for k, v in chunks.items()}
    if "fmt " in chunks:
        fo, fs = chunks["fmt "]
        tag = struct.unpack_from("<H", b, fo)[0]
        out["format_tag"] = tag
        if tag == 0x0165 and fs >= 0x20:   # WAVE_FORMAT_XMA: XMAWAVEFORMAT with one XMASTREAMFORMAT
            bits, enc, skip, streams, loops, version = struct.unpack_from("<HHHHBB", b, fo + 2)
            out.update(bits_per_sample=bits, encode_options=enc, largest_skip=skip, streams=streams,
                       loop_count=loops, xma_version=version)
            s = []
            for k in range(streams):
                so = fo + 12 + 20 * k
                if so + 20 > fo + fs:
                    break
                bps, rate, ls, le, sub, ch, mask = struct.unpack_from("<IIIIBBH", b, so)
                s.append(dict(bytes_per_sec=bps, rate=rate, loop_start=ls, loop_end=le, subframes=sub,
                              channels=ch, channel_mask=mask))
            out["xma_streams"] = s
        elif fs >= 16:                    # WAVEFORMATEX
            ch, rate, bps, align, bits = struct.unpack_from("<HIIHH", b, fo + 2)
            out.update(channels=ch, rate=rate, bytes_per_sec=bps, block_align=align, bits_per_sample=bits)
    if "seek" in chunks:
        so, ss = chunks["seek"]
        out["seek_entries"] = ss // 4
    if "data" in chunks:
        out["data_size"] = chunks["data"][1]
    return out


def read_bank(path: Path, with_riff: bool = True) -> Bank:
    path = Path(path)
    size = path.stat().st_size
    problems: List[str] = []
    with open(path, "rb") as f:
        head = f.read(0x1000)
        if head[:4] != MAGIC:
            raise ValueError(f"{path}: not an NSLB sound bank")
        hdr = {name: _u32(head, off) for off, name in HEADER_FIELDS}
        f.seek(0)
        h = f.read(hdr["header_size"])
        if head[4] != 1 or head[5] != 0:   # the loader's own check
            problems.append(f"version bytes {head[4]} {head[5]} (the game wants 1 0)")
        n = hdr["wave_count"]
        names = h[hdr["names_offset"]:hdr["names_offset"] + hdr["names_size"]]
        bank_name = _cstr(h, 0x40)
        cats = []
        waves: List[Wave] = []
        name_offs = struct.unpack_from(f"<{n}I", h, hdr["name_table_offset"])
        fa = hdr["format_area_offset"]
        for i in range(n):
            fo, doff, dsize, samples = struct.unpack_from("<4I", h, hdr["wave_table_offset"] + 16 * i)
            r = fa + fo
            rate, rate2, codec, f5, chmask, f7, cat, sid = struct.unpack_from("<HHBBBBII", h, r)
            lo, hi = struct.unpack_from("<II", h, r + 0x10)
            mask = lo | hi << 32
            vals = struct.unpack_from(f"<{bin(mask).count('1')}f", h, r + 0x18)
            params = {}
            k = 0
            for bit in range(64):
                if mask >> bit & 1:
                    params[bit] = round(vals[k], 6)
                    k += 1
            catname = _cstr(names, cat) if cat else ""
            if catname and catname not in cats:
                cats.append(catname)
            streamed = bool(f5 & 1)
            base = hdr["stream_offset"] if streamed else hdr["resident_offset"]
            w = Wave(i, _cstr(names, name_offs[i]), catname, fo, base + doff, dsize, samples, rate, rate2,
                     codec, streamed, bool(f5 & 2), chmask, f7, sid, params)
            if with_riff:
                f.seek(w.data_offset)
                blob = f.read(min(dsize, 0x400))
                w.riff = parse_riff(blob)
                if w.riff.get("riff_size") != dsize:
                    problems.append(f"wave {i} {w.name}: RIFF size {w.riff.get('riff_size')} != table size {dsize}")
                st = (w.riff.get("xma_streams") or [{}])[0]
                rr = st.get("rate", w.riff.get("rate"))
                if rr and rr != rate:
                    problems.append(f"wave {i} {w.name}: RIFF rate {rr} != format rate {rate}")
                ch = st.get("channels", w.riff.get("channels"))
                if ch and ch != w.channels:
                    problems.append(f"wave {i} {w.name}: RIFF channels {ch} != format channels {w.channels}")
            if w.data_offset + dsize > size:
                problems.append(f"wave {i} {w.name}: data past the end of the file")
            if codec not in CODECS:
                problems.append(f"wave {i} {w.name}: unknown codec byte 0x{codec:02X}")
            waves.append(w)
        # records end where the next one starts: 0x18 + 4 * params, rounded to 16 (0x82121038)
        # (the last record may end the area unpadded)
        for w in waves:
            exact = 0x18 + 4 * len(w.params)
            need = (exact + 15) & ~15
            later = [x.format_offset for x in waves if x.format_offset > w.format_offset]
            room = (min(later) if later else hdr["format_area_size"]) - w.format_offset
            if room != need and not (not later and exact <= room <= need):
                problems.append(f"wave {w.index}: format record is {room} bytes, expected {need}")
                break
        res_end = hdr["resident_offset"] + hdr["resident_size"]
        if hdr["resident_size"] and res_end > size:
            problems.append("resident data runs past the end of the file")
        if hdr["stream_size"] and hdr["stream_offset"] + hdr["stream_size"] > size:
            problems.append("stream data runs past the end of the file")
    return Bank(str(path), size, hdr, bank_name, cats, waves, problems)


def summary(b: Bank) -> str:
    h = b.header
    streamed = sum(w.streamed for w in b.waves)
    names = len({w.name for w in b.waves})
    codecs = sorted({CODECS.get(w.codec, hex(w.codec)) for w in b.waves})
    rates = sorted({w.rate for w in b.waves})
    secs = sum(w.samples / w.rate for w in b.waves if w.rate)
    lines = [
        f"{b.path}: bank {b.name!r}, {b.size:,} bytes",
        f"  {h['wave_count']} waves ({names} names, {streamed} streamed, {sum(w.is_3d for w in b.waves)} 3D, "
        f"{sum(w.looped for w in b.waves)} looped), {secs / 60:.1f} min of sound",
        f"  codecs {', '.join(codecs)}; rates {', '.join(map(str, rates))}",
        f"  categories: {', '.join(b.categories) or '-'}",
        f"  header 0x{h['header_size']:X}; resident 0x{h['resident_size']:X} at 0x{h['resident_offset']:X}; "
        f"stream 0x{h['stream_size']:X} at 0x{h['stream_offset']:X}",
    ]
    lines.append("  checks: " + ("all passed" if not b.problems else f"{len(b.problems)} problem(s)"))
    lines += [f"    {p}" for p in b.problems[:20]]
    return "\n".join(lines)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("banks", nargs="+", type=Path)
    ap.add_argument("--list", action="store_true", help="one line per wave")
    ap.add_argument("--json", type=Path, help="write everything to this JSON file (one bank)")
    ap.add_argument("--extract", type=Path, help="write each wave's RIFF file into this folder (one bank)")
    a = ap.parse_args(argv)
    bad = False
    for p in a.banks:
        b = read_bank(p)
        print(summary(b))
        bad |= bool(b.problems)
        if a.list:
            for w in b.waves:
                print(f"  {w.index:4} {w.name:40.40} {w.category:9.9} {CODECS.get(w.codec, hex(w.codec)):5.5} "
                      f"{w.rate:6} {w.channels}ch {'S' if w.streamed else '-'}{'L' if w.looped else '-'}"
                      f"{'3' if w.is_3d else '-'} {w.samples / max(w.rate, 1):7.2f}s "
                      + " ".join(f"{PARAMS.get(k, k)}={v:g}" for k, v in w.params.items()))
        if a.json:
            a.json.write_text(json.dumps(asdict(b), indent=1))
        if a.extract:
            a.extract.mkdir(parents=True, exist_ok=True)
            seen: Dict[str, int] = {}
            with open(p, "rb") as f:
                for w in b.waves:
                    k = seen.get(w.name, 0)
                    seen[w.name] = k + 1
                    f.seek(w.data_offset)
                    ext = ".xma" if w.codec == 0x30 else ".wav"
                    (a.extract / f"{w.name}{'_' + str(k) if k else ''}{ext}").write_bytes(f.read(w.data_size))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
