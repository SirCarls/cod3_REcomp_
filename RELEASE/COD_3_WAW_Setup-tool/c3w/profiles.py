"""Named copies of the tool's settings ("profiles").

A profile is a copy of the dict the GUI saves to config/settings.json, kept
as config/profiles/<name>.json. Only the settings listed in SETTING_TYPES are
kept, each with its type checked, so a profile can't carry anything else
into the tool. Names are checked so they make a plain file name on Windows
and Linux (no separators, no device names like CON or NUL).

Exported profiles are meant to be shared or moved to another PC, so they
leave out anything personal: paths inside the project folder are written
relative to it, paths elsewhere (a home folder, another drive) are left out.
Importing turns relative paths back into paths inside this project folder.

Every function raises common.SetupError with a plain message on bad input.
"""
from __future__ import annotations

import json
import re
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any, Dict, List, Optional

from . import common, upscale, waw

PROFILES_DIR = "profiles"
FORMAT = "cod3-setup-profile"
FORMAT_VERSION = 1
MAX_NAME = 64              # characters in a profile name
MAX_FILE = 256 * 1024      # bytes in a profile file
MAX_TEXT = 4096            # characters in one text setting
MAX_LIST = 64              # entries in a list setting

# The settings a profile keeps and their types: what gui.App._save writes.
# A new setting in the GUI needs a line here to be kept in profiles.
SETTING_TYPES: Dict[str, type] = {
    "cod3_source": str, "waw_steam": str, "waw_xbox360": str, "waw_pc_disc": str,
    "waw_alternative": str, "waw_map_packs": list, "use_waw": bool,
    "build_linux": bool, "build_windows": bool, "output_dir": str, "work_dir": str,
    "jobs": int, "build_recomp": bool, "appimage": bool, "install_deps": bool, "fidelityfx": bool,
    "cleanup": bool, "keep_sdk": bool, "upscale_textures": bool, "upscale_model": str,
    "upscale_noise": int, "upscale_scale": int, "cod3_windowed": bool, "cod3_args": str,
}
# Settings that hold paths (a list of paths for waw_map_packs).
PATH_KEYS = ("cod3_source", "waw_steam", "waw_xbox360", "waw_pc_disc", "output_dir", "work_dir",
             "waw_map_packs")
# Settings with a fixed set of values.
ALLOWED: Dict[str, tuple] = {
    "waw_alternative": (waw.XBOX360, waw.PC_DISC),
    "upscale_model": tuple(upscale.MODELS),
    "upscale_noise": tuple(upscale.NOISE_LEVELS),
    "upscale_scale": tuple(upscale.SCALES),
}
JOBS_RANGE = (1, 256)

# Names Windows keeps for devices, with or without an extension.
_DEVICES = {"con", "prn", "aux", "nul", "conin$", "conout$"} | \
    {f"{d}{n}" for d in ("com", "lpt") for n in list("123456789") + ["¹", "²", "³"]}
_BAD_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f\x7f]')
_CONTROL = re.compile(r"[\x00-\x1f\x7f]")


# ---------------------------------------------------------------------------
# Names and checks
# ---------------------------------------------------------------------------
def check_name(name: Any) -> str:
    """The name, trimmed, if it makes a safe file name; else SetupError."""
    if not isinstance(name, str):
        raise common.SetupError("A profile name must be text.")
    name = name.strip()
    if not name:
        raise common.SetupError("Type a name for the profile.")
    if len(name) > MAX_NAME:
        raise common.SetupError(f"A profile name can have at most {MAX_NAME} characters.")
    if _BAD_CHARS.search(name):
        raise common.SetupError('A profile name can\'t have these characters: < > : " / \\ | ? *')
    if name.startswith(".") or name.endswith("."):
        raise common.SetupError("A profile name can't start or end with a dot.")
    if name.split(".")[0].strip().lower() in _DEVICES:
        raise common.SetupError(f'"{name}" is a name Windows keeps for itself. Pick another name.')
    return name


def _folder(config_dir: Path) -> Path:
    return Path(config_dir) / PROFILES_DIR


def _file(config_dir: Path, name: str) -> Path:
    folder = _folder(config_dir)
    path = folder / f"{check_name(name)}.json"
    if path.parent != folder:   # can't happen after check_name; checked anyway
        raise common.SetupError("That profile name isn't allowed.")
    return path


def _find(config_dir: Path, name: str) -> Optional[Path]:
    """The file of the profile called name (ignoring case), or None."""
    name = check_name(name)
    folder = _folder(config_dir)
    exact = folder / f"{name}.json"
    if exact.is_file():
        return exact
    for other in list_profiles(config_dir):
        if other.lower() == name.lower():
            return folder / f"{other}.json"
    return None


