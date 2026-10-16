"""What "Check this PC" says besides found / missing.

For every missing or wrong tool: why the build needs it and the exact fix.
For the PC itself (mostly Windows): the things that make a build fail or
crawl there, such as a project folder that OneDrive syncs, the Microsoft
Store's Python, a proxy that Git doesn't know about, or Windows' 260-character
path limit. The same notes go into the setup log, so a user's log is enough
to tell what went wrong.

And after a command fails: a plain hint for the errors whose cause is known
(failure_hint), so the message says what to do, not only "exit code 1".

Nothing here installs anything or changes a setting: it only looks.
"""
from __future__ import annotations

import os
import platform
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional

from . import common

# Notes on how bad something is: the build can't work, it may fail or crawl,
# or it's worth knowing.
ERROR, WARN, INFO = "error", "warn", "info"

PYTHON_ORG = "https://www.python.org/downloads/windows/"
VS_BUILD_TOOLS_PAGE = "https://visualstudio.microsoft.com/visual-cpp-build-tools/"
# The short folder the build reaches a long work folder through (common.build_path).
SHORT_LINK_HINT = r"C:\c3w-work"
# Space a first full build takes, roughly: the SDK and the game's objects in
# the work folder (pipeline.WORK_NEEDED_RECOMP plus the ripper's), and the
# rips and the game in the output folder.
WORK_SPACE_GB = 8
OUTPUT_SPACE_GB = 12


@dataclass
class Note:
    level: str          # ERROR, WARN or INFO
    title: str          # one line: what it is
    detail: str = ""    # why it matters
    fix: str = ""       # what to do about it

    def text(self) -> str:
        """The note as plain text, for the log and the command line."""
        head = {"error": "PROBLEM", "warn": "WARNING", "info": "NOTE"}.get(self.level, "NOTE")
        parts = [f"{head}: {self.title}"]
        if self.detail:
            parts.append(f"  Why: {self.detail}")
        if self.fix:
            parts.append(f"  Fix: {self.fix}")
        return "\n".join(parts)


