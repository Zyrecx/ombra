# libx264 (GPL-2.0-or-later, see COPYING), 8-bit 4:2:0 only, static, for X264Encoder.
# Source: https://code.videolan.org/videolan/x264 branch `stable`, commit
# b35605ace3ddf7c1a5d67a2eb553f034aef41d55 (X264_BUILD 165). Trimmed to the library: no CLI, no
# OpenCL, no non-x86 asm. config.h / x264_config.h are what that commit's own ./configure wrote with
# CC=cl --disable-cli --enable-static --bit-depth=8 --chroma-format=420 --disable-opencl (and every
# input/output library disabled); the source lists and flags below mirror its Makefile.
# The asm needs nasm (build.ps1 fetches a pinned one): without it x264 runs several times slower.

enable_language(C)
enable_language(ASM_NASM)

set(X264_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Bit-depth independent (Makefile SRCS).
set(X264_SRCS
    common/osdep.c common/base.c common/cpu.c common/tables.c encoder/api.c
    common/win32thread.c)

# Built once per bit depth (Makefile SRCS_X), here only 8.
set(X264_SRCS_X
    common/mc.c common/predict.c common/pixel.c common/macroblock.c
    common/frame.c common/dct.c common/cabac.c
    common/common.c common/rectangle.c
    common/set.c common/quant.c common/deblock.c common/vlc.c
    common/mvpred.c common/bitstream.c
    encoder/analyse.c encoder/me.c encoder/ratecontrol.c
    encoder/set.c encoder/macroblock.c encoder/cabac.c
    encoder/cavlc.c encoder/encoder.c encoder/lookahead.c
    common/threadpool.c
    common/x86/mc-c.c common/x86/predict-c.c)

set(X264_ASM_X
    common/x86/dct-64.asm common/x86/trellis-64.asm
    common/x86/bitstream-a.asm common/x86/const-a.asm common/x86/cabac-a.asm
    common/x86/dct-a.asm common/x86/deblock-a.asm common/x86/mc-a.asm
    common/x86/mc-a2.asm common/x86/pixel-a.asm common/x86/predict-a.asm
    common/x86/quant-a.asm common/x86/sad-a.asm)

set_source_files_properties(${X264_SRCS_X} PROPERTIES
    COMPILE_DEFINITIONS "HIGH_BIT_DEPTH=0;BIT_DEPTH=8")
set_source_files_properties(${X264_ASM_X} PROPERTIES
    COMPILE_DEFINITIONS "BIT_DEPTH=8;private_prefix=x264_8")

add_library(x264 STATIC ${X264_SRCS} ${X264_SRCS_X} common/x86/cpu-a.asm ${X264_ASM_X})
target_include_directories(x264
    PUBLIC  "${X264_DIR}"
    PRIVATE "${X264_DIR}/common/x86")
target_compile_definitions(x264 PRIVATE
    $<$<COMPILE_LANGUAGE:C>:HAVE_STRING_H _CRT_SECURE_NO_WARNINGS>
    $<$<COMPILE_LANGUAGE:ASM_NASM>:ARCH_X86_64=1 STACK_ALIGNMENT=16>)
# configure's MSVC flags (-O2 -fp:fast -GS-); third-party code, so no warnings.
target_compile_options(x264 PRIVATE
    $<$<COMPILE_LANGUAGE:C>:/O2 /fp:fast /GS- /W0 /utf-8>)
