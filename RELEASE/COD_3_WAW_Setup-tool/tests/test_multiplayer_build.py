"""Tests for building the multiplayer program (cod3mp) next to the campaign:
which codmp_xenonf.xex goes into the project, and how the packaged game
starts one program or the other. Nothing is built; the files are made up.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import hashlib
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import pipeline  # noqa: E402
from c3w.common import Log  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def make_pipeline(tmp: Path) -> Pipeline:
    o = BuildOptions(output_dir=str(tmp / "out"), work_dir=str(tmp / "work"))
    return Pipeline(o, Log(sinks=[]), threading.Event())


class CopyMpXexTests(unittest.TestCase):
    def test_known_build_goes_into_the_project(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = make_pipeline(Path(tmp))
            data = b"multiplayer executable"
            (p.assets).mkdir(parents=True)
            (p.assets / pipeline.MP_XEX).write_bytes(data)
            known = {hashlib.sha1(data).hexdigest(): "test build"}
            with mock.patch.object(pipeline, "KNOWN_MP_XEX", known):
                self.assertTrue(p._copy_mp_xex())
            self.assertEqual((p.proj / "assets" / pipeline.MP_XEX).read_bytes(), data)

    def test_other_build_is_left_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = make_pipeline(Path(tmp))
            p.assets.mkdir(parents=True)
            (p.assets / pipeline.MP_XEX).write_bytes(b"some other build")
            # A copy from an earlier build must go too, or CMake would still add mp/.
            stale = p.proj / "assets" / pipeline.MP_XEX
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"old")
            self.assertFalse(p._copy_mp_xex())
            self.assertFalse(stale.exists())

    def test_no_multiplayer_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = make_pipeline(Path(tmp))
            p.assets.mkdir(parents=True)
            self.assertFalse(p._copy_mp_xex())

    def test_the_known_hash_is_the_one_the_hooks_are_for(self):
        # recomp/mp/config/codmp.toml names the build its addresses are from.
        config = (ROOT / "recomp" / "mp" / "config" / "codmp.toml").read_text(encoding="utf-8")
        for sha in pipeline.KNOWN_MP_XEX:
            self.assertIn(sha, config)


class ProjectLayoutTests(unittest.TestCase):
    def test_mp_folder_is_added_only_with_its_executable(self):
        top = (ROOT / "recomp" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/codmp_xenonf.xex"', top)
        self.assertIn("add_subdirectory(mp)", top)
        # Last: mp/'s generated glue redefines the SDK's macros.
        self.assertGreater(top.index("add_subdirectory(mp)"), top.index("rexglue_setup_target(cod3"))

    def test_mp_manifest_reads_the_projects_assets(self):
        manifest = (ROOT / "recomp" / "mp" / "cod3mp_manifest.toml").read_text(encoding="utf-8")
        self.assertIn('file_path = "../assets/codmp_xenonf.xex"', manifest)
        self.assertIn('name = "cod3mp"', manifest)
        self.assertTrue((ROOT / "recomp" / "mp" / "generated" / "rexglue.cmake").is_file())

    def test_shared_sources_hook_no_campaign_address(self):
        # A sub_XXXXXXXX hook in a file compiled into cod3mp would replace
        # whatever function has that address in codmp_xenonf.xex.
        import re
        cmake = (ROOT / "recomp" / "mp" / "CMakeLists.txt").read_text(encoding="utf-8")
        sources = re.findall(r"\$\{COD3_SRC\}/(\S+\.cpp)", cmake)
        self.assertIn("game/script_threads.cpp", sources)
        allowed = {"sub_823143D0"}   # codmp_xenonf.xex's own SetEvent (game/mp_hooks.cpp)
        for rel in sources:
            text = (ROOT / "recomp" / "src" / rel).read_text(encoding="utf-8")
            for name in re.findall(r"REX_(?:HOOK_RAW|EXTERN)\((?:__imp__)?(sub_[0-9A-F]{8})\)", text):
                self.assertIn(name, allowed, f"{rel} hooks {name}")


class AppRunTests(unittest.TestCase):
    def test_apprun_starts_the_program_asked_for(self):
        src = (ROOT / "c3w" / "pipeline.py").read_text(encoding="utf-8")
        self.assertIn('"$COD3_PROGRAM" = cod3mp', src)
        relaunch = (ROOT / "recomp" / "src" / "game" / "relaunch.cpp").read_text(encoding="utf-8")
        self.assertIn('"COD3_PROGRAM"', relaunch)


if __name__ == "__main__":
    unittest.main()
