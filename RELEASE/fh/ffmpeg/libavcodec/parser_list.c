/* [acct2-fh] 2026-11-13: FFmpeg's parsers for Finest Hour's films (fh/cmake/fh_ffmpeg.cmake): none, since
 * src/fh_movie.c cuts the Bink file into packets itself. libavcodec/parsers.c includes this list in place of the one
 * FFmpeg's configure writes. */
static const AVCodecParser * const parser_list[] = {
    NULL };
