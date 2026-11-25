"""Tests for c3w/port_hd.py: HD textures for World at War, Call of Duty 2 and Call of Duty 4, made from the textures
the game saves while played [setup-layout]. No game, ripper or waifu2x needed: the ripper's and waifu2x's parts are
stood in for, and nothing is built.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
from __future__ import annotations

import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import port_hd, upscale, x360_ports  # noqa: E402
from c3w.common import IS_LINUX, Log, SetupError  # noqa: E402
from c3w.pipeline import BuildOptions  # noqa: E402


def _save(root: Path, lines: int, broken_last: bool = False) -> None:
    """A capture folder as the game leaves it: captured.tsv (and no need for the DDS files here)."""
    folder = port_hd.capture_dir(root)
    folder.mkdir(parents=True, exist_ok=True)
    text = "".join(f"{i:016x}\t64\t64\t18\t{i:016x}.dds\n" for i in range(1, lines + 1))
    if broken_last:
        text += "00000000000000"   # a session cut off mid-line
    (folder / port_hd.CAPTURE_LIST).write_text(text)


def _pack(root: Path, files: int) -> None:
    pack = port_hd.pack_dir(root)
    pack.mkdir(parents=True, exist_ok=True)
    (pack / upscale.PACK_INDEX).write_text(
        "".join(f"{i:016x}\t64\t64\t18\tp{i}.dds\n" for i in range(files)))


class FolderTests(unittest.TestCase):
    def test_counts_and_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "waw"
            self.assertEqual(port_hd.captured_count(root), 0)
            self.assertEqual(port_hd.status(root), "No textures saved yet.")
            _save(root, 3, broken_last=True)
            self.assertEqual(port_hd.captured_count(root), 4)   # the ripper skips the broken one later
            self.assertIn("Build again", port_hd.status(root))
            _pack(root, 2)
            self.assertEqual(port_hd.status(root), "4 textures saved. HD pack: 2 textures.")

    def test_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "cod4"
            # Nothing to draw with yet: only the capture can be asked for.
            self.assertEqual(port_hd.arguments(root, True, False, False), [])
            self.assertEqual(port_hd.arguments(root, True, True, False),
                             [f"--texture_capture_path={port_hd.capture_dir(root).resolve()}"])
            _pack(root, 1)
            pack = f"--texture_replacement_path={port_hd.pack_dir(root).resolve()}"
            self.assertEqual(port_hd.arguments(root, True, False, False), [pack])
            self.assertEqual(port_hd.arguments(root, False, False, False), [])
            # A match checks every image against the game's own alpha.
            self.assertEqual(port_hd.arguments(root, True, False, True),
                             [pack, "--texture_replacement_check_alpha=true"])


class BuildTests(unittest.TestCase):
    def runner(self, rip_rc: int = 0, write_keys: bool = True, pack_rc: int = 0):
        """A runner whose ripper "converts" and "packs" (writes the files the real one would)."""
        calls = []

        def run(cmd, **kw):
            cmd = [str(c) for c in cmd]
            calls.append(cmd)
            if cmd[1] in ("captured-textures", "waw-hd-textures") and write_keys:
                textures = Path(cmd[3]) / port_hd.TEXTURES_DIR
                textures.mkdir(parents=True, exist_ok=True)
                (textures / upscale.KEYS_FILE).write_text("# key\n")
                return rip_rc
            if cmd[1] == "cod3-hd-pack":
                if pack_rc == 0:
                    (Path(cmd[4]) / upscale.PACK_INDEX).write_text("0000000000000001\t64\t64\t18\ta.dds\n")
                return pack_rc
            return rip_rc
        r = mock.Mock()
        r.run.side_effect = run
        return r, calls

    def build(self, root: Path, runner, cancel=None, port_id: str = ""):
        with mock.patch.object(upscale, "upscale_folder", return_value=(5, 0)) as up:
            count = port_hd.build(Path("rip"), root, Path("waifu2x"), root / "staging", upscale.Settings(), 2,
                                  runner, Log(), cancel or threading.Event(), port_id=port_id)
        return count, up

    def test_world_at_war_reads_its_disc_files(self):
        """[setup-layout] 2026-11-24: World at War's pack comes from the fastfiles in assets/, with highmip/, and
        no saved textures are needed."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "waw"
            self.assertTrue(port_hd.has_source("waw", root))
            self.assertFalse(port_hd.has_source("cod4", root))
            runner, calls = self.runner()
            with self.assertRaises(SetupError):     # no fastfiles copied: nothing to read
                self.build(root, runner, port_id="waw")
            self.assertEqual(calls, [])
            (root / "assets" / "highmip").mkdir(parents=True)
            (root / "assets" / "ber1.ff").write_bytes(b"")
            count, up = self.build(root, runner, port_id="waw")
            self.assertEqual(count, 1)
            self.assertEqual(calls[0][1:], ["waw-hd-textures", str(root / "assets"), str(port_hd.hd_dir(root)),
                                            "--highmip", str(root / "assets" / "highmip")])
            self.assertEqual(calls[1][1], "cod3-hd-pack")
            up.assert_called_once()
            self.assertEqual(port_hd.status(root, "waw"), "HD pack: 1 textures.")
            self.assertNotIn("saved", port_hd.status(Path(tmp) / "none", "waw"))

    def test_nothing_saved_makes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            runner, calls = self.runner()
            count, up = self.build(Path(tmp) / "waw", runner)
            self.assertIsNone(count)
            self.assertEqual(calls, [])
            up.assert_not_called()

    def test_the_three_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "waw"
            _save(root, 2)
            runner, calls = self.runner(rip_rc=1)   # 1: some files skipped, the rest usable
            count, up = self.build(root, runner)
            self.assertEqual(count, 1)
            self.assertEqual(calls[0][1:], ["captured-textures", str(port_hd.capture_dir(root)),
                                            str(port_hd.hd_dir(root))])
            self.assertEqual(calls[1][1], "cod3-hd-pack")
            src, dst = up.call_args.args[1], up.call_args.args[2]
            self.assertEqual((src, dst), (port_hd.hd_dir(root) / "textures",
                                          port_hd.hd_dir(root) / upscale.OUTPUT_NAME))
            self.assertTrue(port_hd.pack_ready(root))

    def test_failures_are_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "waw"
            _save(root, 2)
            runner, _ = self.runner(rip_rc=2)
            with self.assertRaises(SetupError):
                self.build(root, runner)
            # A ripper that ends well but writes no key list (the one from the run above is gone first).
            (port_hd.hd_dir(root) / port_hd.TEXTURES_DIR / upscale.KEYS_FILE).unlink()
            runner, _ = self.runner(write_keys=False)
            with self.assertRaises(SetupError):
                self.build(root, runner)
            runner, _ = self.runner(pack_rc=3)
            with self.assertRaises(SetupError):
                self.build(root, runner)
            cancel = threading.Event()
            cancel.set()
            runner, calls = self.runner()
            with self.assertRaises(SetupError):
                self.build(root, runner, cancel)
            self.assertEqual(len(calls), 1, "nothing is upscaled or packed after Cancel")


