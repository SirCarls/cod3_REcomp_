/* crtversion.h for llvm-mingw, which has none (it belongs to Microsoft's C
 * runtime). FFmpeg includes it to read _VC_CRT_MAJOR_VERSION; leaving that
 * undefined is right for the mingw-w64 C runtime. */
#pragma once
