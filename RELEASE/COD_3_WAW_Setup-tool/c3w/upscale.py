"""Optional: upscales the ripped Call of Duty 3 textures with waifu2x.

waifu2x (https://github.com/nagadomi/waifu2x) is a neural network upscaler
that also removes compression noise. This uses waifu2x-ncnn-vulkan by nihui
(MIT licence), a pinned, checksummed download (deps.PORTABLE) that runs on
any Vulkan graphics card (NVIDIA, AMD, Intel) on Windows and Linux.

The scale is the user's choice (waifu2x's -s): 1x (noise reduction only),
2x (the default) or 4x. Nothing after this step assumes a size: the ripper
packs any scale (owner, 2026-10-10: "you cannot rely on how big each texture
should be, as with waifu the model may change").

The upscaled copies go to data/cod3/textures_upscaled/, next to the originals
in data/cod3/textures/, under the same names. The originals are never
changed. A textures_2x folder from an earlier build (when 2x was the only
scale) is renamed to textures_upscaled, never deleted. Textures that already
have an up-to-date copy are skipped, so a second build is quick and a Cancel
loses nothing done so far. The scale of the copies is kept in
textures_upscaled/.scale; after a change of scale every texture is done
again (copies already at the new scale, from an interrupted run, are kept).

Only the pictures the HD pack can use are upscaled: the files the ripper's
textures/hd_keys.tsv lists with a kind that is upscaled. The ripper works
out each texture's kind from the materials that use it (its `kind` column:
diffuse, normal, specular, detail, other, mixed, unknown); only `diffuse`
(color pictures) is upscaled by default (DEFAULT_KINDS). Normal, specular
and detail maps hold directions and shininess: upscaled and denoised as if
they were pictures they change the lighting (flat or wrong shading), and a
texture no material names (`unknown`: HUD, fonts, effects, menu pictures,
trees) may be anything, so those stay as the game has them. Lightmaps, fog
ramps and other lookup tables are left out by the ripper itself (`-` as the
file). The list includes the other pictures saved under a name
(`<name>__<key>.png`: every level has its own lightmaps and fog ramp, and
long names are cut short in the game's archives, so a color map and its
normal map can share one). Without that list, or with one from an older
ripper (no kind column), nothing is upscaled: the build rips again first.

If normal maps are ever asked for (`kinds`), they are only enlarged, never
denoised: the denoiser would treat their fine detail as noise.

Upscaling is optional: if it fails (no Vulkan driver, no graphics card), the
build carries on and says why.

For the playable game, the next step packs the copies into data/cod3/
hd_textures/ (`rip cod3-hd-pack`, run by the pipeline): each one compressed
in the same GPU format as the game's own texture (DXT1/3/5, ATI2 or 32-bit),
at its upscaled size with all its mip levels, plus an index.tsv that tells
the game's texture replacement (SDK patch 0016) which game texture it
replaces. The owner chose compressed storage (2026-10-10): at 2x about 4x the
original texture memory (the square of the scale), instead of 16x for
uncompressed copies. The game uses the pack when F2 > Video > "HD textures
(waifu2x)" is on (the default).
"""
from __future__ import annotations

import os
import re
import shutil
import struct
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from . import user_textures
from .common import IS_WINDOWS, Cancelled, Log, Runner, SetupError, safe_rmtree

OUTPUT_NAME = "textures_upscaled"   # next to the ripper's "textures" folder
# The folder's name from before the scale could be chosen (always 2x). An
# existing one is renamed to OUTPUT_NAME, never deleted.
LEGACY_OUTPUT_NAME = "textures_2x"
LEGACY_SCALE = 2
# In OUTPUT_NAME: the scale its copies were made at, written when a run
# leaves nothing missing.
SCALE_FILE = ".scale"

