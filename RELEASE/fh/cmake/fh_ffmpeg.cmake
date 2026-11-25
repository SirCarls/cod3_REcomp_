# [acct2-fh] 2026-11-13: FFmpeg for Finest Hour's films (src/fh_movie.c), built here from the FFmpeg 4.4 source
# Call of Duty 3's SDK carries (work/rexglue-sdk/thirdparty/FFmpeg, fetched with the SDK at its pinned commit). The
# owner, 11-13 19:49: "remember our portable plan you can't depend on the user having something": nothing is
# downloaded for the films and nothing of the system's is used, the way SDL3 comes from the SDK's build.
#
# What is built: libavutil, and libavcodec as the SDK builds it for its own sound (the same source lists as the SDK's
# thirdparty/CMakeLists.txt, so it compiles and links as the SDK's does; its sound decoders come along and the linker
# leaves them out), with Bink's picture and sound decoders and the two picture helpers they use added. Not built:
# FFmpeg's file readers (libavformat) and picture converter (libswscale); src/fh_movie.c reads the Bink file and turns
# its pictures into pixels itself. fh/ffmpeg holds what is FH's own: config.h (the SDK's configuration with Bink
# switched on), libavcodec/codec_list.c and its two neighbours (the decoders, parsers and filters built in), and
# x86_stubs.c (two x86 set-ups whose assembly isn't built).
#
# Licence: FFmpeg's LGPL; the SDK's configuration builds it without its GPL parts (CONFIG_GPL 0). It is linked into
# the port as the SDK links the same FFmpeg into Call of Duty 3, and the player builds both from source on their PC.
#
#   FH_FFMPEG_SOURCE_DIR  the FFmpeg 4.4 source; empty: the SDK's, in the project's work folder
#
# Sets FH_FFMPEG_FOUND and, when it is ON, the target fh_ffmpeg (both libraries) and FH_FFMPEG_INCLUDE_DIR (the folder
# with libavcodec/avcodec.h, for src/fh_movie.c alone). Without it the films are not played.
set(FH_FFMPEG_SOURCE_DIR "" CACHE PATH
    "FFmpeg 4.4 source for the films; empty: Call of Duty 3's SDK's (work/rexglue-sdk/thirdparty/FFmpeg)")
set(FH_FFMPEG_FOUND OFF)
set(_ffmpeg "${FH_FFMPEG_SOURCE_DIR}")
if(NOT _ffmpeg)
    set(_ffmpeg "${FH_REPO_DIR}/work/rexglue-sdk/thirdparty/FFmpeg")
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _ffmpeg_cpu)

if(NOT EXISTS "${_ffmpeg}/libavcodec/bink.c" OR NOT EXISTS "${_ffmpeg}/config_linux_x86_64.h")
    message(WARNING "FFmpeg 4.4 source with the SDK's configuration not found in '${_ffmpeg}' (FH_FFMPEG_SOURCE_DIR): "
                    "the game's films are not played")
elseif(NOT _ffmpeg_cpu MATCHES "^(x86_64|amd64)$")
    message(WARNING "The films' FFmpeg is built for x86-64 only (the SDK's configuration), not "
                    "'${CMAKE_SYSTEM_PROCESSOR}': the game's films are not played")