def _note(notes: Optional[List[str]], text: str) -> None:
    if notes is not None:
        notes.append(text)


def clean_settings(settings: Any, notes: Optional[List[str]] = None) -> dict:
    """Only the known settings with the right types and values. What is left
    out is listed in notes (plain sentences)."""
    if not isinstance(settings, dict):
        raise common.SetupError("The profile doesn't hold settings.")
    out: dict = {}
    for key, value in settings.items():
        kind = SETTING_TYPES.get(key)
        if kind is None:
            _note(notes, f"Left out an unknown setting: {str(key)[:40]}")
            continue
        ok = isinstance(value, kind) and not (kind is int and isinstance(value, bool))
        if ok and kind is str:
            ok = len(value) <= MAX_TEXT and not _CONTROL.search(value)
        if ok and kind is list:
            ok = len(value) <= MAX_LIST and all(isinstance(v, str) and len(v) <= MAX_TEXT and
                                                not _CONTROL.search(v) for v in value)
        if ok and key in ALLOWED:
            ok = value in ALLOWED[key]
        if ok and key == "jobs":
            ok = JOBS_RANGE[0] <= value <= JOBS_RANGE[1]
        if not ok:
            _note(notes, f"Left out {key}: its value isn't valid.")
            continue
        out[key] = list(value) if kind is list else value
    return out


# ---------------------------------------------------------------------------
# Profiles
# ---------------------------------------------------------------------------
def list_profiles(config_dir: Path) -> List[str]:
    """Names of the saved profiles, sorted (case-insensitive)."""
    folder = _folder(config_dir)
    try:
        files = [p for p in folder.iterdir() if p.suffix.lower() == ".json" and p.is_file()]
    except OSError:
        return []
    names = []
    for p in files:
        try:
            names.append(check_name(p.stem))
        except common.SetupError:
            continue   # not one of ours
    return sorted(names, key=lambda n: (n.lower(), n))


def save_profile(config_dir: Path, name: str, settings: dict, *, notes: Optional[List[str]] = None) -> None:
    """Save settings under name (replaces a profile with that name)."""
    name = check_name(name)
    clean = clean_settings(settings, notes)
    old = _find(config_dir, name)
    path = _file(config_dir, name)
    try:
        common.write_json(path, {"format": FORMAT, "version": FORMAT_VERSION, "name": name, "settings": clean})
        if old and old.name != path.name and old.exists() and not old.samefile(path):
            old.unlink()   # the same name with other capitals
    except OSError as e:
        raise common.SetupError(f"Couldn't save the profile: {e.strerror or e}") from None


def _read(path: Path) -> dict:
    """A profile or settings file's JSON object (size and syntax checked)."""
    try:
        if path.stat().st_size > MAX_FILE:
            raise common.SetupError(f"{path.name} is too big for a profile.")
        text = path.read_text(encoding="utf-8-sig")
    except OSError as e:
        raise common.SetupError(f"Couldn't read {path.name}: {e.strerror or e}") from None
    except UnicodeDecodeError:
        raise common.SetupError(f"{path.name} isn't a profile (not text).") from None
    try:
        data = json.loads(text)
    except (ValueError, RecursionError):
        raise common.SetupError(f"{path.name} isn't a profile (not valid JSON).") from None
    if not isinstance(data, dict):
        raise common.SetupError(f"{path.name} isn't a profile.")
    return data


def _settings_of(data: dict) -> Any:
    """The settings in a profile file, or the whole object for a plain
    settings.json."""
    if data.get("format") == FORMAT:
        return data.get("settings")
    return data


def load_profile(config_dir: Path, name: str, *, notes: Optional[List[str]] = None) -> dict:
    """The settings saved under name (only known keys, checked)."""
    path = _find(config_dir, name)
    if not path:
        raise common.SetupError(f'There is no profile called "{name}".')
    return clean_settings(_settings_of(_read(path)), notes)


def delete_profile(config_dir: Path, name: str) -> None:
    """Delete the profile called name."""
    path = _find(config_dir, name)
    if not path:
        raise common.SetupError(f'There is no profile called "{name}".')
    try:
        path.unlink()
    except OSError as e:
        raise common.SetupError(f"Couldn't delete the profile: {e.strerror or e}") from None


