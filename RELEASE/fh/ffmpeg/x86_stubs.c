/* [acct2-fh] 2026-11-13: two of FFmpeg's x86 set-ups, for Finest Hour's films (fh/cmake/fh_ffmpeg.cmake).
 *
 * Bink's picture decoder sets up two of libavcodec's picture helpers, blockdsp.c and hpeldsp.c. On x86 each then calls
 * one of these to swap in faster versions written in assembly. FFmpeg is built here without its assembly, as
 * Call of Duty 3's SDK builds it (its configuration has HAVE_X86ASM 0). So these two do nothing and the helpers keep
 * the C versions they have already set. The films are 640 x 480 at 30 a second, which the C versions decode with
 * plenty of time to spare. */
#include "libavcodec/blockdsp.h"
#include "libavcodec/hpeldsp.h"

void ff_blockdsp_init_x86(BlockDSPContext *c, AVCodecContext *avctx)
{
    (void)c;
    (void)avctx;
}

void ff_hpeldsp_init_x86(HpelDSPContext *c, int flags)
{
    (void)c;
    (void)flags;
}