# ---------------------------------------------------------------------------
# Tools: why each is needed and how to get it
# ---------------------------------------------------------------------------
def tool_notes(tc, recomp: bool = True, rust: bool = True) -> List[Note]:
    """A note for each problem tc.problems() reports, with the fix."""
    from . import deps   # deps imports nothing from here; kept local to stay import-light
    notes: List[Note] = []
    win = common.IS_WINDOWS
    build_button = "Press Build (or \"Install missing\" here): it lists the downloads, and after your approval " \
                   "puts them in work\\tools in the project folder."
    if rust:
        if not tc.cargo or tc.rust_version < deps.MIN_RUST:
            have = f" (found {'.'.join(map(str, tc.rust_version))})" if tc.cargo and tc.rust_version else ""
            notes.append(Note(ERROR, f"Rust {'.'.join(map(str, deps.MIN_RUST))} or newer{have}",
                              "The ripper (engine/tools/rip), which turns the game's files into open formats, is "
                              "written in Rust. A release download with engine/bin/ has it built already.",
                              build_button if win else "Press Build: it installs Rust with rustup into "
                              "work/tools/rust after your approval (or install it from https://rustup.rs)."))
        if win and tc.cargo and tc.needs_msvc_linker():
            notes.append(Note(ERROR, "Rust here links with Microsoft's linker (link.exe), which isn't installed",
                              f"This Rust ({tc.rust_host}) uses the Visual Studio Build Tools' linker, and they "
                              "aren't on this PC.",
                              "Leave \"Also build the playable Call of Duty 3\" on and press Build: it installs "
                              "the Build Tools (with your approval). Or install them yourself from "
                              f"{VS_BUILD_TOOLS_PAGE} (\"Desktop development with C++\"), or switch Rust to its "
                              "GNU toolchain: rustup default stable-x86_64-pc-windows-gnu"))
        if common.IS_LINUX and not tc.linker:
            notes.append(Note(ERROR, "A C compiler for linking (cc)",
                              "Rust links the ripper through the system's C compiler.",
                              "Install your distribution's build-essential (Debian, Ubuntu, Mint), base-devel "
                              "(Arch) or gcc (Fedora, openSUSE); Build does it with your approval."))
    if not recomp:
        return notes
    if not tc.git or (win and tc.git_version and tc.git_version < deps.MIN_GIT_WINDOWS):
        old = f" (found {'.'.join(map(str, tc.git_version))}, too old)" if tc.git else ""
        notes.append(Note(ERROR, f"Git{old}",
                          "Git downloads the ReXGlue SDK's source (about 1.5 GB with its parts).",
                          build_button + " (MinGit, Git for Windows' portable version)" if win else
                          "Install git with your package manager; Build does it with your approval."))
    if not tc.cmake or tc.cmake_version < deps.MIN_CMAKE:
        have = f" (found {'.'.join(map(str, tc.cmake_version))})" if tc.cmake and tc.cmake_version else ""
        notes.append(Note(ERROR, f"CMake {'.'.join(map(str, deps.MIN_CMAKE))} or newer{have}",
                          "CMake sets up the SDK's and the game's builds.",
                          build_button if win else "Build downloads a portable CMake into work/tools after "
                          "your approval."))
    if not tc.ninja:
        notes.append(Note(ERROR, "Ninja", "Ninja runs the compiler for every source file.",
                          build_button if win else "Build downloads it into work/tools after your approval."))
    if not tc.cc or tc.clang_version < deps.MIN_CLANG:
        have = f" (found clang {tc.clang_version})" if tc.cc and tc.clang_version else ""
        notes.append(Note(ERROR, f"Clang {deps.MIN_CLANG} or newer{have}",
                          "The SDK and the recompiled game are built with clang (the SDK's own choice)."
                          + (" On Windows it must be the x86-64 MSVC kind (LLVM's Windows release): MSYS2's "
                             "clang and 32-bit LLVM can't use Visual Studio's libraries." if win else ""),
                          build_button + " (LLVM 20, unpacked, its installer isn't run)" if win else
                          "Build installs clang from your distribution, or LLVM 20 into work/tools."))
    if win:
        if not tc.vs_path:
            notes.append(Note(ERROR, "Visual Studio Build Tools with C++ (MSVC x64 tools and a Windows SDK)",
                              "Clang on Windows uses Microsoft's C++ library, linker (link.exe), resource "
                              "compiler and the Windows SDK's headers and libraries (Direct3D 12, DXGI, XInput). "
                              "Visual Studio without the C++ parts doesn't count.",
                              "Press Build or \"Install missing\": after your approval it installs the Build "
                              "Tools 2022 with Microsoft's installer (a UAC prompt appears; 3-6 GB in Program "
                              "Files, the only thing outside the project folder). Or install them yourself from "
                              f"{VS_BUILD_TOOLS_PAGE}: workload \"Desktop development with C++\", with \"MSVC "
                              "v143 x64/x86 build tools\" and a \"Windows 11 SDK\"."))
        elif tc.vs_env_error:
            notes.append(Note(ERROR, "Visual Studio is installed, but its build environment didn't start",
                              f"vcvars64.bat in {tc.vs_path} failed: {tc.vs_env_error}",
                              "Open the Visual Studio Installer, choose Repair on that installation (or Modify "
                              "and tick \"Desktop development with C++\"), then build again."))
        elif not tc.windows_sdk:
            notes.append(Note(ERROR, "The Windows SDK (part of the Visual Studio Build Tools)",
                              f"Visual Studio in {tc.vs_path} has the C++ compiler but no complete Windows SDK "
                              "(windows.h, d3d12.h, dxgi1_6.h, Xinput.h and their .lib files).",
                              "Press Build or \"Install missing\": it adds the Windows 11 SDK to the Build Tools "
                              "(UAC prompt). Or in the Visual Studio Installer: Modify, Individual components, "
                              "tick \"Windows 11 SDK (10.0.22621.0)\" or newer."))
    if common.IS_LINUX and tc.missing_libs:
        notes.append(Note(ERROR, "Development libraries: " + ", ".join(tc.missing_libs),
                          "The SDK's window, sound and input code (SDL3, GTK file dialogs, Vulkan) needs them.",
                          "Build installs them with your package manager after your approval."))
    return notes


