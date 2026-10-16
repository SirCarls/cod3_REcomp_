"""Tests for the user texture pack (c3w/user_textures.py): the insignia list
shipped in data/, the folders, the ripper command, the build step, and that
the HD upscaler leaves alone the pictures the player replaces. The ripper
isn't run: a stand-in runner records the command and writes what it would.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import re
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import pipeline, upscale, user_textures  # noqa: E402
from c3w.common import Log, SetupError  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402

TOOL_ROOT = Path(__file__).resolve().parent.parent
KEYS_HEADER = "# key\twidth\theight\txenos_format\tname\tfile\tkind\n"


class FakeRip:
    """Stands in for Runner.run(rip cod3-user-pack ...): writes an index
    with one line per PNG in the picture folder, prints its progress."""

    def __init__(self, rc: int = 0):
        self.calls = []
        self.rc = rc

    def run(self, cmd, quiet=False, check=True, line_filter=None, **kw):
        cmd = [str(c) for c in cmd]
        self.calls.append(cmd)
        pictures, pack = Path(cmd[3]), Path(cmd[4])
        pack.mkdir(parents=True, exist_ok=True)
        pngs = sorted(p for p in pictures.glob("*.png"))
        lines = [f"{i:016x}\t16\t16\t18\t{p.stem}.dds" for i, p in enumerate(pngs, 1)]
        index = pack / "index.tsv"
        if lines:
            index.write_text("# key\twidth\theight\txenos_format\tfile\n" + "\n".join(lines) + "\n",
                             encoding="utf-8")
        elif index.exists():
            index.unlink()
        if line_filter:
            for i in range(len(pngs)):
                line_filter(f"{i + 1}/{len(pngs)} textures")
            line_filter(f"done: {len(pngs)} packed, 0 skipped")
        return self.rc


def output_with_rip(root: Path) -> Path:
    """An output folder with ripped textures (hd_keys.tsv) as a build leaves it."""
    data = root / "out" / "data" / "cod3"
    textures = data / "textures"
    textures.mkdir(parents=True)
    (textures / "hd_keys.tsv").write_text(
        KEYS_HEADER + "e26a0a1331259fd8\t512\t256\t18\tcloth_masked$of_flag_german\tcloth_masked$of_flag_german"
        "\tdiffuse\n0000000000000001\t64\t64\t18\tbrick\tbrick\tdiffuse\n", encoding="utf-8")
    for name in ("cloth_masked$of_flag_german", "brick"):
        (textures / f"{name}.png").write_bytes(b"png")
    return data


class InsigniaListTests(unittest.TestCase):
    def setUp(self):
        self.rows = user_textures.read_list(TOOL_ROOT)

    def test_the_list_is_shipped_and_complete(self):
        self.assertEqual(len(self.rows), 76)
        keys = [r.key for r in self.rows]
        self.assertEqual(len(set(keys)), len(keys))
        files = {r.file for r in self.rows}
        for name in ("cloth_masked$of_flag_german", "decal$of_germanflag_dirty_c", "of_c_pnzgren_helm1_c",
                     "metal$of_v_fw190", "panzezriv_c", "cloth$of_germanflag_dirty_c", "of_knife148_c"):
            self.assertIn(name, files)
        kinds = {r.kind for r in self.rows}
        self.assertTrue(kinds <= {"party", "military", "related"}, kinds)

    def test_listed_sizes_follow_the_rules(self):
        for r in self.rows:
            self.assertIn(r.format, ("DXT1", "DXT3", "DXT5"))
            sizes = r.sizes.split(", ")
            self.assertEqual(sizes[0], f"{r.width}x{r.height}", r.file)
            for size in sizes:
                w, h = map(int, size.split("x"))
                # The same aspect ratio, block multiples, at most 8192.
                self.assertEqual(w * r.height, h * r.width, (r.file, size))
                self.assertEqual((w % 4, h % 4), (0, 0), (r.file, size))
                self.assertLessEqual(max(w, h), 8192, (r.file, size))

    def test_worn_textures_are_never_used_in_multiplayer(self):
        # Soldiers' and vehicles' textures. World insignia may be "no" too
        # until the game's own list has them (checked below).
        worn = ("of_c_", "c_", "panzezriv", "of_king_tiger", "king_tiger", "of_tigeri", "pantherv", "hanomag",
                "horch1a", "kubelwagon", "sdk_", "v_sdk", "v_opel_blitz")
        for r in self.rows:
            if r.file.startswith(worn):
                self.assertFalse(r.multiplayer, r.file)

    def test_the_games_multiplayer_list_matches(self):
        """The game keeps its own copy of the multiplayer keys (its trust
        anchor: a data file could be edited). Checked when the game's source
        in recomp/ has it."""
        source = TOOL_ROOT / "recomp" / "src" / "game" / "hd_textures.cpp"
        text = source.read_text(encoding="utf-8") if source.is_file() else ""
        block = re.search(r"kMultiplayerInsigniaKeys\[\] = \{(.*?)\};", text, re.S)
        if not block:
            self.skipTest("the game's source here predates the multiplayer list")
        game = set(re.findall(r"0x([0-9a-f]{16})", block.group(1)))
        self.assertEqual(game, {r.key for r in self.rows if r.multiplayer})

    def test_malformed_rows_are_skipped(self):
        rows = user_textures.parse_list("# x\nname\tnot-a-key\t1\t1\tDXT1\t1x1\topaque\tyes\tparty\ta\tb\n"
                                        "short\tline\n")
        self.assertEqual(rows, [])


class FolderTests(unittest.TestCase):
    def test_prepare_folder_puts_the_list_next_to_the_pictures(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / "data" / "cod3"
            folder = user_textures.prepare_folder(data, TOOL_ROOT)
            self.assertEqual(folder, data / "user_textures_png")
            copy = folder / user_textures.LIST_NAME
            self.assertEqual(copy.read_bytes(), user_textures.list_path(TOOL_ROOT).read_bytes())
            # Only PNG files count as pictures (any case of the extension).
            (folder / "a.png").write_bytes(b"x")
            (folder / "B.PNG").write_bytes(b"x")
            (folder / "sub.png").mkdir()
            self.assertEqual([p.name for p in user_textures.user_pictures(data)], ["B.PNG", "a.png"])
            self.assertEqual(user_textures.replaced_names(data), {"a", "B"})

    def test_the_upscaler_leaves_replaced_pictures_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = output_with_rip(Path(tmp))
            textures = data / "textures"
            names = lambda: [p.stem for p in upscale.sources(textures)]   # noqa: E731
            self.assertEqual(names(), ["brick", "cloth_masked$of_flag_german"])
            folder = user_textures.prepare_folder(data, TOOL_ROOT)
            (folder / "cloth_masked$of_flag_german.png").write_bytes(b"mine")
            self.assertEqual(names(), ["brick"])
            self.assertEqual([p.stem for p in upscale.pending(textures, data / "up")], ["brick"])


class BuildTests(unittest.TestCase):
    def test_runs_the_user_pack_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = output_with_rip(Path(tmp))
            folder = user_textures.prepare_folder(data, TOOL_ROOT)
            (folder / "cloth_masked$of_flag_german.png").write_bytes(b"mine")
            events = []
            log = Log(sinks=[lambda kind, text, frac: events.append((kind, text, frac))])
            fake = FakeRip()
            keys = user_textures.build(Path(tmp) / "rip", data, TOOL_ROOT, 2, fake, log)
            self.assertEqual(keys, 1)
            self.assertEqual(fake.calls, [[str(Path(tmp) / "rip"), "cod3-user-pack", str(data / "textures"),
                                           str(folder), str(data / "user_textures"), "--jobs", "2", "--list",
                                           str(user_textures.list_path(TOOL_ROOT))]])
            self.assertIn(("progress", "packed 1/1 user textures", 1.0),
                          [(k, t, f) for k, t, f in events if k == "progress"])
            self.assertTrue(any("User texture pack ready" in t for k, t, _ in events if k == "info"))

    def test_nothing_to_do_without_pictures_or_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = output_with_rip(Path(tmp))
            fake = FakeRip()
            self.assertEqual(user_textures.build(Path(tmp) / "rip", data, TOOL_ROOT, 1, fake, Log(sinks=[])), 0)
            self.assertEqual(fake.calls, [])

    def test_an_old_pack_is_cleared_when_the_pictures_are_gone(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = output_with_rip(Path(tmp))
            pack = user_textures.pack_dir(data)
            pack.mkdir(parents=True)
            (pack / "index.tsv").write_text("# old\n0000000000000001\t64\t64\t18\tbrick.dds\n", encoding="utf-8")
            fake = FakeRip()
            self.assertEqual(user_textures.build(Path(tmp) / "rip", data, TOOL_ROOT, 1, fake, Log(sinks=[])), 0)
            self.assertEqual(len(fake.calls), 1)
            self.assertFalse((pack / "index.tsv").exists())

    def test_errors_are_explained(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / "data" / "cod3"
            with self.assertRaises(SetupError):   # no rip yet
                user_textures.build(Path(tmp) / "rip", data, TOOL_ROOT, 1, FakeRip(), Log(sinks=[]))
            data = output_with_rip(Path(tmp))
            (user_textures.prepare_folder(data, TOOL_ROOT) / "brick.png").write_bytes(b"x")
            with self.assertRaises(SetupError) as e:
                user_textures.build(Path(tmp) / "rip", data, TOOL_ROOT, 1, FakeRip(rc=1), Log(sinks=[]))
            self.assertIn("exit code 1", str(e.exception))


class PipelineStepTests(unittest.TestCase):
    def titles(self, **opts) -> list:
        with tempfile.TemporaryDirectory() as tmp:
            o = BuildOptions(output_dir=str(Path(tmp) / "out"), work_dir=str(Path(tmp) / "work"),
                             build_recomp=False, **opts)
            with mock.patch.object(pipeline, "IS_LINUX", True), mock.patch.object(pipeline, "IS_WINDOWS", False):
                return [s.title for s in Pipeline(o, Log(sinks=[]), threading.Event()).steps()]

    def test_the_step_follows_the_rip_with_or_without_upscaling(self):
        for opts in ({}, {"upscale_textures": True}):
            t = self.titles(**opts)
            self.assertIn("User texture pack", t)
            self.assertLess(t.index("Ripping Call of Duty 3"), t.index("User texture pack"))
        self.assertNotIn("User texture pack", self.titles(binaries_only=True))

    def test_the_step_is_optional_and_offline(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = output_with_rip(Path(tmp))
            out = data.parent.parent
            events = []
            log = Log(sinks=[lambda kind, text, frac: events.append((kind, text))])
            p = Pipeline(BuildOptions(output_dir=str(out), work_dir=str(Path(tmp) / "work"), jobs=2), log,
                         threading.Event())
            fake = FakeRip(rc=3)
            p.rip, p.runner = Path(tmp) / "rip", fake
            p.user_texture_pack()   # no pictures: nothing run, the folder is made
            self.assertEqual(fake.calls, [])
            self.assertTrue((data / "user_textures_png" / user_textures.LIST_NAME).is_file())
            (data / "user_textures_png" / "brick.png").write_bytes(b"x")
            with mock.patch.object(pipeline.APPROVAL, "require", side_effect=AssertionError("asked")):
                p.user_texture_pack()   # a failing ripper only warns
            self.assertEqual(len(fake.calls), 1)
            self.assertTrue(any(k == "warn" and "user texture pack wasn't made" in t for k, t in events))


if __name__ == "__main__":
    unittest.main()
