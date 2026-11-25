/* [acct2-fh] 2026-11-13: FFmpeg's configuration for Finest Hour's films (fh/cmake/fh_ffmpeg.cmake builds FFmpeg with
 * it, from the FFmpeg 4.4 source Call of Duty 3's SDK carries).
 *
 * It is the configuration the SDK made for that FFmpeg (its config_windows_x86_64.h or config_linux_x86_64.h, found
 * on the include path in the FFmpeg folder), with the parts Bink needs switched on. The SDK builds FFmpeg for its
 * own sound (XMA), so in its configuration Bink's decoders are off. FFmpeg's sources include "config.h"; this folder
 * comes first on their include path, so they get this one. */
#ifndef FH_FFMPEG_CONFIG_H
#define FH_FFMPEG_CONFIG_H

#if defined(_WIN32)
#include "config_windows_x86_64.h"
#else
#include "config_linux_x86_64.h"
#endif

/* [waw-decomp/fh1] 2026-11-23: the SDK's Windows configuration was made for Microsoft's C runtime (HAVE_LIBC_MSVCRT
 * 1), so with it libavutil/internal.h includes <crtversion.h>, which only Microsoft's runtime has: the llvm-mingw
 * Windows build stopped there (fh1-winbuild2, every libavutil file). FFmpeg's own configure gives a mingw-w64 build
 * its own C library kind (mingw64), not msvcrt, so it is 0 there. HAVE_LIBC_MSVCRT does nothing else in the files
 * fh_ffmpeg.cmake builds. Linux, and a build with Microsoft's compiler, are unchanged. */
#if defined(_WIN32) && defined(__MINGW32__)
#undef HAVE_LIBC_MSVCRT
#define HAVE_LIBC_MSVCRT 0
#endif

/* Bink's picture decoder (libavcodec/bink.c) and its two sound decoders (libavcodec/binkaudio.c, which keeps the
 * DCT and RDFT kinds apart by these two) */
#undef CONFIG_BINK_DECODER
#define CONFIG_BINK_DECODER 1
#undef CONFIG_BINKAUDIO_DCT_DECODER
#define CONFIG_BINKAUDIO_DCT_DECODER 1
#undef CONFIG_BINKAUDIO_RDFT_DECODER
#define CONFIG_BINKAUDIO_RDFT_DECODER 1

/* the two picture helpers bink.c uses (libavcodec/blockdsp.c, libavcodec/hpeldsp.c) */
#undef CONFIG_BLOCKDSP
#define CONFIG_BLOCKDSP 1
#undef CONFIG_HPELDSP
#define CONFIG_HPELDSP 1

/* No inline assembly: plain C everywhere, as in the SDK's Windows configuration. The Linux configuration turns
 * FFmpeg 4.4's inline x86 assembly on, and newer GNU assemblers (binutils 2.41 and later) refuse some of it
 * (libavcodec/x86/mathops.h: "operand type mismatch for 'shr'"; build27 stopped there). The films are small, so the
 * C code is fast enough, and it builds the same with every compiler. HAVE_X86ASM (the separate .asm files) is
 * already off in both configurations. */
#undef HAVE_INLINE_ASM
#define HAVE_INLINE_ASM 0
#undef HAVE_INLINE_ASM_DIRECT_SYMBOL_REFS
#define HAVE_INLINE_ASM_DIRECT_SYMBOL_REFS 0
#undef HAVE_INLINE_ASM_LABELS
#define HAVE_INLINE_ASM_LABELS 0
#undef HAVE_INLINE_ASM_NONLOCAL_LABELS
#define HAVE_INLINE_ASM_NONLOCAL_LABELS 0
#undef HAVE_AESNI_INLINE
#define HAVE_AESNI_INLINE 0
#undef HAVE_AMD3DNOW_INLINE
#define HAVE_AMD3DNOW_INLINE 0
#undef HAVE_AMD3DNOWEXT_INLINE
#define HAVE_AMD3DNOWEXT_INLINE 0
#undef HAVE_AVX_INLINE
#define HAVE_AVX_INLINE 0
#undef HAVE_AVX2_INLINE
#define HAVE_AVX2_INLINE 0
#undef HAVE_AVX512_INLINE
#define HAVE_AVX512_INLINE 0
#undef HAVE_FMA3_INLINE
#define HAVE_FMA3_INLINE 0
#undef HAVE_FMA4_INLINE
#define HAVE_FMA4_INLINE 0
#undef HAVE_MMX_INLINE
#define HAVE_MMX_INLINE 0
#undef HAVE_MMXEXT_INLINE
#define HAVE_MMXEXT_INLINE 0
#undef HAVE_SSE_INLINE
#define HAVE_SSE_INLINE 0
#undef HAVE_SSE2_INLINE
#define HAVE_SSE2_INLINE 0
#undef HAVE_SSE3_INLINE
#define HAVE_SSE3_INLINE 0
#undef HAVE_SSE4_INLINE
#define HAVE_SSE4_INLINE 0
#undef HAVE_SSE42_INLINE
#define HAVE_SSE42_INLINE 0
#undef HAVE_SSSE3_INLINE
#define HAVE_SSSE3_INLINE 0
#undef HAVE_XOP_INLINE
#define HAVE_XOP_INLINE 0

#endif