# waifu2x's -s: how many times larger the copies are. waifu2x has networks
# that only remove noise (1x) for its "cunet" model alone, so 1x uses it.
SCALES: Dict[int, str] = {1: "1x (noise reduction only)", 2: "2x", 4: "4x (slow)"}
DEFAULT_SCALE = 2
ONE_X_MODEL = "cunet"

# The HD texture pack for the playable game, also next to "textures". The
# game looks for PACK_NAME/PACK_INDEX (recomp/src/game/hd_textures.cpp).
PACK_NAME = "hd_textures"
PACK_INDEX = "index.tsv"
KEYS_FILE = "hd_keys.tsv"       # written by the ripper into "textures": which texture is which name

# Model folder (inside the download) and the label shown in the setup tool.
MODELS: Dict[str, Tuple[str, str]] = {
    "photo": ("models-upconv_7_photo", "Photo (fast, for realistic textures)"),
    "art": ("models-upconv_7_anime_style_art_rgb", "Artwork (fast, for drawn images)"),
    "cunet": ("models-cunet", "Best quality (several times slower)"),
}
DEFAULT_MODEL = "photo"

# waifu2x's -n: how hard it removes compression noise.
NOISE_LEVELS: Dict[int, str] = {-1: "None", 0: "Very light", 1: "Light", 2: "Medium", 3: "Strong"}
DEFAULT_NOISE = 1

# Texture names that hold directions or other data rather than colors:
# "_n", "_nm", "_nml", "_nrm", "_norm", "_normal", "_bump", "_ddn" as a word
# of the name (the word may be followed by digits).
_DATA_MAP = re.compile(r"(?:^|[_\-. $])(?:n|nm|nml|nrm|norm|normal|normalmap|bump|ddn)(?:\d*)(?=$|[_\-. $])",
                       re.IGNORECASE)


# Lightmaps and lookup tables, by name (the ripper's hd::keep_original
# decides; this is for repairing packs from before it did).
_KEEP_ORIGINAL = re.compile(r"^lm\d+$|colorlm|lightmap|^fogramp", re.IGNORECASE)
# The ripper's list of keys and the pictures they use (see hd.rs): columns
# key, width, height, format, name, file, kind.
KEYS_FILE = "hd_keys.tsv"
NO_FILE = "-"
KEYS_COLUMNS = 7
# The kinds of texture the HD pack upscales and packs unless told otherwise:
# color pictures only (the ripper's `Kind::upscaled_by_default`; `rip
# cod3-hd-pack --kinds` takes the same names).
KINDS = ("diffuse", "normal", "specular", "detail", "other", "mixed", "unknown")
DEFAULT_KINDS = frozenset({"diffuse"})


def wanted_pictures(textures: Path, kinds: frozenset = DEFAULT_KINDS) -> Optional[set]:
    """The PNG names (without .png) the HD pack uses, from the ripper's
    hd_keys.tsv: the `file` of every key whose `kind` is in `kinds`. None
    when the file is missing or from an older ripper (fewer columns): the
    game must be ripped again, so nothing is upscaled."""
    try:
        text = (textures / KEYS_FILE).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    wanted = set()
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) < KEYS_COLUMNS:
            return None
        if cols[5] != NO_FILE and cols[6] in kinds:
            wanted.add(cols[5])
    return wanted


def exe_name() -> str:
    return "waifu2x-ncnn-vulkan.exe" if IS_WINDOWS else "waifu2x-ncnn-vulkan"


def portable_key() -> str:
    return "waifu2x-win" if IS_WINDOWS else "waifu2x-linux"


def is_data_map(png: Path) -> bool:
    """A normal map or similar, by its name: enlarged without denoising."""
    return bool(_DATA_MAP.search(png.stem))