class PortTests(unittest.TestCase):
    def pipe(self, tmp: Path, port=None, **kw) -> "x360_ports.PortPipeline":
        opts = BuildOptions(output_dir=str(tmp / "out"), work_dir=str(tmp / "work"), **kw)
        return x360_ports.PortPipeline(port or x360_ports.WAW, str(tmp / "disc"), opts, Log(), threading.Event())

    def test_the_build_makes_the_pack_only_with_saved_textures_and_upscaling_on(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cod4 = x360_ports.COD4
            self.assertNotIn("hd", [s.key for s in self.pipe(tmp, port=cod4, upscale_textures=True).steps()])
            _save((tmp / "out" / cod4.subdir).resolve(), 1)
            self.assertNotIn("hd", [s.key for s in self.pipe(tmp, port=cod4).steps()])
            self.assertIn("hd", [s.key for s in self.pipe(tmp, port=cod4, upscale_textures=True).steps()])
            # World at War reads its disc files: upscaling on is enough.
            self.assertNotIn("hd", [s.key for s in self.pipe(tmp).steps()])
            pipe = self.pipe(tmp, upscale_textures=True)
            keys = [s.key for s in pipe.steps()]
            self.assertEqual(keys[-3:], ["hd", "finish", "cleanup"])
            # Call of Duty 3's own texture steps stay off.
            self.assertFalse(pipe.o.upscale_textures)
            # The ripper an earlier build left in the output folder is used: no Rust then.
            with mock.patch.object(x360_ports.PortPipeline, "_output_rip", return_value=Path("rip")):
                self.assertFalse(pipe._needs_rust)
            with mock.patch.object(x360_ports.PortPipeline, "_output_rip", return_value=None), \
                    mock.patch.object(x360_ports.rippers, "needs_build", return_value=True):
                self.assertTrue(pipe._needs_rust)

    def test_a_failed_pack_doesnt_stop_the_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            _save((tmp / "out" / "waw").resolve(), 1)
            pipe = self.pipe(tmp, upscale_textures=True)
            with mock.patch.object(x360_ports.PortPipeline, "_output_rip", return_value=Path("rip")), \
                    mock.patch.object(x360_ports.deps, "fetch_portable", return_value=Path("w")), \
                    mock.patch.object(port_hd, "build", side_effect=SetupError("no graphics card")), \
                    mock.patch.object(pipe.log, "warn") as warn:
                pipe.port_hd_textures()
            self.assertIn("no graphics card", warn.call_args.args[0])

    def test_play_passes_the_pack_and_the_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            system = "windows" if not IS_LINUX else "linux"
            p = x360_ports.COD4
            folder = out / p.subdir
            (folder / "bin").mkdir(parents=True)
            exes = {q.name: f"bin/{q.name}" for q in p.programs}
            for rel in exes.values():
                (folder / rel).write_bytes(b"")
            (folder / p.manifest).write_text(json.dumps({"game": p.id, f"exe_{system}": exes}))
            _pack(folder, 1)
            cmd, why = x360_ports.play_command(p, out, x360_ports.PlayOptions(capture_textures=True))
            self.assertEqual(why, "")
            self.assertIn(f"--texture_replacement_path={port_hd.pack_dir(folder).resolve()}", cmd)
            self.assertIn(f"--texture_capture_path={port_hd.capture_dir(folder).resolve()}", cmd)
            self.assertNotIn("--texture_replacement_check_alpha=true", cmd)
            cmd, _ = x360_ports.play_command(p, out, x360_ports.PlayOptions(program="cod4mp"))
            self.assertIn("--texture_replacement_check_alpha=true", cmd)
            cmd, _ = x360_ports.play_command(p, out, x360_ports.PlayOptions(hd_textures=False))
            self.assertFalse(any(a.startswith("--texture_") for a in cmd))

    def test_the_boxes_are_kept(self):
        p = x360_ports.WAW
        o = x360_ports.PlayOptions(hd_textures=False, capture_textures=True)
        back = x360_ports.PlayOptions.from_settings(p, o.to_settings(p))
        self.assertEqual((back.hd_textures, back.capture_textures), (False, True))
        fresh = x360_ports.PlayOptions.from_settings(p, {})
        self.assertEqual((fresh.hd_textures, fresh.capture_textures), (True, False))


if __name__ == "__main__":
    unittest.main()
