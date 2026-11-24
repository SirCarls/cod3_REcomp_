#!/bin/sh
# COD_3_WAW Setup tool - bootstrap for Linux, and for Windows in Git Bash,
# MSYS2 or Cygwin.
# Makes sure Python 3 (3.9+) with Tkinter is installed, then starts the tool.
# Any arguments are passed through (e.g. `sh setup.sh doctor`, `sh setup.sh build --help`).
# If double-clicking does nothing (folder on a drive mounted without exec
# rights), open a terminal here and run:  sh setup.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

# [acct2-ocr] 2026-11-22 what this script decided (which Python, what it installed, why it stopped) goes to
# config/logs/setup-sh.log, kept to its last 1000 lines: the tool's own logs only start once Python runs.
SHLOG="$HERE/../config/logs/setup-sh.log"
shlog() {
  { mkdir -p "$(dirname "$SHLOG")" && printf '%s [%s] setup.sh: %s\n' "$(date '+%H:%M:%S')" "${LEVEL:-info}" "$*" \
      >> "$SHLOG"; } 2>/dev/null || true
}
if [ -f "$SHLOG" ] && [ "$(wc -l < "$SHLOG" 2>/dev/null || echo 0)" -gt 2000 ]; then
  { tail -n 1000 "$SHLOG" > "$SHLOG.tmp" && mv "$SHLOG.tmp" "$SHLOG"; } 2>/dev/null || true
fi
shlog "---- session ---- $(date '+%Y-%m-%d %H:%M:%S %Z'); $(uname -srm 2>/dev/null || echo 'uname failed'); in $HERE; arguments: $*"
# set -e stops at the first failing command without a word: the log says that it stopped, and with what
trap 'rc=$?; [ "$rc" -eq 0 ] || LEVEL=error shlog "stopped with exit code $rc (the lines above it on screen say why)"' EXIT

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
      shlog "Windows shell $(uname -s): starting the tool with py -3"
      exec py -3 "$TOOL" "$@"
    fi
    if command -v python >/dev/null 2>&1 && python -c "$CHECK" 2>/dev/null; then
      shlog "Windows shell $(uname -s): starting the tool with $(command -v python)"
      exec python "$TOOL" "$@"
    fi
    shlog "Windows shell $(uname -s): no 64-bit Python 3.9+ with Tkinter; handing over to setup.bat"
    # No Windows Python yet: setup.bat installs it (winget) and starts the
    # tool. MSYS turns a lone "/c" into a path, so it gets "//c" there.
    # [acct2-decomp] 2026-11-21: "call" first: when what follows /c starts
    # with a quote, cmd drops that quote and the last one on the line unless
    # the quoted path is plain (no ( ) & ^ @ < > |, no other quotes), so a
    # folder like "cod3 (1)" or any quoted argument broke the start.
    echo "Python for Windows isn't installed yet; setup.bat installs it..."
    case "$(uname -s)" in
      CYGWIN*) exec cmd.exe /c call "$(winpath "$HERE/setup.bat")" "$@" ;;
      *) exec cmd.exe //c call "$(winpath "$HERE/setup.bat")" "$@" ;;
    esac
    ;;
esac

# [acct2-decomp] 2026-11-21: a Python 3.9+ that has Tkinter first, also the
# system's own /usr/bin/python3 when another python3 comes first on PATH
# (pyenv or conda builds often lack Tkinter, and the package manager adds it
# to the system's Python only); without one, the first Python 3.9+.
WITH_TK='import sys, tkinter; sys.exit(sys.version_info < (3, 9))'
find_python() {
  for c in python3 python /usr/bin/python3; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c "$WITH_TK" 2>/dev/null; then
      echo "$c"; return 0
    fi
  done
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
  shlog "no package manager that can add packages here (image-based or read-only system)"
  echo "Couldn't install Python with Tkinter here (no apt, dnf, pacman, zypper or xbps that can add packages)."
  echo "On an image-based system (SteamOS, Bazzite, Fedora Atomic, openSUSE Aeon, NixOS ...) run the tool"
  echo "inside a regular Linux container instead:"
  echo "    distrobox create --name cod3-build --image ubuntu:24.04"
  echo "    distrobox enter cod3-build"
  echo "    sh setup.sh"
  echo "Otherwise install Python 3.9+ with Tkinter yourself and run this again."
  exit 1
}

