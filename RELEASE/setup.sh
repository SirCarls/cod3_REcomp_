#!/bin/sh
# Starts the COD_3_WAW setup tool: Linux, or Windows in Git Bash, MSYS2 or
# Cygwin. It installs anything missing (Python + Tkinter here; Rust,
# compilers and libraries when you press Build).
#   sh setup.sh            the window
#   sh setup.sh --help     the command line options
exec sh "$(dirname "$0")/COD_3_WAW_Setup-tool/setup.sh" "$@"
