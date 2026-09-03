# opencore-amrwb (AMR-WB 디코더) — ext/opencore-amr 소스를 MSVC 로 빌드하는 CMake 래퍼 (ue_sdk.md §6).
# upstream 은 autotools 만 있어 amrwb/Makefile.am 의 소스 목록·include 경로를 그대로 옮겼다.
# 설치 레이아웃은 pjproject cmake/FindOpenCoreAMRWB.cmake 가 찾는 형태(include/opencore-amrwb/dec_if.h, lib/opencore-amrwb.lib).
cmake_minimum_required(VERSION 3.16)
project(opencore-amrwb C CXX)

if(NOT DEFINED OC_SRC)
    set(OC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../ext/opencore-amr)
endif()
get_filename_component(OC_SRC ${OC_SRC} ABSOLUTE)
set(AMR_BASE ${OC_SRC}/opencore/codecs_v2/audio/gsm_amr)
set(DEC_DIR  ${AMR_BASE}/amr_wb/dec)

file(GLOB DEC_SRC ${DEC_DIR}/src/*.cpp)
list(FILTER DEC_SRC EXCLUDE REGEX "decoder_amr_wb\\.cpp$")      # Makefile.am 과 동일하게 제외
add_library(opencore-amrwb STATIC ${OC_SRC}/amrwb/wrapper.cpp ${DEC_SRC})
target_include_directories(opencore-amrwb PRIVATE
    ${OC_SRC}/oscl ${DEC_DIR}/src ${DEC_DIR}/include ${AMR_BASE}/common/dec/include)
target_compile_definitions(opencore-amrwb PRIVATE _CRT_SECURE_NO_WARNINGS)

install(TARGETS opencore-amrwb ARCHIVE DESTINATION lib)
install(FILES ${OC_SRC}/amrwb/dec_if.h ${DEC_DIR}/include/pvamrwbdecoder_api.h
        DESTINATION include/opencore-amrwb)
