"""Tests for packing the Windows game: which DLLs go next to cod3.exe.

The import tables are made up (binaries.needed is replaced), so the tests
run anywhere without a Windows build. They check what went wrong once for
real: the GPU plugin is loaded by name, so it is in no import table, and a
MinGW build names it librexgpu-xenos.dll (Visual Studio: rexgpu-xenos.dll).

    python -m unittest discover -s COD_3_WAW_Setup-tool/tests
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from c3w import binaries, cross  # noqa: E402

# A small Windows program header is enough for gather() to tell the kind.
PE_HEAD = b"MZ" + bytes(62)
ELF_HEAD = b"\x7fELF\x02\x01" + bytes(58)


def make(folder: Path, *names: str, head: bytes = PE_HEAD) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    for name in names:
        (folder / name).write_bytes(head)
    return folder


class FakeImports:
    """binaries.needed() replaced by a table: file name -> imported names."""

    def __init__(self, table):
        self.table = {k.lower(): v for k, v in table.items()}

    def __call__(self, path: Path):
        return list(self.table.get(Path(path).name.lower(), []))


class GatherWindowsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        # What the cross build leaves behind: the game's folder, the SDK's
        # bin folder and llvm-mingw's runtime folder.
        self.build = make(root / "build", "cod3.exe", "libcod3_blkbrn.dll", "librexgpu-xenos.dll",
                          "librexruntime.dll")
        self.sdk_bin = make(root / "sdk" / "bin", "librexgpu-xenos.dll", "librexruntime.dll", "libTracyClient.dll")
        self.runtime = make(root / "mingw" / "bin", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll")
        self.imports = FakeImports({
            "cod3.exe": ["KERNEL32.dll", "USER32.dll", "SHELL32.dll", "librexruntime.dll", "libc++.dll",
                         "api-ms-win-crt-runtime-l1-1-0.dll"],
            "libcod3_blkbrn.dll": ["KERNEL32.dll", "librexruntime.dll", "libc++.dll"],
            "librexruntime.dll": ["KERNEL32.dll", "dxgi.dll", "WS2_32.dll", "libc++.dll", "libunwind.dll",
                                  "api-ms-win-crt-heap-l1-1-0.dll"],
            "librexgpu-xenos.dll": ["KERNEL32.dll", "librexruntime.dll", "libc++.dll", "libunwind.dll", "ole32.dll"],
            "libc++.dll": ["KERNEL32.dll", "libunwind.dll", "api-ms-win-crt-string-l1-1-0.dll"],
            "libunwind.dll": ["KERNEL32.dll"],
        })
        self.search = [self.build, self.sdk_bin, self.runtime]

    def tearDown(self):
        self.tmp.cleanup()

    def names(self, files):
        return sorted(f.name for f in files)

    def test_follows_imports_and_skips_windows_own_dlls(self):
        start = [self.build / "cod3.exe", self.build / "libcod3_blkbrn.dll"]
        with mock.patch.object(binaries, "needed", self.imports):
            files, missing = binaries.gather(start, self.search, cross.is_windows_system_dll)
        self.assertEqual(missing, [])
        self.assertEqual(self.names(files), ["cod3.exe", "libc++.dll", "libcod3_blkbrn.dll", "librexruntime.dll",
                                             "libunwind.dll"])
        # Taken from the first folder that has it (the build's own copy).
        self.assertEqual([f for f in files if f.name == "librexruntime.dll"][0].parent, self.build)
        # Only what is imported is packed, not everything the folders hold.
        self.assertNotIn("libwinpthread-1.dll", self.names(files))
        self.assertNotIn("libTracyClient.dll", self.names(files))

    def test_windows_names_match_regardless_of_case(self):
        start = [self.build / "cod3.exe"]
        imports = FakeImports({"cod3.exe": ["LIBREXRUNTIME.dll", "LibC++.DLL"],
                               "librexruntime.dll": ["libunwind.dll"]})
        with mock.patch.object(binaries, "needed", imports):
            files, missing = binaries.gather(start, self.search, cross.is_windows_system_dll)
        self.assertEqual(missing, [])
        self.assertEqual(self.names(files), ["cod3.exe", "libc++.dll", "librexruntime.dll", "libunwind.dll"])

    def test_a_dll_from_nowhere_is_reported_with_who_needs_it(self):
        start = [self.build / "cod3.exe"]
        imports = FakeImports({"cod3.exe": ["librexruntime.dll"],
                               "librexruntime.dll": ["KERNEL32.dll", "libamd_fidelityfx_vk.dll", "d3d12.dll"]})
        with mock.patch.object(binaries, "needed", imports):
            files, missing = binaries.gather(start, self.search, cross.is_windows_system_dll)
        self.assertEqual(missing, [("librexruntime.dll", "libamd_fidelityfx_vk.dll")])

    def test_gpu_plugin_is_not_an_import_so_it_is_looked_up(self):
        # Nothing imports the plugin: gather alone leaves it out.
        start = [self.build / "cod3.exe"]
        with mock.patch.object(binaries, "needed", self.imports):
            files, _ = binaries.gather(start, self.search, cross.is_windows_system_dll)
        self.assertNotIn("librexgpu-xenos.dll", self.names(files))
        # The explicit lookup finds it under either Windows name, and its
        # own imports are then followed.
        plugin = binaries.find_gpu_plugin("xenos", self.search, windows=True)
        self.assertEqual(plugin, self.build / "librexgpu-xenos.dll")
        with mock.patch.object(binaries, "needed", self.imports):
            files, missing = binaries.gather([start[0], plugin], self.search, cross.is_windows_system_dll)
        self.assertEqual(missing, [])
        self.assertIn("librexgpu-xenos.dll", self.names(files))
        self.assertIn("libunwind.dll", self.names(files))

    def test_gpu_plugin_names_for_each_build(self):
        self.assertEqual(binaries.gpu_plugin_names("xenos", windows=True),
                         ["rexgpu-xenos.dll", "librexgpu-xenos.dll"])
        self.assertEqual(binaries.gpu_plugin_names("xenos", windows=False), ["librexgpu-xenos.so"])
        msvc = make(Path(self.tmp.name) / "msvc", "rexgpu-xenos.dll")
        self.assertEqual(binaries.find_gpu_plugin("xenos", [msvc], windows=True), msvc / "rexgpu-xenos.dll")
        self.assertIsNone(binaries.find_gpu_plugin("xenos", [self.runtime], windows=True))
        # The SDK's install folder serves when the build has no copy.
        self.assertEqual(binaries.find_gpu_plugin("xenos", [self.runtime, self.sdk_bin], windows=True),
                         self.sdk_bin / "librexgpu-xenos.dll")

    def test_linux_names_are_matched_exactly(self):
        root = Path(self.tmp.name)
        build = make(root / "linux", "cod3", "librexruntime.so", head=ELF_HEAD)
        imports = FakeImports({"cod3": ["LIBREXRUNTIME.so", "libc.so.6"]})
        with mock.patch.object(binaries, "needed", imports):
            files, missing = binaries.gather([build / "cod3"], [build], {"libc.so.6"}.__contains__)
        self.assertEqual(self.names(files), ["cod3"])
        self.assertEqual(missing, [("cod3", "LIBREXRUNTIME.so")])


class SystemDllTests(unittest.TestCase):
    def test_windows_own_dlls_by_name(self):
        for name in ("KERNEL32.dll", "user32.dll", "dxgi.dll", "D3D12.dll", "vulkan-1.dll", "ole32.dll",
                     "api-ms-win-crt-runtime-l1-1-0.dll"):
            self.assertTrue(cross.is_windows_system_dll(name), name)
        for name in ("libc++.dll", "libunwind.dll", "librexruntime.dll", "librexgpu-xenos.dll",
                     "libamd_fidelityfx_vk.dll", "msvcp140.dll"):
            self.assertFalse(cross.is_windows_system_dll(name), name)


if __name__ == "__main__":
    unittest.main()
