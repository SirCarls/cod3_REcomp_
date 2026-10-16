# Building the Windows game on Linux (llvm-mingw)

The setup tool can build the Windows version of the recompiled game (`cod3.exe` and its DLLs) on a Linux machine, with the open-source [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) toolchain. No Microsoft license is involved: llvm-mingw brings its own Windows headers and C/C++ runtime (mingw-w64, libc++).

How it fits together (`c3w/cross.py`):

1. The ReXGlue SDK is built twice: for Linux (its `rexglue` tool runs the codegen) and for Windows with llvm-mingw (`toolchain.cmake`, written into the work folder).
2. The game is configured for Windows with `REXGLUE_CODEGEN` pointing at the Linux `rexglue`, since the Windows one can't run here. The generated C++ is the same either way.
3. Packaging reads every DLL the game imports and adds the ones built with it plus llvm-mingw's `libc++.dll` and `libunwind.dll`. The rest come with Windows 10 and 11.

`include/` holds two headers llvm-mingw lacks, written for this project:

- `crtversion.h`: Microsoft's C runtime version header, which FFmpeg includes. Empty here.
- `DXProgrammableCapture.h`: the graphics debugger's capture interface (`IDXGraphicsAnalysis`).

The SDK's sources also include some Windows headers with different capitalization than llvm-mingw's files (`Windows.h` for `windows.h`). Linux file names are case-sensitive, so the tool adds links with the expected names to its own copy of this folder before building.

SDK patch `patches/mingw/0005-build-with-llvm-mingw.patch` makes the remaining code compile with it, and exports the kernel functions from `librexruntime.dll` through a generated `.def` file (MinGW's automatic export skips their `__imp__` names). The tool applies it only for this build. Every change in it is also valid for the other compilers.

FidelityFX is off in this build: its shaders are compiled by a Windows program.
