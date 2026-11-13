"""[rexglue-native-settings] 2026-11-11 (coordinator 08:30 / 08:54, dependency audit, P1 and P5): what the native
renderer (ngl_vk) needs on Linux besides the build: glslc, the compiler of its shaders (ngl_vk/shaders), and the
Vulkan headers (vulkan/vulkan.h, found where CMake's FindVulkan looks). Both are optional: without them the build
leaves the native renderer out (recomp/CMakeLists.txt), so they are rows marked optional and one warning note, never
a problem that stops a build. The build packages ("libs") bring glslc (P1). Nothing is installed or run here.

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import stat
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOL))

from c3w import common, deps, pc_check  # noqa: E402
from c3w.common import Log, Runner  # noqa: E402

# Each distro's package with glslc in it (P1).
GLSLC_PACKAGE = {"apt": "glslc", "dnf": "glslc", "pacman": "shaderc", "zypper": "shaderc", "xbps": "shaderc"}


def linux():
    return [mock.patch.object(common, "IS_WINDOWS", False), mock.patch.object(common, "IS_LINUX", True),
            mock.patch.object(deps, "IS_WINDOWS", False), mock.patch.object(deps, "IS_LINUX", True)]


def windows():
    return [mock.patch.object(common, "IS_WINDOWS", True), mock.patch.object(common, "IS_LINUX", False),
            mock.patch.object(deps, "IS_WINDOWS", True), mock.patch.object(deps, "IS_LINUX", False)]


def make_header(include: Path) -> Path:
    (include / "vulkan").mkdir(parents=True)
    (include / "vulkan" / "vulkan.h").write_text("/* stand-in */\n")
    return include


class GlslcTests(unittest.TestCase):
    def patched(self, patches):
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    @staticmethod
    def renderer_notes(notes):
        return [n for n in notes if "native renderer" in n.title]

    def test_linux_rows_are_optional_and_name_the_renderer(self):
        self.patched(linux())
        rows = {name: (found, detail) for name, found, detail in deps.Toolchain().rows()}
        for name in ("glslc (optional)", "Vulkan headers (optional)"):   # "optional": shown as "Not installed"
            self.assertIn(name, rows)
            self.assertFalse(rows[name][0])
            self.assertIn("native renderer", rows[name][1])
        rows = {name: (found, detail) for name, found, detail in
                deps.Toolchain(glslc=Path("/usr/bin/glslc"), vulkan_headers=Path("/usr/include")).rows()}
        self.assertEqual(rows["glslc (optional)"], (True, "/usr/bin/glslc"))
        self.assertEqual(rows["Vulkan headers (optional)"], (True, "/usr/include"))

    def test_not_a_problem(self):
        self.patched(linux())
        tc = deps.Toolchain(cargo=Path("/usr/bin/cargo"), rust_version=(1, 90))
        self.assertFalse(any("glslc" in p or "Vulkan headers" in p for p in tc.problems(recomp=True)))

    def test_one_warning_naming_what_is_missing(self):
        self.patched(linux())
        both = self.renderer_notes(pc_check.tool_notes(deps.Toolchain(), recomp=True))
        self.assertEqual(len(both), 1)
        self.assertEqual(both[0].level, pc_check.WARN)
        self.assertIn("glslc and the Vulkan headers", both[0].title)
        self.assertIn("shaderc", both[0].fix)
        self.assertIn("libvulkan-dev", both[0].fix)
        headers = self.renderer_notes(pc_check.tool_notes(deps.Toolchain(glslc=Path("/usr/bin/glslc")), recomp=True))
        self.assertEqual(len(headers), 1)
        self.assertNotIn("glslc", headers[0].title)
        self.assertNotIn("shaderc", headers[0].fix)
        glslc = self.renderer_notes(pc_check.tool_notes(deps.Toolchain(vulkan_headers=Path("/usr/include")),
                                                        recomp=True))
        self.assertEqual(len(glslc), 1)
        self.assertNotIn("Vulkan headers", glslc[0].title)
        self.assertNotIn("libvulkan-dev", glslc[0].fix)
        full = deps.Toolchain(glslc=Path("/usr/bin/glslc"), vulkan_headers=Path("/usr/include"))
        self.assertEqual(self.renderer_notes(pc_check.tool_notes(full, recomp=True)), [])
        self.assertEqual(self.renderer_notes(pc_check.tool_notes(deps.Toolchain(), recomp=False)), [])

    def test_windows_has_neither(self):
        self.patched(windows())
        tc = deps.Toolchain()
        names = [r[0] for r in tc.rows()]
        self.assertNotIn("glslc (optional)", names)
        self.assertNotIn("Vulkan headers (optional)", names)
        self.assertEqual(self.renderer_notes(pc_check.tool_notes(tc, recomp=True)), [])

    def test_build_packages_bring_glslc(self):
        self.assertEqual(set(GLSLC_PACKAGE), set(deps.LINUX_PACKAGES))
        for pm, name in GLSLC_PACKAGE.items():
            self.assertIn(name, deps.LINUX_PACKAGES[pm]["libs"], pm)
            self.assertIn(name, deps.linux_packages(pm, minimal=False, windows_exe=False), pm)
            self.assertNotIn(name, deps.linux_packages(pm, minimal=True, windows_exe=False), pm)   # ripper only

    def test_headers_found_where_cmake_looks(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp) / "sdk"
            system = make_header(Path(tmp) / "usr" / "include")
            with mock.patch.object(deps, "VULKAN_INCLUDE_DIRS", [str(system)]):
                self.assertEqual(deps.vulkan_headers({}), system)
                make_header(sdk / "include")
                self.assertEqual(deps.vulkan_headers({"VULKAN_SDK": str(sdk)}), sdk / "include")   # the SDK first
            with mock.patch.object(deps, "VULKAN_INCLUDE_DIRS", [str(Path(tmp) / "nowhere")]):
                self.assertIsNone(deps.vulkan_headers({}))

    @unittest.skipUnless(sys.platform.startswith("linux"), "makes a stand-in glslc program")
    def test_detect_finds_both_in_a_vulkan_sdk(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp) / "sdk"
            (sdk / "bin").mkdir(parents=True)
            fake = sdk / "bin" / "glslc"
            fake.write_text("#!/bin/sh\nexit 0\n")
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            make_header(sdk / "include")
            empty = Path(tmp) / "empty"
            empty.mkdir()
            self.patched(linux() + [mock.patch.dict(os.environ, {"PATH": str(empty), "VULKAN_SDK": str(sdk)}),
                                    mock.patch.object(deps, "VULKAN_INCLUDE_DIRS", []),
                                    mock.patch.object(deps, "find_cargo", lambda tools_dir=None: None),
                                    mock.patch.object(deps, "_pkgconfig_missing", lambda runner, env: [])])
            tc = deps.detect(Path(tmp) / "tools", Runner(Log(sinks=[]), threading.Event()))
            self.assertEqual(tc.glslc, fake)
            self.assertEqual(tc.vulkan_headers, sdk / "include")


if __name__ == "__main__":
    unittest.main()
