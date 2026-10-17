"""Find (and if allowed, install) everything the build needs.

Two groups of tools:

- **The ripper** (always): Rust. If it's missing, rustup installs it into
  the user's home folder (no admin rights), from the official
  static.rust-lang.org download.
- **The recomp** (only when it is built): Git, CMake, Ninja, clang 19+, and
  on Linux the -dev libraries / on Windows the Visual Studio Build Tools.

Linux : the distro package manager (apt / dnf / pacman / zypper / xbps) for the
        compiler and the -dev libraries, run through pkexec (GUI password
        prompt) or sudo. CMake, Ninja, 7-Zip and as a last resort LLVM are
        also available as portable downloads that need no root.
Windows: pinned downloads into the work folder for Git, CMake, Ninja, LLVM
        and 7-Zip; the Visual Studio Build Tools (clang uses its C++ library
        and the Windows SDK) with winget or Microsoft's installer.

Nothing is downloaded or installed before the user approves the list
(plan(), common.APPROVAL), and everything that can be goes into the project
folder: the work folder's tools/, Rust in tools/rust. Only system packages
(Linux) and Visual Studio (Windows) go where their installers put them.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from . import common as _common  # (install_windows has a local named `common`)
from .common import (IS_LINUX, IS_WINDOWS, APPROVAL, Install, Log, Runner, SetupError, download, extract_archive,
                     safe_rmtree, url_host)

# The SDK's own check accepts clang 18, but clang 18 can't build it against
# GCC's C++ library (libstdc++ 13/14, as on Ubuntu 24.04 / Linux Mint 22):
# clang 18 reports C++20 concepts as incomplete (__cpp_concepts 201907), so
# libstdc++ hides std::expected and the SDK fails in map_parser.cpp. Clang 19
# fixed that. (Seen in a real build, 2026-10-06.)
MIN_CLANG = 19
PREFERRED_CLANG = 20    # what the SDK's CI builds with
MIN_CMAKE = (3, 25)
# Windows: Git 2.31 or newer takes settings from the environment
# (GIT_CONFIG_COUNT), which is how the git that CMake starts for the SDK's
# FidelityFX download learns core.longpaths. An older one is replaced by the
# pinned MinGit.
MIN_GIT_WINDOWS = (2, 31)

# Rust: the ripper's minimum (engine/Cargo.toml rust-version) and rustup's
# official installer.
MIN_RUST = (1, 80)
RUSTUP_INIT = {
    "linux": "https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init",
    "win": "https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe",
}

# Portable tools, each pinned to one release and checked against its SHA-256
# before use (a download that doesn't match is deleted and the build stops).
# Where the hashes come from: CMake's own cmake-3.31.6-SHA-256.txt; LLVM's
# signed release attestation (LLVM-20.1.8-Linux-X64.tar.xz.jsonl); for the
# others (no published hash), the files as downloaded from the projects'
# GitHub releases on 2026-10-07.
PORTABLE: Dict[str, Tuple[str, str, str]] = {
    "cmake-linux": ("https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz",
                    "cmake-3.31.6-linux-x86_64/bin",
                    "5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf"),
    "cmake-win": ("https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip",
                  "cmake-3.31.6-windows-x86_64/bin",
                  "d163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836"),
    "ninja-linux": ("https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip", ".",
                    "6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255"),
    "ninja-win": ("https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip", ".",
                  "f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a"),
    # The official 7-Zip build (7zz); it also opens RAR archives.
    "7zip-linux": ("https://github.com/ip7z/7zip/releases/download/24.09/7z2409-linux-x64.tar.xz", ".",
                   "914c7e20ad5ef8e4d3cf08620ff8894b28fe11b7eb99809d6930870fbe48a281"),
    "innoextract-win": ("https://github.com/dscharrer/innoextract/releases/download/1.9/innoextract-1.9-windows.zip",
                        ".", "6989342c9b026a00a72a38f23b62a8e6a22cc5de69805cf47d68ac2fec993065"),
    "llvm-linux": ("https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/LLVM-20.1.8-Linux-X64.tar.xz",
                   "LLVM-20.1.8-Linux-X64/bin",
                   "1ead36b3dfcb774b57be530df42bec70ab2d239fbce9889447c7a29a4ddc1ae6"),
    # Windows cross toolchain (clang + mingw-w64 + libc++), for building the
    # Windows game on Linux. Hash: the file as downloaded on 2026-10-07.
    "llvm-mingw-linux": ("https://github.com/mstorsjo/llvm-mingw/releases/download/20260908/"
                         "llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64.tar.xz",
                         "llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin",
                         "2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092"),
    "appimagetool": ("https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage",
                     ".", "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"),
    # waifu2x-ncnn-vulkan (MIT), for the optional texture upscaling
    # (c3w/upscale.py). Hashes: the files as downloaded on 2026-10-10.
    "waifu2x-linux": ("https://github.com/nihui/waifu2x-ncnn-vulkan/releases/download/20220728/"
                      "waifu2x-ncnn-vulkan-20220728-ubuntu.zip",
                      "waifu2x-ncnn-vulkan-20220728-ubuntu",
                      "f2244412aeaf474d58e262f636737abca24ee24cd632d86eb8f0a4c4f9649aaa"),
    "waifu2x-win": ("https://github.com/nihui/waifu2x-ncnn-vulkan/releases/download/20220728/"
                    "waifu2x-ncnn-vulkan-20220728-windows.zip",
                    "waifu2x-ncnn-vulkan-20220728-windows",
                    "3f60ba0b26763c602cb75178c2051bf0c46f3cc9d13975a052a902773988a34b"),
}

# pkg-config modules the SDK (SDL3 + GTK file dialogs + Vulkan) needs on Linux.
# xi (XInput2) also gives SDL raw relative mouse motion on X11.
LINUX_PKGCONFIG = ["gtk+-3.0", "x11", "x11-xcb", "xcb", "xext", "xcursor", "xi", "xrandr", "xfixes",
                   "wayland-client", "xkbcommon", "alsa", "libpulse"]

LINUX_PACKAGES: Dict[str, Dict[str, List[str]]] = {
    "apt": {
        "base": ["git", "cmake", "ninja-build", "build-essential", "pkg-config", "curl", "python3-tk",
                 "ca-certificates"],
        "clang": ["clang-20", "lld-20"],
        "libs": ["libgtk-3-dev", "libx11-dev", "libx11-xcb-dev", "libxext-dev", "libxcursor-dev",
                 "libxi-dev", "libxrandr-dev", "libxfixes-dev", "libxss-dev", "libvulkan-dev", "libwayland-dev",
                 "libwayland-bin", "wayland-protocols", "libxkbcommon-dev", "libdecor-0-dev",
                 "libasound2-dev", "libpulse-dev", "libpipewire-0.3-dev"],
        "extras": ["p7zip-full", "innoextract", "unshield", "unar", "file"],
        "mingw": ["gcc-mingw-w64-x86-64"],
    },
    "dnf": {
        "base": ["git", "cmake", "ninja-build", "gcc-c++", "make", "pkgconf-pkg-config", "curl",
                 "python3-tkinter"],
        "clang": ["clang", "lld"],
        "libs": ["gtk3-devel", "libX11-devel", "libxcb-devel", "libXext-devel", "libXcursor-devel",
                 "libXi-devel", "libXrandr-devel", "libXfixes-devel", "libXScrnSaver-devel", "vulkan-loader-devel",
                 "vulkan-headers", "wayland-devel", "wayland-protocols-devel", "libxkbcommon-devel",
                 "libdecor-devel", "alsa-lib-devel", "pulseaudio-libs-devel", "pipewire-devel"],
        "extras": ["p7zip", "p7zip-plugins", "innoextract", "unshield", "unar", "file"],
        "mingw": ["mingw64-gcc"],
    },
    "pacman": {
        "base": ["git", "cmake", "ninja", "base-devel", "pkgconf", "curl", "tk"],
        "clang": ["clang", "lld"],
        "libs": ["gtk3", "libx11", "libxcb", "libxext", "libxcursor", "libxi", "libxrandr",
                 "libxfixes", "libxss", "vulkan-icd-loader", "vulkan-headers", "wayland",
                 "wayland-protocols", "libxkbcommon", "libdecor", "alsa-lib", "libpulse", "pipewire"],
        "extras": ["7zip", "innoextract", "unshield", "unarchiver", "file"],
        "mingw": ["mingw-w64-gcc"],
    },
    "zypper": {
        "base": ["git", "cmake", "ninja", "gcc-c++", "make", "pkg-config", "curl", "python3-tk"],
        "clang": ["clang", "lld"],
        "libs": ["gtk3-devel", "libX11-devel", "libxcb-devel", "libXext-devel", "libXcursor-devel",
                 "libXi-devel", "libXrandr-devel", "libXfixes-devel", "libXss-devel", "vulkan-devel",
                 "wayland-devel", "wayland-protocols-devel", "libxkbcommon-devel", "libdecor-devel",
                 "alsa-devel", "libpulse-devel", "pipewire-devel"],
        "extras": ["7zip", "innoextract", "unshield", "unar", "file"],
        "mingw": ["mingw64-cross-gcc"],
    },
    "xbps": {   # Void Linux
        "base": ["git", "cmake", "ninja", "base-devel", "pkg-config", "curl", "python3-tkinter"],
        "clang": ["clang", "lld"],
        "libs": ["gtk+3-devel", "libX11-devel", "libxcb-devel", "libXext-devel", "libXcursor-devel",
                 "libXi-devel", "libXrandr-devel", "libXfixes-devel", "libXScrnSaver-devel", "vulkan-loader-devel",
                 "Vulkan-Headers", "wayland-devel", "wayland-protocols", "libxkbcommon-devel", "libdecor-devel",
                 "alsa-lib-devel", "pulseaudio-devel", "pipewire-devel"],
        "extras": ["7zip", "innoextract", "unshield", "unar", "file"],
        "mingw": ["cross-x86_64-w64-mingw32"],
    },
}

# Linux systems whose package manager can't add packages to the running
# system (image-based / immutable) or that have none of the above. The build
# works there inside a distrobox / toolbox container of a regular distro.
CONTAINER_HINT = (
    "Build inside a regular Linux container instead: install distrobox if it isn't there, then run\n"
    "    distrobox create --name cod3-build --image ubuntu:24.04\n"
    "    distrobox enter cod3-build\n"
    "and start `sh setup.sh` from this folder inside it (your home folder is shared, so the output lands "
    "in the same place). On Fedora Atomic, `toolbox create` / `toolbox enter` works the same way.")

# Ubuntu 24.04 for the Linux build inside WSL 2 on Windows (c3w/wsl.py):
# Canonical's official WSL image, checked against the SHA256SUMS list
# Canonical publishes in the same folder. The second address is where the
# image moves once a newer point release replaces it.
WSL_IMAGE_NAME = "ubuntu-24.04.5-wsl-amd64.wsl"
WSL_IMAGE_DIRS = ["https://releases.ubuntu.com/noble/", "https://old-releases.ubuntu.com/releases/noble/"]

# Downloads that target another system. They are used only by the step that
# needs them (by full path), never put on PATH for the rest of the build.
CROSS_TOOLCHAINS = {"llvm-mingw-linux"}
# Downloads only one step runs by full path, also kept off PATH (waifu2x's
# Windows folder carries its own vcomp140.dll).
STEP_ONLY = CROSS_TOOLCHAINS | {"waifu2x-linux", "waifu2x-win"}

# Windows: the tools as pinned downloads into the work folder (no winget, so
# nothing lands in Program Files) that need no admin rights, checked against
# their SHA-256 like the ones above. Nothing in them is run to install:
# 7zr.exe (7-Zip's standalone console program) unpacks the full 7-Zip from
# its installer, and 7-Zip unpacks the LLVM installer.
# Hashes: Git for Windows' and LLVM's own published SHA-256 for these files;
# 7-Zip publishes none, so the files as downloaded on 2026-10-09.
WINDOWS_FALLBACK: Dict[str, Tuple[str, str, str]] = {
    # key: (url, folder with the program, sha256)
    "7zr-win": ("https://github.com/ip7z/7zip/releases/download/24.09/7zr.exe", ".",
                "d2c0045523cf053a6b43f9315e9672fc2535f06aeadd4ffa53c729cd8b2b6dfe"),
    "7zip-win": ("https://github.com/ip7z/7zip/releases/download/24.09/7z2409-x64.exe", ".",
                 "bdd1a33de78618d16ee4ce148b849932c05d0015491c34887846d431d29f308e"),
    "git-win": ("https://github.com/git-for-windows/git/releases/download/v2.48.1.windows.1/"
                "MinGit-2.48.1-64-bit.zip", "cmd",
                "11e8f462726827acccc7ecdad541f2544cbe5506d70fef4fa1ffac7c16288709"),
    "llvm-win": ("https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/LLVM-20.1.8-win64.exe",
                 "bin", "3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b"),
}
# Microsoft's Build Tools bootstrapper. It changes with every Visual Studio
# update, so instead of a fixed hash its Authenticode signature is checked
# (valid, signed by Microsoft Corporation) before it runs.
VS_BOOTSTRAPPER = "https://aka.ms/vs/17/release/vs_BuildTools.exe"
# Visual Studio components the SDK build needs: the C++ compiler and
# libraries (clang uses them) and a Windows SDK (Direct3D 12, DXGI, XInput).
VS_COMPONENTS = ["Microsoft.VisualStudio.Workload.VCTools", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "Microsoft.VisualStudio.Component.Windows11SDK.22621"]
# Windows SDK files the build includes or links; checked after vcvars64.bat.
WINDOWS_SDK_FILES = ["Include/{v}/um/windows.h", "Include/{v}/um/d3d12.h", "Include/{v}/shared/dxgi1_6.h",
                     "Include/{v}/um/Xinput.h", "Lib/{v}/um/x64/d3d12.lib", "Lib/{v}/um/x64/kernel32.lib",
                     "Lib/{v}/ucrt/x64/ucrt.lib"]

# What each pinned download is, as the approval list shows it (name, size).
TOOL_INFO: Dict[str, Tuple[str, str]] = {
    "cmake-linux": ("CMake 3.31.6 (build tool)", "about 55 MB"),
    "cmake-win": ("CMake 3.31.6 (build tool)", "about 45 MB"),
    "ninja-linux": ("Ninja 1.12.1 (build tool)", "under 1 MB"),
    "ninja-win": ("Ninja 1.12.1 (build tool)", "under 1 MB"),
    "7zip-linux": ("7-Zip 24.09 (opens discs and archives)", "about 2 MB"),
    "7zr-win": ("7-Zip 24.09 unpacker (7zr)", "under 1 MB"),
    "7zip-win": ("7-Zip 24.09 (opens discs and archives)", "about 2 MB"),
    "innoextract-win": ("innoextract 1.9 (opens map pack installers; optional)", "about 2 MB"),
    "git-win": ("MinGit 2.48.1 (downloads the SDK source)", "about 40 MB"),
    "llvm-linux": ("LLVM/clang 20.1.8 (C++ compiler)", "about 2 GB unpacked"),
    "llvm-win": ("LLVM/clang 20.1.8 (C++ compiler)", "about 400 MB, 2.5 GB unpacked"),
    "llvm-mingw-linux": ("llvm-mingw 20260908 (compiles the Windows version on Linux)", "about 150 MB, 1 GB unpacked"),
    "appimagetool": ("appimagetool 1.9.1 (makes the Linux AppImage)", "about 20 MB"),
    "waifu2x-linux": ("waifu2x-ncnn-vulkan 20220728 (texture upscaling)", "about 35 MB"),
    "waifu2x-win": ("waifu2x-ncnn-vulkan 20220728 (texture upscaling)", "about 35 MB"),
}


def tool_install(key: str, tools_dir: Path) -> Install:
    """The approval entry for one pinned download (PORTABLE or WINDOWS_FALLBACK)."""
    url = (PORTABLE.get(key) or WINDOWS_FALLBACK[key])[0]
    what, size = TOOL_INFO.get(key, (url.rsplit("/", 1)[-1], ""))
    return Install(key, what, str(tools_dir / key), size, url_host(url))


def rust_install(tools_dir: Path, update: bool = False) -> Install:
    if update:
        return Install("rust", "Rust: update to the latest stable", str(cargo_bin_dir().parent), "up to 300 MB",
                       "static.rust-lang.org")
    return Install("rust", "Rust (compiles the ripper), with rustup", str(rust_dir(tools_dir)), "about 300 MB",
                   "static.rust-lang.org")


VS_INSTALL = Install("vs-buildtools", "Visual Studio 2022 Build Tools: C++ compiler, libraries and the Windows SDK",
                     "where Microsoft's installer puts them (Program Files (x86)\\Microsoft Visual Studio and "
                     "Windows Kits); they can't be moved into the project folder", "about 3-6 GB", "Microsoft")
VS_BUILDTOOLS_ID = "Microsoft.VisualStudio.2022.BuildTools"
VS_BUILDTOOLS_OVERRIDE = "--wait --passive --norestart " + " ".join(f"--add {c}" for c in VS_COMPONENTS)
# winget searches the Microsoft Store as well unless told which source to use;
# where the Store is blocked or its terms weren't accepted, that search fails.
WINGET_SOURCE = ["--source", "winget"]


@dataclass
class Toolchain:
    git: Optional[Path] = None
    cmake: Optional[Path] = None
    ninja: Optional[Path] = None
    cc: Optional[Path] = None
    cxx: Optional[Path] = None
    clang_version: int = 0
    cmake_version: Tuple[int, ...] = ()
    sevenzip: Optional[Path] = None
    innoextract: Optional[Path] = None
    unshield: Optional[Path] = None
    unar: Optional[Path] = None      # RAR archives, when 7-Zip has no RAR support
    mingw: Optional[Path] = None     # Linux: links the Windows build of the ripper
    vs_path: Optional[Path] = None
    windows_sdk: Optional[Path] = None   # Windows: the SDK's Include/<version> folder, when complete
    missing_libs: List[str] = field(default_factory=list)
    cargo: Optional[Path] = None
    rust_version: Tuple[int, ...] = ()
    linker: Optional[Path] = None    # Linux: Rust links through the system C compiler
    env: Dict[str, str] = field(default_factory=lambda: dict(os.environ))
    # Versions and details for "Check this PC" and the log.
    git_version: Tuple[int, ...] = ()
    ninja_version: Tuple[int, ...] = ()
    rust_host: str = ""              # rustc's host, e.g. x86_64-pc-windows-msvc
    vs_version: str = ""             # Windows: "Visual Studio Build Tools 2022 17.14.2"
    vs_env_error: str = ""           # Windows: why vcvars64.bat failed, if it did

    def needs_msvc_linker(self) -> bool:
        """Windows: this Rust links with Microsoft's link.exe (an MSVC host),
        which only Visual Studio's C++ tools bring."""
        return IS_WINDOWS and self.rust_host.endswith("-msvc") and not self.vs_path

    def git_ok(self) -> bool:
        """A git that can do the SDK download (on Windows, new enough)."""
        return bool(self.git) and not (IS_WINDOWS and self.git_version and self.git_version < MIN_GIT_WINDOWS)

    def vs_missing(self) -> bool:
        """Windows: the Build Tools (or their Windows SDK) need installing. A
        Visual Studio whose vcvars64.bat fails isn't installed again: it
        needs a repair, which the note for it says."""
        return not self.vs_path or (not self.windows_sdk and not self.vs_env_error)

    def rust_problems(self) -> List[str]:
        out = []
        if not self.cargo or self.rust_version < MIN_RUST:
            out.append(f"Rust >= {'.'.join(map(str, MIN_RUST))} (for the ripper)")
        if self.cargo and self.needs_msvc_linker():
            out.append("Microsoft's linker for Rust (link.exe, from the Visual Studio Build Tools)")
        if IS_LINUX and not self.linker:
            out.append("a C compiler for linking (gcc, from build-essential)")
        return out

    def problems(self, recomp: bool = True) -> List[str]:
        """What is missing: the ripper's needs always, the recomp's only when
        it is being built."""
        out = self.rust_problems()
        if not recomp:
            return out
        if not self.git:
            out.append("git")
        elif IS_WINDOWS and self.git_version and self.git_version < MIN_GIT_WINDOWS:
            out.append(f"git >= {'.'.join(map(str, MIN_GIT_WINDOWS))} (found {'.'.join(map(str, self.git_version))})")
        if not self.cmake or self.cmake_version < MIN_CMAKE:
            out.append(f"cmake >= {'.'.join(map(str, MIN_CMAKE))}")
        if not self.ninja:
            out.append("ninja")
        if not self.cc or self.clang_version < MIN_CLANG:
            out.append(f"clang >= {MIN_CLANG}")
        if IS_WINDOWS and not self.vs_path:
            out.append("Visual Studio 2022 Build Tools (C++ workload + Windows SDK)")
        elif IS_WINDOWS and self.vs_env_error:
            out.append("a working Visual Studio build environment (vcvars64.bat failed; repair Visual Studio)")
        elif IS_WINDOWS and not self.windows_sdk:
            out.append("Windows 10/11 SDK (a Visual Studio Build Tools component)")
        if IS_LINUX and self.missing_libs:
            out.append("development libraries: " + ", ".join(self.missing_libs))
        return out

    def rows(self) -> List[Tuple[str, bool, str]]:
        """(tool, found, where and which version) for each tool, for the
        setup tool's "Check this PC" window and the log."""
        def ver(v) -> str:
            return " (" + ".".join(map(str, v)) + ")" if v else ""
        rust = ", ".join(x for x in (".".join(map(str, self.rust_version)), self.rust_host) if x)
        rows = [("Rust", bool(self.cargo), f"{self.cargo or ''}{f' ({rust})' if self.cargo and rust else ''}"),
                ("Git", bool(self.git), f"{self.git or ''}{ver(self.git_version) if self.git else ''}"),
                ("CMake", bool(self.cmake), f"{self.cmake or ''}{ver(self.cmake_version)}"),
                ("Ninja", bool(self.ninja), f"{self.ninja or ''}{ver(self.ninja_version) if self.ninja else ''}"),
                ("Clang", bool(self.cxx), f"{self.cxx or ''}{f' (v{self.clang_version})' if self.cxx else ''}"),
                ("7-Zip", bool(self.sevenzip), str(self.sevenzip or "")),
                ("innoextract (optional)", bool(self.innoextract), str(self.innoextract or ""))]
        if IS_WINDOWS:
            vs = f"{self.vs_path or ''}{f' ({self.vs_version})' if self.vs_path and self.vs_version else ''}"
            if self.vs_path and self.vs_env_error:
                vs += f"; its vcvars64.bat failed: {self.vs_env_error}"
            rows.append(("Visual Studio tools", bool(self.vs_path) and not self.vs_env_error, vs))
            rows.append(("Windows SDK", bool(self.windows_sdk), str(self.windows_sdk or "")))
            if _common.UNDER_WINE:
                rows.append(("Wine (optional)", True, f"{_common.wine_version()}: the rips work here; to build "
                             "the game on Linux, `sh setup.sh` makes the Windows version natively"))
        if IS_LINUX:
            rows.append(("Development libraries", not self.missing_libs,
                         "all there" if not self.missing_libs else "missing " + ", ".join(self.missing_libs)))
        return rows

    def summary(self) -> str:
        width = max(len(name) for name, _, _ in self.rows())
        return "\n".join(f"{name:<{width}}  {detail if found else 'MISSING' + (f' ({detail})' if detail else '')}"
                         for name, found, detail in self.rows())


# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------
def _windows_extra_dirs() -> List[Path]:
    dirs = []
    for var, sub in (("ProgramFiles", "CMake/bin"), ("ProgramFiles", "LLVM/bin"), ("ProgramFiles", "Git/cmd"),
                     ("ProgramFiles", "7-Zip"), ("ProgramFiles(x86)", "LLVM/bin"),
                     ("LOCALAPPDATA", "Microsoft/WinGet/Links"), ("LOCALAPPDATA", "Programs/Git/cmd")):
        base = os.environ.get(var)
        if base:
            dirs.append(Path(base) / sub)
    return dirs


def refresh_windows_path() -> None:
    """winget updates PATH in the registry only; pull it into this process."""
    if not IS_WINDOWS:
        return
    try:
        import winreg
        parts: List[str] = []
        for hive, key in ((winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"),
                          (winreg.HKEY_CURRENT_USER, r"Environment")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    val, _ = winreg.QueryValueEx(k, "Path")
                    parts += [os.path.expandvars(p) for p in str(val).split(";") if p]
            except OSError:
                pass
        current = os.environ.get("PATH", "").split(os.pathsep)
        merged = current + [p for p in parts if p not in current]
        os.environ["PATH"] = os.pathsep.join(merged)
    except ImportError:
        pass


def _which(names: List[str], extra: List[Path], env_path: Optional[str] = None) -> Optional[Path]:
    search = os.pathsep.join([str(d) for d in extra] + [env_path or os.environ.get("PATH", "")])
    for n in names:
        p = shutil.which(n, path=search)
        if p:
            return Path(p)
    return None


def _version(runner: Runner, exe: Path, pattern: str) -> Tuple[int, ...]:
    rc, out = runner.capture([exe, "--version"], timeout=30)
    m = re.search(pattern, out)
    return tuple(int(x) for x in m.groups() if x is not None) if m else ()


def _portable_bin(tools_dir: Path, key: str) -> Path:
    url, sub, _ = PORTABLE[key] if key in PORTABLE else WINDOWS_FALLBACK[key]
    return tools_dir / key / sub


def _portable_ok(tools_dir: Path, key: str) -> bool:
    """A portable download finished unpacking and is the currently pinned
    one: its `.verified` marker (written last) holds the pinned SHA-256. A
    half-unpacked folder or one from an older pin doesn't count."""
    _, _, sha = PORTABLE[key] if key in PORTABLE else WINDOWS_FALLBACK[key]
    marker = tools_dir / key / ".verified"
    try:
        return marker.read_text(encoding="ascii").strip().lower() == sha.lower()
    except OSError:
        return False


def _vs_installer_dir() -> Optional[Path]:
    pf86 = os.environ.get("ProgramFiles(x86)") or os.environ.get("ProgramFiles")
    return Path(pf86) / "Microsoft Visual Studio" / "Installer" if pf86 else None


def find_vs(runner: Runner, require_cpp: bool = True, products: str = "*") -> Optional[Path]:
    """The newest Visual Studio (any edition, Build Tools included, or only
    `products`). With `require_cpp`, only one that has the C++ compiler
    installed; without, also one that lacks it (so the missing parts can be
    added to it)."""
    d = _vs_installer_dir()
    vswhere = d / "vswhere.exe" if d else None
    if not vswhere or not vswhere.exists():
        return None
    cmd = [vswhere, "-latest", "-products", products]
    if require_cpp:
        cmd += ["-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"]
    rc, out = runner.capture(cmd + ["-property", "installationPath"])
    line = out.strip().splitlines()[0].strip() if rc == 0 and out.strip() else ""
    return Path(line) if line and Path(line).exists() else None


def env_get(env: Dict[str, str], name: str, default: str = "") -> str:
    """A variable from `env`, whatever the case of its name (Windows' variable
    names ignore case: PATH, Path)."""
    if name in env:
        return env[name]
    low = name.lower()
    return next((v for k, v in env.items() if k.lower() == low), default)


def merge_env(base: Dict[str, str], new: Dict[str, str]) -> Dict[str, str]:
    """`base` updated with `new`, a variable whose name differs only in case
    (Windows' `set` says Path, Python's os.environ PATH) replacing the old one
    under the old name, so there is never a PATH and a Path side by side."""
    env = dict(base)
    names = {k.lower(): k for k in env}
    for k, v in new.items():
        env[names.get(k.lower(), k)] = v
    return env


def find_windows_sdk(env: Dict[str, str]) -> Optional[Path]:
    """The Windows SDK vcvars64.bat picked, if every file the build needs is
    there. Returns its Include/<version> folder."""
    sdk = env_get(env, "WindowsSdkDir").strip()
    ver = env_get(env, "WindowsSDKVersion").strip().strip("\\/")
    if not sdk or not ver:
        return None
    root = Path(sdk)
    if all((root / f.format(v=ver)).is_file() for f in WINDOWS_SDK_FILES):
        return root / "Include" / ver
    return None


def vs_details(runner: Runner, vs: Path) -> str:
    """The product name and version of a Visual Studio installation, as
    vswhere reports them ("Visual Studio Build Tools 2022 17.14.2"), or ""."""
    d = _vs_installer_dir()
    vswhere = d / "vswhere.exe" if d else None
    if not vswhere or not vswhere.exists():
        return ""
    rc, out = runner.capture([vswhere, "-all", "-products", "*", "-format", "json", "-utf8"])
    try:
        import json
        for item in json.loads(out) if rc == 0 else []:
            if str(item.get("installationPath", "")).lower() == str(vs).lower():
                version = (item.get("catalog") or {}).get("productDisplayVersion") or item.get("installationVersion")
                return " ".join(str(x) for x in (item.get("displayName"), version) if x)
    except (ValueError, AttributeError, TypeError):
        pass
    return ""


# vcvars64.bat's own success marker, echoed only when it returns 0.
_VCVARS_OK = "C3W-VCVARS-OK"


def _run_vcvars(vcvars: Path, env: Dict[str, str]) -> Tuple[Optional[Dict[str, str]], str]:
    """Runs vcvars64.bat in `env`. Returns the variables it leaves set (None
    if it failed) and, on failure, the end of its own messages."""
    comspec = env_get(env, "ComSpec") or "cmd.exe"
    flags = 0x08000000   # no console window
    try:
        # The command line goes to cmd as it is (a string, no shell=True
        # around it). /d: no AutoRun commands (Anaconda and Clink add some,
        # which print text or fail). /u: `set` writes UTF-16, so names like
        # "José" in paths survive (the OEM code page would garble them).
        out = subprocess.run(f'"{comspec}" /d /u /s /c ""{vcvars}" >nul 2>&1 && echo {_VCVARS_OK}&& set"',
                             capture_output=True, env=env, timeout=180, creationflags=flags,
                             stdin=subprocess.DEVNULL).stdout.decode("utf-16-le", "replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)
    lines = out.splitlines()
    if _VCVARS_OK in (line.strip() for line in lines):
        found = {}
        for line in lines:
            if "=" in line and not line.startswith("="):
                k, v = line.split("=", 1)
                found[k] = v
        if not (env_get(found, "INCLUDE") and env_get(found, "LIB")):
            return None, "it finished without setting INCLUDE and LIB (the C++ tools are incomplete)"
        # With a PATH too long for cmd, vcvars64.bat goes on after its PATH
        # line fails: everything is set but the compiler's folder isn't on PATH.
        if "\\vc\\tools\\msvc\\" not in env_get(found, "PATH").lower():
            return None, "it couldn't add the compiler's folder to PATH (The input line is too long?)"
        return found, ""
    # Failed: run it once more for its messages (in the console's code page).
    try:
        raw = subprocess.run(f'"{comspec}" /d /s /c ""{vcvars}""', capture_output=True, env=env, timeout=180,
                             creationflags=flags, stdin=subprocess.DEVNULL)
        try:
            text = (raw.stdout + raw.stderr).decode("oem", "replace")
        except LookupError:
            text = (raw.stdout + raw.stderr).decode("latin-1", "replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        text = str(e)
    said = [line.strip() for line in text.splitlines() if line.strip() and not line.strip().startswith("*")]
    return None, " / ".join(said[-3:]) or "it stopped with an error"


def vs_environment_checked(vs: Path, base_env: Dict[str, str]) -> Tuple[Dict[str, str], str]:
    """Runs vcvars64.bat and returns the environment it sets up (INCLUDE, LIB,
    PATH to the compiler and the Windows SDK tools) so clang and the linker
    find everything, and "" or why it failed.

    A long PATH makes vcvars64.bat fail ("The input line is too long": cmd
    allows 8191 characters, and it adds a dozen folders), so after a failure
    it runs again with Windows' own folders only, and the rest of PATH is
    added back behind what it set."""
    vcvars = vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.exists():
        return base_env, f"{vcvars} is missing"
    found, error = _run_vcvars(vcvars, base_env)
    if found is None:
        root = env_get(base_env, "SystemRoot") or env_get(base_env, "windir") or "C:\\Windows"
        short = merge_env(base_env, {"PATH": ";".join([root + "\\System32", root, root + "\\System32\\Wbem",
                                                       root + "\\System32\\WindowsPowerShell\\v1.0"])})
        found, _ = _run_vcvars(vcvars, short)
        if found is not None:
            have = [p for p in env_get(found, "PATH").split(";") if p]
            rest = [p for p in env_get(base_env, "PATH").split(";") if p and p not in have]
            found = merge_env(found, {"PATH": ";".join(have + rest)})
            error = ""
    if found is None:
        return base_env, error
    return merge_env(base_env, found), ""


def vs_environment(runner: Runner, vs: Path, base_env: Dict[str, str]) -> Dict[str, str]:
    """vs_environment_checked's environment (the base one if it failed)."""
    return vs_environment_checked(vs, base_env)[0]


def _pkgconfig_missing(runner: Runner, env: Dict[str, str]) -> List[str]:
    pc = shutil.which("pkg-config", path=env.get("PATH")) or shutil.which("pkgconf", path=env.get("PATH"))
    if not pc:
        return ["pkg-config"] + LINUX_PKGCONFIG
    missing = []
    for mod in LINUX_PKGCONFIG:
        rc, _ = runner.capture([pc, "--exists", mod])
        if rc != 0:
            missing.append(mod)
    return missing


def _clang_cxx(cc: Path, path: str, runner: Runner) -> Optional[Path]:
    """The C++ driver that goes with a clang: clang++-NN next to clang-NN or
    on PATH, else a clang++ in the same folder that is the same clang (the
    official LLVM archive has clang-20 and clang++ but no clang++-20). None
    when there is no matching one."""
    cxx_name = cc.name.replace("clang", "clang++", 1)
    if cc.with_name(cxx_name).is_file():
        return cc.with_name(cxx_name)
    if cxx_name.replace(".exe", "") != "clang++":
        found = shutil.which(cxx_name.replace(".exe", ""), path=path)
        if found:
            return Path(found)
    plain = cc.with_name("clang++" + cc.suffix)
    if plain.is_file():
        same = os.path.realpath(plain) == os.path.realpath(cc)
        if not same:
            same = _version(runner, plain, r"clang version (\d+)") == _version(runner, cc, r"clang version (\d+)")
        if same:
            return plain
    return None


def _targets_msvc_x64(runner: Runner, clang: Path) -> bool:
    rc, out = runner.capture([clang, "--version"], timeout=30)
    return rc == 0 and "x86_64-pc-windows-msvc" in out


def detect(tools_dir: Path, runner: Runner) -> Toolchain:
    refresh_windows_path()
    use_local_rust(tools_dir)   # before tc.env is copied from os.environ
    tc = Toolchain()
    extra: List[Path] = []
    # Portable downloads from earlier runs take part in the search, except
    # cross toolchains: llvm-mingw's clang-NN and x86_64-w64-mingw32-gcc
    # would otherwise be taken for the host compiler and for MinGW-w64's gcc
    # (owner's build 2026-10-10: "clang++-23 is not a full path to an
    # existing compiler tool", and the Windows ripper missing libgcc).
    for key in PORTABLE:
        if key not in STEP_ONLY and _portable_ok(tools_dir, key):
            extra.append(_portable_bin(tools_dir, key))
    if IS_WINDOWS:
        extra += [_portable_bin(tools_dir, key) for key in WINDOWS_FALLBACK if _portable_ok(tools_dir, key)]
        extra += _windows_extra_dirs()
    tc.env = dict(os.environ)
    tc.env["PATH"] = os.pathsep.join([str(d) for d in extra if d.exists()] + [os.environ.get("PATH", "")])
    path = tc.env["PATH"]

    tc.git = _which(["git"], [], path)
    if tc.git:
        tc.git_version = _version(runner, tc.git, r"git version (\d+)\.(\d+)(?:\.(\d+))?")
    # CMake: prefer whichever new-enough one comes first.
    for cand in [shutil.which("cmake", path=os.pathsep.join(str(d) for d in extra)), shutil.which("cmake", path=path)]:
        if cand:
            v = _version(runner, Path(cand), r"cmake version (\d+)\.(\d+)(?:\.(\d+))?")
            if v and v >= MIN_CMAKE:
                tc.cmake, tc.cmake_version = Path(cand), v
                break
            if not tc.cmake:
                tc.cmake, tc.cmake_version = Path(cand), v
    tc.ninja = _which(["ninja", "ninja-build"], [], path)
    if tc.ninja:
        tc.ninja_version = _version(runner, tc.ninja, r"(\d+)\.(\d+)(?:\.(\d+))?")

    # Clang: the newest clang >= 18 we can find.
    names = ["clang"] if IS_WINDOWS else [f"clang-{v}" for v in range(24, MIN_CLANG - 1, -1)] + ["clang"]
    best: Tuple[int, Optional[Path], Optional[Path]] = (0, None, None)
    for n in names:
        p = _which([n], [], path)
        if not p:
            continue
        cxx = _clang_cxx(p, path, runner)
        if not cxx:
            continue   # a clang without its C++ driver can't build the game
        if IS_WINDOWS and not _targets_msvc_x64(runner, p):
            continue   # MSYS2's mingw clang or a 32-bit LLVM can't use Visual Studio's libraries
        v = _version(runner, p, r"clang version (\d+)")
        ver = v[0] if v else 0
        if ver > best[0]:
            best = (ver, p, cxx)
    if best[1]:
        tc.cc, tc.cxx, tc.clang_version = best[1], best[2], best[0]

    # Rust links through `cc` on Linux (not gcc/clang by another name).
    tc.linker = _which(["cc"], [], path) if IS_LINUX else None
    tc.cargo = find_cargo(tools_dir)
    if tc.cargo:
        tc.rust_version = _version(runner, tc.cargo, r"cargo (\d+)\.(\d+)")
        tc.rust_host = rust_host(runner, tc.cargo)
    # Not p7zip's `7za`: it can't open ISO, RAR or NSIS files. The official
    # 7-Zip (`7zz`) first: it refuses links in an archive that point out of
    # the folder it unpacks to, which old p7zip `7z` builds create.
    tc.sevenzip = _which(["7zz", "7z"], [], path)
    tc.innoextract = _which(["innoextract"], [], path)
    tc.unshield = _which(["unshield"], [], path)
    tc.unar = _which(["unar"], [], path)
    tc.mingw = _which(["x86_64-w64-mingw32-gcc"], [], path) if IS_LINUX else None

    if IS_WINDOWS:
        tc.vs_path = find_vs(runner)
        if tc.vs_path:
            tc.vs_version = vs_details(runner, tc.vs_path)
            tc.env, tc.vs_env_error = vs_environment_checked(tc.vs_path, tc.env)
            tc.windows_sdk = find_windows_sdk(tc.env)
        # Every git started from here on allows long paths, also the one CMake
        # starts to fetch the SDK's FidelityFX part (it can't be given -c).
        tc.env = merge_env(tc.env, {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.longpaths",
                                    "GIT_CONFIG_VALUE_0": "true"})
    if IS_LINUX:
        tc.missing_libs = _pkgconfig_missing(runner, tc.env)
    return tc


def rust_host(runner: Runner, cargo: Path) -> str:
    """The host triple of the Rust next to `cargo` (x86_64-pc-windows-msvc,
    x86_64-unknown-linux-gnu ...), or "" if rustc doesn't say."""
    rustc = Path(cargo).with_name("rustc.exe" if IS_WINDOWS else "rustc")
    rc, out = runner.capture([rustc if rustc.is_file() else "rustc", "-vV"], timeout=30)
    m = re.search(r"^host:\s*(\S+)", out, re.MULTILINE) if rc == 0 else None
    return m.group(1) if m else ""


def cargo_bin_dir() -> Path:
    """Where rustup puts cargo: $CARGO_HOME/bin, by default ~/.cargo/bin."""
    home = os.environ.get("CARGO_HOME")
    return (Path(home) if home else Path.home() / ".cargo") / "bin"


def rust_dir(tools_dir: Path) -> Path:
    """Where this tool installs Rust when the PC has none: tools/rust in the
    work folder (cargo/ and rustup/), so it stays in the project folder and
    nothing goes into the user's home (owner, 2026-10-10)."""
    return tools_dir / "rust"


def use_local_rust(tools_dir: Path) -> Optional[Path]:
    """The Rust in tools/rust, if there is one: its cargo, with CARGO_HOME
    and RUSTUP_HOME pointed there for everything started from now on (rustup's
    cargo needs them to find its toolchain)."""
    home = rust_dir(tools_dir)
    cargo = home / "cargo" / "bin" / ("cargo.exe" if IS_WINDOWS else "cargo")
    if not cargo.is_file():
        return None
    os.environ["CARGO_HOME"] = str(home / "cargo")
    os.environ["RUSTUP_HOME"] = str(home / "rustup")
    return cargo


def find_cargo(tools_dir: Optional[Path] = None) -> Optional[Path]:
    """The tool's own Rust (tools/rust) first, else one already on the PC."""
    local = use_local_rust(tools_dir) if tools_dir else None
    return local or _which(["cargo"], [cargo_bin_dir()])


# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
def install_rust(tools_dir: Path, runner: Runner, log: Log, cancel: threading.Event, have_msvc: bool) -> None:
    """Installs Rust with rustup into tools/rust (rust_dir), or updates the
    Rust the PC already has.

    Nothing needs admin rights. Neither the shell profile nor PATH is touched
    (--no-modify-path); the tool finds its cargo by itself. On Windows without
    the Visual Studio Build Tools, the GNU toolchain is used, which brings its
    own linker."""
    rustup = _which(["rustup"], [cargo_bin_dir()])
    if rustup:
        APPROVAL.require([rust_install(tools_dir, update=True)], log)
        log.info("Updating Rust (rustup update stable)...")
        runner.run([rustup, "update", "stable", "--no-self-update"], check=False)
        runner.run([rustup, "default", "stable"], check=False)
        return
    key = "win" if IS_WINDOWS else "linux"
    url = RUSTUP_INIT[key]
    approval = rust_install(tools_dir)
    APPROVAL.require([approval], log)
    home = rust_dir(tools_dir)
    log.info(f"Rust isn't installed; installing it with rustup into {home} (about 300 MB, no admin rights)...")
    init = download(url, tools_dir / "downloads" / url.rsplit("/", 1)[1], log, cancel,
                    sha256=_published_sha256(url + ".sha256", log), approval=approval)
    if not IS_WINDOWS:
        init.chmod(0o755)
    env = dict(os.environ, CARGO_HOME=str(home / "cargo"), RUSTUP_HOME=str(home / "rustup"))
    cmd = [init, "-y", "--profile", "minimal", "--default-toolchain", "stable", "--no-modify-path"]
    if IS_WINDOWS and not have_msvc:
        cmd += ["--default-host", "x86_64-pc-windows-gnu"]
    runner.run(cmd, env=env)
    use_local_rust(tools_dir)



def _published_sha256(url: str, log: Log) -> str:
    """The SHA-256 a project publishes next to a download (`<file>.sha256`)."""
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "cod3-waw-setup"}),
                                    timeout=60) as r:
            text = r.read(4096).decode("ascii", "replace")
    except OSError as e:
        raise SetupError(f"Couldn't get the checksum {url} ({e}); not running an unchecked download.")
    m = re.search(r"\b([0-9a-fA-F]{64})\b", text)
    if not m:
        raise SetupError(f"No checksum in {url}; not running an unchecked download.")
    return m.group(1)


def published_sha256_in_list(sums_url: str, filename: str) -> str:
    """The SHA-256 for `filename` in a published `SHA256SUMS` list
    ("<hash> *<name>" or "<hash>  <name>" per line)."""
    try:
        with urllib.request.urlopen(urllib.request.Request(sums_url, headers={"User-Agent": "cod3-waw-setup"}),
                                    timeout=60) as r:
            text = r.read(1 << 20).decode("utf-8", "replace")
    except OSError as e:
        raise SetupError(f"Couldn't get the checksum list {sums_url} ({e}); not using an unchecked download.")
    sha = sha256_from_list(text, filename)
    if not sha:
        raise SetupError(f"{filename} isn't in {sums_url}; not using an unchecked download.")
    return sha


def sha256_from_list(text: str, filename: str) -> Optional[str]:
    for line in text.splitlines():
        m = re.match(r"^([0-9a-fA-F]{64})\s+\*?(\S+)\s*$", line)
        if m and m.group(2) == filename:
            return m.group(1).lower()
    return None


def wsl_install(work: Path) -> Install:
    return Install("wsl-ubuntu", "Ubuntu 24.04 for WSL: a private build system for the Linux version, with its "
                   "own build packages and SDK inside; removed again afterwards", str(work / "wsl"),
                   "about 350 MB download, 8-12 GB while building", "releases.ubuntu.com")


def fetch_wsl_image(downloads: Path, log: Log, cancel: threading.Event) -> Path:
    """Canonical's Ubuntu 24.04 WSL image (about 350 MB), checked."""
    errors = []
    for base in WSL_IMAGE_DIRS:
        try:
            sha = published_sha256_in_list(base + "SHA256SUMS", WSL_IMAGE_NAME)
            return download(base + WSL_IMAGE_NAME, downloads / WSL_IMAGE_NAME, log, cancel, sha256=sha,
                            approval=wsl_install(downloads.parent))
        except (SetupError, OSError) as e:
            errors.append(str(e))
    raise SetupError("Couldn't download the Ubuntu image for WSL:\n  " + "\n  ".join(errors))


_PM_EXE = {"apt": "apt-get", "dnf": "dnf", "pacman": "pacman", "zypper": "zypper", "xbps": "xbps-install"}
# Distro family (os-release ID / ID_LIKE) -> its own package manager.
_PM_FAMILY = [({"debian", "ubuntu", "linuxmint", "pop", "elementary", "zorin", "kali", "raspbian"}, "apt"),
              ({"fedora", "rhel", "centos", "rocky", "almalinux", "nobara", "ol"}, "dnf"),
              ({"arch", "manjaro", "endeavouros", "garuda", "cachyos", "artix"}, "pacman"),
              ({"suse", "opensuse", "opensuse-tumbleweed", "opensuse-leap", "sles"}, "zypper"),
              ({"void"}, "xbps")]


def _linux_package_manager() -> Optional[str]:
    """The distro's own package manager (from os-release, so a second one
    installed on the side, like dnf on openSUSE, isn't picked), else the
    first one found."""
    rel = os_release()
    ids = [rel.get("ID", "")] + rel.get("ID_LIKE", "").split()
    for i in ids:
        for family, pm in _PM_FAMILY:
            if i in family and shutil.which(_PM_EXE[pm]):
                return pm
    for pm, exe in _PM_EXE.items():
        if shutil.which(exe):
            return pm
    return None


def os_release() -> Dict[str, str]:
    """/etc/os-release as a dict (ID, ID_LIKE, NAME, VARIANT_ID ...)."""
    out: Dict[str, str] = {}
    for path in ("/etc/os-release", "/usr/lib/os-release"):
        try:
            text = Path(path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"').strip("'")
        break
    return out


def in_container() -> bool:
    """Inside distrobox / toolbox / podman / docker (packages install normally there)."""
    return (Path("/run/.containerenv").exists() or Path("/.dockerenv").exists()
            or bool(os.environ.get("CONTAINER_ID")) or bool(os.environ.get("TOOLBOX_PATH")))


def immutable_system() -> Optional[str]:
    """The name of an image-based Linux whose system packages can't simply be
    added (Fedora Atomic / Bazzite / Bluefin, SteamOS, openSUSE MicroOS /
    Aeon, NixOS ...), or None."""
    if not IS_LINUX or in_container():
        return None
    rel = os_release()
    name = rel.get("PRETTY_NAME") or rel.get("NAME") or "this system"
    ids = {rel.get("ID", "")} | set(rel.get("ID_LIKE", "").split())
    # The program names alone aren't enough: rpm-ostree and
    # transactional-update can be installed on ordinary Fedora / openSUSE.
    if Path("/run/ostree-booted").exists():
        return name
    if "nixos" in ids or "steamos" in ids:
        return name
    if ids & {"opensuse-microos", "opensuse-aeon", "opensuse-kalpa", "suse-microos"} or (
            shutil.which("transactional-update") and _root_read_only()):
        return name
    return None


def _root_read_only() -> bool:
    try:
        return bool(os.statvfs("/").f_flag & os.ST_RDONLY)
    except (OSError, AttributeError):
        return False


def _elevate_prefix(interactive: bool) -> List[str]:
    if os.geteuid() == 0:  # type: ignore[attr-defined]
        return []
    if not interactive and shutil.which("pkexec") and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        return ["pkexec"]
    if shutil.which("sudo"):
        return ["sudo"]
    if shutil.which("pkexec"):
        return ["pkexec"]
    raise SetupError("Installing packages needs root, but neither sudo nor pkexec is available.")


# Probe commands used inside the generated install script: does a package
# exist in this distro's repositories (or is it installed already)?
_PROBE = {
    "apt": 'apt-cache show "$1" >/dev/null 2>&1',
    "dnf": 'dnf -q info "$1" >/dev/null 2>&1',
    "pacman": 'pacman -Si "$1" >/dev/null 2>&1 || pacman -Qi "$1" >/dev/null 2>&1',
    "zypper": 'zypper --non-interactive -q search --match-exact "$1" >/dev/null 2>&1',
    "xbps": 'xbps-query -R "$1" >/dev/null 2>&1 || xbps-query "$1" >/dev/null 2>&1',
}
_INSTALL = {
    "apt": "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends",
    "dnf": "dnf install -y",
    "pacman": "pacman -S --needed --noconfirm",
    "zypper": "zypper --non-interactive install --no-recommends",
    "xbps": "xbps-install -y",
}
# Refreshing the package lists first (pacman: -Sy alone would be a partial
# upgrade, so it only uses what `pacman -S` resolves against).
_REFRESH = {
    "apt": "apt-get update || true",
    "xbps": "xbps-install -S || true",
}


def linux_packages(pm: str, minimal: bool, windows_exe: bool) -> List[str]:
    spec = LINUX_PACKAGES[pm]
    wanted = spec["base"] + spec["extras"] if minimal else spec["base"] + spec["libs"] + spec["extras"]
    return wanted + (spec["mingw"] if windows_exe else [])


def linux_packages_install(pm: str, wanted: List[str]) -> Install:
    return Install("system-packages", f"Build packages from your distribution ({len(wanted)}, any already "
                   f"installed are skipped): " + ", ".join(wanted),
                   f"the system's own folders, by {pm} (packages can only go there; it asks for your password)",
                   "depends on what you have; often 0.5-2 GB", f"your distribution's {pm} repositories")


def install_linux(tc: Toolchain, runner: Runner, log: Log, minimal: bool = False, windows_exe: bool = False) -> None:
    """Installs the build packages with the distro's package manager. With
    `minimal`, only the basics the ripper needs (a C compiler for linking,
    7-Zip, unshield, innoextract). With `windows_exe`, also the MinGW-w64
    cross compiler, which links the Windows build of the ripper."""
    immutable = immutable_system()
    if immutable:
        raise SetupError(f"{immutable} is an image-based system: packages can't be added to it the usual way, "
                         "and the build needs development libraries. " + CONTAINER_HINT)
    pm = _linux_package_manager()
    if not pm:
        log.warn("No supported package manager (apt, dnf, pacman, zypper, xbps) found; skipping package "
                 "install. Portable CMake / Ninja / 7-Zip / LLVM will still be downloaded if needed. "
                 + CONTAINER_HINT)
        return
    spec = LINUX_PACKAGES[pm]
    wanted = linux_packages(pm, minimal, windows_exe)
    APPROVAL.require([linux_packages_install(pm, wanted)], log)
    if pm == "apt":
        clang_choices = [f"clang-{v}:lld-{v}" for v in (PREFERRED_CLANG, 21, 22, 19)]
    else:
        clang_choices = [":".join(spec["clang"])]
    need_clang = 1 if tc.clang_version < MIN_CLANG and not minimal else 0

    # One script, run once with root rights: it filters out packages this
    # distro doesn't have (so one odd name can't abort the whole install),
    # picks the newest suitable clang, and installs everything. It only holds
    # package names from the lists above, and is written to a new private
    # folder (readable by this user only) so nothing can change it before it
    # runs as root.
    private = Path(tempfile.mkdtemp(prefix="cod3-waw-setup-"))
    script = private / "install-packages.sh"
    script.write_text(f"""#!/bin/sh
# Generated by the COD_3_WAW setup tool ({pm}).
set -u
have() {{ {_PROBE[pm]}; }}
{_REFRESH.get(pm, "")}
pkgs=""
for p in {" ".join(wanted)}; do
  if have "$p"; then pkgs="$pkgs $p"; else echo "skipping (not in your repositories): $p"; fi
done
if [ {need_clang} = 1 ]; then
  for c in {" ".join(clang_choices)}; do
    cc=${{c%%:*}}; ld=${{c#*:}}
    if have "$cc"; then pkgs="$pkgs $cc $ld"; break; fi
  done
fi
echo "installing:$pkgs"
{_INSTALL[pm]} $pkgs
""", encoding="utf-8")
    interactive = sys.stdin is not None and sys.stdin.isatty()
    prefix = _elevate_prefix(interactive)
    log.info(f"Installing build dependencies with {pm}" +
             (" - enter your password when asked." if prefix else "."))
    if prefix == ["sudo"] and interactive:
        # sudo needs the terminal for its password prompt, so don't capture.
        rc = subprocess.call(prefix + ["sh", str(script)])
    else:
        rc = runner.run(prefix + ["sh", str(script)], check=False)
    if rc != 0:
        raise SetupError(f"Package installation failed (exit code {rc}). You can run it yourself:\n"
                         f"  sudo sh \"{script}\"\nthen start the build again.")
    script.unlink()
    private.rmdir()


def install_windows(tc: Toolchain, tools_dir: Path, runner: Runner, log: Log, cancel: threading.Event) -> None:
    """Installs what's missing on Windows. Git, LLVM and 7-Zip come from the
    pinned downloads into the work folder's tools/ (owner, 2026-10-10:
    everything in the project folder), not winget, which would put them in
    Program Files. Only Visual Studio's Build Tools go where Microsoft's
    installer puts them; one that lacks the C++ tools or the Windows SDK gets
    them added."""
    winget = shutil.which("winget")
    after = detect(tools_dir, runner)
    if not after.sevenzip:
        fetch_windows_fallback("7zip-win", tools_dir, runner, log, cancel)
    if not after.git_ok():
        fetch_windows_fallback("git-win", tools_dir, runner, log, cancel)
    if not after.cc or after.clang_version < MIN_CLANG:
        log.info("Getting LLVM/clang 20 (about 400 MB download, 2.5 GB unpacked).")
        fetch_windows_fallback("llvm-win", tools_dir, runner, log, cancel)

    if after.vs_missing() and _common.UNDER_WINE:
        # Microsoft's Build Tools installer doesn't run under Wine.
        log.warn("Running under Wine: Visual Studio's Build Tools can't be installed here, so the playable "
                 "game can't be compiled under Wine yet (the rips work). On Linux, `sh setup.sh` builds the "
                 "Windows version (cod3.exe) natively instead.")
    elif after.vs_missing():
        install_vs_build_tools(tools_dir, runner, log, cancel, winget)
    elif after.vs_env_error:
        log.warn(f"Visual Studio in {after.vs_path} is installed, but its vcvars64.bat failed: {after.vs_env_error}. "
                 "Open the Visual Studio Installer and choose Repair (or Modify, \"Desktop development with "
                 "C++\"), then build again.")
    refresh_windows_path()


def _microsoft_signed(runner: Runner, exe: Path) -> bool:
    """True if `exe` carries a valid Authenticode signature by Microsoft."""
    ps = shutil.which("powershell") or shutil.which("pwsh")
    if not ps:
        return False
    path = str(exe).replace("'", "''")
    script = (f"$s = Get-AuthenticodeSignature -LiteralPath '{path}'; "
              "if ($s.Status -eq 'Valid' -and $s.SignerCertificate.Subject -match 'O=Microsoft Corporation') "
              "{ exit 0 } else { exit 1 }")
    rc, _ = runner.capture([ps, "-NoProfile", "-NonInteractive", "-Command", script], timeout=120)
    return rc == 0


VS_BUILDTOOLS_PRODUCT = "Microsoft.VisualStudio.Product.BuildTools"


def install_vs_build_tools(tools_dir: Path, runner: Runner, log: Log, cancel: threading.Event,
                           winget: Optional[str]) -> None:
    """Gets the C++ tools and the Windows SDK: added to an installed Build
    Tools, or a Build Tools installed (next to any other Visual Studio
    edition; the Windows SDK is shared by all of them). Uses Microsoft's
    bootstrapper, the only one of its installers that supports --wait, after
    checking its signature. Windows asks for administrator rights (UAC)."""
    APPROVAL.require([VS_INSTALL], log)
    adds: List[str] = []
    for c in VS_COMPONENTS:
        adds += ["--add", c]
    flags = ["--passive", "--norestart", "--wait"]
    boot: Optional[Path] = None

    def bootstrapper() -> Path:
        nonlocal boot
        if boot is None:
            boot = download(VS_BOOTSTRAPPER, tools_dir / "downloads" / "vs_BuildTools.exe", log, cancel,
                            approval=VS_INSTALL)
            if not _microsoft_signed(runner, boot):
                boot.unlink(missing_ok=True)
                raise SetupError("The Visual Studio Build Tools installer isn't validly signed by Microsoft, so "
                                 "it wasn't run. Install the Build Tools yourself (C++ workload and Windows SDK) "
                                 "from https://visualstudio.microsoft.com/visual-cpp-build-tools/ and run the "
                                 "build again.")
        return boot

    from .pc_check import vs_installer_result

    def report(rc: int) -> None:
        # Microsoft's installer says little; its exit code says what happened.
        (log.info if rc in (0, 1641, 3010) else log.warn)(f"Visual Studio installer: {vs_installer_result(rc)}")

    existing = find_vs(runner, require_cpp=False, products=VS_BUILDTOOLS_PRODUCT)
    if existing:
        log.info(f"Adding the C++ tools and the Windows SDK to the Build Tools in {existing} (a UAC prompt "
                 "will appear; about 3 GB).")
        report(runner.run([bootstrapper(), "modify", "--installPath", existing, *adds, *flags], check=False))
        return
    if winget:
        log.info("winget: installing the Visual Studio 2022 Build Tools with the C++ workload "
                 "(large download; a UAC prompt will appear)")
        rc = runner.run([winget, "install", "-e", "--id", VS_BUILDTOOLS_ID, *WINGET_SOURCE,
                         "--override", VS_BUILDTOOLS_OVERRIDE,
                         "--accept-package-agreements", "--accept-source-agreements", "--disable-interactivity"],
                        check=False)
        if find_vs(runner, products=VS_BUILDTOOLS_PRODUCT):
            return
        log.warn(f"winget couldn't install the Build Tools (exit code {rc}); using Microsoft's installer directly.")
    log.info("Installing the Visual Studio 2022 Build Tools (C++ workload and Windows SDK; about 3 GB; "
             "a UAC prompt will appear).")
    report(runner.run([bootstrapper(), *adds, *flags], check=False))


def fetch_windows_fallback(key: str, tools_dir: Path, runner: Runner, log: Log, cancel: threading.Event) -> Path:
    """One of the WINDOWS_FALLBACK tools: downloaded, checked against its
    SHA-256 and unpacked into tools/<key>. Installers are unpacked with
    7-Zip, never run."""
    url, sub, sha256 = WINDOWS_FALLBACK[key]
    dest = tools_dir / key
    bindir = dest / sub
    if bindir.exists() and any(bindir.iterdir()) and _portable_ok(tools_dir, key):
        return bindir
    archive = download(url, tools_dir / "downloads" / url.rsplit("/", 1)[1], log, cancel, sha256=sha256,
                       approval=tool_install(key, tools_dir))
    if key == "7zr-win":
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(archive, dest / "7zr.exe")
    elif archive.suffix.lower() == ".exe":
        # 7-Zip's own installer is a 7z archive (7zr opens it); LLVM's is an
        # NSIS installer, which the full 7-Zip opens.
        if key == "7zip-win":
            unpacker = fetch_windows_fallback("7zr-win", tools_dir, runner, log, cancel) / "7zr.exe"
        else:
            unpacker = fetch_windows_fallback("7zip-win", tools_dir, runner, log, cancel) / "7z.exe"
        if dest.exists():
            safe_rmtree(dest, tools_dir, [], log)
        runner.run([unpacker, "x", "-y", f"-o{dest}", archive], quiet=True)
        # NSIS installers carry an uninstaller and plugin folder; not needed.
        for leftover in ("Uninstall.exe", "$PLUGINSDIR"):
            p = dest / leftover
            if p.is_file():
                p.unlink()
    else:
        extract_archive(archive, dest, log)
    if not bindir.is_dir() or not any(bindir.iterdir()):
        raise SetupError(f"{archive.name} didn't contain {sub}/ as expected.")
    (dest / ".verified").write_text(sha256, encoding="ascii")
    return bindir


def fetch_portable(key: str, tools_dir: Path, runner: Runner, log: Log, cancel: threading.Event) -> Path:
    url, sub, sha256 = PORTABLE[key]
    dest = tools_dir / key
    bindir = dest / sub
    if bindir.exists() and any(bindir.iterdir()) and _portable_ok(tools_dir, key):
        return bindir
    archive = download(url, tools_dir / "downloads" / url.rsplit("/", 1)[1], log, cancel, sha256=sha256,
                       approval=tool_install(key, tools_dir))
    if archive.suffix.lower() == ".appimage":
        dest.mkdir(parents=True, exist_ok=True)
        target = dest / archive.name
        shutil.copyfile(archive, target)
        target.chmod(0o755)
        (dest / ".verified").write_text(sha256, encoding="ascii")
        return dest
    extract_archive(archive, dest, log)
    if not IS_WINDOWS:
        for p in bindir.iterdir():
            if p.is_file():
                p.chmod(p.stat().st_mode | 0o111)
    (dest / ".verified").write_text(sha256, encoding="ascii")
    return bindir


def plan(tc: Toolchain, tools_dir: Path, allow_install: bool, recomp: bool = True, windows_exe: bool = False,
         rust: bool = True, shown_dir: Optional[Path] = None, archives: bool = True) -> List[Install]:
    """What ensure() is about to download or install, for one approval up
    front. Something it can't foresee (say, a too-old CMake from the package
    manager) is asked for when it comes up. `shown_dir` is the tools folder
    as the user knows it (tools_dir may be the build's space-free link).
    Without `archives` (only Rust programs are built), no 7-Zip."""
    items: List[Install] = []
    shown = shown_dir or tools_dir

    def want(key: str) -> None:
        if not _portable_ok(tools_dir, key):
            items.append(tool_install(key, shown))

    def windows_tools() -> None:
        if not tc.sevenzip:
            want("7zr-win")
            want("7zip-win")
        if not tc.git_ok():
            want("git-win")
        if not tc.cc or tc.clang_version < MIN_CLANG:
            if not tc.sevenzip:
                want("7zr-win")
                want("7zip-win")   # unpacks LLVM's installer
            want("llvm-win")

    def linux_pkgs(minimal: bool) -> None:
        pm = _linux_package_manager()
        if pm and not immutable_system():
            items.append(linux_packages_install(pm, linux_packages(pm, minimal, windows_exe)))

    rustup = _which(["rustup"], [cargo_bin_dir()])
    if not recomp:
        if allow_install and IS_LINUX and (not tc.linker or (windows_exe and not tc.mingw)):
            linux_pkgs(minimal=True)
        if allow_install and (not tc.cargo or tc.rust_version < MIN_RUST):
            items.append(rust_install(shown, update=bool(rustup)))
        if rust and allow_install and tc.needs_msvc_linker() and not _common.UNDER_WINE:
            items.append(VS_INSTALL)   # this Rust links with Visual Studio's link.exe
        if not tc.sevenzip and archives:
            if IS_LINUX:
                want("7zip-linux")
            elif IS_WINDOWS and allow_install:
                want("7zr-win")
                want("7zip-win")
        return items
    cpp_problems = [p for p in tc.problems() if p not in tc.rust_problems()]
    packages = False
    if allow_install and (cpp_problems or (IS_LINUX and ((rust and not tc.linker) or
                                                        (windows_exe and not tc.mingw)))):
        if IS_LINUX:
            linux_pkgs(minimal=False)
            packages = bool(items)
        elif IS_WINDOWS:
            windows_tools()
            if tc.vs_missing() and not _common.UNDER_WINE:
                items.append(VS_INSTALL)
    # The package manager brings CMake and Ninja; without it, the portable ones.
    if not packages:
        if not tc.cmake or tc.cmake_version < MIN_CMAKE:
            want("cmake-win" if IS_WINDOWS else "cmake-linux")
        if not tc.ninja:
            want("ninja-win" if IS_WINDOWS else "ninja-linux")
        if IS_LINUX and not tc.sevenzip:
            want("7zip-linux")
        if IS_LINUX and allow_install and (not tc.cc or tc.clang_version < MIN_CLANG):
            want("llvm-linux")
    if IS_WINDOWS and allow_install:
        windows_tools()
    if IS_WINDOWS and not tc.innoextract:
        want("innoextract-win")
    if rust and allow_install and (not tc.cargo or tc.rust_version < MIN_RUST):
        items.append(rust_install(shown, update=bool(rustup)))
    return items


def ensure(tools_dir: Path, runner: Runner, log: Log, cancel: threading.Event,
           allow_install: bool, recomp: bool = True, windows_exe: bool = False, rust: bool = True,
           archives: bool = True) -> Toolchain:
    """Detects the tools and installs what's missing (when allowed). Without
    `recomp`, only the ripper's tools (Rust, 7-Zip) are wanted; without
    `rust`, Rust isn't installed (the game-only build inside WSL). With
    `windows_exe` (Linux), also what building the Windows ripper needs.
    Without `archives` (with `recomp` off: only Rust programs that open no
    archives, such as newcopy), 7-Zip isn't wanted either."""
    tc = detect(tools_dir, runner)
    log.info("Found:\n" + tc.summary())
    APPROVAL.require(plan(tc, tools_dir, allow_install, recomp, windows_exe, rust, archives=archives), log)
    if not recomp:
        if allow_install and IS_LINUX and (not tc.linker or (windows_exe and not tc.mingw)):
            try:
                install_linux(tc, runner, log, minimal=True, windows_exe=windows_exe)
            except SetupError as e:
                log.warn(str(e))
            tc = detect(tools_dir, runner)
        if allow_install and (not tc.cargo or tc.rust_version < MIN_RUST):
            install_rust(tools_dir, runner, log, cancel, have_msvc=bool(tc.vs_path))
        if rust and allow_install and IS_WINDOWS and not _common.UNDER_WINE and \
                detect(tools_dir, runner).needs_msvc_linker():
            # The PC's own Rust links with Microsoft's link.exe, which only the
            # Build Tools bring (a Rust this tool installs uses the GNU linker then).
            install_vs_build_tools(tools_dir, runner, log, cancel, shutil.which("winget"))
        if not tc.sevenzip and archives:
            if IS_LINUX:
                fetch_portable("7zip-linux", tools_dir, runner, log, cancel)
            elif IS_WINDOWS and allow_install:
                fetch_windows_fallback("7zip-win", tools_dir, runner, log, cancel)
        return detect(tools_dir, runner)
    cpp_problems = [p for p in tc.problems() if p not in tc.rust_problems()]
    # 7-Zip has a portable fallback and innoextract is optional, so neither
    # alone is worth a root install on every run.
    if allow_install and (cpp_problems or (IS_LINUX and ((rust and not tc.linker) or
                                                        (windows_exe and not tc.mingw)))):
        if IS_LINUX:
            try:
                install_linux(tc, runner, log, windows_exe=windows_exe)
            except SetupError as e:
                log.warn(str(e))
        elif IS_WINDOWS:
            install_windows(tc, tools_dir, runner, log, cancel)
        tc = detect(tools_dir, runner)

    # Portable fall-backs that need no admin rights.
    if not tc.cmake or tc.cmake_version < MIN_CMAKE:
        fetch_portable("cmake-win" if IS_WINDOWS else "cmake-linux", tools_dir, runner, log, cancel)
    if not tc.ninja:
        fetch_portable("ninja-win" if IS_WINDOWS else "ninja-linux", tools_dir, runner, log, cancel)
    if IS_LINUX and not tc.sevenzip:
        fetch_portable("7zip-linux", tools_dir, runner, log, cancel)
    if IS_WINDOWS and allow_install:
        # The pinned downloads (no admin rights, into tools/).
        if not tc.sevenzip:
            fetch_windows_fallback("7zip-win", tools_dir, runner, log, cancel)
        if not tc.git_ok():
            fetch_windows_fallback("git-win", tools_dir, runner, log, cancel)
        if not tc.cc or tc.clang_version < MIN_CLANG:
            fetch_windows_fallback("llvm-win", tools_dir, runner, log, cancel)
    if IS_WINDOWS and not tc.innoextract:
        try:
            fetch_portable("innoextract-win", tools_dir, runner, log, cancel)
        except (SetupError, OSError) as e:
            log.warn(f"innoextract download failed ({e}); Inno Setup map installers will need 7-Zip instead.")
    if IS_LINUX and (not tc.cc or tc.clang_version < MIN_CLANG) and allow_install:
        log.warn("No usable clang from the package manager; downloading the official LLVM 20 release "
                 "(about 2 GB, no root needed).")
        fetch_portable("llvm-linux", tools_dir, runner, log, cancel)
    tc = detect(tools_dir, runner)
    if rust and allow_install and (not tc.cargo or tc.rust_version < MIN_RUST):
        # After the C++ tools, so on Windows rustup can use the Visual Studio linker.
        install_rust(tools_dir, runner, log, cancel, have_msvc=bool(tc.vs_path))
        tc = detect(tools_dir, runner)
    log.info("Toolchain:\n" + tc.summary())
    if 0 < tc.clang_version < PREFERRED_CLANG:
        log.warn(f"clang {tc.clang_version} found; the SDK is tested with clang {PREFERRED_CLANG}.")
    return tc


def find_appimagetool(tools_dir: Path, runner: Runner, log: Log, cancel: threading.Event) -> Optional[Path]:
    """The pinned appimagetool AppImage. Always this one, not one installed on
    the system: the game's AppImage reuses its runtime (see pipeline)."""
    try:
        d = fetch_portable("appimagetool", tools_dir, runner, log, cancel)
        return d / "appimagetool-x86_64.AppImage"
    except (SetupError, OSError) as e:
        log.warn(f"Could not get appimagetool ({e}).")
        return None