def repair_old_pack(pack: Path, log: Log) -> bool:
    """Packs made before 2026-10-11 gave every texture of a name the picture
    of one of them (another level's lightmaps on the ground, another level's
    fog). Such an index (a file listed for several keys) is rewritten to
    keep only the textures whose file is certainly their own, without
    lightmaps and fog ramps, and the old one is kept as index-before-fix.tsv.
    The next pack replaces it with a complete one. Returns True if it was
    rewritten."""
    index = pack / PACK_INDEX
    try:
        text = index.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    rows = [line.split("\t") for line in text.splitlines() if line and not line.startswith("#")]
    rows = [r for r in rows if len(r) >= 5]
    per_file: Dict[str, int] = {}
    for r in rows:
        per_file[r[4]] = per_file.get(r[4], 0) + 1
    if not any(n > 1 for n in per_file.values()):
        return False
    keep = [r for r in rows if per_file[r[4]] == 1 and not _KEEP_ORIGINAL.search(Path(r[4]).stem)]
    backup = pack / "index-before-fix.tsv"
    try:
        if not backup.exists():
            shutil.copy2(index, backup)
        lines = ["# key\twidth\theight\txenos_format\tfile",
                 "# Repaired: only textures whose file is their own; lightmaps and fog ramps left as they are."]
        lines += ["\t".join(r[:5]) for r in keep]
        index.write_text("\n".join(lines) + "\n", encoding="utf-8")
    except OSError as e:
        log.warn(f"The HD texture pack in {pack} gives some textures another texture's picture and couldn't be "
                 f"repaired ({e}); turn off F2 > Video > HD textures until it is rebuilt.")
        return False
    log.info(f"Repaired the HD texture pack's index in {pack}: an earlier build gave {len(rows) - len(keep)} "
             f"textures another texture's picture (dark ground, odd colors). {len(keep)} textures kept until the "
             f"pack is rebuilt; the old index is saved as {backup.name}.")
    return True


def pack_ready(pack: Path) -> bool:
    """True when `pack` holds a usable HD texture pack (its index exists)."""
    return (pack / PACK_INDEX).is_file()


def pack_size(pack: Path) -> int:
    """How many texture files the pack's index lists. The index has a line per
    game texture key ("key, width, height, format, file"), and one file can
    stand for several keys, so the files are counted."""
    files = set()
    try:
        with open(pack / PACK_INDEX, encoding="utf-8", errors="replace") as f:
            for line in f:
                cols = line.rstrip("\r\n").split("\t")
                if len(cols) >= 5 and not line.startswith("#"):
                    files.add(cols[4])
    except OSError:
        return 0
    return len(files)


def png_size(path: Path) -> Optional[Tuple[int, int]]:
    """A PNG's width and height from its header, or None if it isn't one."""
    try:
        with open(path, "rb") as f:
            head = f.read(24)
    except OSError:
        return None
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", head[16:24])


