"""Tests for the parts that keep builds working and safe: reading which
libraries a program needs (so the game is packed with everything it needs),
the space-free build link, and archive extraction that can't write outside
its folder. They use this Python's own executable and small made-up files;
no game data.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import io
import os
import shutil
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import binaries, common  # noqa: E402

PYTHON = Path(sys.executable).resolve()


class BinariesTests(unittest.TestCase):
    def test_needed_reads_this_python(self):
        libs = binaries.needed(PYTHON)
        self.assertIsInstance(libs, list)
        if common.IS_WINDOWS:
            self.assertTrue(any(n.lower() == "kernel32.dll" for n in libs), libs)
        elif PYTHON.read_bytes()[:4] == b"\x7fELF" and libs:
            self.assertTrue(all(n.endswith(".so") or ".so." in n for n in libs), libs)

    def test_gather_reports_and_finds_libraries(self):
        libs = binaries.needed(PYTHON)
        if not libs:
            self.skipTest("this Python is statically linked")
        # Nothing counts as installed: every needed library is reported.
        files, missing = binaries.gather([PYTHON], [], lambda name: False)
        self.assertEqual(files, [PYTHON])
        self.assertEqual({lib for _, lib in missing}, set(libs))
        # Everything counts as installed: nothing is reported.
        files, missing = binaries.gather([PYTHON], [], lambda name: True)
        self.assertEqual(missing, [])

    def test_gather_packs_a_library_built_with_the_game(self):
        libs = binaries.needed(PYTHON)
        if not libs:
            self.skipTest("this Python is statically linked")
        with tempfile.TemporaryDirectory() as tmp:
            built = Path(tmp)
            # Pretend the first library Python needs was built with the game.
            shutil.copyfile(PYTHON, built / libs[0])
            files, missing = binaries.gather([PYTHON], [built], lambda name: name != libs[0])
            self.assertIn(built / libs[0], files)
            self.assertEqual(missing, [])

    def test_section_end_is_inside_the_file(self):
        if PYTHON.read_bytes()[:4] != b"\x7fELF":
            self.skipTest("not an ELF system")
        end = binaries.elf_section_end(PYTHON)
        self.assertTrue(0 < end <= PYTHON.stat().st_size)


class BuildLinkTests(unittest.TestCase):
    def test_path_with_spaces_gets_a_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "games and stuff" / "work"
            link = common.build_path(work)
            self.assertTrue(work.is_dir())
            if link == work:
                self.skipTest("no link could be made on this system")
            self.assertNotIn(" ", str(link))
            self.assertTrue(os.path.samefile(link, work))
            (work / "x.txt").write_text("hi", encoding="utf-8")
            self.assertEqual((link / "x.txt").read_text(encoding="utf-8"), "hi")
            self.assertEqual(common.build_path(work), link)   # reused
            common.remove_build_link(link, work)
            self.assertFalse(os.path.lexists(link))
            self.assertTrue((work / "x.txt").exists())          # the files stay

    def test_plain_path_is_used_as_is(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "work"
            if " " in str(work) or (common.IS_WINDOWS and len(str(work)) > 60):
                self.skipTest("the temporary folder itself has spaces or is long")
            self.assertEqual(common.build_path(work), work)


class ArchiveTests(unittest.TestCase):
    def test_tar_link_out_of_the_folder_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "bad.tar.gz"
            with tarfile.open(archive, "w:gz") as t:
                link = tarfile.TarInfo("pkg/escape")
                link.type = tarfile.SYMTYPE
                link.linkname = "../../../../outside"
                t.addfile(link)
            with self.assertRaises(common.SetupError):
                common.extract_archive(archive, Path(tmp) / "out", common.Log(sinks=[]))

    def test_tar_link_inside_the_folder_is_kept(self):
        if common.IS_WINDOWS:
            self.skipTest("symbolic links need extra rights on Windows")
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "good.tar.gz"
            with tarfile.open(archive, "w:gz") as t:
                data = tarfile.TarInfo("pkg/bin/tool-20")
                data.size = 2
                t.addfile(data, io.BytesIO(b"hi"))
                link = tarfile.TarInfo("pkg/bin/tool")
                link.type = tarfile.SYMTYPE
                link.linkname = "tool-20"
                t.addfile(link)
            out = common.extract_archive(archive, Path(tmp) / "out", common.Log(sinks=[]))
            self.assertEqual((out / "pkg" / "bin" / "tool").read_bytes(), b"hi")

    def test_download_cache_is_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            cached = Path(tmp) / "tool.zip"
            cached.write_bytes(b"not what was pinned")
            # A wrong cached file is deleted before downloading again; the
            # download itself fails here (no such host), which is fine.
            with self.assertRaises(Exception):
                common.download("https://invalid.invalid/tool.zip", cached, common.Log(sinks=[]),
                                __import__("threading").Event(), sha256="0" * 64)
            self.assertFalse(cached.exists())


class DependencyTests(unittest.TestCase):
    def test_every_download_is_pinned(self):
        from c3w import deps
        for table in (deps.PORTABLE, deps.WINDOWS_FALLBACK):
            for key, (url, sub, sha) in table.items():
                self.assertTrue(url.startswith("https://"), key)
                self.assertRegex(sha, r"^[0-9a-f]{64}$", key)
                self.assertFalse(Path(sub).is_absolute() or ".." in Path(sub).parts, key)

    def test_vc_runtime_is_packed(self):
        # Present on the build PC, but not on one without the redistributable.
        self.assertFalse(binaries.windows_is_system_dll("MSVCP140.dll"))
        self.assertFalse(binaries.windows_is_system_dll("vcruntime140_1.dll"))
        self.assertTrue(binaries.windows_is_system_dll("api-ms-win-crt-runtime-l1-1-0.dll"))

    def test_vc_runtime_dirs_newest_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            vs = Path(tmp)
            for ver in ("14.38.33135", "14.40.33807", "14.9.1"):
                (vs / "VC" / "Redist" / "MSVC" / ver / "x64" / "Microsoft.VC143.CRT").mkdir(parents=True)
            dirs = binaries.vc_runtime_dirs(vs)
            self.assertEqual([d.parts[-3] for d in dirs], ["14.40.33807", "14.38.33135", "14.9.1"])
            self.assertEqual(binaries.vc_runtime_dirs(None), [])

    def test_windows_sdk_needs_every_file(self):
        from c3w import deps
        with tempfile.TemporaryDirectory() as tmp:
            env = {"WindowsSdkDir": tmp + os.sep, "WindowsSDKVersion": "10.0.22621.0\\"}
            for f in deps.WINDOWS_SDK_FILES[:-1]:
                p = Path(tmp) / f.format(v="10.0.22621.0")
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("")
            self.assertIsNone(deps.find_windows_sdk(env))
            last = Path(tmp) / deps.WINDOWS_SDK_FILES[-1].format(v="10.0.22621.0")
            last.parent.mkdir(parents=True, exist_ok=True)
            last.write_text("")
            self.assertEqual(deps.find_windows_sdk(env), Path(tmp) / "Include" / "10.0.22621.0")
            self.assertIsNone(deps.find_windows_sdk({}))


if __name__ == "__main__":
    unittest.main()