py_line() {   # the Python found, for the log: which, its version and where it is, and whether it has Tkinter
  if [ -z "${1:-}" ]; then echo "none found"; return; fi
  printf '%s: %s; Tkinter %s' "$1" \
    "$("$1" -c 'import sys; print(sys.version.split()[0], sys.executable)' 2>/dev/null || echo '?')" \
    "$("$1" -c 'import tkinter; print("yes, Tk", tkinter.TkVersion)' 2>/dev/null || echo 'no')"
}
for c in python3 python /usr/bin/python3; do
  if command -v "$c" >/dev/null 2>&1; then shlog "seen: $(py_line "$c")"; fi
done
PY="$(find_python || true)"
shlog "chosen: $(py_line "$PY")"
if [ -z "$PY" ] || ! "$PY" -c 'import tkinter' 2>/dev/null; then
  shlog "no Python 3.9+ with Tkinter: asking to install one"
  echo "Python 3 with Tkinter is needed to run the setup tool. With your approval, your package"
  echo "manager installs it into the system's folders (the only place packages go); it asks for"
  echo "your password."
  if [ -t 0 ]; then
    printf "Install Python 3 with Tkinter now? [y/N] "
    read -r ANSWER || ANSWER=""
    case "$ANSWER" in
      y|Y|yes|YES|Yes) shlog "install approved" ;;
      *) echo "Nothing was installed."; shlog "install not approved"; exit 1 ;;
    esac
  else
    echo "Nothing was installed: run  sh setup.sh  in a terminal to approve it, or install"
    echo "python3 and python3-tk (or your distro's Tkinter package) yourself."
    shlog "not in a terminal, so nothing could be approved or installed"
    exit 1
  fi
  SUDO=""
  [ "$(id -u)" = 0 ] || SUDO="sudo"
  if [ -e /run/ostree-booted ] && [ ! -e /run/.containerenv ]; then
    container_hint  # Fedora Atomic / Bazzite / Bluefin: dnf can't add packages to the system
  elif command -v steamos-readonly >/dev/null 2>&1 && [ ! -e /run/.containerenv ]; then
    container_hint  # SteamOS: the system is read-only and reset by updates
  elif command -v apt-get >/dev/null 2>&1; then
    shlog "installing python3 python3-tk with apt-get"
    # [acct2-decomp] 2026-11-21: one broken extra repository makes "update"
    # fail; the install still works from the lists already there.
    $SUDO apt-get update || true
    $SUDO apt-get install -y python3 python3-tk
  elif command -v dnf >/dev/null 2>&1; then
    shlog "installing python3 python3-tkinter with dnf"
    $SUDO dnf install -y python3 python3-tkinter
  elif command -v pacman >/dev/null 2>&1; then
    shlog "installing python tk with pacman"
    $SUDO pacman -S --needed --noconfirm python tk
  elif command -v zypper >/dev/null 2>&1 && ! root_read_only; then
    shlog "installing python3 python3-tk with zypper"
    $SUDO zypper --non-interactive install python3 python3-tk
  elif command -v xbps-install >/dev/null 2>&1; then
    shlog "installing python3 python3-tkinter with xbps-install"
    $SUDO xbps-install -Sy python3 python3-tkinter
  else
    container_hint
  fi
  # [acct2-decomp] 2026-11-21: say so instead of stopping without a word (set -e).
  PY="$(find_python || true)"
  shlog "after the install: $(py_line "$PY")"
  if [ -z "$PY" ] || ! "$PY" -c 'import tkinter' 2>/dev/null; then
    echo "The packages were installed, but no Python 3.9+ with Tkinter is found yet${PY:+ ($PY has no Tkinter)}."
    echo "Open a new terminal and run  sh setup.sh  again, or install Tkinter for that Python yourself."
    exit 1
  fi
fi

shlog "starting the tool with $PY (its own log: config/logs/tool-*.log)"
exec "$PY" "$HERE/cod3_waw_setup.py" "$@"
