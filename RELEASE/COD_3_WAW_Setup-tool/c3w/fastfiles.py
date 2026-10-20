"""Which game a Call of Duty fastfile (.ff) comes from, read from its header.

Call of Duty 2 on the Xbox 360 and every IW-engine game from Call of Duty 4
on keep their assets in fastfiles. From Call of Duty 4 on they all start the
same way: an 8-byte magic, then a u32 version (Call of Duty 2's have no
magic; CONSOLE_ZLIB_STARTS and COD2_XBOX360_VERSION below). The
magic says how the file is packed ("IWffu100" unsigned, "IWff0100" signed,
"TAff0100" Black Ops II, "S1ff0100" Advanced Warfare); the version says
which game and which platform it was built for. Consoles (Xbox 360, PS3, Wii)
store the version big-endian, PCs little-endian, so the byte order tells a
console file from a PC file even when two versions are equal (1 is Call of
Duty 4 on a console and Modern Warfare 3 on PC).

The setup tool uses this to rip only World at War's fastfiles from a folder
and to say what the others are; the Rust engine's `fastfile` crate reads
the same table (docs/formats/fastfiles.md has both, with sources).

Sources (docs/SOURCES.md, "Fastfiles of every game"):
  * found by us in the owner's PC copies (2026-10-20): Call of Duty 4 5,
    World at War 387, Modern Warfare 2 276, Black Ops 473, Modern Warfare 3 1,
    Black Ops II 147 ("TAff0100"), Ghosts 565; and on his Xbox 360 discs
    (2026-10-07 and 10-20): Call of Duty 2 (no magic, see below), Call of
    Duty 4 1, World at War 387, Modern Warfare 2 269, Black Ops 473, all
    big-endian;
  * the console and Wii versions: the COD Engine Research wiki's FastFiles
    page (codresearch.dev), checked against CoD-FF-Tools' table where both
    have them.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

MAGIC_UNSIGNED = b"IWffu100"
MAGIC_SIGNED = b"IWff0100"
MAGIC_TREYARCH = b"TAff0100"     # Black Ops II (and III)
MAGIC_SLEDGEHAMMER = b"S1ff0100"  # Advanced Warfare
MAGICS = (MAGIC_UNSIGNED, MAGIC_SIGNED, MAGIC_TREYARCH, MAGIC_SLEDGEHAMMER)
# Signed files carry an authentication header with this magic right after
# the container header (World at War Xbox 360 at byte 12, Modern Warfare 2
# and 3 PC at byte 21).
MAGIC_AUTH = b"IWffs100"
# Encrypted files (Black Ops II on PC at byte 12, Black Ops on the Xbox 360 at
# byte 16): this magic, then the zone name. We don't read those.
MAGIC_ENCRYPTED = b"PHEEBs71"
HEADER_SIZE = 12        # magic + version
READ_SIZE = 64          # enough of the file for every check below

# Games (keys stay stable: the setup tool's logs and tests use them).
COD2 = "cod2"
COD4 = "cod4"
WAW = "waw"
MW2 = "mw2"
BO1 = "bo1"
MW3 = "mw3"
BO2 = "bo2"
GHOSTS = "ghosts"
AW = "aw"
BO3 = "bo3"

GAME_NAMES = {
    COD2: "Call of Duty 2",
    COD4: "Call of Duty 4: Modern Warfare",
    WAW: "Call of Duty: World at War",
    MW2: "Call of Duty: Modern Warfare 2",
    BO1: "Call of Duty: Black Ops",
    MW3: "Call of Duty: Modern Warfare 3",
    BO2: "Call of Duty: Black Ops II",
    GHOSTS: "Call of Duty: Ghosts",
    AW: "Call of Duty: Advanced Warfare",
    BO3: "Call of Duty: Black Ops III",
}

PC = "PC"
CONSOLE = "Xbox 360 or PS3"   # the two share their versions; see _refine_console
XBOX360 = "Xbox 360"
WII = "Wii"
WIIU = "Wii U"

# (game, platform, version, big-endian, where the number comes from)
VERSIONS: Tuple[Tuple[str, str, int, bool, str], ...] = (
    (COD4, PC, 0x5, False, "owner's PC copy"),
    (COD4, CONSOLE, 0x1, True, "owner's Xbox 360 disc"),
    (COD4, WII, 0x1A2, True, "codresearch.dev"),
    (WAW, PC, 0x183, False, "owner's PC copy"),
    (WAW, CONSOLE, 0x183, True, "owner's Xbox 360 disc"),
    (WAW, WII, 0x19B, True, "codresearch.dev"),
    (MW2, PC, 0x114, False, "owner's PC copy"),
    (MW2, CONSOLE, 0x10D, True, "owner's Xbox 360 disc"),
    (BO1, PC, 0x1D9, False, "owner's PC copy"),
    (BO1, CONSOLE, 0x1D9, True, "owner's Xbox 360 disc"),
    (BO1, WII, 0x1DD, True, "codresearch.dev"),
    (MW3, PC, 0x1, False, "owner's PC copy"),
    (MW3, CONSOLE, 0x70, True, "codresearch.dev"),
    (MW3, WII, 0x6B, True, "codresearch.dev"),
    (BO2, PC, 0x93, False, "owner's PC copy"),
    (BO2, CONSOLE, 0x92, True, "codresearch.dev"),
    (BO2, WIIU, 0x94, True, "codresearch.dev"),
    (GHOSTS, PC, 0x235, False, "owner's PC copy"),
    (GHOSTS, CONSOLE, 0x22E, True, "codresearch.dev"),
    (AW, PC, 0x72E, False, "codresearch.dev"),
    (AW, CONSOLE, 0x72E, True, "codresearch.dev"),
    (BO3, PC, 0x1FB, False, "codresearch.dev (beta)"),
    (BO3, XBOX360, 0x253, True, "codresearch.dev"),
    (BO3, "PS3", 0x25E, True, "codresearch.dev"),
)

# Which magics each game uses (a version under another game's magic is not
# that game: Black Ops II's 0x93 under "IWffu100" means nothing).
GAME_MAGICS: Dict[str, Tuple[bytes, ...]] = {
    BO2: (MAGIC_TREYARCH,),
    BO3: (MAGIC_TREYARCH,),
    AW: (MAGIC_SLEDGEHAMMER,),
}
DEFAULT_MAGICS = (MAGIC_UNSIGNED, MAGIC_SIGNED)


@dataclass(frozen=True)
class Identity:
    """What a fastfile's header says about it."""
    game: str            # COD4, WAW, ... (GAME_NAMES has the full name)
    platform: str        # PC, XBOX360, CONSOLE, WII, WIIU, "PS3"
    version: int
    big_endian: bool
    signed: bool         # RSA-signed, read through its hash chunks
    encrypted: bool      # the zone is encrypted (Black Ops II on PC, Black Ops on the Xbox 360); not read

    @property
    def game_name(self) -> str:
        return GAME_NAMES.get(self.game, self.game)

    def describe(self) -> str:
        """"Call of Duty 4: Modern Warfare (PC, version 5)"."""
        extra = ", signed" if self.signed else ""
        extra += ", encrypted" if self.encrypted else ""
        return f"{self.game_name} ({self.platform}, version {self.version}{extra})"


