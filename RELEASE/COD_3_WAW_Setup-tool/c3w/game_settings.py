"""The game's own settings file, <output>/cod3.toml, for the Play page.

The game reads cod3.toml when it starts and writes it from its F2 settings
window (and when it quits): one `name = value` line per setting changed from
its default, strings escaped like SDK patch 0026 does. Nested tables work too
([cod3] then `max_fps = 60` is cod3_max_fps), as the SDK reads them.

SETTINGS lists the common ones with the names, choices, ranges and defaults
of the game's source (ui/hud_overlay.cpp, game/render_resolution.cpp,
game/shadows.cpp, game/button_prompts.cpp, cod3_app.cpp and the SDK's own
settings). load() reads them; save() changes only the given ones and keeps
every other byte of the file as it was (comments, order, other settings).
save() refuses while the game runs, since the game would write over it.
The field of view isn't here: the game keeps it in userdata/cod3_config.cfg
(cg_fov), not in cod3.toml.
"""
from __future__ import annotations

import csv
import io
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional, Set, Tuple

from . import common

FILE_NAME = "cod3.toml"
HEADER = "# Auto-generated cvar configuration\n"   # what the SDK starts the file with
MAX_FILE = 1024 * 1024   # bytes; the game's own file is a few KB


# Custom render sizes the game takes (kMin/MaxCustomWidth/Height in
# game/render_resolution.h; the console's r_mode uses the same limits).
MIN_CUSTOM_SIZE = (640, 360)
MAX_CUSTOM_SIZE = (15360, 4320)
# Picture shapes the game takes for cod3_aspect_ratio (game/ultrawide.h):
# from the console's own 4:3 to three 16:9 screens side by side.
MIN_ASPECT = 4 / 3
MAX_ASPECT = 48 / 9


def _is_resolution(value: Any) -> bool:
    """A size the game takes for cod3_render_resolution besides the listed
    ones: "WxH" within the custom limits, or the other names "4k" and "8k"
    (ParseResolutionChoice)."""
    if value in ("4k", "8k"):
        return True
    m = re.fullmatch(r"(\d{1,5})x(\d{1,5})", value) if isinstance(value, str) else None
    return bool(m) and MIN_CUSTOM_SIZE[0] <= int(m.group(1)) <= MAX_CUSTOM_SIZE[0] and \
        MIN_CUSTOM_SIZE[1] <= int(m.group(2)) <= MAX_CUSTOM_SIZE[1]


def _is_aspect(value: Any) -> bool:
    """A picture shape of the player's own for cod3_aspect_ratio: "W:H"
    (24:10, 2.39:1) or a number (2.39), from 4:3 to 48:9 (ParseAspect)."""
    m = re.fullmatch(r"(\d{1,5}(?:\.\d{1,4})?)(?::(\d{1,5}(?:\.\d{1,4})?))?", value) \
        if isinstance(value, str) and len(value) <= 16 else None
    if not m:
        return False
    w, h = float(m.group(1)), float(m.group(2) or 1)
    return w > 0 and h > 0 and MIN_ASPECT - 0.001 <= w / h <= MAX_ASPECT + 0.001


@dataclass(frozen=True)
class Setting:
    key: str                 # the name in cod3.toml (or a name of our own when `cvars` is set)
    label: str               # short label for the GUI
    help: str                # one or two plain sentences
    kind: str                # "choice", "bool" or "int"
    choices: Tuple[Tuple[Any, str], ...] = ()   # (value, label); for "int", suggested values
    min: int = 0             # for "int"
    max: int = 0
    default: Any = None
    platforms: Tuple[str, ...] = ("linux", "windows")
    # Where the value is kept when it isn't `key`: every name listed is
    # written, the first one is read. `invert`: a bool kept inverted.
    cvars: Tuple[str, ...] = ()
    invert: bool = False
    scale: int = 1           # "int" kept divided by this (volume 0-100 % as 0.0-1.0)
    also: Optional[Callable[[Any], bool]] = field(default=None, compare=False)   # other values a "choice" takes

    @property
    def names(self) -> Tuple[str, ...]:
        return self.cvars or (self.key,)

    def valid(self, value: Any) -> bool:
        """Whether value (as the GUI holds it) is allowed."""
        if self.kind == "bool":
            return isinstance(value, bool)
        if self.kind == "int":
            return isinstance(value, int) and not isinstance(value, bool) and self.min <= value <= self.max
        if self.kind == "choice":
            if any(type(value) is type(v) and value == v for v, _ in self.choices):
                return True
            return bool(self.also and self.also(value))
        return False


