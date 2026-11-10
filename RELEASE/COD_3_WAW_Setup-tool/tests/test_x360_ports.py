"""Tests for c3w/x360_ports.py: World at War, Call of Duty 2 and Call of Duty 4 recompiled from their Xbox 360
discs on Call of Duty 3's SDK [waw-decomp]. No disc, compiler or Tkinter needed: made-up programs stand in for the
discs' (their SHA-1s are swapped in for the test), and nothing is built.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import threading
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import x360_ports  # noqa: E402
from c3w.common import IS_LINUX, Log, SetupError  # noqa: E402
from c3w.pipeline import BuildOptions  # noqa: E402

CAMPAIGN = b"made-up campaign program"
MULTI = b"made-up multiplayer program"


def fake_port(base: x360_ports.Port) -> x360_ports.Port:
    """`base` with the made-up programs' SHA-1s."""
    progs = list(base.programs)
    progs[0] = replace(progs[0], sha1=hashlib.sha1(CAMPAIGN).hexdigest())
    if len(progs) > 1:
        progs[1] = replace(progs[1], sha1=hashlib.sha1(MULTI).hexdigest())
    return replace(base, programs=tuple(progs))


class TableTests(unittest.TestCase):
    def test_three_ports(self):
        self.assertEqual(x360_ports.PORT_ORDER, ("cod2", "waw", "cod4"))
        for pid in x360_ports.PORT_ORDER:
            p = x360_ports.port(pid)
            self.assertEqual(p.programs[0].xex, "default.xex")
            self.assertEqual(p.programs[0].label, "Campaign")
            for q in p.programs:
                self.assertRegex(q.sha1, r"^[0-9a-f]{40}$")
                self.assertEqual(q.codegen_target, q.name + "_codegen")
        with self.assertRaises(SetupError):
            x360_ports.port("halo")

    def test_projects(self):
        self.assertEqual(x360_ports.COD2.projects, [""])        # cod2mp is inside the port's own project
        self.assertEqual(x360_ports.COD4.projects, ["", "mp"])  # cod4mp is a project of its own
        self.assertEqual(x360_ports.WAW.projects, [""])
        self.assertEqual(x360_ports.COD4.program("cod4mp").asset_dir, "mp/assets")

    def test_the_ports_folders_are_next_to_the_tool(self):
        tool = Path(__file__).resolve().parent.parent
        for p in x360_ports.PORTS.values():
            self.assertTrue((tool / p.template / "CMakeLists.txt").is_file(), p.template)
            for q in p.programs:
                manifest = tool / p.template / (q.project or "") / f"{q.name}_manifest.toml"
                if q.name == "cod2mp":
                    manifest = tool / p.template / "mp" / "cod2mp_manifest.toml"
                self.assertTrue(manifest.is_file(), manifest)
                self.assertIn(q.sha1, manifest.read_text(encoding="utf-8") +
                              (tool / p.template / f"{p.programs[0].name}_manifest.toml").read_text(encoding="utf-8"))

    def test_launcher_names(self):
        self.assertEqual(x360_ports.launcher_name(x360_ports.COD2, x360_ports.COD2.programs[1]),
                         "Call of Duty 2 Multiplayer.sh")
        self.assertEqual(x360_ports.launcher_name(x360_ports.WAW, x360_ports.WAW.programs[0]), "Call of Duty.sh")
        self.assertEqual(x360_ports.launcher_name(x360_ports.COD4, x360_ports.COD4.programs[0]), "Call of Duty 4.sh")


class DiscTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.port = fake_port(x360_ports.COD2)

    def tearDown(self):
        self.tmp.cleanup()

    def disc(self, campaign=CAMPAIGN, multi=MULTI) -> Path:
        d = self.root / "disc"
        d.mkdir(exist_ok=True)
        if campaign is not None:
            (d / "default.xex").write_bytes(campaign)
        if multi is not None:
            (d / "default_mp.xex").write_bytes(multi)
        return d

    def test_known_disc(self):
        disc, line = x360_ports.check_disc(self.port, str(self.disc()))
        self.assertIsNotNone(disc)
        self.assertIn("the disc the port knows", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc)], ["cod2", "cod2mp"])

    def test_unknown_campaign_is_refused_unless_forced(self):
        d = self.disc(campaign=b"another release")
        disc, line = x360_ports.check_disc(self.port, str(d))
        self.assertIsNone(disc)
        self.assertIn("isn't the one the port was made for", line)
        disc, line = x360_ports.check_disc(self.port, str(d), force_unknown=True)
        self.assertIsNotNone(disc)
        self.assertIn("unknown", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc, True)], ["cod2", "cod2mp"])

    def test_missing_or_unknown_multiplayer_leaves_it_out(self):
        disc, line = x360_ports.check_disc(self.port, str(self.disc(multi=None)))
        self.assertIn("no default_mp.xex", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc)], ["cod2"])
        disc, line = x360_ports.check_disc(self.port, str(self.disc(multi=b"other")))
        self.assertIn("isn't the known one", line)
        self.assertEqual([q.name for q in x360_ports.buildable(self.port, disc)], ["cod2"])

    def test_not_a_disc(self):
        self.assertIsNone(x360_ports.check_disc(self.port, "")[0])
        self.assertIsNone(x360_ports.check_disc(self.port, str(self.root / "nope"))[0])
        empty = self.root / "empty"
        empty.mkdir()
        disc, line = x360_ports.check_disc(self.port, str(empty))
        self.assertIsNone(disc)
        self.assertIn("No default.xex", line)


