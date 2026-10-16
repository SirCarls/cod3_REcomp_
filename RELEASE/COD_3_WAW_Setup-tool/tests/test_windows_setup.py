"""Tests for the Windows setup path (2026-10-13, "users are reporting issues
building on Windows"). Windows is simulated: IS_WINDOWS, subprocess and the
environment are patched; nothing is installed, downloaded or run for real.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import binaries, common, deps, pc_check, pipeline  # noqa: E402
from c3w.common import Log, Runner, SetupError  # noqa: E402


def on_windows():
    """Patches that make common, deps and pc_check take their Windows paths."""
    return [mock.patch.object(common, "IS_WINDOWS", True), mock.patch.object(common, "IS_LINUX", False),
            mock.patch.object(deps, "IS_WINDOWS", True), mock.patch.object(deps, "IS_LINUX", False)]


class Patched(unittest.TestCase):
    def patch_all(self, patches):
        for p in patches:
            p.start()
            self.addCleanup(p.stop)


class FailureHintTests(Patched):
    def test_known_windows_errors_get_a_fix(self):
        self.patch_all(on_windows())
        cases = {
            "error: unable to create file thirdparty/x/y.h: Filename too long": "C:\\Games\\COD3",
            "fatal: unable to access 'https://github.com/x': SSL certificate problem: unable to get local issuer "
            "certificate": "http.sslBackend schannel",
            "LINK : fatal error LNK1104: cannot open file 'cod3.exe'": "Close the game",
            "error STL1000: Unexpected compiler version, expected Clang 20.0.0 or newer.": "LLVM 20",
            "fatal error: 'windows.h' file not found": "Windows 11 SDK",
            "fatal: unable to access 'https://github.com/': Could not resolve host: github.com": "HTTPS_PROXY",
            "There is not enough space on the disk.": "disk is full",
        }
        for line, wanted in cases.items():
            hint = pc_check.failure_hint(["[12/900] compiling", line])
            self.assertTrue(hint.startswith("What to do: "), line)
            self.assertIn(wanted, hint, line)
        self.assertEqual(pc_check.failure_hint(["all fine"]), "")

    def test_linux_messages_unchanged(self):
        with mock.patch.object(common, "IS_WINDOWS", False):
            self.assertEqual(pc_check.failure_hint(["Filename too long"]), "")

    def test_runner_adds_the_hint(self):
        runner = Runner(Log(sinks=[]), threading.Event())
        runner.failure_hint = lambda lines: "HINT " + lines[-1] if lines else ""
        with self.assertRaises(SetupError) as ctx:
            runner.run([sys.executable, "-c", "print('Filename too long'); raise SystemExit(3)"], quiet=True)
        self.assertIn("exit code 3", str(ctx.exception))
        self.assertIn("HINT Filename too long", str(ctx.exception))

    def test_blocked_program_is_explained_on_windows(self):
        self.patch_all([mock.patch.object(common, "IS_WINDOWS", True),
                        mock.patch.object(common.subprocess, "CREATE_NEW_PROCESS_GROUP", 0x200, create=True),
                        mock.patch.object(common.subprocess, "Popen",
                                          side_effect=PermissionError(13, "Access is denied"))])
        runner = Runner(Log(sinks=[]), threading.Event())
        with self.assertRaises(SetupError) as ctx:
            runner.run(["C:\\work\\tools\\cmake.exe", "--version"])
        self.assertIn("antivirus", str(ctx.exception))

    def test_vs_installer_codes(self):
        self.assertIn("restart", pc_check.vs_installer_result(3010))
        self.assertIn("canceled", pc_check.vs_installer_result(1602))
        self.assertIn("dd_", pc_check.vs_installer_result(12345))


class VisualStudioEnvironmentTests(Patched):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.vs = Path(self.tmp.name) / "BuildTools"
        vcvars = self.vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        vcvars.parent.mkdir(parents=True)
        vcvars.write_text("@echo off\r\n")

    @staticmethod
    def utf16(text: str) -> bytes:
        return text.replace("\n", "\r\n").encode("utf-16-le")

    def test_long_path_retry_keeps_the_rest_of_path(self):
        good = ("C3W-VCVARS-OK\nPath=C:\\VS\\VC\\Tools\\MSVC\\14.44\\bin\\HostX64\\x64;C:\\Windows\\System32\n"
                "INCLUDE=C:\\VS\\include\nLIB=C:\\VS\\lib\nWindowsSdkDir=C:\\Kits\\10\\\n")
        calls = []

        def fake_run(cmd, **kw):
            calls.append(kw["env"])
            if len(calls) == 1:   # the user's long PATH: vcvars64.bat fails
                return subprocess.CompletedProcess(cmd, 1, b"", b"")
            if len(calls) == 2:   # the second look, for its message
                return subprocess.CompletedProcess(cmd, 1, b"The input line is too long.\r\n", b"")
            return subprocess.CompletedProcess(cmd, 0, self.utf16(good), b"")
        self.patch_all(on_windows() + [mock.patch.object(deps.subprocess, "run", fake_run)])
        base = {"PATH": "C:\\Users\\user\\tools;C:\\Windows\\System32", "SYSTEMROOT": "C:\\Windows"}
        env, error = deps.vs_environment_checked(self.vs, base)
        self.assertEqual(error, "")
        self.assertNotIn("Path", env)   # one PATH, under the name it had
        parts = env["PATH"].split(";")
        self.assertEqual(parts[0], "C:\\VS\\VC\\Tools\\MSVC\\14.44\\bin\\HostX64\\x64")
        self.assertIn("C:\\Users\\user\\tools", parts)   # the user's PATH is kept behind
        self.assertEqual(parts.count("C:\\Windows\\System32"), 1)
        self.assertNotIn("user", calls[2]["PATH"])   # the retry ran with Windows' folders only
        self.assertEqual(deps.env_get(env, "windowssdkdir"), "C:\\Kits\\10\\")

    def test_failure_is_reported_not_reinstalled(self):
        def fake_run(cmd, **kw):
            if "/u" in cmd:
                return subprocess.CompletedProcess(cmd, 1, b"", b"")
            return subprocess.CompletedProcess(cmd, 1, b"ERROR: Cannot determine the location of the VS Common "
                                                         b"Tools folder.\r\n", b"")
        self.patch_all(on_windows() + [mock.patch.object(deps.subprocess, "run", fake_run)])
        env, error = deps.vs_environment_checked(self.vs, {"PATH": "C:\\Windows"})
        self.assertIn("VS Common Tools", error)
        tc = deps.Toolchain(vs_path=self.vs, vs_env_error=error, cc=Path("clang.exe"), cxx=Path("clang++.exe"),
                            clang_version=20, git=Path("git.exe"), cmake=Path("cmake.exe"), cmake_version=(3, 31),
                            ninja=Path("ninja.exe"), cargo=Path("cargo.exe"), rust_version=(1, 90))
        self.assertFalse(tc.vs_missing())
        self.assertTrue(any("vcvars64.bat" in p for p in tc.problems()))
        with mock.patch.object(common, "UNDER_WINE", False):
            items = deps.plan(tc, Path(self.tmp.name), True)
        self.assertNotIn("vs-buildtools", [i.key for i in items])   # repaired, not installed again
        notes = pc_check.tool_notes(tc)
        self.assertTrue(any("Repair" in n.fix for n in notes))


class ToolchainOnWindowsTests(Patched):
    def test_msvc_rust_without_visual_studio(self):
        self.patch_all(on_windows() + [mock.patch.object(common, "UNDER_WINE", False),
                                       mock.patch.object(deps, "_which", lambda *a, **k: None)])
        tc = deps.Toolchain(cargo=Path("cargo.exe"), rust_version=(1, 90), rust_host="x86_64-pc-windows-msvc")
        self.assertTrue(tc.needs_msvc_linker())
        self.assertTrue(any("link.exe" in p for p in tc.rust_problems()))
        with tempfile.TemporaryDirectory() as tmp:
            keys = [i.key for i in deps.plan(tc, Path(tmp), True, recomp=False)]
        self.assertIn("vs-buildtools", keys)
        tc.rust_host = "x86_64-pc-windows-gnu"
        self.assertFalse(tc.needs_msvc_linker())

    def test_old_git_is_replaced_by_mingit(self):
        self.patch_all(on_windows() + [mock.patch.object(common, "UNDER_WINE", False),
                                       mock.patch.object(deps, "_which", lambda *a, **k: None)])
        tc = deps.Toolchain(git=Path("git.exe"), git_version=(2, 20, 1))
        self.assertFalse(tc.git_ok())
        self.assertTrue(any(p.startswith("git >= 2.31") for p in tc.problems()))
        with tempfile.TemporaryDirectory() as tmp:
            keys = [i.key for i in deps.plan(tc, Path(tmp), True)]
        self.assertIn("git-win", keys)

    def test_every_problem_has_a_fix(self):
        self.patch_all(on_windows() + [mock.patch.object(common, "UNDER_WINE", False)])
        tc = deps.Toolchain()   # nothing installed
        notes = pc_check.tool_notes(tc)
        self.assertEqual(len(notes), len(tc.problems()))
        self.assertTrue(all(n.fix and n.detail for n in notes))
        self.assertTrue(any("visual-cpp-build-tools" in n.fix for n in notes))

    def test_rows_show_versions(self):
        tc = deps.Toolchain(git=Path("/usr/bin/git"), git_version=(2, 48, 1), ninja=Path("/usr/bin/ninja"),
                            ninja_version=(1, 12, 1), cargo=Path("/c/cargo"), rust_version=(1, 90),
                            rust_host="x86_64-pc-windows-msvc")
        rows = {name: detail for name, _, detail in tc.rows()}
        self.assertEqual(rows["Git"], "/usr/bin/git (2.48.1)")
        self.assertEqual(rows["Ninja"], "/usr/bin/ninja (1.12.1)")
        self.assertEqual(rows["Rust"], "/c/cargo (1.90, x86_64-pc-windows-msvc)")


class PathTests(Patched):
    def test_awkward_paths_on_windows(self):
        with mock.patch.object(common, "IS_WINDOWS", True):
            self.assertTrue(common.awkward_path("C:\\Users\\user\\Jos\u00e9\\COD3\\work"))
            self.assertTrue(common.awkward_path("C:\\Users\\user\\My Games\\COD3\\work"))
            self.assertTrue(common.awkward_path("C:\\" + "x" * 70))
            self.assertFalse(common.awkward_path("C:\\Games\\COD3\\work"))
        with mock.patch.object(common, "IS_WINDOWS", False):   # Linux: only spaces, as before
            self.assertFalse(common.awkward_path("/home/user/jos\u00e9/" + "x" * 80))
            self.assertTrue(common.awkward_path("/media/user/games and more/work"))

    def test_store_python(self):
        store = ("C:\\Users\\user\\AppData\\Local\\Microsoft\\WindowsApps\\"
                 "PythonSoftwareFoundation.Python.3.12_qbz5n2kfra8p0\\python.exe")
        with mock.patch.object(common, "IS_WINDOWS", True), mock.patch.object(common.sys, "executable", store):
            self.assertTrue(common.store_python())
            self.assertEqual(pc_check.python_kind(store, store), "Microsoft Store")
        with mock.patch.object(common, "IS_WINDOWS", True), \
                mock.patch.object(common.sys, "executable", "C:\\Python312\\python.exe"):
            self.assertFalse(common.store_python())

    def test_onedrive_and_long_work_folder_notes(self):
        with tempfile.TemporaryDirectory() as tmp:
            onedrive = Path(tmp) / "OneDrive"
            work = onedrive / "Documents" / "COD3 project" / "work"
            work.mkdir(parents=True)
            self.patch_all(on_windows() + [
                mock.patch.dict(os.environ, {"OneDrive": str(onedrive)}),
                mock.patch.object(pc_check, "long_paths_enabled", lambda: False),
                mock.patch.object(pc_check, "windows_proxy_set", lambda: True),
                mock.patch.object(common, "UNDER_WINE", False)])
            for var in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
                self.patch_all([mock.patch.dict(os.environ, {var: ""})])
            titles = [n.title for n in pc_check.system_notes(work)]
        self.assertTrue(any("work folder is inside OneDrive" in t for t in titles), titles)
        self.assertTrue(any("path has spaces" in t for t in titles), titles)
        self.assertTrue(any("long path support is off" in t for t in titles), titles)
        self.assertTrue(any("HTTPS_PROXY isn't set" in t for t in titles), titles)

    def test_controlled_folder_access_note(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "Documents" / "COD3" / "work"
            work.mkdir(parents=True)
            self.patch_all(on_windows() + [
                mock.patch.dict(os.environ, {"USERPROFILE": tmp}),
                mock.patch.object(pc_check, "controlled_folder_access", lambda: True),
                mock.patch.object(pc_check, "long_paths_enabled", lambda: True),
                mock.patch.object(pc_check, "windows_proxy_set", lambda: False),
                mock.patch.object(common, "UNDER_WINE", False)])
            titles = [n.title for n in pc_check.system_notes(work)]
            self.assertTrue(any(t.startswith("Controlled folder access is on, and the work folder") for t in titles),
                            titles)
            with mock.patch.object(pc_check, "controlled_folder_access", lambda: None):   # can't tell: quiet
                self.assertFalse(any("Controlled" in n.title for n in pc_check.system_notes(work)))

    def test_system_lines_name_the_versions(self):
        lines = pc_check.system_lines()
        self.assertTrue(lines[0].startswith(f"Setup tool {pipeline.TOOL_VERSION}, Python "))
        self.assertIn("CPUs", lines[1])


class PipelineHelpersTests(unittest.TestCase):
    def test_crlf_patch_gets_an_lf_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            lf = tmp / "0001-a.patch"
            lf.write_bytes(b"diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n")
            self.assertEqual(pipeline.lf_patch(lf, tmp / "lf"), lf)
            crlf = tmp / "0002-b.patch"
            crlf.write_bytes(lf.read_bytes().replace(b"\n", b"\r\n"))
            copy = pipeline.lf_patch(crlf, tmp / "lf")
            self.assertNotEqual(copy, crlf)
            self.assertEqual(copy.read_bytes(), lf.read_bytes())
            mixed = tmp / "0003-c.patch"   # one CRLF line in a file that changes a CRLF file: left alone
            mixed.write_bytes(b"diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\r\n+b\r\n")
            self.assertEqual(pipeline.lf_patch(mixed, tmp / "lf"), mixed)

    def test_codegen_runs_again_after_an_sdk_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            proj = Path(tmp)
            gen = proj / "generated"
            for module in ("default", "saint_lo"):
                (gen / module).mkdir(parents=True)
                (gen / module / "codegen.stamp").write_text("fingerprint")
                (gen / module / "code.cpp").write_text("// code")
            (gen / "default" / "codegen.build.stamp").write_text("x")
            old_sdk, new_sdk = {"patches": ["a"]}, {"patches": ["a", "0027"]}
            # No record yet (a work folder from before this check): translate again.
            self.assertTrue(pipeline.codegen_sdk_changed(proj, old_sdk))
            common.write_json(gen / pipeline.CODEGEN_SDK_STAMP, old_sdk)
            self.assertFalse(pipeline.codegen_sdk_changed(proj, old_sdk))
            self.assertTrue(pipeline.codegen_sdk_changed(proj, new_sdk))
            self.assertTrue(pipeline.forget_codegen(proj))
            self.assertEqual(sorted(gen.glob("*/codegen*stamp")), [])
            self.assertTrue((gen / "saint_lo" / "code.cpp").is_file())   # the code itself stays
            self.assertFalse(pipeline.forget_codegen(proj))

    def test_launcher_bat(self):
        bat = pipeline.launcher_bat(Path("C:/Py 100%/pythonw.exe"))
        self.assertTrue(bat.endswith("\r\n"))
        self.assertNotIn("\n", bat.replace("\r\n", ""))
        self.assertIn('set PYW="', bat)
        self.assertIn("100%%", bat)   # a % in the path is escaped for cmd
        self.assertIn(":no_python", bat)
        self.assertIn("where pyw", bat)
        self.assertNotIn("set PYW=\"", pipeline.launcher_bat(None))

    def test_visual_cpp_runtime_from_system32(self):
        with tempfile.TemporaryDirectory() as tmp:
            system32 = Path(tmp) / "System32"
            system32.mkdir()
            (system32 / "vcruntime140.dll").write_bytes(b"MZ")
            (system32 / "kernel32.dll").write_bytes(b"MZ")
            with mock.patch.dict(os.environ, {"SystemRoot": tmp}):
                found, still = binaries.windows_runtime_from_system(
                    [("cod3.exe", "VCRUNTIME140.dll"), ("cod3_x.dll", "vcruntime140.dll"),
                     ("cod3.exe", "libfoo.dll")])
        self.assertEqual([f.name for f in found], ["vcruntime140.dll"])
        self.assertEqual(still, [("cod3.exe", "libfoo.dll")])


class SetupBatTests(unittest.TestCase):
    def test_bat_files(self):
        inner = (TOOL / "setup.bat").read_text()
        top = (TOOL.parent / "setup.bat").read_text()
        for marker in ("--source winget", "--architecture x64", "sys.maxsize", ":not_extracted",
                       'if not exist "%~dp0cod3_waw_setup.py"', ":install_failed"):
            self.assertIn(marker, inner)
        self.assertIn(":not_extracted", top)
        # No parenthesized blocks: a ")" in a folder name would end them early.
        for text in (inner, top):
            for line in text.splitlines():
                stripped = line.strip()
                if stripped.lower().startswith(("echo", "rem", "set ")):
                    continue
                self.assertFalse(stripped.endswith("("), line)

    def test_sh_wants_64_bit_on_windows(self):
        self.assertIn("sys.maxsize < 2**32", (TOOL / "setup.sh").read_text())


if __name__ == "__main__":
    unittest.main()
