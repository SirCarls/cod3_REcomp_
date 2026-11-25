"""[acct2-bro] 2026-11-22 (owner 11-21 23:49 "Please fix the vulkan issue. It shouldn't be able to ship without the
native renderer."): every Call of Duty 3 the setup tool builds has the native renderer (ngl_vk). A Windows PC without
the Vulkan SDK has no glslc for its shaders, so the build takes Big Red One's (Khronos' glslang through glslc.bat,
a pinned download in the one approval list), and recomp/CMakeLists.txt stops (COD3_REQUIRE_NGL_VK) rather than leave
the renderer out, as it did quietly before (the user reports of 11-18 and 11-21). Nothing is downloaded, built or
run here.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import pipeline  # noqa: E402
from c3w.common import SetupError  # noqa: E402

GLSLANG_BAT = Path("C:/w/tools/bro-windows/glslc.bat")
# [rexglue-native-platform-fix] 2026-11-23: recomp/CMakeLists.txt hands ngl_vk's search, glslc and the stop to the
# project's platform layer (xplat_find_ngl_vk, next to the setup tool like ngl_vk/)
XPLAT_NGL_VK = TOOL.parent / "xplat" / "cmake" / "XplatNglVk.cmake"


class GlslcArgsTests(unittest.TestCase):
    def test_every_name_the_build_looks_it_up_by(self):
        self.assertEqual(pipeline.ngl_vk_glslc_args(Path("/t/glslc.bat")),
                         ["-DVulkan_GLSLC_EXECUTABLE=/t/glslc.bat", "-DNGL_VK_GLSLC=/t/glslc.bat",
                          "-DCOD3_NGL_VK_GLSLC=/t/glslc.bat"])
        self.assertEqual(pipeline.ngl_vk_glslc_args(None), [])

    def test_the_names_are_the_cmake_files_own(self):
        recomp = (TOOL / "recomp" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("xplat_find_ngl_vk(COD3 ", recomp)   # [rexglue-native-platform-fix] (was find_program here)
        module = XPLAT_NGL_VK.read_text(encoding="utf-8")
        self.assertIn("set(names NGL_VK_GLSLC)", module)          # taken as given, as before:
        self.assertIn("${ARG_PREFIX}_NGL_VK_GLSLC", module)       # NGL_VK_GLSLC and COD3_NGL_VK_GLSLC
        ngl = TOOL.parent / "ngl_vk" / "CMakeLists.txt"   # the project's, next to the setup tool
        if ngl.is_file():
            self.assertIn("find_program(NGL_VK_GLSLC ", ngl.read_text(encoding="utf-8"))

    def test_the_vulkan_sdks_glslc_off_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "Bin" / pipeline.exe_name("glslc")
            exe.parent.mkdir()
            exe.write_bytes(b"")
            with mock.patch.object(pipeline.shutil, "which", return_value=None), \
                    mock.patch.dict(os.environ, {"VULKAN_SDK": tmp}):
                self.assertEqual(pipeline.system_glslc(), exe)
            with mock.patch.object(pipeline.shutil, "which", return_value=None), mock.patch.dict(os.environ):
                os.environ.pop("VULKAN_SDK", None)
                self.assertIsNone(pipeline.system_glslc())
            with mock.patch.object(pipeline.shutil, "which", return_value="/usr/bin/glslc"):
                self.assertEqual(pipeline.system_glslc(), Path("/usr/bin/glslc"))


class PipelineTests(unittest.TestCase):
    """The Pipeline's own steps, on a stand-in without a build behind it."""

    def make(self, windows: bool, native: bool = True, cod3: bool = True):
        p = pipeline.Pipeline.__new__(pipeline.Pipeline)
        p.tools_dir = Path("/w/tools")
        p.log = mock.Mock()
        p.cancel = mock.Mock()
        patches = [mock.patch.object(pipeline, "IS_WINDOWS", windows),
                   mock.patch.object(pipeline, "IS_LINUX", not windows),
                   mock.patch.object(pipeline.Pipeline, "native", new_callable=mock.PropertyMock,
                                     return_value=native),
                   mock.patch.object(pipeline.Pipeline, "_builds_cod3", return_value=cod3)]
        for m in patches:
            m.start()
            self.addCleanup(m.stop)
        return p

    def test_windows_without_glslc_takes_glslang(self):
        p = self.make(windows=True)
        with mock.patch.object(pipeline, "system_glslc", return_value=None), \
                mock.patch("c3w.bro_windows.ensure_glslc", return_value=GLSLANG_BAT) as ensure:
            first = p._ngl_vk_args()
            again = p._ngl_vk_args()
        self.assertEqual(first, pipeline.ngl_vk_glslc_args(GLSLANG_BAT))
        self.assertEqual(again, first)
        ensure.assert_called_once()   # made once, then kept

    def test_windows_with_glslc_takes_its_own(self):
        p = self.make(windows=True)
        mine = Path("C:/VulkanSDK/1.4/Bin/glslc.exe")
        with mock.patch.object(pipeline, "system_glslc", return_value=mine), \
                mock.patch("c3w.bro_windows.ensure_glslc") as ensure:
            self.assertEqual(p._ngl_vk_args(), pipeline.ngl_vk_glslc_args(mine))
        ensure.assert_not_called()

    def test_linux_leaves_it_to_cmake(self):
        p = self.make(windows=False)
        with mock.patch("c3w.bro_windows.ensure_glslc") as ensure:
            self.assertEqual(p._ngl_vk_args(), [])
        ensure.assert_not_called()

    def test_glslang_is_in_the_approval_list_only_when_needed(self):
        with mock.patch.object(pipeline, "system_glslc", return_value=None), \
                mock.patch("c3w.bro_windows.have_download", return_value=False):
            self.assertTrue(self.make(windows=True)._ngl_vk_needs_glslang())
            self.assertFalse(self.make(windows=True, cod3=False)._ngl_vk_needs_glslang())   # a port's build
            self.assertFalse(self.make(windows=True, native=False)._ngl_vk_needs_glslang())
            self.assertFalse(self.make(windows=False)._ngl_vk_needs_glslang())
        with mock.patch.object(pipeline, "system_glslc", return_value=None), \
                mock.patch("c3w.bro_windows.have_download", return_value=True):
            self.assertFalse(self.make(windows=True)._ngl_vk_needs_glslang())   # downloaded before
        with mock.patch.object(pipeline, "system_glslc", return_value=Path("C:/VulkanSDK/Bin/glslc.exe")):
            self.assertFalse(self.make(windows=True)._ngl_vk_needs_glslang())

    def test_a_build_without_the_renderer_stops(self):
        p = self.make(windows=True)
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            p._check_ngl_vk(build)   # no cache yet: nothing to say
            (build / "CMakeCache.txt").write_text("COD3_NGL_VK_BUILT:INTERNAL=ON\n", encoding="utf-8")
            p._check_ngl_vk(build)
            (build / "CMakeCache.txt").write_text("COD3_NGL_VK_BUILT:INTERNAL=OFF\n", encoding="utf-8")
            with self.assertRaises(SetupError):
                p._check_ngl_vk(build)
            (build / "CMakeCache.txt").write_text("COD3_REQUIRE_NGL_VK:BOOL=OFF\nCOD3_NGL_VK_BUILT:INTERNAL=OFF\n",
                                                  encoding="utf-8")
            p._check_ngl_vk(build)   # asked for by hand


