#!/bin/sh
# COD_3_WAW Setup tool - bootstrap for Linux, and for Windows in Git Bash,
# MSYS2 or Cygwin.
# Makes sure Python 3 (3.9+) with Tkinter is installed, then starts the tool.
# Any arguments are passed through (e.g. `sh setup.sh doctor`, `sh setup.sh build --help`).
# If double-clicking does nothing (folder on a drive mounted without exec
# rights), open a terminal here and run:  sh setup.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

# Windows (Git Bash, MSYS2, Cygwin): the tool runs on Windows' own Python, the
# one setup.bat installs, so it builds the Windows version as setup.bat does.
case "$(uname -s 2>/dev/null || echo unknown)" in
  MINGW*|MSYS*|CYGWIN*)
    winpath() {
      if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
    }
    TOOL="$(winpath "$HERE/cod3_waw_setup.py")"
    # 64-bit too: a 32-bit Python can't start every build tool.
    CHECK='import sys, tkinter; sys.exit(sys.platform != "win32" or sys.version_info < (3, 9) or sys.maxsize < 2**32)'
    if command -v py >/dev/null 2>&1 && py -3 -c "$CHECK" 2>/dev/null; then
      exec py -3 "$TOOL" "$@"
    fi
    if command -v python >/dev/null 2>&1 && python -c "$CHECK" 2>/dev/null; then
      exec python "$TOOL" "$@"
    fi
    # No Windows Python yet: setup.bat installs it (winget) and starts the
    # tool. MSYS turns a lone "/c" into a path, so it gets "//c" there.
    echo "Python for Windows isn't installed yet; setup.bat installs it..."
    case "$(uname -s)" in
      CYGWIN*) exec cmd.exe /c "$(winpath "$HERE/setup.bat")" "$@" ;;
      *) exec cmd.exe //c "$(winpath "$HERE/setup.bat")" "$@" ;;
    esac
    ;;
esac

find_python() {
  for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(sys.version_info < (3, 9))' 2>/dev/null; then
      echo "$c"; return 0
    fi
  done
  return 1
}

# openSUSE MicroOS / Aeon: the root file system is read-only.
root_read_only() {
  awk '$2 == "/" { print $4 }' /proc/mounts 2>/dev/null | grep -qE '(^|,)ro(,|$)'
}

container_hint() {
  echo "Couldn't install Python with Tkinter here (no apt, dnf, pacman, zypper or xbps that can add packages)."
  echo "On an image-based system (SteamOS, Bazzite, Fedora Atomic, openSUSE Aeon, NixOS ...) run the tool"
  echo "inside a regular Linux container instead:"
  echo "    distrobox create --name cod3-build --image ubuntu:24.04"
  echo "    distrobox enter cod3-build"
  echo "    sh setup.sh"
  echo "Otherwise install Python 3.9+ with Tkinter yourself and run this again."
  exit 1
}

PY="$(find_python || true)"
if [ -z "$PY" ] || ! "$PY" -c 'import tkinter' 2>/dev/null; then
  echo "Python 3 with Tkinter is needed to run the setup tool. With your approval, your package"
  echo "manager installs it into the system's folders (the only place packages go); it asks for"
  echo "your password."
  if [ -t 0 ]; then
    printf "Install Python 3 with Tkinter now? [y/N] "
    read -r ANSWER || ANSWER=""
    case "$ANSWER" in
      y|Y|yes|YES|Yes) ;;
      *) echo "Nothing was installed."; exit 1 ;;
    esac
  else
    echo "Nothing was installed: run  sh setup.sh  in a terminal to approve it, or install"
    echo "python3 and python3-tk (or your distro's Tkinter package) yourself."
    exit 1
  fi
  SUDO=""
  [ "$(id -u)" = 0 ] || SUDO="sudo"
  if [ -e /run/ostree-booted ] && [ ! -e /run/.containerenv ]; then
    container_hint  # Fedora Atomic / Bazzite / Bluefin: dnf can't add packages to the system
  elif command -v steamos-readonly >/dev/null 2>&1 && [ ! -e /run/.containerenv ]; then
    container_hint  # SteamOS: the system is read-only and reset by updates
  elif command -v apt-get >/dev/null 2>&1; then
    $SUDO apt-get update && $SUDO apt-get install -y python3 python3-tk
  elif command -v dnf >/dev/null 2>&1; then
    $SUDO dnf install -y python3 python3-tkinter
  elif command -v pacman >/dev/null 2>&1; then
    $SUDO pacman -S --needed --noconfirm python tk
  elif command -v zypper >/dev/null 2>&1 && ! root_read_only; then
    $SUDO zypper --non-interactive install python3 python3-tk
  elif command -v xbps-install >/dev/null 2>&1; then
    $SUDO xbps-install -Sy python3 python3-tkinter
  else
    container_hint
  fi
  PY="$(find_python)"
fi

exec "$PY" "$HERE/cod3_waw_setup.py" "$@"
