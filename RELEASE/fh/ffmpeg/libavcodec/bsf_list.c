/* [acct2-fh] 2026-11-13: FFmpeg's bitstream filters for Finest Hour's films (fh/cmake/fh_ffmpeg.cmake): only the
 * "null" one, which every decoder is set up with (libavcodec/decode.c). libavcodec/bitstream_filters.c includes this
 * list in place of the one FFmpeg's configure writes. */
static const AVBitStreamFilter * const bitstream_filters[] = {
    &ff_null_bsf,
    NULL };