_FPS = (0, 30, 60, 85, 120, 144, 165, 200, 240, 300, 360, 500, 1000)

SETTINGS: List[Setting] = [
    # The same choices, in the same order, as kResolutionChoices in the
    # game's game/render_resolution.h (F2 and the game's Options > Video).
    Setting("cod3_render_resolution", "Render resolution",
            "How many pixels the picture has. Natively the game draws every pixel itself (no MSAA, no "
            "scaling) up to about 1920x1080; above that the graphics card draws each of the game's pixels "
            "as several. A size bigger than the screen or window is drawn at its size.",
            "choice", (("auto", "Match the screen (native)"), ("720p", "720p (1280x720)"),
                       ("1080p", "1080p (1920x1080)"), ("1440p", "1440p (2560x1440)"),
                       ("2160p", "2160p / 4K (3840x2160)"), ("4320p", "4320p / 8K (7680x4320)"),
                       ("original", "Xbox 360 original (1040x624, stretched)")),
            default="auto", also=_is_resolution),
    Setting("cod3_render_method", "How it is drawn",
            "Native: the game draws the whole picture itself, without MSAA. Scaled is the older way: at "
            "most 1280x720 with the game's own 2x MSAA, multiplied by the graphics card.",
            "choice", (("native", "Native: the game draws every pixel itself"),
                       ("scaled", "Scaled: smaller with the game's 2x MSAA, multiplied by the GPU")),
            default="native"),
    Setting("cod3_render_percent", "Resolution scale (%)",
            "Below 100 draws fewer pixels and stretches them to the screen, for speed.",
            "int", min=25, max=100, default=100),
    Setting("cod3_live_resolution", "Change the resolution without restarting",
            "On: a new render resolution applies while the game runs. Off: it applies at the next start.",
            "bool", default=True),
    Setting("cod3_max_fps", "Frame rate limit",
            "The most frames per second the game may draw (0 = no limit). It's a cap, not a target. "
            "F2, the game's Options and com_maxfps in its console change this same setting.",
            "int", tuple((v, "No limit" if v == 0 else str(v)) for v in _FPS), min=0, max=1000, default=60),
    Setting("fullscreen", "Display mode",
            "Play in a window, or fill the whole monitor without changing its resolution (borderless).",
            "choice", ((False, "Windowed"), (True, "Fullscreen (borderless)")), default=True),
    # [claude-9b] start_in_background (SDK patch 0063) isn't listed: a launch option for testing only
    # (owner 2026-10-20), --start_in_background=true in Extra arguments (game/relaunch.h).
    Setting("vsync", "V-Sync (no tearing)",
            "Shows each new picture only when the monitor starts a new refresh, so there is no tearing. "
            "It doesn't cap the frame rate.",
            "bool", default=False, invert=True,
            cvars=("vulkan_allow_present_mode_immediate", "d3d12_allow_variable_refresh_rate_and_tearing")),
    # The same choices, in the same order, as F2 > Video > Aspect ratio
    # (ui/hud_overlay.cpp); "resolution" is kMatchResolution (game/ultrawide.h).
    Setting("cod3_aspect_ratio", "Aspect ratio",
            "The shape of the picture; the view up and down stays the game's. Wider than 16:9 shows more at "
            "the sides (the HUD and menus stay 16:9), narrower shows less. \"Match the render resolution\" "
            "takes the shape of a custom render size such as 2560x1080. F2 also takes a shape of your own "
            "(for example 24:10).",
            "choice", (("auto", "Automatic (the window's shape)"), ("resolution", "Match the render resolution"),
                       ("4:3", "4:3 (the classic TV shape)"), ("16:10", "16:10"),
                       ("16:9", "16:9 (the game's own)"), ("21:9", "21:9 (ultrawide)"),
                       ("32:9", "32:9 (super ultrawide)")), default="auto",
            also=_is_aspect),
    Setting("swap_post_effect", "Anti-aliasing",
            "Smooths jagged edges after the picture is drawn. Costs very little; Extreme smooths more.",
            "choice", (("none", "Off"), ("fxaa", "FXAA"), ("fxaa_extreme", "FXAA (extreme)")), default="none"),
    Setting("anisotropic_override", "Anisotropic filtering",
            "Keeps floors and walls sharp when you look along them at a distance.",
            "choice", ((-1, "Game default"), (0, "Off"), (1, "1x"), (2, "2x"), (3, "4x"), (4, "8x"), (5, "16x")),
            default=3),
    Setting("cod3_shadow_quality", "Shadows",
            "Shadows of soldiers, vehicles and other moving things. Off removes them and keeps the "
            "sunlight.",
            "choice", (("original", "Original"), ("off", "Off")), default="original"),
    # [post-effects] The game's own depth of field and motion blur
    # (recomp/src/game/post_effects.h); campaign and multiplayer.
    Setting("cod3_depth_of_field", "Depth of field",
            "The blurred background the game draws while you aim, where a mission or map uses it. "
            "Off keeps the whole picture sharp.", "bool", default=True),
    Setting("cod3_motion_blur", "Motion blur",
            "The screen trails the game shows at some moments. Off never draws them.", "bool", default=True),
    Setting("cod3_hd_textures", "HD textures (waifu2x)",
            "Use the HD texture pack this tool made, when it is there.", "bool", default=True),
    Setting("cod3_user_textures", "Your own textures",
            "Use your own texture pack (for example the insignia), when it is there.", "bool", default=True),
    Setting("cod3_graphics_api", "Graphics API",
            "Which graphics API draws the game. Automatic uses Vulkan; Direct3D 12 is there if Vulkan "
            "doesn't work on your PC.",
            "choice", (("auto", "Automatic (Vulkan)"), ("d3d12", "Direct3D 12"), ("vulkan", "Vulkan")),
            default="auto", platforms=("windows",)),
    Setting("cod3_skip_movies", "Skip cutscenes",
            "Skips the logos, the briefing films and the level intros as soon as the game allows it.",
            "bool", default=False),
    Setting("cod3_volume", "Master volume",
            "Everything the game plays, in percent. Music, effects and voices have their own sliders in "
            "the game's Options.", "int", min=0, max=100, default=100, scale=100),
    # [voice] game/voice_language.h; the other languages come from another disc (c3w/voices.py)
    Setting("cod3_voice_language", "Voices",
            "The language the soldiers speak in the campaign and its films. German, French, Spanish and "
            "Italian need that disc's voices: Add voices from another disc. The text stays as it is.",
            "choice", (("disc", "The game's own"), ("en", "English"), ("de", "German"), ("fr", "French"),
                       ("es", "Spanish"), ("it", "Italian")), default="disc"),
    # The SDK's own setting (XGetLanguage): Xbox 360 language numbers.
    Setting("user_language", "Console language",
            "The language the game is told the console uses. It picks the films' sound and, on the "
            "French/Spanish/Italian disc, the soldiers' language and the text. Leave English for the "
            "USA/Europe disc.",
            "choice", ((1, "English"), (3, "German"), (4, "French"), (5, "Spanish"), (6, "Italian")),
            default=1),
    Setting("cod3_glyph_style", "Controller button style",
            "How the game's prompts show controller buttons.",
            "choice", (("xbox", "Xbox 360 (the game's own)"), ("playstation", "PlayStation"),
                       ("gamecube", "GameCube")), default="xbox"),
]
BY_KEY: Dict[str, Setting] = {s.key: s for s in SETTINGS}