def _is_zlib_header(two: bytes) -> bool:
    """A zlib stream starts with CMF/FLG: deflate with a window up to 32 KB,
    and the pair a multiple of 31 (RFC 1950, section 2.2)."""
    return len(two) == 2 and two[0] & 0x0F == 8 and two[0] >> 4 <= 7 and ((two[0] << 8) | two[1]) % 31 == 0


# Where an unsigned Xbox 360 file's zlib stream starts: right after the
# 12-byte header (Call of Duty 4, World at War, checked on the owner's discs),
# or after Modern Warfare 2's 25 more bytes (online-update flag, file time,
# region, entry count and sizes; checked on the owner's disc).
CONSOLE_ZLIB_STARTS = (HEADER_SIZE, HEADER_SIZE + 25)

# Call of Duty 2 on the Xbox 360 (2005) has fastfiles too, without a magic: a
# big-endian u32 0x68, the zone's size, three block sizes, then one zlib
# stream from byte 20 (checked on all 111 fastfiles of the owner's disc: the
# stream inflates to exactly the size the header gives).
COD2_XBOX360_VERSION = 0x68
COD2_XBOX360_ZLIB_START = 20


def _refine_console(header: bytes, signed: bool) -> str:
    """The Xbox 360 and PS3 builds share their version numbers. A signed file
    with its authentication header at byte 12 is the Xbox 360's (World at War's
    disc, checked); so is an unsigned one whose zlib stream starts at byte 12
    (the Xbox 360 packs the zone as one stream; CoD-FF-Tools describes PS3
    files as raw deflate blocks, each after a 2-byte size). Anything else
    stays "Xbox 360 or PS3"."""
    if signed and header[HEADER_SIZE:HEADER_SIZE + 8] == MAGIC_AUTH:
        return XBOX360
    if not signed and any(_is_zlib_header(header[at:at + 2]) for at in CONSOLE_ZLIB_STARTS):
        return XBOX360
    return CONSOLE