# ---------------------------------------------------------------------------
# The PC itself
# ---------------------------------------------------------------------------
def python_kind(executable: Optional[str] = None, prefix: Optional[str] = None) -> str:
    """Where this Python comes from: "this project (setup.bat)", "Microsoft
    Store", "embeddable", "conda", "MSYS2", "python.org or another installer"
    (Windows), or "" elsewhere."""
    if not common.IS_WINDOWS:
        return ""
    exe = (executable or sys.executable or "").replace("/", "\\")
    pre = (prefix or sys.base_prefix or "").replace("/", "\\")
    low = (exe + "|" + pre).lower()
    if "\\windowsapps\\" in low or "pythonsoftwarefoundation.python" in low:
        return "Microsoft Store"
    if (Path(pre) / "conda-meta").exists():
        return "conda"
    if "msys" in low or "mingw" in sys.version.lower():
        return "MSYS2"
    try:
        here = common.project_dir() / "work" / "tools" / "python"
        if common.is_within(Path(exe), here):
            return "this project (setup.bat)"
    except (OSError, ValueError):
        pass
    if exe and any(Path(exe).parent.glob("python*._pth")):
        return "embeddable"
    return "python.org or another installer"


def python_line() -> str:
    bits = 64 if sys.maxsize > 2 ** 32 else 32
    kind = python_kind()
    return (f"Python {platform.python_version()} ({bits}-bit{', ' + kind if kind else ''}) "
            f"at {sys.executable}")


def is_arm64_windows() -> bool:
    """Windows on ARM, also when this Python is an x64 one running emulated."""
    if not common.IS_WINDOWS:
        return False
    ids = " ".join(os.environ.get(v, "") for v in ("PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITEW6432",
                                                     "PROCESSOR_IDENTIFIER"))
    return "ARM64" in ids.upper() or "ARMV8" in ids.upper() or platform.machine().upper() == "ARM64"


def long_paths_enabled() -> Optional[bool]:
    """Windows' LongPathsEnabled setting (None when it can't be read)."""
    if not common.IS_WINDOWS:
        return None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem") as k:
            return bool(winreg.QueryValueEx(k, "LongPathsEnabled")[0])
    except (ImportError, OSError):
        return None


def onedrive_folder(path: Path) -> Optional[Path]:
    """The OneDrive folder `path` is in, if any (Windows' OneDrive variables)."""
    for var in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial"):
        root = os.environ.get(var)
        if root and common.is_within(path, Path(root)):
            return Path(root)
    return None


def controlled_folder_access() -> Optional[bool]:
    """Windows Security's "Controlled folder access" (ransomware protection),
    which stops programs it doesn't know from writing in Documents, Desktop,
    Pictures, Videos and Music. None when the setting can't be read."""
    if not common.IS_WINDOWS:
        return None
    try:
        import winreg
        key = (r"SOFTWARE\Microsoft\Windows Defender\Windows Defender Exploit Guard"
               r"\Controlled Folder Access")
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
            return winreg.QueryValueEx(k, "EnableControlledFolderAccess")[0] == 1
    except (ImportError, OSError):
        return None


def in_protected_user_folder(path: Path) -> bool:
    """`path` is in a folder Controlled folder access guards by default."""
    home = os.environ.get("USERPROFILE")
    if not home:
        return False
    return any(common.is_within(path, Path(home) / sub)
               for sub in ("Documents", "Desktop", "Pictures", "Videos", "Music"))


def windows_proxy_set() -> bool:
    """A proxy server is switched on in Windows' Internet settings."""
    if not common.IS_WINDOWS:
        return False
    try:
        import winreg
        key = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
            enabled = bool(winreg.QueryValueEx(k, "ProxyEnable")[0])
            try:
                auto = bool(winreg.QueryValueEx(k, "AutoConfigURL")[0])
            except OSError:
                auto = False
            return enabled or auto
    except (ImportError, OSError):
        return False


