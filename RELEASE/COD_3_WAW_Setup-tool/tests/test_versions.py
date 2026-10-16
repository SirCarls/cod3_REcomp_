"""Tests for building both versions of the game (Linux AppImage and Windows
.exe) from either system, and for the Linux distro families the tool knows.
Nothing is built or downloaded; the pipeline's step list, the WSL helpers
and the package tables are checked with made-up folders.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import deps, pipeline, wsl  # noqa: E402
from c3w.pipeline import BuildOptions, Pipeline  # noqa: E402
from c3w.common import Log  # noqa: E402
import threading  # noqa: E402


def titles(host: str, **opts) -> list:
    with tempfile.TemporaryDirectory() as tmp:
        o = BuildOptions(output_dir=str(Path(tmp) / "out"), work_dir=str(Path(tmp) / "work"), **opts)
        with mock.patch.object(pipeline, "IS_LINUX", host == "linux"), \
                mock.patch.object(pipeline, "IS_WINDOWS", host == "windows"):
            p = Pipeline(o, Log(sinks=[]), threading.Event())
            return [s.title for s in p.steps()]


class VersionStepsTests(unittest.TestCase):
    def test_linux_host_both(self):
        t = titles("linux", build_linux=True, build_windows=True)
        self.assertIn("Recompiling Call of Duty 3", t)
        self.assertIn("Packaging", t)
        self.assertIn("Windows version (cod3.exe)", t)
        self.assertNotIn("Linux version (AppImage, built in WSL)", t)

    def test_linux_host_windows_only_still_translates(self):
        t = titles("linux", build_linux=False, build_windows=True)
        self.assertIn("ReXGlue SDK", t)                      # the cross build uses its code translator
        self.assertIn("Translating Call of Duty 3's code", t)
        self.assertNotIn("Packaging", t)                     # no Linux game
        self.assertIn("Windows version (cod3.exe)", t)

    def test_windows_host_both(self):
        t = titles("windows", build_linux=True, build_windows=True)
        self.assertIn("Packaging", t)
        self.assertIn("Linux version (AppImage, built in WSL)", t)
        self.assertNotIn("Windows version (cod3.exe)", t)

    def test_windows_host_linux_only(self):
        t = titles("windows", build_linux=True, build_windows=False)
        self.assertNotIn("ReXGlue SDK", t)                   # nothing compiled on Windows itself
        self.assertIn("Linux version (AppImage, built in WSL)", t)

    def test_rips_only(self):
        t = titles("linux", build_recomp=False, build_linux=True, build_windows=True)
        self.assertNotIn("ReXGlue SDK", t)
        self.assertNotIn("Windows version (cod3.exe)", t)
        self.assertIn("Ripping Call of Duty 3", t)

    def test_binaries_only_skips_rips_and_finish(self):
        t = titles("linux", build_linux=True, binaries_only=True)
        self.assertNotIn("Ripper", t)
        self.assertNotIn("World at War", t)
        self.assertNotIn("Finishing", t)                     # the outer (Windows) run writes the manifest
        self.assertIn("Packaging", t)


class WorldAtWarOptionalTests(unittest.TestCase):
    def test_no_source_keeps_an_earlier_rip(self):
        # Owner's build 2026-10-10 with World at War unticked: the step
        # removed the earlier data/waw/ rip before noticing there was no source.
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            old = out / "data" / "waw" / "nazi_zombie_prototype"
            old.mkdir(parents=True)
            (old / "x.gsc").write_text("keep")
            o = BuildOptions(output_dir=str(out), work_dir=str(Path(tmp) / "work"))
            p = Pipeline(o, Log(sinks=[]), threading.Event())
            p.rip, p.tc = Path(tmp) / "rip", mock.Mock()
            p.rip_waw()
            self.assertTrue((old / "x.gsc").exists())


class ReviewFixTests(unittest.TestCase):
    """Fixes from the 2026-10-10 logic review."""

    def test_missing_levels(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for lvl in pipeline.LEVELS[:-1]:
                (d / f"libcod3_{lvl}.so").write_bytes(b"")
            self.assertEqual(pipeline.missing_levels(d), [pipeline.LEVELS[-1]])
            (d / f"cod3_{pipeline.LEVELS[-1]}.dll").write_bytes(b"")
            self.assertEqual(pipeline.missing_levels(d), [])

    def test_portable_only_when_verified_against_the_pin(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = Path(tmp)
            key = "ninja-linux"
            (tools / key).mkdir()
            self.assertFalse(deps._portable_ok(tools, key))            # half unpacked
            (tools / key / ".verified").write_text("0" * 64)
            self.assertFalse(deps._portable_ok(tools, key))            # an older pin
            (tools / key / ".verified").write_text(deps.PORTABLE[key][2])
            self.assertTrue(deps._portable_ok(tools, key))

    def test_package_manager_from_os_release(self):
        have = {"dnf", "zypper"}   # openSUSE with dnf installed on the side
        with mock.patch.object(deps, "os_release", return_value={"ID": "opensuse-tumbleweed",
                                                                  "ID_LIKE": "opensuse suse"}), \
                mock.patch.object(deps.shutil, "which", side_effect=lambda n, *a, **k: n if n in have else None):
            self.assertEqual(deps._linux_package_manager(), "zypper")

    def test_inner_progress_is_relayed_line_by_line(self):
        import io
        from c3w import common
        buf = io.StringIO()
        with mock.patch.object(sys, "stdout", buf):
            common.set_console_line_mode(True)
            try:
                inner = Log(sinks=[common.console_sink])
                inner.step("[4/7] ReXGlue SDK")
                inner.progress(0.42, "SDK 10/24")
            finally:
                common.set_console_line_mode(False)
        got = []
        p = Pipeline.__new__(Pipeline)
        p.log = Log(sinks=[lambda k, t, f: got.append((k, t, f))])
        relay = p._relay_inner_log("Linux (WSL)")
        for line in buf.getvalue().splitlines():
            relay(line)
        self.assertIn(("progress", "Linux (WSL): SDK 10/24", 0.42), got)
        self.assertIn(("out", "==> [4/7] ReXGlue SDK", None), got)


class WslHelperTests(unittest.TestCase):
    def test_paths(self):
        self.assertEqual(wsl.to_wsl_path(Path("C:\\Games\\CoD3 out")), "/mnt/c/Games/CoD3 out")
        self.assertEqual(wsl.to_wsl_path(Path("D:\\")), "/mnt/d")

    def test_network_path_refused(self):
        with self.assertRaises(Exception):
            wsl.to_wsl_path(Path("\\\\server\\share\\out"))

    def test_distro_list_utf16(self):
        raw = "Ubuntu\r\n".encode("utf-16-le").decode("utf-8", "replace") + \
              "c3w-builder\r\n".encode("utf-16-le").decode("utf-8", "replace")
        self.assertEqual(wsl.parse_distro_list(raw), ["Ubuntu", "c3w-builder"])
        self.assertEqual(wsl.parse_distro_list("Ubuntu\nc3w-builder\n"), ["Ubuntu", "c3w-builder"])

    def test_checksum_list(self):
        text = ("a" * 64 + " *ubuntu-24.04.5-live-server-amd64.iso\n" +
                "B" * 64 + " *ubuntu-24.04.5-wsl-amd64.wsl\n")
        self.assertEqual(deps.sha256_from_list(text, "ubuntu-24.04.5-wsl-amd64.wsl"), "b" * 64)
        self.assertIsNone(deps.sha256_from_list(text, "missing.wsl"))


class DistroTests(unittest.TestCase):
    def test_every_family_has_every_list(self):
        keys = {"base", "clang", "libs", "extras", "mingw"}
        for pm, spec in deps.LINUX_PACKAGES.items():
            self.assertEqual(set(spec), keys, pm)
            self.assertIn(pm, deps._PROBE)
            self.assertIn(pm, deps._INSTALL)

    def test_families(self):
        # Debian/Ubuntu/Mint, Fedora, Arch/Manjaro/SteamOS, openSUSE, Void.
        self.assertTrue({"apt", "dnf", "pacman", "zypper", "xbps"} <= set(deps.LINUX_PACKAGES))

    def test_container_is_not_immutable(self):
        with mock.patch.object(deps, "in_container", return_value=True):
            self.assertIsNone(deps.immutable_system())


@unittest.skipUnless(sys.platform.startswith("linux"), "uses shell scripts as fake compilers")
class CompilerDetectionTests(unittest.TestCase):
    """The owner's build of 2026-10-10 took llvm-mingw's clang-23 (a Windows
    cross compiler left in tools/ by an earlier run) for the host compiler and
    stopped at "clang++-23 is not a full path to an existing compiler tool"."""

    def _fake(self, folder: Path, name: str, version: int) -> Path:
        folder.mkdir(parents=True, exist_ok=True)
        f = folder / name
        f.write_text(f"#!/bin/sh\necho 'clang version {version}.1.1'\n")
        f.chmod(0o755)
        return f

    def test_cross_toolchain_not_used_for_the_host(self):
        from c3w.common import Runner
        with tempfile.TemporaryDirectory() as tmp:
            tools = Path(tmp) / "tools"
            mingw_bin = deps._portable_bin(tools, "llvm-mingw-linux")
            self._fake(mingw_bin, "clang-23", 23)
            self._fake(mingw_bin, "x86_64-w64-mingw32-gcc", 23)
            with mock.patch.dict("os.environ", {"PATH": "/usr/bin:/bin"}):
                tc = deps.detect(tools, Runner(Log(sinks=[]), threading.Event()))
            for p in (tc.cc, tc.cxx, tc.mingw):
                if p:
                    self.assertNotIn(str(tools), str(p))
            self.assertNotIn(str(mingw_bin), tc.env["PATH"])

    def test_clang_without_its_cxx_is_skipped(self):
        from c3w.common import Runner
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / "bin"
            cc = self._fake(d, "clang-23", 23)
            runner = Runner(Log(sinks=[]), threading.Event())
            self.assertIsNone(deps._clang_cxx(cc, str(d), runner))
            self._fake(d, "clang++", 23)      # same version next to it: accepted
            self.assertEqual(deps._clang_cxx(cc, str(d), runner), d / "clang++")
            self._fake(d, "clang++-23", 23)   # the exact name wins
            self.assertEqual(deps._clang_cxx(cc, str(d), runner), d / "clang++-23")

    def test_mismatched_plain_cxx_is_refused(self):
        from c3w.common import Runner
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / "bin"
            cc = self._fake(d, "clang-20", 20)
            self._fake(d, "clang++", 18)       # e.g. the distro's default alternative
            self.assertIsNone(deps._clang_cxx(cc, str(d), Runner(Log(sinks=[]), threading.Event())))


if __name__ == "__main__":
    unittest.main()