def settings_file(out_dir: Path) -> Path:
    return Path(out_dir) / FILE_NAME


def for_platform(platform: str) -> List[Setting]:
    """The settings that apply to "linux" or "windows"."""
    return [s for s in SETTINGS if platform in s.platforms]


# ---------------------------------------------------------------------------
# Reading TOML (the part of it a settings file uses)
# ---------------------------------------------------------------------------
_PART = r'(?:[A-Za-z0-9_-]+|"(?:[^"\\\n]|\\.)*"|\'[^\'\n]*\')'
_KEY_LINE = re.compile(r"^([ \t]*)(" + _PART + r"(?:[ \t]*\.[ \t]*" + _PART + r")*)([ \t]*=[ \t]*)")
_TABLE = re.compile(r"^[ \t]*\[([^\[\]]*)\][ \t]*(?:#.*)?$")
_ARRAY_TABLE = re.compile(r"^[ \t]*\[\[")
_REST = re.compile(r"[ \t]*(?:#.*)?$")
_ESCAPES = {"b": "\b", "t": "\t", "n": "\n", "f": "\f", "r": "\r", '"': '"', "\\": "\\"}


def _key_name(text: str) -> str:
    """A dotted key or table name joined with "_", as the SDK flattens them."""
    parts = re.findall(_PART, text)
    out = []
    for p in parts:
        if p.startswith('"'):
            p = _unescape(p[1:-1])
        elif p.startswith("'"):
            p = p[1:-1]
        out.append(p)
    return "_".join(out)