def identify(header: bytes) -> Optional[Identity]:
    """The game and platform of a fastfile, from its first bytes (READ_SIZE
    is enough); None when it isn't a fastfile we know."""
    if len(header) < HEADER_SIZE:
        return None
    magic = bytes(header[:8])
    if magic not in MAGICS:
        if (int.from_bytes(header[:4], "big") == COD2_XBOX360_VERSION
                and _is_zlib_header(header[COD2_XBOX360_ZLIB_START:COD2_XBOX360_ZLIB_START + 2])):
            return Identity(COD2, XBOX360, COD2_XBOX360_VERSION, True, False, False)
        return None
    raw = bytes(header[8:12])
    signed = magic == MAGIC_SIGNED
    for game, platform, version, big_endian, _source in VERSIONS:
        if int.from_bytes(raw, "big" if big_endian else "little") != version:
            continue
        if magic not in GAME_MAGICS.get(game, DEFAULT_MAGICS):
            continue
        if platform == CONSOLE:
            platform = _refine_console(header, signed)
        encrypted = MAGIC_ENCRYPTED in bytes(header[HEADER_SIZE:HEADER_SIZE + 20])
        return Identity(game, platform, version, big_endian, signed, encrypted)
    return None


def unknown_version(header: bytes) -> Optional[str]:
    """For a file with a fastfile magic but a version no game here uses: a
    short description ("IWffu100, version 0x1234"), else None."""
    if len(header) >= HEADER_SIZE and bytes(header[:8]) in MAGICS and identify(header) is None:
        return (f"{header[:8].decode('ascii')}, version {int.from_bytes(header[8:12], 'little'):#x} "
                f"(read little-endian; {int.from_bytes(header[8:12], 'big'):#x} big-endian)")
    return None


def identify_file(path: Path) -> Optional[Identity]:
    """identify() on the start of a file (None if it can't be read)."""
    try:
        with open(path, "rb") as f:
            return identify(f.read(READ_SIZE))
    except OSError:
        return None


def is_world_at_war(path: Path) -> bool:
    ident = identify_file(path)
    return ident is not None and ident.game == WAW


def other_game(identities: Iterable[Optional[Identity]]) -> Optional[Identity]:
    """For a set of files that should be World at War's: the first other
    game's identity when none of them is World at War's (None when one is, or
    when none could be placed: such files are left for the ripper to judge)."""
    found = [i for i in identities if i is not None]
    if any(i.game == WAW for i in found):
        return None
    return found[0] if found else None


def split_by_game(files: Iterable[Path]) -> Tuple[List[Path], Dict[str, List[Path]]]:
    """(World at War's fastfiles, every other game's by its description). A
    file whose header can't be placed counts as World at War's: the ripper
    then says what is wrong with it."""
    waw: List[Path] = []
    others: Dict[str, List[Path]] = {}
    for path in files:
        ident = identify_file(path)
        if ident is None or ident.game == WAW:
            waw.append(path)
        else:
            others.setdefault(f"{ident.game_name} ({ident.platform})", []).append(path)
    return waw, others