def proxy_env_set() -> bool:
    return any(os.environ.get(v) for v in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"))


def path_trouble(path: Path) -> List[str]:
    """What about a folder's path some Windows build tools choke on."""
    text = str(path)
    out = []
    if " " in text:
        out.append("spaces")
    if not text.isascii():
        out.append("letters outside plain English (like é or ж)")
    if len(text) > 60:
        out.append(f"{len(text)} characters")
    return out


def system_notes(work: Path, out: Optional[Path] = None) -> List[Note]:
    """Notes about this PC and its folders (Windows; on Linux only the disk)."""
    notes: List[Note] = []
    work = Path(work)
    if common.IS_WINDOWS:
        notes += _windows_python_notes()
        if is_arm64_windows():
            notes.append(Note(WARN, "Windows on ARM",
                              "The game is built for x86-64; Windows 11 on ARM runs it, and the build tools, "
                              "through its x64 emulation. This hasn't been tested and is much slower.",
                              "Use the 64-bit x86 (x64) Python from python.org (setup.bat picks it for you); the "
                              "ARM64 Python can't run this build."))
        for label, folder in (("project", common.project_dir()), ("work", work), ("output", out)):
            if folder is None:
                continue
            root = onedrive_folder(Path(folder))
            if root:
                # (Not the OneDrive folder's name: a work account's names the company.)
                notes.append(Note(WARN, f"The {label} folder is inside OneDrive",
                                  "OneDrive uploads every file the build writes (tens of thousands, several GB) "
                                  "and locks files while it does, which makes builds fail with \"Access is "
                                  "denied\" or \"being used by another process\". Files kept online-only also "
                                  "have to download first.",
                                  "Move the whole project folder somewhere OneDrive doesn't sync, for example "
                                  "C:\\Games\\COD3, and start setup.bat from there (or pause OneDrive while "
                                  "building)."))
        guarded = [label for label, folder in (("project", common.project_dir()), ("work", work), ("output", out))
                   if folder is not None and in_protected_user_folder(Path(folder))]
        if guarded and controlled_folder_access():
            notes.append(Note(WARN, f"Controlled folder access is on, and the {' and '.join(guarded)} folder is in "
                                    "Documents, Desktop or another folder it guards",
                              "Windows Security then stops Python, Git, CMake and the compilers from writing there "
                              "(\"Access is denied\", and a \"Unauthorized changes blocked\" notice).",
                              "Move the project folder to a folder like C:\\Games\\COD3, or allow the programs in "
                              "Windows Security, Virus & threat protection, Ransomware protection."))
        trouble = path_trouble(work)
        if trouble:
            notes.append(Note(INFO, f"The work folder's path has {', '.join(trouble)}",
                              "Some build tools choke on that, and Windows limits most paths to 260 characters.",
                              f"Nothing to do: the build reaches it through a short link ({SHORT_LINK_HINT} or "
                              "c3w-work in your AppData\\Local folder), made without admin rights. If that link "
                              "can't be made, move the project folder to a short plain path like C:\\Games\\COD3."))
        lp = long_paths_enabled()
        if lp is False:
            notes.append(Note(INFO, "Windows' long path support is off (the default)",
                              "Paths over 260 characters fail in some tools. Git is told to allow them, and the "
                              "build goes through a short link when the work folder's path is long.",
                              "Usually nothing. If a build stops with \"Filename too long\", move the project "
                              "folder to a short path like C:\\Games\\COD3."))
        if windows_proxy_set() and not proxy_env_set():
            notes.append(Note(WARN, "Windows uses a proxy server, but HTTPS_PROXY isn't set",
                              "The setup tool's own downloads follow Windows' proxy setting, but Git (the SDK "
                              "download) and CMake don't: they need the HTTPS_PROXY variable.",
                              "If the SDK download fails, open a Command Prompt, run  set "
                              "HTTPS_PROXY=http://<proxy>:<port>  (your proxy's address), then start setup.bat "
                              "from that same window."))
        if common.UNDER_WINE:
            notes.append(Note(INFO, f"Running under Wine ({common.wine_version()})",
                              "The rips work here. Visual Studio's Build Tools don't install under Wine, so the "
                              "playable game isn't compiled here.",
                              "On Linux, run sh setup.sh: it builds the Windows version (cod3.exe) natively."))
    try:
        free = common.free_space(work)
    except OSError:
        free = -1
    if 0 <= free < WORK_SPACE_GB * 1024 ** 3:
        notes.append(Note(WARN, f"Only {common.human(free)} free on the work folder's drive",
                          f"A first full build needs about {WORK_SPACE_GB} GB there for the SDK and the compiled "
                          f"game, plus about {OUTPUT_SPACE_GB} GB in the output folder.",
                          "Free some space, or pick a work folder on another drive (Set up page, \"Show more "
                          "options\", Work folder)."))
    return notes


def _windows_python_notes() -> List[Note]:
    notes = []
    if sys.maxsize <= 2 ** 32:
        notes.append(Note(ERROR, "This Python is 32-bit",
                          "A 32-bit program on 64-bit Windows sees another System32 folder (no wsl.exe) and can't "
                          "start every tool the build needs.",
                          f"Install the 64-bit Python from {PYTHON_ORG} (or delete the 32-bit one and run "
                          "setup.bat, which installs a 64-bit one into work\\tools\\python)."))
    kind = python_kind()
    if kind == "Microsoft Store":
        notes.append(Note(WARN, "Python from the Microsoft Store",
                          "The Store's Python runs in a sandbox that keeps some of what it writes in AppData to "
                          "itself, so other programs may not see it. The setup tool avoids AppData for the build, "
                          "but if a build says a file or folder isn't there, this is the likely cause.",
                          f"Install Python from {PYTHON_ORG} (tick \"tcl/tk and IDLE\"), or uninstall the Store "
                          "Python and run setup.bat, which installs one into work\\tools\\python."))
    elif kind == "embeddable":
        notes.append(Note(WARN, "The embeddable Python",
                          "It has no Tkinter, so there is no window, only the command line.",
                          "Run setup.bat: it installs a full Python into work\\tools\\python."))
    return notes


def system_lines(tc=None) -> List[str]:
    """Versions and paths for the log: the setup tool's view of this PC.
    (Bug reports scrub user and computer names: c3w/logtools.py.)"""
    from .pipeline import TOOL_VERSION
    lines = [f"Setup tool {TOOL_VERSION}, {python_line()}",
             f"System: {platform.platform()} ({platform.machine() or 'unknown CPU'}), {common.cpu_count()} CPUs, "
             f"{common.total_ram_gb():.1f} GB RAM"]
    if common.IS_WINDOWS:
        lp = long_paths_enabled()
        lines.append("Windows long paths: " + ("on" if lp else "off (the default)" if lp is False else "unknown")
                     + f"; winget: {'yes' if _which('winget') else 'no'}"
                     + f"; proxy: {'Windows setting' if windows_proxy_set() else 'none'}"
                     + (", HTTPS_PROXY set" if proxy_env_set() else ""))
        if is_arm64_windows():
            lines.append("Windows on ARM (x64 emulation)")
    return lines


def _which(name: str) -> Optional[str]:
    import shutil
    return shutil.which(name)


# ---------------------------------------------------------------------------
# After a failure
# ---------------------------------------------------------------------------
# (what the output says, lower case) -> what to do. The first match wins, so
# the specific ones come first.
_WINDOWS_HINTS = [
    (("stl1000", "unexpected compiler version"),
     "Visual Studio's C++ library needs a newer clang than the one found. Remove the older LLVM from PATH (or "
     "uninstall it) so the setup tool uses its own LLVM 20 (it downloads it with your approval), or install "
     "LLVM 20 or newer from https://github.com/llvm/llvm-project/releases."),
    (("filename too long", "file name too long", "path too long", "filename or extension is too long",
      "max_path"),
     "A path got longer than Windows allows (260 characters). Move the project folder to a short plain path "
     "such as C:\\Games\\COD3 and build again; or turn on long paths (Windows 11: Settings > System > For "
     "developers > Enable long paths; Windows 10: the LongPathsEnabled registry value, needs admin)."),
    (("ssl certificate problem", "certificate verify failed", "unable to get local issuer certificate",
      "self signed certificate", "self-signed certificate"),
     "The HTTPS connection was intercepted (an antivirus's web shield or a company proxy). Let Git use Windows' "
     "own certificates by running, in a Command Prompt:  git config --global http.sslBackend schannel  (or "
     "switch off the antivirus's HTTPS scanning while building), then build again."),
    (("could not resolve host", "failed to connect to", "connection timed out", "connection was reset",
      "connection reset by peer", "early eof", "rpc failed", "unable to access 'https"),
     "The download was cut off or couldn't connect. Check the internet connection and build again; it "
     "continues where it stopped. Behind a proxy, set HTTPS_PROXY in a Command Prompt (set "
     "HTTPS_PROXY=http://<proxy>:<port>) and start setup.bat from that window."),
    (("not enough space on the disk", "no space left on device", "there is not enough space"),
     "The disk is full. Free some space (a first build needs about 8 GB in the work folder and 12 GB in the "
     "output folder), or pick a work folder on another drive, and build again."),
    (("being used by another process", "access is denied", "permission denied", "lnk1104", "unable to unlink",
      "device or resource busy", "the process cannot access the file"),
     "Windows wouldn't let the build change a file: it's open somewhere. Close the game (cod3.exe) and any "
     "File Explorer window inside the work folder, wait for OneDrive or the antivirus to let go of it (move the "
     "project folder out of OneDrive; add it to the antivirus's exclusions if this keeps happening), and build "
     "again."),
    (("'windows.h' file not found", "cannot open include file", "kernel32.lib", "lnk1181", "lnk1158",
      "no cmake_rc_compiler", "rc.exe", "could not find the windows sdk", "msvcrt.lib", "ucrt.lib"),
     "Visual Studio's Build Tools or their Windows SDK are incomplete. In the Visual Studio Installer, choose "
     "Modify on the Build Tools and tick \"Desktop development with C++\" with \"MSVC v143 x64/x86 build tools\" "
     "and \"Windows 11 SDK\" (or press Repair), then build again."),
    (("linker `link.exe` not found", "link.exe' not found", "link.exe not found"),
     "Microsoft's linker (link.exe) is missing: install the Visual Studio Build Tools with \"Desktop "
     "development with C++\" (Build does it with your approval)."),
    (("out of memory", "virtual memory", "c1060", "std::bad_alloc", "killed signal"),
     "The compiler ran out of memory. Lower \"Build threads\" (Set up page, \"Show more options\"; each takes "
     "about 2 GB) and build again; it continues where it stopped."),
    (("is not recognized as an internal or external command", "program not found"),
     "A program the build calls isn't there. Use \"Check this PC\" on the Tools page to see what's missing, "
     "then build again (it installs what it can with your approval)."),
]


def failure_hint(lines: Iterable[str]) -> str:
    """A plain hint for a failed command's output, when its cause is known
    (Windows; "" when nothing matches)."""
    if not common.IS_WINDOWS:
        return ""
    text = "\n".join(lines).lower()
    for needles, hint in _WINDOWS_HINTS:
        if any(n in text for n in needles):
            return "What to do: " + hint
    return ""


# The Visual Studio installer's exit codes (Microsoft: "Use command-line
# parameters to install Visual Studio", exit codes).
VS_INSTALLER_CODES = {
    0: "done",
    740: "it needs administrator rights (approve the UAC prompt, or start setup.bat as administrator)",
    1001: "the Visual Studio Installer is already running (close it and build again)",
    1003: "Visual Studio is in use (close it and build again)",
    1602: "it was canceled (the UAC prompt or the installer window was closed)",
    1618: "another installation is running (wait for it, or restart Windows, and build again)",
    1641: "done; Windows needs a restart",
    3010: "done; Windows needs a restart",
    5003: "the installer couldn't download its own files (check the internet connection)",
    8006: "Visual Studio programs are running (close them and build again)",
}


def vs_installer_result(rc: int) -> str:
    meaning = VS_INSTALLER_CODES.get(rc)
    if meaning:
        return f"exit code {rc}: {meaning}"
    return (f"exit code {rc}; Microsoft's installer logs are the dd_*.log files in your TEMP folder "
            "(type %TEMP% in File Explorer's address bar)")


__all__ = ["Note", "ERROR", "WARN", "INFO", "tool_notes", "system_notes", "system_lines", "python_kind",
           "python_line", "is_arm64_windows", "long_paths_enabled", "onedrive_folder", "path_trouble",
           "failure_hint", "vs_installer_result"]
