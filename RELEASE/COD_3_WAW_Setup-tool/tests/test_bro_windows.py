"""Tests for Big Red One's Windows version (c3w/bro_windows.py, c3w/bro_glslc.py and their part of c3w/bro.py;
[acct2-ocr] 2026-11-12, owner 23:06 "Also the windows build needs to happen"): where it builds, the toolchain file,
the commands, glslc's stand-in (against the options ngl_vk/CMakeLists.txt really passes), the launcher, installing
both programs, the approval list, the import check. No Tkinter, no network, nothing built or downloaded.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import json
import os
import re
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import bro, bro_glslc, bro_windows, deps  # noqa: E402
from c3w.common import Cancelled, Log, SetupError  # noqa: E402

PROJECT = Path(__file__).resolve().parent.parent.parent


def _log():
    seen = []
    return Log(sinks=[lambda kind, text, frac: seen.append((kind, text))]), seen


class Recorder:
    """A Runner that runs nothing: it keeps the commands, and capture() answers from a function."""

    def __init__(self, answer=None):
        self.cmds = []
        self.answer = answer or (lambda cmd: (1, ""))

    def run(self, cmd, **kw):
        self.cmds.append([str(c) for c in cmd])
        return 0

    def capture(self, cmd, **kw):
        return self.answer([str(c) for c in cmd])


class WhereItBuildsTests(unittest.TestCase):
    def test_linux_and_64_bit_windows(self):
        with mock.patch.object(bro, "IS_LINUX", False), mock.patch.object(bro, "IS_WINDOWS", True):
            self.assertEqual(bro.supported(), (True, ""))
            with mock.patch.object(bro.sys, "maxsize", 2 ** 31 - 1):
                ok, why = bro.supported()
                self.assertFalse(ok)
                self.assertIn("64-bit", why)
        with mock.patch.object(bro, "IS_LINUX", False), mock.patch.object(bro, "IS_WINDOWS", False):
            self.assertFalse(bro.supported()[0])
        with mock.patch.object(bro, "IS_LINUX", True):
            self.assertEqual(bro.supported(), (True, ""))


class PinsTests(unittest.TestCase):
    def test_downloads_are_pinned(self):
        for key, (url, sub, sha) in bro_windows.DOWNLOADS.items():
            self.assertTrue(url.startswith("https://github.com/"), key)
            self.assertRegex(sha, r"^[0-9a-f]{64}$")
            self.assertIn(key, bro_windows.DOWNLOAD_INFO)
        # the same llvm-mingw release as the Linux one Call of Duty 3's Windows version uses
        linux = deps.PORTABLE[bro_windows.LLVM_MINGW_LINUX][0]
        release = re.search(r"/download/(\d+)/", linux).group(1)
        self.assertIn(f"/download/{release}/", bro_windows.DOWNLOADS["bro-llvm-mingw-win"][0])
        self.assertIn(release, bro_windows.DOWNLOADS["bro-llvm-mingw-win"][1])

    def test_sources_are_pinned_commits(self):
        self.assertEqual(set(bro_windows.SOURCES), {"sdl3", "vulkan-headers", "vulkan-loader", "imgui"})
        for name, (url, commit) in bro_windows.SOURCES.items():
            self.assertTrue(url.startswith("https://github.com/"), name)
            self.assertRegex(commit, r"^[0-9a-f]{40}$")
            self.assertIn(name, bro_windows.SOURCE_INFO)


class ToolchainTests(unittest.TestCase):
    def test_toolchain_file(self):
        text = bro_windows.toolchain_file_text(Path("/t/llvm-mingw/bin"), [Path("/t/sdl3")])
        self.assertIn("set(CMAKE_SYSTEM_NAME Windows)", text)
        self.assertIn('set(CMAKE_C_COMPILER "/t/llvm-mingw/bin/x86_64-w64-mingw32-clang")', text)
        self.assertIn('set(CMAKE_CXX_COMPILER "/t/llvm-mingw/bin/x86_64-w64-mingw32-clang++")', text)
        self.assertIn('"/t/llvm-mingw/x86_64-w64-mingw32" "/t/sdl3"', text)
        self.assertIn("CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER", text)   # glslc and Python: this system's
        with mock.patch.object(bro_windows, "IS_WINDOWS", True):
            text = bro_windows.toolchain_file_text(Path("/t/llvm-mingw/bin"))
            self.assertIn("x86_64-w64-mingw32-clang.exe", text)
            self.assertIn("x86_64-w64-mingw32-windres.exe", text)

    def test_generator(self):
        self.assertEqual(bro_windows.generator_args(Path("/n/ninja"))[:2], ["-G", "Ninja"])
        self.assertEqual(bro_windows.generator_args(None), [])
        with mock.patch.object(bro_windows, "IS_WINDOWS", True), self.assertRaises(SetupError):
            bro_windows.generator_args(None)

    def test_configure_command(self):
        libs = bro_windows.Libraries(Path("/s/include"), Path("/s/lib/libSDL3.a"), Path("/v/include"),
                                     Path("/w/libvulkan-1.a"), Path("/i/imgui"))
        cmd = bro_windows.configure_command(Path("cmake"), Path("/n/ninja"), Path("/p/bro"), Path("/w/bw"),
                                            Path("/t/tc.cmake"), Path("/w/xr"), Path("/w/gen"), libs,
                                            Path("/g/glslc"), "/py")
        self.assertEqual(cmd[cmd.index("-S") + 1], str(Path("/p/bro")))
        for arg in ("-DCMAKE_TOOLCHAIN_FILE=" + str(Path("/t/tc.cmake")), "-DBRO_SDL3_INCLUDE_DIR=/s/include",
                    "-DBRO_SDL3_LIBRARY=/s/lib/libSDL3.a", "-DNGL_VK_VULKAN_INCLUDE_DIR=/v/include",
                    "-DVulkan_LIBRARY=/w/libvulkan-1.a", "-DBRO_IMGUI_DIR=/i/imgui", "-DPython3_EXECUTABLE=/py",
                    "-DCMAKE_EXE_LINKER_FLAGS=-static", "-DBRO_GLSLC=/g/glslc", "-DNGL_VK_GLSLC=/g/glslc",
                    "-DVulkan_GLSLC_EXECUTABLE=/g/glslc", "-DBRO_GEN_DIR=" + str(Path("/w/gen"))):
            self.assertIn(arg, cmd)

    def test_int_conversion_stays_a_warning(self):
        """XBOX_PTR (an integer) given to memcpy: an error in clang 16+ and GCC 14+, a warning in GCC 13. Both builds
        keep it a warning (found 2026-11-13 by the end-to-end build: llvm-mingw's clang 23 stopped)."""
        flag = "-DCMAKE_C_FLAGS=-Wno-error=int-conversion"
        libs = bro_windows.Libraries(Path("s"), Path("s.a"), Path("v"), Path("v.a"), Path("i"))
        self.assertIn(flag, bro_windows.configure_command(Path("cmake"), None, Path("p"), Path("b"), Path("t"),
                                                          Path("x"), Path("g"), libs, Path("glslc"), "py"))
        lay = bro.Layout(repo=Path("r"), out=Path("o"), work=Path("w"), tools=Path("t"))
        self.assertIn(flag, bro.configure_command(lay, bro.Tools(cmake=Path("cmake"))))

    def test_sdl3_is_static(self):
        cmd = bro_windows.sdl3_command(Path("cmake"), Path("/s"), Path("/b"), Path("/i"), Path("/tc"), None)
        self.assertIn("-DSDL_SHARED=OFF", cmd)
        self.assertIn("-DSDL_STATIC=ON", cmd)
        self.assertIn("-DCMAKE_INSTALL_PREFIX=" + str(Path("/i")), cmd)

    def test_the_glslc_names_bro_and_ngl_vk_look_up(self):
        """configure_command gives glslc under the names the two CMake files ask for."""
        names = set(re.findall(r"find_program\((\w+) NAMES glslc",
                               (PROJECT / "bro" / "CMakeLists.txt").read_text(encoding="utf-8")
                               + (PROJECT / "ngl_vk" / "CMakeLists.txt").read_text(encoding="utf-8")))
        self.assertTrue(names)
        libs = bro_windows.Libraries(Path("a"), Path("b"), Path("c"), Path("d"), Path("e"))
        cmd = bro_windows.configure_command(Path("cmake"), None, Path("p"), Path("b"), Path("t"), Path("x"),
                                            Path("g"), libs, Path("/g/glslc"), "py")
        for name in names:
            self.assertIn(f"-D{name}=/g/glslc", cmd)


class GlslcTests(unittest.TestCase):
    def ngl_vk_calls(self):
        """Every glslc command line in ngl_vk/CMakeLists.txt, as CMake gives it (variables as placeholders)."""
        text = (PROJECT / "ngl_vk" / "CMakeLists.txt").read_text(encoding="utf-8")
        calls = []
        for m in re.finditer(r'COMMAND "\$\{NGL_VK_GLSLC\}"(.*?)(?:\n\s*DEPENDS|\n\s*COMMENT)', text, re.S):
            body = re.sub(r"#[^\n]*", "", m.group(1))
            calls.append([t.strip('"') for t in body.split()])
        return calls

    def test_every_call_ngl_vk_makes_is_understood(self):
        calls = self.ngl_vk_calls()
        self.assertGreaterEqual(len(calls), 4)
        for args in calls:
            o = bro_glslc.parse(["--glslang", "glslang", *args])
            self.assertTrue(o.output and o.source)

    def test_parse_and_glslang_command(self):
        o = bro_glslc.parse(["--glslang", "/g/glslang", "-mfmt=c", "-DNGL_VK_FOUR_SETS", "-fshader-stage=frag",
                             "-o", "out.inc", "mesh.frag"])
        self.assertEqual((o.output, o.source, o.stage, o.defines), ("out.inc", "mesh.frag", "frag",
                                                                     ["NGL_VK_FOUR_SETS"]))
        cmd = bro_glslc.glslang_command(o, "x.spv")
        self.assertEqual(cmd[:4], ["/g/glslang", "-V", "--target-env", "vulkan1.0"])
        self.assertIn("-DNGL_VK_FOUR_SETS", cmd)
        self.assertEqual(cmd[cmd.index("-S") + 1], "frag")
        self.assertEqual(cmd[-3:], ["-o", "x.spv", "mesh.frag"])
        plain = bro_glslc.glslang_command(bro_glslc.parse(["--glslang", "g", "-mfmt=c", "-o", "a", "q.vert"]), "s")
        self.assertNotIn("-S", plain)

    def test_what_it_refuses(self):
        for args in (["-mfmt=c", "-o", "a", "b"],                       # no glslang
                     ["--glslang", "g", "-o", "a", "b"],                # not -mfmt=c
                     ["--glslang", "g", "-mfmt=c", "-O", "-o", "a", "b"],
                     ["--glslang", "g", "-mfmt=c", "-o", "a", "b", "c"],
                     ["--glslang", "g", "-mfmt=c", "-fshader-stage=nope", "-o", "a", "b"]):
            with self.assertRaises(bro_glslc.UsageError, msg=args):
                bro_glslc.parse(args)
        self.assertEqual(bro_glslc.main(["-o"]), 2)

    def test_glslcs_text(self):
        words = [0x07230203, 0x00010000, 0x0008000B, 0x2E, 0, 0x00020011]
        self.assertEqual(bro_glslc.c_text(words),
                         "{0x07230203,0x00010000,0x0008000b,0x0000002e,\n0x00000000,0x00020011}\n")
        data = struct.pack("<6I", *words)
        self.assertEqual(bro_glslc.spirv_words(data), words)
        with self.assertRaises(ValueError):
            bro_glslc.spirv_words(b"\0" * 24)
        with self.assertRaises(ValueError):
            bro_glslc.spirv_words(data[:-1])

    @unittest.skipIf(os.name == "nt", "a stand-in glslang as a script needs a #! line")
    def test_the_whole_way_with_a_stand_in_glslang(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            fake = tmp / "glslang"
            fake.write_text(f"#!{sys.executable}\nimport struct, sys\na = sys.argv\n"
                            "open(a[a.index('-o') + 1], 'wb').write(struct.pack('<5I', 0x07230203, 0x10000, "
                            "0x8000b, 1, 0))\n", encoding="utf-8")
            fake.chmod(0o755)
            (tmp / "q.vert").write_text("#version 450\nvoid main() {}\n", encoding="utf-8")
            out = tmp / "q.vert.inc"
            rc = bro_glslc.main(["--glslang", str(fake), "-mfmt=c", "-o", str(out), str(tmp / "q.vert")])
            self.assertEqual(rc, 0)
            self.assertEqual(out.read_text(), "{0x07230203,0x00010000,0x0008000b,0x00000001,\n0x00000000}\n")
            self.assertEqual(sorted(p.name for p in tmp.iterdir()), ["glslang", "q.vert", "q.vert.inc"])
            fake.write_text(f"#!{sys.executable}\nimport sys\nsys.exit(3)\n", encoding="utf-8")
            self.assertEqual(bro_glslc.main(["--glslang", str(fake), "-mfmt=c", "-o", str(out),
                                             str(tmp / "q.vert")]), 3)

    def test_the_bat_that_stands_for_glslc(self):
        text = bro_windows.glslc_wrapper_text(r"C:\Py\python.exe", Path(r"C:\t\glslang.exe"))
        self.assertTrue(text.startswith('@"C:\\Py\\python.exe" -I "'))
        self.assertIn("bro_glslc.py", text)
        self.assertIn('--glslang "C:\\t\\glslang.exe" %*', text)
        self.assertTrue(text.endswith("\r\n"))

    def test_glslc_is_needed_on_linux(self):
        log, _ = _log()
        with mock.patch.object(bro_windows, "IS_WINDOWS", False), self.assertRaises(SetupError):
            bro_windows.ensure_glslc(Path("/t"), None, log, threading.Event())
        self.assertEqual(bro_windows.ensure_glslc(Path("/t"), Path("/usr/bin/glslc"), log, threading.Event()),
                         Path("/usr/bin/glslc"))

    def test_glslang_in_its_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(bro_windows.find_glslang(Path(tmp)))
            exe = Path(tmp) / "glslang-16" / "bin" / "glslang.exe"
            exe.parent.mkdir(parents=True)
            exe.write_bytes(b"MZ")
            self.assertEqual(bro_windows.find_glslang(Path(tmp)), exe)


class ImportsTests(unittest.TestCase):
    OBJDUMP = ("\nThe Import Tables:\n  lookup 000a0 time stamp 00000000\n\n    DLL Name: KERNEL32.dll\n"
               "    vma:  Hint/Ord Member-Name Bound-To\n    DLL Name: vulkan-1.dll\n    DLL Name: bcrypt.dll\n"
               "    DLL Name: api-ms-win-crt-runtime-l1-1-0.dll\n")

    def test_windows_own_and_the_vulkan_loader(self):
        r = Recorder(lambda cmd: (0, self.OBJDUMP))
        names = bro_windows.check_imports(Path("bro.exe"), Path("/t/bin"), r)
        self.assertEqual(names, ["KERNEL32.dll", "vulkan-1.dll", "bcrypt.dll", "api-ms-win-crt-runtime-l1-1-0.dll"])
        self.assertTrue(r.answer(["x"])[0] == 0)

    def test_anything_else_stops_the_build(self):
        r = Recorder(lambda cmd: (0, self.OBJDUMP + "    DLL Name: libc++.dll\n"))
        with self.assertRaises(SetupError) as e:
            bro_windows.check_imports(Path("bro.exe"), Path("/t/bin"), r)
        self.assertIn("libc++.dll", str(e.exception))
        with self.assertRaises(SetupError):
            bro_windows.check_imports(Path("bro.exe"), Path("/t/bin"), Recorder(lambda cmd: (1, "no such file")))

    def test_the_cache_says_what_was_not_found(self):
        self.assertEqual(bro_windows.cache_problems("BRO_GLSLC:FILEPATH=/g/glslc\n"), [])
        self.assertEqual(bro_windows.cache_problems("BRO_GLSLC:FILEPATH=BRO_GLSLC-NOTFOUND\n"
                                                    "Vulkan_INCLUDE_DIR:PATH=Vulkan_INCLUDE_DIR-NOTFOUND\n"),
                         ["glslc", "the Vulkan headers"])


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "out"
        self.lay = bro.layout(self.out, Path(self.tmp.name) / "work")
        self.linux = Path(self.tmp.name) / "bro"
        self.linux.write_bytes(b"\x7fELF program")
        self.win = Path(self.tmp.name) / "bro.exe"
        self.win.write_bytes(b"MZ program")
        self.disc = bro.Disc("folder", Path(self.tmp.name), "abc", bro.XbeInfo(1, "t", 1), 1, 1)

    def tearDown(self):
        self.tmp.cleanup()

    @unittest.skipIf(os.name == "nt", "the Linux program beside the Windows one is made on Linux")
    def test_both_programs_on_linux(self):
        bro.install(self.lay, self.linux, self.disc, "9.9.9", exe_windows=self.win)
        m = json.loads(self.lay.manifest.read_text())
        self.assertEqual((m["exe_linux"], m["exe_windows"]), ("bin/bro", "bin/bro.exe"))
        self.assertEqual(self.lay.exe_windows.read_bytes(), b"MZ program")
        self.assertEqual(bro.installed(self.out), self.lay.exe)
        bat = self.lay.launcher_windows.read_bytes()
        self.assertIn(b'"%here%bin\\bro.exe" "%here%game" "%here%saves" %*\r\n', bat)
        self.assertNotIn(b"\n", bat.replace(b"\r\n", b""))   # Windows line ends only
        with mock.patch.object(bro, "IS_WINDOWS", True):
            self.assertEqual(bro.installed(self.out), self.lay.exe_windows)

    def test_windows_only(self):
        with mock.patch.object(bro, "IS_WINDOWS", True):
            bro.install(self.lay, None, self.disc, "9.9.9", exe_windows=self.win)
            m = json.loads(self.lay.manifest.read_text())
            self.assertNotIn("exe_linux", m)
            self.assertEqual(m["exe_windows"], "bin/bro.exe")
            self.assertEqual(bro.installed(self.out), self.lay.exe_windows)
            self.assertFalse((self.lay.out / "play.sh").exists())
            self.assertTrue(self.lay.launcher_windows.is_file())

    def test_the_launcher(self):
        text = bro_windows.LAUNCHER_BAT_TEXT
        self.assertTrue(text.startswith("@echo off\r\n"))
        self.assertIn('if not defined BRO_VK set "BRO_VK=1"', text)
        self.assertIn('set "here=%~dp0"', text)


class ApprovalTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.lay = bro.layout(Path(self.tmp.name) / "out", Path(self.tmp.name) / "work")
        self.ready = bro.Tools(git=Path("git"), cmake=Path("cmake"), cmake_version=(3, 28), cc=Path("cc"),
                               glslc=Path("glslc"), pkgconfig=Path("pkg-config"), system_sdl3=True)

    def tearDown(self):
        self.tmp.cleanup()

    def test_linux_with_the_windows_version(self):
        r = Recorder()   # nothing there: no git answers
        with mock.patch.object(bro_windows, "IS_WINDOWS", False), mock.patch.object(bro, "IS_WINDOWS", False):
            keys = [i.key for i in bro.downloads(self.lay, self.ready, r, True, windows=True)]
            self.assertEqual(keys, ["xboxrecomp", "bro-capstone", bro_windows.LLVM_MINGW_LINUX, "bro-src-sdl3",
                                    "bro-src-vulkan-headers", "bro-src-vulkan-loader", "bro-src-imgui"])
            self.assertEqual([i.key for i in bro.downloads(self.lay, self.ready, r, True)],
                             ["xboxrecomp", "bro-capstone"])

    def test_call_of_duty_3s_sdk_has_the_sources(self):
        commits = {str(bro_windows.sdk_thirdparty(self.lay.tools) / n): c for n, (_, c) in bro_windows.SOURCES.items()}
        for folder in commits:
            (Path(folder) / ".git").mkdir(parents=True)

        def answer(cmd):
            folder = cmd[cmd.index("-C") + 1]
            if "rev-parse" in cmd:
                return (0, commits[folder] + "\n") if folder in commits else (1, "")
            return 0, ""   # status --porcelain: clean
        r = Recorder(answer)
        with mock.patch.object(bro_windows, "IS_WINDOWS", False):
            keys = [i.key for i in bro_windows.downloads(self.lay.tools, Path("git"), Path("glslc"), r)]
        self.assertEqual(keys, [bro_windows.LLVM_MINGW_LINUX])
        # a changed checkout doesn't count
        r = Recorder(lambda cmd: answer(cmd) if "rev-parse" in cmd else (0, " M src/x.c\n"))
        with mock.patch.object(bro_windows, "IS_WINDOWS", False):
            keys = [i.key for i in bro_windows.downloads(self.lay.tools, Path("git"), Path("glslc"), r)]
        self.assertIn("bro-src-sdl3", keys)

    def test_windows(self):
        r = Recorder()
        with mock.patch.object(bro_windows, "IS_WINDOWS", True):
            keys = [i.key for i in bro_windows.downloads(self.lay.tools, None, None, r)]
            self.assertEqual(keys[:2], ["bro-llvm-mingw-win", "bro-glslang-win"])
            keys = [i.key for i in bro_windows.downloads(self.lay.tools, None, Path("glslc.exe"), r)]
            self.assertNotIn("bro-glslang-win", keys)
            host = bro_windows.HostTools()
            self.assertEqual([i.key for i in bro_windows.host_downloads(host, self.lay.tools)],
                             ["git-win", "cmake-win", "ninja-win"])
            log, _ = _log()
            with self.assertRaises(SetupError) as e:
                bro_windows.ensure_host_tools(host, self.lay.tools, False, r, log, threading.Event())
            self.assertIn("Git, CMake, Ninja", str(e.exception))

    def test_a_download_says_where_it_goes(self):
        item = bro_windows.download_install("bro-llvm-mingw-win", self.lay.tools)
        self.assertEqual(item.where, str(self.lay.tools / "bro-llvm-mingw-win"))
        self.assertEqual(item.source, "github.com")
        src = bro_windows.source_install("sdl3", self.lay.tools)
        self.assertIn(bro_windows.SOURCES["sdl3"][1][:12], src.what)


class BuildStepTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.lay = bro.layout(Path(self.tmp.name) / "out", Path(self.tmp.name) / "work")
        self.tools = bro.Tools(git=Path("git"), cmake=Path("cmake"))
        self.opts = bro.BuildOptions(source="x", output_dir="o", work_dir="w", windows=True)

    def tearDown(self):
        self.tmp.cleanup()

    def test_on_linux_a_failure_is_a_warning(self):
        log, seen = _log()
        with mock.patch.object(bro.bro_windows, "build", side_effect=SetupError("no compiler")), \
                mock.patch.object(bro, "IS_WINDOWS", False):
            self.assertIsNone(bro._build_windows(self.opts, self.lay, self.tools, Recorder(), log, threading.Event()))
        self.assertTrue(any(kind == "warn" and "no compiler" in text for kind, text in seen))

    def test_on_windows_it_stops_the_build(self):
        log, _ = _log()
        with mock.patch.object(bro.bro_windows, "build", side_effect=SetupError("no compiler")), \
                mock.patch.object(bro, "IS_WINDOWS", True), self.assertRaises(SetupError):
            bro._build_windows(self.opts, self.lay, self.tools, Recorder(), log, threading.Event())

    def test_a_cancel_is_a_cancel(self):
        log, _ = _log()
        with mock.patch.object(bro.bro_windows, "build", side_effect=Cancelled()), \
                mock.patch.object(bro, "IS_WINDOWS", False), self.assertRaises(Cancelled):
            bro._build_windows(self.opts, self.lay, self.tools, Recorder(), log, threading.Event())

    def test_repository_line_ends(self):
        """xboxrecomp's copy keeps LF whatever Git does on Windows, so bro/patches apply."""
        log, _ = _log()
        r = Recorder()
        bro.prepare_xboxrecomp(self.lay, Path("git"), r, log)
        self.assertTrue(r.cmds)
        for cmd in r.cmds:
            self.assertIn("core.autocrlf=false", cmd)

    def test_the_options_default(self):
        self.assertFalse(bro.BuildOptions(source="", output_dir="", work_dir="").windows)


if __name__ == "__main__":
    unittest.main()
