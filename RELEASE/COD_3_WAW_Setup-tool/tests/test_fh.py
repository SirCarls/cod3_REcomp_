"""[acct2-decomp] 2026-11-21 Tests for the Finest Hour tab's logic (c3w/fh.py; owner 11:18 "add finest hour to the
tabs", a full tab like Big Red One's): the disc table, the disc check, the layout, the recompiler's commands, the
loader's back edge in the C, the install and the Play page's start. Nothing is built or run.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import bro, common, fh  # noqa: E402

EDGE = "0x000691DB"


def fake_xbe(title: str, version: int = 3, pad: bytes = b"") -> bytes:
    """The start of an Xbox program: the XBEH header and a certificate with a title and a version."""
    data = bytearray(0x400)
    data[:4] = b"XBEH"
    base, cert = 0x10000, 0x10000 + 0x200
    struct.pack_into("<I", data, 0x104, base)
    struct.pack_into("<I", data, 0x118, cert)
    struct.pack_into("<I", data, 0x200 + 0x08, 0x41560017)
    name = title.encode("utf-16-le")[:80]
    data[0x20C:0x20C + len(name)] = name
    struct.pack_into("<I", data, 0x200 + 0xAC, version)
    return bytes(data) + pad


class Folder(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def disc(self, title="Call of Duty: Finest Hour", pad=b"") -> Path:
        d = self.root / "disc"
        d.mkdir(exist_ok=True)
        (d / "default.xbe").write_bytes(fake_xbe(title, pad=pad))
        (d / "COD.PAK").write_bytes(b"pak")
        return d

    def table(self, rows) -> dict:
        path = self.root / "discs.tsv"
        path.write_text("xbe_md5\tname\tloader_back_edge\tnotes\n" + "".join("\t".join(r) + "\n" for r in rows),
                        encoding="utf-8")
        return fh.read_discs(path)


class DiscTable(Folder):
    def test_rows_and_their_back_edges(self):
        discs = self.table([("A" * 32, "Europe v3", EDGE, "base"), ("b" * 32, "USA v1", "", "not yet"),
                            ("c" * 32, "Odd", "here", ""), ("# comment", "", "", ""), ("short", "x", EDGE, "")])
        self.assertEqual(sorted(discs), ["a" * 32, "b" * 32, "c" * 32])
        self.assertEqual(discs["a" * 32].loader_back_edge, EDGE)
        self.assertEqual(discs["b" * 32].loader_back_edge, "")
        self.assertEqual(discs["c" * 32].loader_back_edge, "")   # not an address: can't be built

    def test_no_table(self):
        self.assertEqual(fh.read_discs(self.root / "none.tsv"), {})

    def test_the_mark_the_lift_writes(self):
        self.assertEqual(fh.back_edge_mark(EDGE), "RECOMP_BACK_EDGE(0x000691DBu)")
        self.assertEqual(fh.back_edge_mark("0x691db"), "RECOMP_BACK_EDGE(0x000691DBu)")

    def test_the_projects_table(self):
        path = fh.project_root() / "fh" / "config" / "discs.tsv"
        if not path.is_file():
            self.skipTest("no fh/ next to the setup tool")
        discs = fh.read_discs(path)
        self.assertTrue(discs)
        self.assertTrue(any(d.loader_back_edge for d in discs.values()), "a disc the port builds from")


class DiscCheck(Folder):
    def md5(self, d: Path) -> str:
        return hashlib.md5((d / "default.xbe").read_bytes()).hexdigest()

    def test_the_known_disc_builds(self):
        d = self.disc()
        disc, line = fh.check_disc(str(d), self.table([(self.md5(d), "Europe v3", EDGE, "")]))
        self.assertIsNotNone(disc)
        self.assertEqual((disc.kind, disc.files, disc.row.name), ("folder", 2, "Europe v3"))
        self.assertIn("Found:", line)
        disc, _ = fh.check_disc(str(d / "default.xbe"), self.table([(self.md5(d), "Europe v3", EDGE, "")]))
        self.assertIsNotNone(disc)   # (its default.xbe names the folder too)

    def test_a_known_disc_without_a_back_edge_is_named_and_turned_down(self):
        d = self.disc()
        disc, line = fh.check_disc(str(d), self.table([(self.md5(d), "USA v1", "", "")]))
        self.assertIsNone(disc)
        self.assertIn("USA v1", line)
        self.assertIn("doesn't build from this disc yet", line)

    def test_an_unknown_finest_hour_disc(self):
        d = self.disc(pad=b"other")
        disc, line = fh.check_disc(str(d), self.table([]))
        self.assertIsNone(disc)
        self.assertIn(self.md5(d), line)

    def test_another_game(self):
        d = self.disc(title="Call of Duty 2: Big Red One")
        disc, line = fh.check_disc(str(d), self.table([]))
        self.assertIsNone(disc)
        self.assertIn("Big Red One", line)

    def test_nothing_or_nothing_there(self):
        self.assertIn("Choose your", fh.check_disc("", {})[1])
        self.assertIn("isn't there", fh.check_disc(str(self.root / "gone"), {})[1])
        empty = self.root / "empty"
        empty.mkdir()
        self.assertIn("no default.xbe", fh.check_disc(str(empty), {})[1])


class LayoutAndCommands(Folder):
    def lay(self) -> "fh.Layout":
        return fh.layout(self.root / "out", self.root / "work", self.root / "repo")

    def test_folders(self):
        lay = self.lay()
        self.assertEqual(lay.out, self.root / "out" / "fh")
        self.assertEqual(lay.work, self.root / "work" / "fh")
        self.assertEqual(lay.tools, self.root / "work" / "tools")   # shared with Big Red One
        self.assertEqual(lay.exe, lay.out / "bin" / common.exe_name("fh"))
        self.assertEqual(lay.settings, lay.out / "fh.toml")          # the folder bin/ is in
        self.assertEqual(lay.manifest, lay.out / "fh.json")
        self.assertEqual(lay.stand_ins, self.root / "repo" / "fh" / "config" / "stand_in_functions.tsv")
        self.assertEqual(lay.xboxrecomp_cache, bro.layout(self.root / "out", self.root / "work").xboxrecomp_cache)
        self.assertEqual(lay.sdk, self.root / "work" / "rexglue-sdk")

    def test_patches_in_fh_gen3s_order(self):
        lay = self.lay()
        p = self.root / "repo" / "fh" / "patches"
        p.mkdir(parents=True)
        for n in ("z.patch", "a.patch", "notes.txt"):
            (p / n).write_text("x", encoding="utf-8")
        self.assertEqual([x.name for x in lay.patches], ["xboxrecomp-lazy-decode.patch", "a.patch", "z.patch"])
        self.assertEqual(lay.patches[0].parent, self.root / "repo" / "bro" / "patches")
        self.assertEqual([x.name for x in lay.py_patches], list(fh.PY_PATCHES))
        self.assertEqual(fh.PY_PATCHES[0], "xboxrecomp_guest_priority.py")

    def test_the_lift_yields_at_back_edges_and_the_disassembly_has_no_seeds(self):
        lay = self.lay()
        lift = fh.step_command("recomp", lay, Path("py"))
        self.assertIn("--back-edge-yield", lift)
        self.assertEqual(lift[lift.index("--game-name") + 1], "Finest Hour")
        self.assertEqual(lift[lift.index("--gen-dir") + 1], str(lay.gen))
        self.assertNotIn("--seed-functions", fh.step_command("disasm", lay, Path("py")))
        self.assertEqual([s for s, _, _ in fh.STEPS], [s for s, _, _ in bro.STEPS])
        with self.assertRaises(ValueError):
            fh.step_command("nope", lay, Path("py"))

    def test_configure_takes_the_sdks_parts_when_there(self):
        lay = self.lay()
        t = bro.Tools(cmake=Path("cmake"))
        none = fh.SdkParts(None, None, None)
        cmd = fh.configure_command(lay, t, none)
        self.assertIn(f"-DFH_GEN_DIR={lay.gen}", cmd)
        self.assertIn(f"-DXBOXRECOMP_DIR={lay.xboxrecomp}", cmd)
        self.assertFalse([c for c in cmd if c.startswith(("-DSDL3_DIR", "-DFH_FFMPEG", "-DFH_IMGUI"))])
        some = fh.SdkParts(Path("s"), Path("f"), Path("i"))
        cmd = fh.configure_command(lay, bro.Tools(cmake=Path("cmake"), ninja=Path("ninja")), some)
        self.assertEqual(cmd[1:3], ["-G", "Ninja"])
        for flag in ("-DSDL3_DIR=s", "-DFH_FFMPEG_SOURCE_DIR=f", "-DFH_IMGUI_DIR=i"):
            self.assertIn(flag, cmd)

    def test_sdk_parts_found_only_whole(self):
        lay = self.lay()
        self.assertEqual(fh.sdk_parts(lay), fh.SdkParts(None, None, None))
        (lay.sdk / "thirdparty" / "imgui").mkdir(parents=True)
        (lay.sdk / "thirdparty" / "imgui" / "imgui.h").write_text("", encoding="utf-8")
        self.assertEqual(fh.sdk_parts(lay).imgui, lay.sdk / "thirdparty" / "imgui")


class BackEdge(Folder):
    def test_found_and_counted(self):
        lay = fh.layout(self.root / "out", self.root / "work", self.root / "repo")
        lay.gen.mkdir(parents=True)
        (lay.gen / "recomp_0000.c").write_text("RECOMP_BACK_EDGE(0x00012345u);\n", encoding="utf-8")
        (lay.gen / "recomp_0001.c").write_text("x; RECOMP_BACK_EDGE(0x000691DBu); y\n", encoding="utf-8")
        self.assertEqual(fh.check_back_edge(lay, EDGE), 2)

    def test_missing_or_no_c(self):
        lay = fh.layout(self.root / "out", self.root / "work", self.root / "repo")
        lay.gen.mkdir(parents=True)
        with self.assertRaises(common.SetupError):
            fh.check_back_edge(lay, EDGE)          # no C
        (lay.gen / "recomp_0000.c").write_text("RECOMP_BACK_EDGE(0x00012345u);\n", encoding="utf-8")
        with self.assertRaisesRegex(common.SetupError, "0x000691DBu"):
            fh.check_back_edge(lay, EDGE)


class CompileBar(unittest.TestCase):
    def test_the_bar_follows_the_builds_count(self):
        seen = []

        class Bar:
            def progress(self, frac, text=""):
                seen.append(round(frac, 3))

        clock = iter([10.0, 11.0, 12.0])           # the status line is shown twice a second at most
        with mock.patch.object(bro.time, "monotonic", lambda: next(clock)):
            handle = fh.compile_lines(Bar())
            handle("[83/333] Building C object CMakeFiles/fh.dir/recomp_0042.c.o")
            handle("[ 50%] Building C object CMakeFiles/fh.dir/recomp_0042.c.o")
            handle("[0/0] nothing to do")          # no division by zero; the bar stays
        self.assertEqual(seen, [0.249, 0.5, 0.5])


class InstallAndPlay(Folder):
    def test_install_then_play(self):
        repo = self.root / "repo"
        (repo / "fh" / "config").mkdir(parents=True)
        (repo / "fh" / "config" / "fh.toml").write_text("version = 1\n", encoding="utf-8")
        out = self.root / "out"
        lay = fh.layout(out, self.root / "work", repo)
        built = self.root / "built-fh"
        built.write_bytes(b"\x7fELF")
        d = self.disc()
        disc = fh.Disc("folder", d, "f" * 32, bro.xbe_info(fake_xbe("Call of Duty: Finest Hour")), 2, 3,
                       fh.DiscRow("f" * 32, "Europe v3", EDGE, ""))
        self.assertIsNone(fh.installed(out))
        self.assertIn("Not built yet", fh.play_command(out)[1])
        fh.install(lay, built, disc, "9.9.9")
        self.assertEqual(fh.installed(out), lay.exe)
        self.assertEqual(lay.settings.read_text(encoding="utf-8"), "version = 1\n")
        self.assertIn("RECOMP_GUEST_LOCK", lay.launcher.read_text(encoding="utf-8"))
        m = common.read_json(lay.manifest)
        self.assertEqual((m["exe_linux"], m["disc"], m["tool_version"]), ("bin/fh", "Europe v3", "9.9.9"))
        cmd, why = fh.play_command(out)
        self.assertEqual((cmd, why), ([str(lay.exe), str(lay.game), str(lay.saves)], ""))
        # A player's own fh.toml stays through the next build.
        lay.settings.write_text("version = 1\n[display]\nmode = \"fullscreen\"\n", encoding="utf-8")
        fh.install(lay, built, disc, "9.9.10")
        self.assertIn("fullscreen", lay.settings.read_text(encoding="utf-8"))

    def test_a_stopped_build_is_said(self):
        logs = self.root / "out" / "fh" / "logs"
        logs.mkdir(parents=True)
        (logs / "build.log").write_text("[step] Building\n[error] cc failed\n", encoding="utf-8")
        why = fh.play_command(self.root / "out")[1]
        self.assertIn("Building", why)
        self.assertIn("cc failed", why)


class PlayOptions(unittest.TestCase):
    def test_round_trip_and_bad_values(self):
        o = fh.PlayOptions(fullscreen=True, vsync=False, size="1920x1080", language="french", skip_films=True)
        self.assertEqual(fh.PlayOptions.from_settings(o.to_settings()), o)
        bad = fh.PlayOptions.from_settings({"fh_size": "huge", "fh_language": "klingon", "fh_fullscreen": 1,
                                            "fh_vsync": "no"})
        self.assertEqual(bad, fh.PlayOptions())
        self.assertTrue(fh.PlayOptions(size="1x").problems())
        self.assertTrue(fh.PlayOptions(language="klingon").problems())
        self.assertFalse(fh.PlayOptions().problems())

    def test_the_start_has_the_guest_cpu_lock_and_only_what_was_chosen(self):
        base = {"PATH": os.environ.get("PATH", ""), "FH_SIZE": "1x1", "FH_SKIP_FILMS": "1", "RECOMP_GUEST_LOCK": "0"}
        env = fh.play_env(fh.PlayOptions(), base)
        self.assertEqual(env["RECOMP_GUEST_LOCK"], "1")
        self.assertNotIn("FH_SIZE", env)
        self.assertNotIn("FH_SKIP_FILMS", env)
        env = fh.play_env(fh.PlayOptions(fullscreen=True, vsync=False, size="1600x1200", language="italian",
                                         skip_films=True), base)
        self.assertEqual({k: env[k] for k in ("FH_FULLSCREEN", "FH_VSYNC", "FH_SIZE", "FH_LANGUAGE",
                                              "FH_SKIP_FILMS")},
                         {"FH_FULLSCREEN": "1", "FH_VSYNC": "0", "FH_SIZE": "1600x1200", "FH_LANGUAGE": "italian",
                          "FH_SKIP_FILMS": "1"})

    def test_languages_are_the_ports(self):
        # fh/src/main.c's names; the Europe disc has these four.
        self.assertEqual([k for k, _ in fh.LANGUAGES if k], ["english", "french", "spanish", "italian"])


class Profiles(unittest.TestCase):
    def test_a_profile_keeps_every_finest_hour_setting(self):
        from c3w import profiles
        saved = {"fh_source": "/discs/fh.iso",
                 **fh.PlayOptions(fullscreen=True, vsync=False, size="1920x1080", language="french",
                                  skip_films=True).to_settings()}
        notes = []
        self.assertEqual(profiles.clean_settings(saved, notes), saved)
        self.assertEqual(notes, [])
        self.assertIn("fh_source", profiles.PATH_KEYS)     # exported only when inside the project folder
        self.assertEqual(profiles.clean_settings({"fh_language": "german", "fh_size": "1x1"}), {})


class Supported(unittest.TestCase):
    def test_linux_first(self):
        ok, why = fh.supported()
        self.assertEqual(ok, common.IS_LINUX)
        if not ok:
            self.assertIn("Linux", why)


if __name__ == "__main__":
    unittest.main()
