/* [acct2-fh] 2026-11-13: the decoders FFmpeg is built with for Finest Hour's films (fh/cmake/fh_ffmpeg.cmake):
 * Bink's picture decoder and its two sound decoders. libavcodec/allcodecs.c includes this list in place of the one
 * FFmpeg's configure writes; avcodec_find_decoder looks through it. */
static const AVCodec * const codec_list[] = {
    &ff_bink_decoder,
    &ff_binkaudio_dct_decoder,
    &ff_binkaudio_rdft_decoder,
    NULL };
