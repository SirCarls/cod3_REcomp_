"""The Linux version (AppImage) built on a Windows PC, inside WSL 2.

WSL 2 (Windows Subsystem for Linux) runs a real Linux in a light virtual
machine that ships with Windows 10 and 11. The setup tool gives it a private
Ubuntu of its own, named `c3w-builder`:

- the image is Canonical's official Ubuntu 24.04 WSL image, checked against
  the SHA-256 list Canonical publishes next to it;
- it is imported with `wsl --import` (no Microsoft Store, no user account to
  create; the build runs as that Ubuntu's root, which can't touch Windows
  beyond the folders it is given);
- inside it, this same tool runs its normal Linux build with
  `build --binaries-only --linux`, reading the game files the Windows side
  already extracted and writing the AppImage into the same output folder;
- it is removed again (`wsl --unregister`) at the cleanup, unless "keep the
  built SDK" is on, which keeps it for faster rebuilds.

Turning WSL itself on needs administrator rights and a restart, so the tool
never does that: when WSL isn't ready it says what to run, and the Windows
version is unaffected. Nothing here runs Wine or any game, patch or mod
installer.
"""
from __future__ import annotations

import os
import shutil
import threading
from pathlib import Path, PureWindowsPath
from typing import List, Optional

from . import deps
from .common import Cancelled, Log, Runner, SetupError

DISTRO = "c3w-builder"
WORK_INSIDE = "/root/c3w-work"   # the build's work folder, on the Linux disk (much faster than /mnt/c)

HOW_TO_ENABLE = (
    "The Linux version is built inside WSL 2 (Windows Subsystem for Linux), which isn't set up on this PC. "
    "To turn it on: open PowerShell as administrator, run\n"
    "    wsl --install --no-distribution\n"
    "restart Windows, and build again. (WSL 2 also needs virtualization enabled in the BIOS/UEFI; most PCs "
    "have it on.)")


def wsl_exe() -> Optional[Path]:
    found = shutil.which("wsl.exe") or shutil.which("wsl")
    if found:
        return Path(found)
    root = os.environ.get("SystemRoot")
    if root:
        p = Path(root) / "System32" / "wsl.exe"
        if p.is_file():
            return p
    return None


def _env() -> dict:
    # wsl.exe writes UTF-16 unless asked for UTF-8.
    return dict(os.environ, WSL_UTF8="1")


def clean_output(text: str) -> str:
    """wsl.exe output, readable whichever encoding it used (UTF-16 shows up as
    NUL bytes between the letters when read as UTF-8)."""
    return text.replace("\x00", "").replace("\ufeff", "")


def parse_distro_list(text: str) -> List[str]:
    """`wsl --list --quiet`: one distribution name per line."""
    return [line.strip() for line in clean_output(text).splitlines() if line.strip()]


def to_wsl_path(p: Path) -> str:
    """C:\\Games\\out -> /mnt/c/Games/out (WSL's default drive mounts)."""
    w = PureWindowsPath(str(p))
    if not w.drive or not w.drive.endswith(":"):
        raise SetupError(f"{p} isn't on a drive letter, so the Linux build inside WSL can't reach it. "
                         "Pick an output folder on a local drive (for example C:).")
    rest = "/".join(part for part in w.parts[1:])
    return f"/mnt/{w.drive[0].lower()}/{rest}".rstrip("/")


class Wsl:
    def __init__(self, runner: Runner, log: Log, cancel: threading.Event):
        self.runner, self.log, self.cancel = runner, log, cancel
        exe = wsl_exe()
        if not exe:
            raise SetupError(HOW_TO_ENABLE)
        self.exe = exe

    def _capture(self, *args: str, timeout: int = 120) -> tuple:
        rc, out = self.runner.capture([self.exe, *args], env=_env(), timeout=timeout)
        return rc, clean_output(out)

    def check(self) -> None:
        """WSL is installed and can run WSL 2 machines."""
        rc, out = self._capture("--status")
        if rc != 0:
            raise SetupError(HOW_TO_ENABLE)

    def installed(self) -> List[str]:
        rc, out = self._capture("--list", "--quiet")
        return parse_distro_list(out) if rc == 0 else []

    def works(self) -> bool:
        rc, _ = self._capture("--distribution", DISTRO, "--user", "root", "--exec", "true", timeout=300)
        return rc == 0

    def ensure_distro(self, store: Path, downloads: Path) -> None:
        if DISTRO in self.installed():
            if self.works():
                self.log.info(f"Using the build Ubuntu in WSL ({DISTRO}).")
                return
            # Registered but broken (its disk was deleted, say): set it up again.
            self.log.warn(f"The build Ubuntu in WSL ({DISTRO}) doesn't start; setting it up again.")
            self.unregister()
        image = deps.fetch_wsl_image(downloads, self.log, self.cancel)
        store.mkdir(parents=True, exist_ok=True)
        self.log.info(f"Setting up a private Ubuntu in WSL 2 for the build ({DISTRO}, stored in {store})...")
        rc = self.runner.run([self.exe, "--import", DISTRO, str(store), str(image), "--version", "2"],
                             env=_env(), check=False)
        if rc != 0 or DISTRO not in self.installed():
            raise SetupError("WSL couldn't set up the build Ubuntu (wsl --import failed; see the log above). "
                             "If it mentions virtualization, enable it in the BIOS/UEFI; if WSL is older, run "
                             "'wsl --update' in PowerShell.")

    def run_root(self, args: List[str], cwd_linux: Optional[str] = None, quiet: bool = False,
                 line_filter=None) -> int:
        cmd = [self.exe, "--distribution", DISTRO, "--user", "root"]
        if cwd_linux:
            cmd += ["--cd", cwd_linux]
        # --exec: run the program directly, no Linux shell in between, so
        # quotes, $, & or ; in folder names are passed through untouched.
        cmd += ["--exec", *args]
        try:
            return self.runner.run(cmd, env=_env(), check=False, quiet=quiet, line_filter=line_filter)
        except BaseException:   # Cancel, Ctrl+C, an error: nothing keeps running in the build Ubuntu
            self.terminate()
            raise

    def terminate(self) -> None:
        """Stops everything running in the build Ubuntu (Cancel)."""
        self.runner.capture([self.exe, "--terminate", DISTRO], env=_env(), timeout=60)

    def unregister(self) -> None:
        """Deletes the build Ubuntu and its virtual disk."""
        if DISTRO in self.installed():
            self.log.info(f"Removing the build Ubuntu from WSL ({DISTRO}).")
            self.runner.capture([self.exe, "--unregister", DISTRO], env=_env(), timeout=300)