def rename_profile(config_dir: Path, old: str, new: str) -> None:
    """Rename a profile (refuses if new already exists)."""
    new = check_name(new)
    src = _find(config_dir, old)
    if not src:
        raise common.SetupError(f'There is no profile called "{old}".')
    other = _find(config_dir, new)
    if other and other.name.lower() != src.name.lower():
        raise common.SetupError(f'There is already a profile called "{new}".')
    data = _read(src)
    settings = clean_settings(_settings_of(data))
    try:
        dst = _file(config_dir, new)
        common.write_json(dst, {"format": FORMAT, "version": FORMAT_VERSION, "name": new, "settings": settings})
        if src.exists() and src.name != dst.name and not src.samefile(dst):
            src.unlink()
        elif src.name != dst.name:
            # Only the capitals changed on a system that ignores case: the
            # file keeps its old name, so rename it to the new one.
            tmp = dst.with_name(dst.name + ".rename")
            src.replace(tmp)
            tmp.replace(dst)
    except OSError as e:
        raise common.SetupError(f"Couldn't rename the profile: {e.strerror or e}") from None


# ---------------------------------------------------------------------------
# Export and import
# ---------------------------------------------------------------------------
def _relative_to_project(value: str, project: Path) -> Optional[str]:
    """A path inside the project folder as a relative path with forward
    slashes; None for a path elsewhere."""
    if not value.strip():
        return ""
    p = Path(value)
    if not p.is_absolute():
        return None
    try:
        rel = common.resolved(p).relative_to(common.resolved(project))
    except ValueError:
        return None
    return rel.as_posix() or "."


def _from_project(value: str, project: Path) -> Optional[str]:
    """A relative path from an exported profile as a path inside the project
    folder; an absolute one as it is. None for a relative path that would
    lead out of the project folder."""
    if not value.strip():
        return ""
    if common.foreign_path(value):
        return None  # an absolute path from another kind of system means something else here
    if Path(value).is_absolute() or PureWindowsPath(value).is_absolute() or PurePosixPath(value).is_absolute():
        return value
    parts = [x for x in re.split(r"[\\/]+", value) if x not in ("", ".")]
    if any(x == ".." for x in parts) or re.match(r"^[A-Za-z]:", value):
        return None
    return str(project.joinpath(*parts)) if parts else str(project)


def _map_paths(settings: dict, convert, notes: Optional[List[str]], why: str) -> dict:
    out = dict(settings)
    for key in PATH_KEYS:
        if key not in out:
            continue
        if key == "waw_map_packs":
            kept = [c for c in (convert(v) for v in out[key]) if c is not None]
            if len(kept) != len(out[key]):
                _note(notes, f"Left out {len(out[key]) - len(kept)} map pack path(s): {why}")
            out[key] = kept
            continue
        converted = convert(out[key])
        if converted is None:
            del out[key]
            _note(notes, f"Left out {key}: {why}")
        else:
            out[key] = converted
    return out


def export_profile(config_dir: Path, name: str, dest: Path, *, notes: Optional[List[str]] = None) -> List[str]:
    """Write the profile to dest (a .json file the user picked), without
    paths outside the project folder. Returns notes on what was left out."""
    notes = [] if notes is None else notes
    settings = load_profile(config_dir, name, notes=notes)
    project = common.project_dir()
    settings = _map_paths(settings, lambda v: _relative_to_project(v, project), notes,
                          "it is outside the project folder (paths there can name you or your PC).")
    try:
        common.write_json(Path(dest), {"format": FORMAT, "version": FORMAT_VERSION, "name": check_name(name),
                                       "settings": settings})
    except OSError as e:
        raise common.SetupError(f"Couldn't write {Path(dest).name}: {e.strerror or e}") from None
    return notes


def import_profile(config_dir: Path, src: Path, name: Optional[str] = None, *,
                   notes: Optional[List[str]] = None) -> str:
    """Import a profile file (an exported profile or a plain settings.json);
    returns the name it was saved under. With name given, a profile of that
    name is replaced; without it, the file's own name is used and made
    unique (" (2)", " (3)" ...)."""
    src = Path(src)
    data = _read(src)
    settings = clean_settings(_settings_of(data), notes)
    project = common.project_dir()
    settings = _map_paths(settings, lambda v: _from_project(v, project), notes,
                          "a relative path must stay inside the project folder, and a path from "
                          "another kind of system (Linux or Windows) means nothing here.")
    if not settings:
        raise common.SetupError(f"{src.name} has no settings this tool knows.")
    if name is None:
        base = None
        for candidate in (data.get("name") if data.get("format") == FORMAT else None, src.stem):
            try:
                base = check_name(candidate)
                break
            except common.SetupError:
                continue
        base = base or "Imported"
        name, n = base, 2
        while _find(config_dir, name):
            suffix = f" ({n})"
            name = base[:MAX_NAME - len(suffix)].rstrip(". ") + suffix
            n += 1
    save_profile(config_dir, name, settings)
    return check_name(name)