class CMakeTests(unittest.TestCase):
    def test_recomp_stops_without_the_renderer(self):
        text = (TOOL / "recomp" / "CMakeLists.txt").read_text(encoding="utf-8")
        m = re.search(r'option\(COD3_REQUIRE_NGL_VK "[^"]*" (ON|OFF)\)', text)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), "ON")
        # [rexglue-native-platform-fix] 2026-11-23: the stop and COD3_NGL_VK_BUILT are xplat_find_ngl_vk's
        # (${name} is "cod3", ${prefix} "COD3"); OFF here too for COD3_NGL_VK_WINDOWS=OFF
        self.assertIn("xplat_find_ngl_vk(COD3 TARGET cod3 DEFINE COD3_NGL_VK REQUIRE", text)
        self.assertIn("set(COD3_NGL_VK_BUILT OFF CACHE INTERNAL", text)
        module = XPLAT_NGL_VK.read_text(encoding="utf-8")
        self.assertIn('message(FATAL_ERROR "${name}: the native renderer (ngl_vk) can\'t be built', module)
        self.assertIn("set(${prefix}_NGL_VK_BUILT ON CACHE INTERNAL", module)
        self.assertIn("set(${prefix}_NGL_VK_BUILT OFF CACHE INTERNAL", module)


if __name__ == "__main__":
    unittest.main()
