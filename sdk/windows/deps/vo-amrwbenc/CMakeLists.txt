# vo-amrwbenc (AMR-WB 인코더) — ext/vo-amrwbenc-0.1.3 소스의 MSVC CMake 래퍼 (ue_sdk.md §6).
# Makefile.am 의 소스 목록(wrapper.c + common/cmnMemory.c + amrwbenc/src/*.c) 을 옮겼다. ASM 최적화(ARM) 없음.
# 설치 레이아웃은 pjproject cmake/FindVisualOnAMRWBEnc.cmake 기준(include/vo-amrwbenc/enc_if.h, lib/vo-amrwbenc.lib).
cmake_minimum_required(VERSION 3.16)
project(vo-amrwbenc C)

if(NOT DEFINED VO_SRC)
    set(VO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../ext/vo-amrwbenc-0.1.3)
endif()
get_filename_component(VO_SRC ${VO_SRC} ABSOLUTE)

file(GLOB ENC_SRC ${VO_SRC}/amrwbenc/src/*.c)
add_library(vo-amrwbenc STATIC ${VO_SRC}/wrapper.c ${VO_SRC}/common/cmnMemory.c ${ENC_SRC})
target_include_directories(vo-amrwbenc PRIVATE ${VO_SRC}/amrwbenc/inc ${VO_SRC}/common/include)
target_compile_definitions(vo-amrwbenc PRIVATE _CRT_SECURE_NO_WARNINGS)

install(TARGETS vo-amrwbenc ARCHIVE DESTINATION lib)
install(FILES ${VO_SRC}/enc_if.h DESTINATION include/vo-amrwbenc)
