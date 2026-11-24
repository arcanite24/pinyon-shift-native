# In-process LGPL-2.1 XMA2 / WMA Pro audio decoder built from FFmpeg 4.4
# (libavutil + minimal libavcodec wmaprodec/xma2 only, zero external CLI tools).
option(SHIFT_EMBED_XMA2 "Build in-process LGPL FFmpeg XMA2 audio decoder" ON)

if(SHIFT_EMBED_XMA2)
  FetchContent_Declare(ffmpeg_xma2
    GIT_REPOSITORY https://github.com/wmarti/FFmpeg.git
    GIT_TAG 0604b464c7
    SOURCE_SUBDIR _no_cmake_)
  FetchContent_MakeAvailable(ffmpeg_xma2)

  set(SHIFT_FFMPEG_DIR "${ffmpeg_xma2_SOURCE_DIR}")
  set(SHIFT_FFMPEG_OVERLAY "${CMAKE_BINARY_DIR}/ffmpeg_xma2_overlay")
  file(MAKE_DIRECTORY "${SHIFT_FFMPEG_OVERLAY}/libavcodec")

  file(WRITE "${SHIFT_FFMPEG_OVERLAY}/libavcodec/codec_list.c"
    "static const AVCodec * const codec_list[] = {\n"
    "    &ff_xma2_decoder,\n"
    "    &ff_wmapro_decoder,\n"
    "    &ff_xmaframes_decoder,\n"
    "    NULL\n"
    "};\n")
  file(WRITE "${SHIFT_FFMPEG_OVERLAY}/libavcodec/parser_list.c"
    "static const AVCodecParser * const parser_list[] = { NULL };\n")
  file(WRITE "${SHIFT_FFMPEG_OVERLAY}/libavcodec/bsf_list.c"
    "static const AVBitStreamFilter * const bitstream_filters[] = {\n"
    "#if CONFIG_NULL_BSF\n"
    "    &ff_null_bsf,\n"
    "#endif\n"
    "    NULL\n"
    "};\n")

  if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
      set(_shift_ff_arm64 TRUE)
      set(_shift_ff_x86_64 FALSE)
    else()
      set(_shift_ff_arm64 FALSE)
      set(_shift_ff_x86_64 TRUE)
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_shift_ff_arm64 FALSE)
    set(_shift_ff_x86_64 TRUE)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
    set(_shift_ff_arm64 TRUE)
    set(_shift_ff_x86_64 FALSE)
  else()
    set(_shift_ff_arm64 FALSE)
    set(_shift_ff_x86_64 FALSE)
  endif()

  if(WIN32)
    set(_shift_ff_atomics "${SHIFT_FFMPEG_DIR}/compat/atomics/win32")
  else()
    set(_shift_ff_atomics "${SHIFT_FFMPEG_DIR}/compat/atomics/gcc")
  endif()

  if(_shift_ff_arm64)
    enable_language(ASM)
  endif()

  add_library(shift_ffmpeg_avutil STATIC
    "${SHIFT_FFMPEG_DIR}/libavutil/adler32.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/aes.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/aes_ctr.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/audio_fifo.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/avstring.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/avsscanf.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/base64.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/blowfish.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/bprint.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/buffer.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/camellia.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/cast5.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/channel_layout.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/color_utils.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/cpu.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/crc.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/des.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/dict.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/display.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/dovi_meta.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/downmix_info.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/encryption_info.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/error.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/eval.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/fifo.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/file.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/file_open.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/film_grain_params.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/fixed_dsp.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/float_dsp.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/frame.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/hash.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/hdr_dynamic_metadata.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/hmac.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/hwcontext.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/imgutils.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/integer.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/intmath.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/lfg.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/lls.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/log.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/log2_tab.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/mastering_display_metadata.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/mathematics.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/md5.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/mem.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/murmur3.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/opt.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/parseutils.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/pixdesc.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/pixelutils.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/random_seed.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/rational.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/rc4.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/reverse.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/ripemd.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/samplefmt.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/sha.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/sha512.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/slicethread.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/spherical.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/stereo3d.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/tea.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/threadmessage.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/time.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/timecode.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/tree.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/twofish.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/tx.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/tx_double.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/tx_float.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/tx_int32.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/utils.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/video_enc_params.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/xga_font_data.c"
    "${SHIFT_FFMPEG_DIR}/libavutil/xtea.c")

  if(_shift_ff_x86_64)
    target_sources(shift_ffmpeg_avutil PRIVATE
      "${SHIFT_FFMPEG_DIR}/libavutil/x86/cpu.c"
      "${SHIFT_FFMPEG_DIR}/libavutil/x86/fixed_dsp_init.c"
      "${SHIFT_FFMPEG_DIR}/libavutil/x86/float_dsp_init.c"
      "${SHIFT_FFMPEG_DIR}/libavutil/x86/imgutils_init.c"
      "${SHIFT_FFMPEG_DIR}/libavutil/x86/lls_init.c")
  elseif(_shift_ff_arm64)
    target_sources(shift_ffmpeg_avutil PRIVATE
      "${SHIFT_FFMPEG_DIR}/libavutil/aarch64/cpu.c"
      "${SHIFT_FFMPEG_DIR}/libavutil/aarch64/float_dsp_init.c"
      "${SHIFT_FFMPEG_DIR}/libavutil/aarch64/float_dsp_neon.S")
    target_compile_definitions(shift_ffmpeg_avutil PRIVATE HAVE_NEON=1 HAVE_ARMV8=1 HAVE_INLINE_ASM=1)
  endif()

  target_include_directories(shift_ffmpeg_avutil
    PUBLIC "${SHIFT_FFMPEG_DIR}"
    PRIVATE "${_shift_ff_atomics}")
  target_compile_definitions(shift_ffmpeg_avutil PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
  if(MSVC)
    target_compile_options(shift_ffmpeg_avutil PRIVATE /w)
    target_link_libraries(shift_ffmpeg_avutil PRIVATE bcrypt)
  else()
    target_compile_options(shift_ffmpeg_avutil PRIVATE -w)
  endif()
  set_target_properties(shift_ffmpeg_avutil PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

  add_library(shift_ffmpeg_avcodec STATIC
    "${SHIFT_FFMPEG_DIR}/libavcodec/aactab.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/allcodecs.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/avcodec.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/avdct.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/avfft.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/avpacket.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/avpicture.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/bitstream.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/bitstream_filter.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/bitstream_filters.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/bsf.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/codec_desc.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/codec_par.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/d3d11va.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/dct.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/dct32_fixed.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/dct32_float.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/decode.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/dirac.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/dv_profile.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/encode.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/faandct.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/faanidct.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/fdctdsp.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/fft_fixed_32.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/fft_float.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/fft_init_table.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/idctdsp.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/imgconvert.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/jfdctfst.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/jfdctint.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/jni.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/jrevdct.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/mathtables.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/mdct_fixed_32.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/mdct_float.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/mediacodec.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/mpeg12framerate.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/null_bsf.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/options.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/parser.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/parsers.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/profiles.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/pthread.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/pthread_frame.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/pthread_slice.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/qsv_api.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/raw.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/rdft.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/simple_idct.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/sinewin.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/utils.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/wma.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/wma_common.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/wma_freqs.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/wmadec.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/wmaprodec.c"
    "${SHIFT_FFMPEG_DIR}/libavcodec/xiph.c")

  if(WIN32)
    target_sources(shift_ffmpeg_avcodec PRIVATE "${SHIFT_FFMPEG_DIR}/libavcodec/file_open.c")
  endif()
  if(_shift_ff_x86_64)
    target_sources(shift_ffmpeg_avcodec PRIVATE
      "${SHIFT_FFMPEG_DIR}/libavcodec/x86/constants.c"
      "${SHIFT_FFMPEG_DIR}/libavcodec/x86/dct_init.c"
      "${SHIFT_FFMPEG_DIR}/libavcodec/x86/fdct.c"
      "${SHIFT_FFMPEG_DIR}/libavcodec/x86/fdctdsp_init.c"
      "${SHIFT_FFMPEG_DIR}/libavcodec/x86/fft_init.c"
      "${SHIFT_FFMPEG_DIR}/libavcodec/x86/idctdsp_init.c")
  elseif(_shift_ff_arm64)
    target_sources(shift_ffmpeg_avcodec PRIVATE
      "${SHIFT_FFMPEG_DIR}/libavcodec/aarch64/fft_init_aarch64.c"
      "${SHIFT_FFMPEG_DIR}/libavcodec/aarch64/fft_neon.S"
      "${SHIFT_FFMPEG_DIR}/libavcodec/aarch64/mdct_neon.S"
      "${SHIFT_FFMPEG_DIR}/libavcodec/aarch64/neon.S"
      "${SHIFT_FFMPEG_DIR}/libavcodec/aarch64/simple_idct_neon.S"
      "${SHIFT_FFMPEG_DIR}/libavcodec/aarch64/videodsp.S")
    target_compile_definitions(shift_ffmpeg_avcodec PRIVATE HAVE_NEON=1 HAVE_ARMV8=1 HAVE_INLINE_ASM=1)
  endif()

  target_include_directories(shift_ffmpeg_avcodec BEFORE PRIVATE "${SHIFT_FFMPEG_OVERLAY}")
  target_include_directories(shift_ffmpeg_avcodec
    PUBLIC "${SHIFT_FFMPEG_DIR}"
    PRIVATE "${_shift_ff_atomics}")
  target_compile_definitions(shift_ffmpeg_avcodec PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
  if(MSVC)
    target_compile_options(shift_ffmpeg_avcodec PRIVATE /w)
    target_link_libraries(shift_ffmpeg_avcodec PRIVATE bcrypt)
  else()
    target_compile_options(shift_ffmpeg_avcodec PRIVATE -w)
  endif()
  target_link_libraries(shift_ffmpeg_avcodec PUBLIC shift_ffmpeg_avutil)
  set_target_properties(shift_ffmpeg_avcodec PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
endif()