def _unescape(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c != "\\" or i + 1 >= len(s):
            out.append(c)
            i += 1
            continue
        e = s[i + 1]
        if e in _ESCAPES:
            out.append(_ESCAPES[e])
            i += 2
        elif e in "uU":
            n = 4 if e == "u" else 8
            try:
                out.append(chr(int(s[i + 2:i + 2 + n], 16)))
            except (ValueError, OverflowError):
                raise ValueError("bad escape") from None
            i += 2 + n
        else:
            raise ValueError("bad escape")
    return "".join(out)


def _parse_value(text: str) -> Tuple[Any, int]:
    """A one-line TOML value at the start of text: (value, characters used).
    ValueError for anything else (arrays, tables, multi-line strings)."""
    if text.startswith(('"""', "'''")):
        raise ValueError("multi-line string")
    if text.startswith('"'):
        m = re.match(r'"((?:[^"\\\n]|\\.)*)"', text)
        if not m:
            raise ValueError("unterminated string")
        return _unescape(m.group(1)), m.end()
    if text.startswith("'"):
        m = re.match(r"'([^'\n]*)'", text)
        if not m:
            raise ValueError("unterminated string")
        return m.group(1), m.end()
    m = re.match(r"(true|false)(?![A-Za-z0-9_])", text)
    if m:
        return m.group(1) == "true", m.end()
    m = re.match(r"[+-]?(?:inf|nan)(?![A-Za-z0-9_])", text)
    if m:
        return float(m.group(0)), m.end()
    m = re.match(r"0x[0-9A-Fa-f_]+|0o[0-7_]+|0b[01_]+", text)
    if m:
        return int(m.group(0).replace("_", ""), 0), m.end()
    m = re.match(r"[+-]?\d[\d_]*(\.\d[\d_]*)?([eE][+-]?\d[\d_]*)?", text)
    if m:
        number = m.group(0).replace("_", "")
        return (float(number) if (m.group(1) or m.group(2)) else int(number)), m.end()
    raise ValueError("not a value")


@dataclass
class _Line:
    name: Optional[str] = None     # the flattened setting name of a `key = value` line
    value: Any = None
    start: int = 0                 # where the value starts and ends in the line
    end: int = 0
    header: bool = False           # a [table] line


def _scan(lines: List[str]) -> List[_Line]:
    """What each line is. Lines that aren't simple `key = value` lines are
    left alone (and their settings unread)."""
    out: List[_Line] = []
    prefix: Optional[str] = ""
    skip_until: Optional[str] = None
    for raw in lines:
        line = raw.rstrip("\r\n")
        info = _Line()
        out.append(info)
        if skip_until:   # inside a multi-line string or array
            if skip_until in line:
                skip_until = None
            continue
        if _ARRAY_TABLE.match(line):
            info.header, prefix = True, None   # [[array of tables]]: not settings
            continue
        m = _TABLE.match(line)
        if m:
            info.header, prefix = True, _key_name(m.group(1).strip())
            continue
        m = _KEY_LINE.match(line)
        if not m or prefix is None:
            continue
        rest = line[m.end():]
        if rest.startswith(('"""', "'''")):
            if rest.count(rest[:3]) < 2:
                skip_until = rest[:3]
            continue
        if rest.startswith(("[", "{")):
            if rest.count(rest[0]) > rest.count("]" if rest[0] == "[" else "}"):
                skip_until = "]" if rest[0] == "[" else "}"
            continue
        try:
            value, used = _parse_value(rest)
        except ValueError:
            continue
        if not _REST.match(rest[used:]):
            continue
        name = _key_name(m.group(2))
        info.name = f"{prefix}_{name}" if prefix else name
        info.value, info.start, info.end = value, m.end(), m.end() + used
    return out


def _read_text(path: Path) -> str:
    """The file's text; bytes that aren't UTF-8 are kept as they are."""
    try:
        if path.stat().st_size > MAX_FILE:
            raise common.SetupError(f"{path.name} is too big to be the game's settings file.")
        return path.read_bytes().decode("utf-8", "surrogateescape")
    except FileNotFoundError:
        return ""
    except OSError as e:
        raise common.SetupError(f"Couldn't read {path.name}: {e.strerror or e}") from None


def read_values(out_dir: Path) -> Dict[str, Any]:
    """Every simple `name = value` setting in cod3.toml, by flattened name
    (the last one wins, as when the game reads it)."""
    lines = _read_text(settings_file(out_dir)).splitlines(keepends=True)
    return {info.name: info.value for info in _scan(lines) if info.name}


# ---------------------------------------------------------------------------
# Settings <-> TOML values
# ---------------------------------------------------------------------------
def _from_toml(s: Setting, raw: Any) -> Any:
    """A file value as the GUI holds it, or None when it doesn't fit s."""
    if s.kind == "bool" or (s.kind == "choice" and isinstance(s.default, bool)):
        if isinstance(raw, str) and raw in ("true", "false"):
            raw = raw == "true"
        if not isinstance(raw, bool):
            return None
        return (not raw) if s.invert else raw
    if s.kind == "int" or (s.kind == "choice" and isinstance(s.default, int)):
        if isinstance(raw, str) and re.fullmatch(r"[+-]?\d+(\.\d+)?", raw.strip()):
            raw = float(raw)
        if isinstance(raw, bool) or not isinstance(raw, (int, float)) or raw != raw:
            return None
        value = raw * s.scale
        if abs(value - round(value)) > 1e-6 and s.scale == 1:
            return None
        return int(round(value))
    return raw if isinstance(raw, str) else None


def _to_toml(s: Setting, value: Any) -> str:
    """The value as TOML text for the file."""
    if isinstance(value, bool):
        return "true" if (value != s.invert) else "false"
    if isinstance(value, int):
        if s.scale != 1:
            return f"{value / s.scale:.6f}"   # as the game writes a decimal setting
        return str(value)
    return toml_string(str(value))


def toml_string(value: str) -> str:
    """A TOML basic string, escaped like SDK patch 0026 (TomlString):
    quotes, backslashes and control characters, so a value can't break the
    file or add lines to it."""
    out = ['"']
    for c in value:
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif ord(c) < 0x20 or ord(c) == 0x7F:
            out.append(f"\\u{ord(c):04X}")
        else:
            out.append(c)
    out.append('"')
    return "".join(out)


def load(out_dir: Path) -> dict:
    """Every Setting's value from cod3.toml, as the GUI shows it (defaults
    for settings that aren't in the file or don't fit)."""
    found = read_values(out_dir)
    result = {}
    for s in SETTINGS:
        value = None
        raw = next((found[n] for n in s.names if n in found), None)
        if raw is not None:
            value = _from_toml(s, raw)
        result[s.key] = value if value is not None and s.valid(value) else s.default
    return result


def check(values: dict) -> None:
    """SetupError for an unknown setting or a value it doesn't allow."""
    for key, value in values.items():
        s = BY_KEY.get(key)
        if s is None:
            raise common.SetupError(f"Unknown game setting: {str(key)[:40]}")
        if not s.valid(value):
            if s.kind == "int":
                raise common.SetupError(f"{s.label}: pick a whole number from {s.min} to {s.max}.")
            raise common.SetupError(f"{s.label}: that value isn't one the game takes.")


def save(out_dir: Path, values: dict) -> None:
    """Change only the given settings in cod3.toml and keep everything else
    in the file byte for byte. Refuses while the game runs (it writes the
    file itself)."""
    check(values)
    if not values:
        return
    if game_running(out_dir):
        raise common.SetupError("Close the game first: it saves its own settings when it quits.")
    path = settings_file(out_dir)
    if not Path(out_dir).is_dir():
        raise common.SetupError("The output folder isn't there. Build the game first.")
    text = _read_text(path)
    lines = text.splitlines(keepends=True)
    newline = "\r\n" if "\r\n" in text else "\n"
    wanted: Dict[str, Tuple[Setting, Any]] = {}
    for key, value in values.items():
        for name in BY_KEY[key].names:
            wanted[name] = (BY_KEY[key], value)
    done: Set[str] = set()
    first_header = None
    for i, info in enumerate(_scan(lines)):
        if info.header and first_header is None:
            first_header = i
        if info.name in wanted:
            s, value = wanted[info.name]
            done.add(info.name)
            if _from_toml(s, info.value) == value and type(_from_toml(s, info.value)) is type(value):
                continue   # already so: the line stays as it is
            line = lines[i]
            lines[i] = line[:info.start] + _to_toml(s, value) + line[info.end:]
    # A setting the file doesn't have is its default already; only others are added.
    added = [f"{name} = {_to_toml(s, value)}{newline}" for name, (s, value) in wanted.items()
             if name not in done and not (value == s.default and type(value) is type(s.default))]
    if added:
        if not lines:
            lines = [HEADER.replace("\n", newline)]
        # New settings go with the other top-level ones, before any [table].
        at = first_header if first_header is not None else len(lines)
        while first_header is not None and at > 0 and not lines[at - 1].strip():
            at -= 1   # above the blank lines before the table
        if at > 0 and not lines[at - 1].endswith(("\n", "\r")):
            lines[at - 1] += newline
        lines[at:at] = added
    data = "".join(lines).encode("utf-8", "surrogateescape")
    tmp = path.with_name(path.name + ".tmp")
    try:
        tmp.write_bytes(data)
        os.replace(tmp, path)
    except OSError as e:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise common.SetupError(f"Couldn't write {path.name}: {e.strerror or e}") from None


# ---------------------------------------------------------------------------
# Is the game running?
# ---------------------------------------------------------------------------
DEFAULT_EXES = ("cod3", "cod3.exe", "CoD3-x86_64.AppImage")


def exe_names(out_dir: Optional[Path] = None) -> Set[str]:
    """The game's executable names (lowercase): the usual ones and those the
    build's record (cod3_waw.json) names."""
    names = {n.lower() for n in DEFAULT_EXES}
    if out_dir:
        manifest = common.read_json(Path(out_dir) / "cod3_waw.json")
        if isinstance(manifest, dict):
            for key in ("cod3_exe", "cod3_exe_linux", "cod3_exe_windows"):
                value = manifest.get(key)
                if isinstance(value, str) and value.strip():
                    names.add(re.split(r"[\\/]", value.strip())[-1].lower())
    return names


_LOADERS = ("wine", "proton")


def _base(arg: str) -> str:
    return re.split(r"[\\/]", arg)[-1].lower()


def _matches(candidates: Iterable[str], names: Set[str]) -> bool:
    for c in candidates:
        c = _base(c) if c else ""
        # /proc/<pid>/comm keeps 15 characters.
        if c and (c in names or (len(c) == 15 and any(n.startswith(c) for n in names))):
            return True
    return False


def _ended(pid_dir: Path) -> bool:
    """True for a process that has ended but not been collected by its parent
    yet (state Z, "zombie"; X, dead): its name stays in /proc until then, so it
    would look like a game that is still running."""
    try:
        stat = (pid_dir / "stat").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    # "<pid> (<comm>) <state> ...": the name may hold spaces or parentheses.
    rest = stat[stat.rfind(")") + 1:].split()
    return bool(rest) and rest[0] in ("Z", "X")


def processes_in(proc: Path) -> Iterable[List[str]]:
    """For each process in a /proc-style folder: its comm, executable link
    and first two command-line arguments (what's readable). Processes that
    have ended (zombies) are left out."""
    try:
        pids = [p for p in proc.iterdir() if p.name.isdigit()]
    except OSError:
        return
    me = str(os.getpid())
    for p in pids:
        if p.name == me or _ended(p):
            continue
        found: List[str] = []
        try:
            found.append((p / "comm").read_text(encoding="utf-8", errors="replace").strip())
        except OSError:
            pass
        try:
            found.append(os.readlink(p / "exe"))
        except OSError:
            pass
        try:
            args = [a.decode("utf-8", "replace") for a in (p / "cmdline").read_bytes().split(b"\0")[:2]]
        except OSError:
            args = []
        found += args[:1]
        # "wine cod3.exe": the program is the second argument.
        if len(args) > 1 and _base(args[0]).startswith(_LOADERS):
            found.append(args[1])
        yield found


def _windows_tasks() -> List[str]:
    """Image names of the running programs (tasklist; Wine has one too)."""
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=15,
                             creationflags=flags).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    names = []
    for row in csv.reader(io.StringIO(out)):
        if row:
            names.append(row[0])
    return names


def game_running(out_dir: Optional[Path] = None, proc: Optional[Path] = None) -> bool:
    """True when the game seems to be running (best effort): a process with
    one of the game's executable names (exe_names). Linux reads /proc; Windows
    asks tasklist, and under Wine also looks at the Linux side's /proc."""
    names = exe_names(out_dir)
    roots = [proc] if proc else []
    if not proc:
        if common.IS_WINDOWS:
            if _matches(_windows_tasks(), names):
                return True
            if common.UNDER_WINE:
                roots.append(Path("Z:/proc"))
        else:
            roots.append(Path("/proc"))
    for root in roots:
        if root.is_dir() and any(_matches(found, names) for found in processes_in(root)):
            return True
    return False