class PlayTests(unittest.TestCase):
    def test_arguments(self):
        p = x360_ports.COD2
        o = x360_ports.PlayOptions(fullscreen=True, mute=True, skip_films=True, max_fps=144)
        args = o.arguments(p)
        self.assertIn("--fullscreen=true", args)
        self.assertIn("--audio_mute=true", args)
        self.assertIn("--cod2_skip_intro_movies=true", args)
        self.assertIn("--cod2_max_fps=144", args)
        # World at War has no skip setting yet: nothing is passed for it.
        self.assertFalse([a for a in x360_ports.PlayOptions(skip_films=True).arguments(x360_ports.WAW)
                          if "skip" in a])
        self.assertIn("--cod4_fov=90", x360_ports.PlayOptions(fov=90).arguments(x360_ports.COD4))

    def test_problems(self):
        p = x360_ports.COD4
        self.assertEqual(x360_ports.PlayOptions().problems(p), [])
        self.assertTrue(x360_ports.PlayOptions(program="cod2").problems(p))
        self.assertTrue(x360_ports.PlayOptions(fov=10).problems(p))
        self.assertTrue(x360_ports.PlayOptions(max_fps=5000).problems(p))
        self.assertTrue(x360_ports.PlayOptions(graphics_api="glide").problems(p))

    def test_settings_round_trip(self):
        p = x360_ports.COD4
        o = x360_ports.PlayOptions(program="cod4mp", fullscreen=True, fov=95, max_fps=0, graphics_api="d3d12")
        s = json.loads(json.dumps(o.to_settings(p)))
        again = x360_ports.PlayOptions.from_settings(p, s)
        self.assertEqual((again.program, again.fullscreen, again.fov, again.max_fps, again.graphics_api),
                         ("cod4mp", True, 95.0, 0, "d3d12"))
        junk = x360_ports.PlayOptions.from_settings(p, {"port_cod4_fov": "wide", "port_cod4_max_fps": True,
                                                        "port_cod4_graphics_api": "glide"})
        self.assertEqual((junk.fov, junk.max_fps, junk.graphics_api), (0.0, -1, "auto"))

    def test_play_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            p = x360_ports.COD2
            cmd, why = x360_ports.play_command(p, out, x360_ports.PlayOptions())
            self.assertIsNone(cmd)
            self.assertIn("Build", why)
            system = "windows" if not IS_LINUX else "linux"
            exe = "bin/cod2.exe" if system == "windows" else "bin/cod2"
            (out / "cod2" / "bin").mkdir(parents=True)
            (out / "cod2" / exe).write_bytes(b"")
            (out / "cod2" / "cod2.json").write_text(json.dumps({"game": "cod2", f"exe_{system}": {"cod2": exe}}))
            cmd, why = x360_ports.play_command(p, out, x360_ports.PlayOptions(mute=True))
            self.assertEqual(why, "")
            self.assertEqual(Path(cmd[0]), out / "cod2" / exe)
            self.assertIn("--audio_mute=true", cmd)
            cmd, why = x360_ports.play_command(p, out, x360_ports.PlayOptions(program="cod2mp"))
            self.assertIsNone(cmd)
            self.assertIn("isn't built", why)


class PipelineTests(unittest.TestCase):
    def pipe(self, tmp: Path, **kw) -> "x360_ports.PortPipeline":
        opts = BuildOptions(output_dir=str(tmp / "out"), work_dir=str(tmp / "work"), **kw)
        return x360_ports.PortPipeline(x360_ports.COD4, str(tmp / "disc"), opts, Log(), threading.Event())

    def test_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            keys = [s.key for s in self.pipe(tmp, build_linux=True, build_windows=True).steps()]
            if IS_LINUX:
                self.assertEqual(keys, ["inputs", "tools", "assets", "sdk", "recompile", "windows", "finish",
                                        "cleanup"])
                keys = [s.key for s in self.pipe(tmp, build_linux=False, build_windows=True).steps()]
                self.assertNotIn("recompile", keys)
                self.assertIn("windows", keys)
            pipe = self.pipe(tmp)
            self.assertEqual(pipe.root, (tmp / "out" / "cod4").resolve())
            self.assertFalse(pipe._needs_rust)
            self.assertFalse(pipe.wsl_linux)
            self.assertEqual(pipe.protected, [(tmp / "disc").resolve(), (tmp / "out").resolve()])

    def test_projects_follow_the_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            pipe = self.pipe(Path(tmp))
            pipe.programs = list(x360_ports.COD4.programs)
            self.assertEqual([(proj, [q.name for q in progs]) for proj, progs in pipe._projects()],
                             [("", ["cod4"]), ("mp", ["cod4mp"])])
            pipe.programs = [x360_ports.COD4.programs[0]]
            self.assertEqual([proj for proj, _ in pipe._projects()], [""])
            self.assertTrue(any(a.startswith("-DCOD3_RECOMP_SRC=") for a in pipe._extra_args()))

    def test_no_disc_is_refused_before_anything_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            pipe = self.pipe(Path(tmp))
            with self.assertRaises(SetupError) as e:
                pipe.preflight()
            self.assertIn("disc image", str(e.exception))

    def test_cod3_options_are_left_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            opts = BuildOptions(cod3_source="x.iso", waw_steam="steam", appimage=True, upscale_textures=True,
                                output_dir=str(tmp / "out"), work_dir=str(tmp / "work"))
            pipe = x360_ports.PortPipeline(x360_ports.WAW, str(tmp / "disc"), opts, Log(), threading.Event())
            self.assertEqual((pipe.o.cod3_source, pipe.o.waw_steam, pipe.o.appimage, pipe.o.upscale_textures),
                             ("", "", False, False))
            self.assertIsNone(pipe.cod3_src)


if __name__ == "__main__":
    unittest.main()