def recorded_scale(folder: Path) -> int:
    """The scale the copies in `folder` were made at (SCALE_FILE); a folder
    without one is from before the choice, when 2x was the only scale."""
    try:
        return int((folder / SCALE_FILE).read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return LEGACY_SCALE


def record_scale(folder: Path, scale: int) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    (folder / SCALE_FILE).write_text(f"{scale}\n", encoding="utf-8")


def output_dir(data_dir: Path, log: Log) -> Path:
    """`data_dir`/textures_upscaled, after renaming a textures_2x folder from
    an earlier build to it (its copies are 2x, which is recorded). The old
    folder is never deleted: when both exist, both stay as they are."""
    out = data_dir / OUTPUT_NAME
    legacy = data_dir / LEGACY_OUTPUT_NAME
    if not legacy.is_dir():
        return out
    if out.exists():
        log.info(f"{legacy} from an earlier build is left as it is; the upscaled textures are in {out}. "
                 "The old folder can be deleted.")
        return out
    try:
        legacy.rename(out)
    except OSError as e:
        log.warn(f"Couldn't rename {legacy} to {out} ({e}); using it under its old name.")
        return legacy
    if not (out / SCALE_FILE).exists():
        record_scale(out, LEGACY_SCALE)
    log.info(f"Renamed {legacy} to {out} (the folder's name no longer gives the scale).")
    return out


def sources(src: Path, kinds: frozenset = DEFAULT_KINDS) -> List[Path]:
    """The PNGs in `src` to upscale: those hd_keys.tsv lists with a kind in
    `kinds` (none without that list, or with one from an older ripper).
    Pictures the player replaces with their own (user_textures_png next to
    `src`, c3w/user_textures.py) are left alone: the user pack wins in the
    game."""
    replaced = user_textures.replaced_names(src.parent)
    wanted = wanted_pictures(src, kinds)
    if wanted is None:
        return []
    return [p for p in sorted(src.glob("*.png")) if p.stem in wanted and p.stem not in replaced]


def pending(src: Path, dst: Path, scale: Optional[int] = None, kinds: frozenset = DEFAULT_KINDS
            ) -> List[Path]:
    """The PNGs `sources` picks in `src` without an up-to-date copy in `dst`:
    a copy is up to date when it's newer than the original. With `scale`
    (after a change of scale), it must also be the original's size times
    `scale`, which is what waifu2x makes, so copies at the old scale are
    done again."""
    todo = []
    for png in sources(src, kinds):
        out = dst / png.name
        try:
            if out.stat().st_mtime >= png.stat().st_mtime and out.stat().st_size > 0:
                if scale is None:
                    continue
                size, out_size = png_size(png), png_size(out)
                if size and out_size == (size[0] * scale, size[1] * scale):
                    continue
        except OSError:
            pass
        todo.append(png)
    return todo


@dataclass
class Settings:
    model: str = DEFAULT_MODEL
    noise: int = DEFAULT_NOISE
    scale: int = DEFAULT_SCALE
    # The texture kinds to upscale (hd_keys.tsv's kind column); the pack
    # step must be given the same (`rip cod3-hd-pack --kinds`).
    kinds: frozenset = DEFAULT_KINDS

    def validated(self) -> "Settings":
        model = self.model if self.model in MODELS else DEFAULT_MODEL
        noise = self.noise if self.noise in NOISE_LEVELS else DEFAULT_NOISE
        scale = self.scale if self.scale in SCALES else DEFAULT_SCALE
        kinds = frozenset(k for k in self.kinds if k in KINDS) or DEFAULT_KINDS
        return Settings(model, noise, scale, kinds)


def command(exe: Path, src: Path, dst: Path, model_dir: Path, noise: int, scale: int = DEFAULT_SCALE
            ) -> List[str]:
    """waifu2x-ncnn-vulkan for every image in folder `src`, written as PNG
    with the same names into `dst`, `scale` times larger. The graphics card
    and tile size are its automatic choice."""
    return [str(exe), "-i", str(src), "-o", str(dst), "-n", str(noise), "-s", str(scale),
            "-m", str(model_dir), "-f", "png", "-v"]


def _copy_as_is(files: List[Path], dst: Path, log: Log, total: int, done_before: int) -> None:
    """1x without noise reduction: waifu2x would change nothing, so the
    originals are copied (with their times, so they count as up to date)."""
    for i, f in enumerate(files, 1):
        shutil.copy2(f, dst / f.name)
        log.progress((done_before + i) / max(total, 1), f"upscaled {done_before + i}/{total} textures")


def _stage(files: List[Path], folder: Path) -> None:
    """Puts `files` in `folder` for one waifu2x run: hard links where the
    file system allows them (instant, no extra space), copies otherwise."""
    folder.mkdir(parents=True, exist_ok=True)
    for f in files:
        target = folder / f.name
        try:
            os.link(f, target)
        except OSError:
            shutil.copy2(f, target)


def _progress(log: Log, total: int, done_before: int) -> Callable[[str], None]:
    """Counts waifu2x's "in -> out done" lines (it prints one per image with -v)."""
    state = {"done": done_before}

    def handle(line: str) -> None:
        if line.endswith(" done"):
            state["done"] += 1
            log.progress(state["done"] / max(total, 1), f"upscaled {state['done']}/{total} textures")
        elif line and "->" not in line and not line.startswith("["):
            log.output(line)   # its errors (a file it couldn't read, no GPU ...)
    return handle


def upscale_folder(tool_dir: Path, src: Path, dst: Path, staging: Path, settings: Settings, runner: Runner,
                   log: Log, cancel: threading.Event) -> Tuple[int, int]:
    """Upscales the PNGs in `src` that `dst` lacks. `tool_dir` is the unpacked
    waifu2x-ncnn-vulkan; `staging` a scratch folder (emptied afterwards).
    Returns (upscaled now, still missing)."""
    s = settings.validated()
    exe = tool_dir / exe_name()
    # 1x only removes noise, which only the cunet model has networks for.
    model = ONE_X_MODEL if s.scale == 1 else s.model
    model_dir = tool_dir / MODELS[model][0]
    if not exe.is_file() or not model_dir.is_dir():
        raise SetupError(f"waifu2x is incomplete in {tool_dir}")
    # After a change of scale every copy is made again, except those already
    # at the new scale (a run at that scale was interrupted).
    old_scale = recorded_scale(dst)
    rescale = old_scale != s.scale and any(dst.glob("*.png"))
    check_scale = s.scale if rescale else None
    todo = pending(src, dst, check_scale, s.kinds)
    total = len(sources(src, s.kinds))
    if wanted_pictures(src, s.kinds) is None:
        raise SetupError(f"{src / KEYS_FILE} is missing or from an older ripper: rip the game again "
                         "(a build does that) before upscaling.")
    if not todo:
        log.info(f"All {total} textures already have a {s.scale}x copy in {dst}.")
        if total:
            record_scale(dst, s.scale)
        return 0, 0
    dst.mkdir(parents=True, exist_ok=True)
    if rescale:
        log.info(f"The copies in {dst} were made at {old_scale}x, so they are made again at {s.scale}x.")
    if model != s.model:
        log.info(f"1x only reduces noise, which waifu2x does with its {MODELS[model][1].split(' (')[0]} model "
                 "alone; that one is used.")
    log.info(f"Upscaling {len(todo)} of {total} textures {s.scale}x with waifu2x ({MODELS[model][1]}, noise "
             f"reduction {NOISE_LEVELS[s.noise].lower()}) into {dst}...")
    if s.kinds == DEFAULT_KINDS:
        log.info("Only the textures the game's materials use as color pictures are upscaled; normal, specular "
                 "and detail maps stay as the game has them (upscaled, they change the lighting), and so do "
                 "textures no material names (HUD, fonts, effects, menus).")
    colors = [p for p in todo if not is_data_map(p)]
    data_maps = [p for p in todo if is_data_map(p)]
    if data_maps:
        log.info(f"{len(data_maps)} of them look like normal maps; those are enlarged without noise reduction.")
    batches = [(colors, s.noise, "colors"), (data_maps, -1, "maps")]
    done_before = total - len(todo)
    try:
        for files, noise, name in batches:
            if not files:
                continue
            if cancel.is_set():
                raise Cancelled()
            if s.scale == 1 and noise == -1:
                _copy_as_is(files, dst, log, total, done_before)
                done_before += len(files)
                continue
            # The whole folder at once when every PNG in it is wanted;
            # otherwise only the wanted files, through the staging folder.
            if len(files) == len(list(src.glob("*.png"))):
                source = src
            else:
                source = staging / name
                _stage(files, source)
            runner.run(command(exe, source, dst, model_dir, noise, s.scale), quiet=True,
                       line_filter=_progress(log, total, done_before))
            done_before += len(files)
    finally:
        if staging.exists():
            safe_rmtree(staging, staging.parent, [src, dst], log)
    missing = pending(src, dst, check_scale, s.kinds)
    if not missing:
        record_scale(dst, s.scale)
    return len(todo) - len(missing), len(missing)