else()
    set(FH_FFMPEG_FOUND ON)
    set(FH_FFMPEG_INCLUDE_DIR "${_ffmpeg}")
    set(_ffmpeg_own "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")

    # libavutil, as the SDK lists it
    set(FH_AVUTIL_SOURCES
        adler32.c aes.c aes_ctr.c audio_fifo.c avstring.c avsscanf.c base64.c blowfish.c bprint.c buffer.c cast5.c
        camellia.c channel_layout.c color_utils.c cpu.c crc.c des.c dict.c display.c dovi_meta.c downmix_info.c
        encryption_info.c error.c eval.c fifo.c file.c file_open.c float_dsp.c fixed_dsp.c frame.c hash.c
        hdr_dynamic_metadata.c hmac.c hwcontext.c imgutils.c integer.c intmath.c lfg.c lls.c log.c log2_tab.c
        mathematics.c mastering_display_metadata.c md5.c mem.c murmur3.c opt.c parseutils.c pixdesc.c pixelutils.c
        random_seed.c rational.c reverse.c rc4.c ripemd.c samplefmt.c sha.c sha512.c slicethread.c spherical.c
        stereo3d.c threadmessage.c time.c timecode.c tree.c twofish.c utils.c xga_font_data.c xtea.c tea.c tx.c
        tx_float.c tx_double.c tx_int32.c video_enc_params.c film_grain_params.c
    )
    list(TRANSFORM FH_AVUTIL_SOURCES PREPEND "${_ffmpeg}/libavutil/")
    # the SDK's x86 set-ups for libavutil (C and inline assembly; the separate assembly files are not built)
    set(FH_AVUTIL_X86_SOURCES cpu.c fixed_dsp_init.c float_dsp_init.c imgutils_init.c lls_init.c)
    list(TRANSFORM FH_AVUTIL_X86_SOURCES PREPEND "${_ffmpeg}/libavutil/x86/")

    # libavcodec, as the SDK lists it ...
    set(FH_AVCODEC_SOURCES
        ac3_parser.c adts_parser.c allcodecs.c avcodec.c avdct.c avpacket.c avpicture.c bitstream.c bitstream_filter.c
        bitstream_filters.c bsf.c codec_desc.c codec_par.c d3d11va.c decode.c dirac.c dv_profile.c encode.c imgconvert.c
        jni.c mathtables.c mediacodec.c mpeg12framerate.c options.c parser.c parsers.c profiles.c qsv_api.c raw.c
        utils.c vorbis_parser.c xiph.c faandct.c faanidct.c fdctdsp.c jfdctfst.c jfdctint.c idctdsp.c simple_idct.c
        jrevdct.c mdct_float.c mdct_fixed_32.c sinewin.c wma_freqs.c wmaprodec.c wmadec.c wma.c wma_common.c mpegaudio.c
        mpegaudiodata.c mpegaudiodecheader.c mpegaudiodec_common.c mpegaudiodec_float.c mpegaudiodec_fixed.c
        mpegaudiodsp.c mpegaudiodsp_data.c mpegaudiodsp_float.c mpegaudiodsp_fixed.c mpegaudio_parser.c dct.c
        dct32_float.c dct32_fixed.c rdft.c aactab.c null_bsf.c pthread.c pthread_slice.c pthread_frame.c avfft.c
        fft_float.c fft_fixed_32.c fft_init_table.c
        # ... and what Bink's decoders add: the picture decoder, its helpers, and the sound decoders
        bink.c binkdsp.c blockdsp.c hpeldsp.c binkaudio.c
    )
    list(TRANSFORM FH_AVCODEC_SOURCES PREPEND "${_ffmpeg}/libavcodec/")
    # and its x86 set-ups for libavcodec
    set(FH_AVCODEC_X86_SOURCES constants.c fdctdsp_init.c fft_init.c idctdsp_init.c fdct.c mpegaudiodsp.c dct_init.c)
    list(TRANSFORM FH_AVCODEC_X86_SOURCES PREPEND "${_ffmpeg}/libavcodec/x86/")
    if(WIN32)
        list(APPEND FH_AVCODEC_SOURCES "${_ffmpeg}/libavcodec/file_open.c")
    endif()

    add_library(fh_avutil STATIC ${FH_AVUTIL_SOURCES} ${FH_AVUTIL_X86_SOURCES})
    add_library(fh_avcodec STATIC ${FH_AVCODEC_SOURCES} ${FH_AVCODEC_X86_SOURCES} "${_ffmpeg_own}/x86_stubs.c")
    if(WIN32)
        set(_ffmpeg_atomics "${_ffmpeg}/compat/atomics/win32")
    else()
        set(_ffmpeg_atomics "${_ffmpeg}/compat/atomics/gcc")
    endif()
    foreach(lib fh_avutil fh_avcodec)
        # fh/ffmpeg first, so FFmpeg's sources find its config.h and libavcodec/*_list.c before the SDK's
        target_include_directories(${lib} BEFORE PRIVATE "${_ffmpeg_own}")
        target_include_directories(${lib} PRIVATE "${_ffmpeg}" "${_ffmpeg_atomics}")
        target_compile_definitions(${lib} PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
        # FFmpeg's code isn't written for this project's warnings; and it counts on the compiler dropping code its
        # configuration switches off, which needs optimizing even in a debug build
        target_compile_options(${lib} PRIVATE -w -O2)
    endforeach()
    target_link_libraries(fh_avcodec PUBLIC fh_avutil)
    if(WIN32)
        target_link_libraries(fh_avutil PUBLIC bcrypt)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(fh_avutil PUBLIC Threads::Threads m)
    endif()
    add_library(fh_ffmpeg INTERFACE)
    target_link_libraries(fh_ffmpeg INTERFACE fh_avcodec fh_avutil)
    message(STATUS "FFmpeg for the films: built from ${_ffmpeg}")
endif()
