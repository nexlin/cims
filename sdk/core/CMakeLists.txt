# libcimsue — 단말 SDK 코어 (docs/design/features/ue_sdk.md §4·§8)
#
# 루트 CMakeLists.txt 가 add_subdirectory 로 포함한다(CIMS_UE_SDK=ON). 엔진 pjproject 는 루트의
# ExternalProject(`pjproject`)가 pkg/pjproject 에 설치하며, 여기서는 그 산출물을 링크한다.
# 라이브러리 이름의 타겟 접미(x86_64-pc-linux-gnu)는 pjproject aconfigure 가 config.guess 로 정하므로
# 같은 방식으로 구한다 — configure 시점에 .pc 파일이 아직 없어도(첫 빌드) 링크 목록을 만들 수 있다.
#
# 크로스 빌드(Android NDK): PJ_INC / PJ_LIB_PATH / PJ_TARGET / PJ_SYS_LIBS 를 -D 로 주면 그대로 쓴다
# (sdk/android/build-native.sh — 후속 단계).
# Windows(MSVC, sdk/windows 슈퍼빌드): pjproject 를 자체 CMake 로 설치한 뒤 PJ_LIBS(링크 대상 목록 — imported
# target 또는 .lib 이름)·PJ_INC·PJ_LIB_PATH 를 -D 로 준다. config.guess 접미 규칙은 autoconf 빌드 전용이다.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PJ_DIR)
    message(FATAL_ERROR "sdk/core 는 루트 CMake 에서만 빌드한다 (PJ_DIR/PJ_PKG_DIR 필요)")
endif()
# 레포 루트 — 루트 CMake 뿐 아니라 sdk/windows 슈퍼빌드(CMAKE_SOURCE_DIR=sdk/windows)에서도 include 되므로 자기 위치에서 구한다.
get_filename_component(CIMS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

if(NOT DEFINED PJ_LIBS)
    if(NOT DEFINED PJ_TARGET)
        if(WIN32)
            message(FATAL_ERROR "Windows 빌드는 PJ_LIBS(또는 PJ_TARGET) 를 -D 로 지정한다 (ue_sdk.md §6)")
        endif()
        execute_process(COMMAND sh ${PJ_DIR}/config.guess
                        OUTPUT_VARIABLE PJ_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE _cg_rc)
        if(NOT _cg_rc EQUAL 0 OR PJ_TARGET STREQUAL "")
            message(FATAL_ERROR "pjproject config.guess 실패 — PJ_TARGET 을 -D 로 지정")
        endif()
    endif()
    # libpjproject.pc 의 Libs + Libs.private 순서 (헤드리스 Linux 구성 기준)
    set(_pj_names pjsua2 pjsua pjsip-ua pjsip-simple pjsip pjmedia-codec pjmedia-videodev pjmedia-audiodev
                  pjmedia pjnath pjlib-util pj milenage srtp resample speex)
    set(PJ_LIBS "")
    foreach(_n IN LISTS _pj_names)
        list(APPEND PJ_LIBS "${_n}-${PJ_TARGET}")
    endforeach()
endif()
message(STATUS "libcimsue: pjproject target=${PJ_TARGET} pkg=${PJ_PKG_DIR}")
if(NOT DEFINED PJ_SYS_LIBS)
    if(WIN32)
        set(PJ_SYS_LIBS ws2_32 winmm ole32 iphlpapi crypt32)   # pjlib(winsock)·WMME·OpenSSL 정적 링크 요건
    else()
        set(PJ_SYS_LIBS ssl crypto opencore-amrnb opencore-amrwb vo-amrwbenc m rt pthread)
    endif()
endif()

# floor 정의 헤더는 생성물이다 — 정본 테이블이 바뀌면 재생성 (scripts/gen_floor_defs.py)
# 인터프리터는 find_package 로 잡는다 — Windows(python.org 설치본)에는 `python3` 이름이 없다.
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_EXECUTABLE)
    set(Python3_EXECUTABLE python3)
endif()
set(FLOOR_DEFS_YAML ${CIMS_ROOT}/docs/design/features/mcptt_floor_defs.yaml)
set(FLOOR_DEFS_H ${CMAKE_CURRENT_SOURCE_DIR}/src/floor/floor_defs.h)
add_custom_command(
    OUTPUT ${FLOOR_DEFS_H}
    COMMAND ${Python3_EXECUTABLE} ${CIMS_ROOT}/scripts/gen_floor_defs.py
    DEPENDS ${FLOOR_DEFS_YAML} ${CIMS_ROOT}/scripts/gen_floor_defs.py
    COMMENT "floor_defs.h ← mcptt_floor_defs.yaml")

# Windows SDK 는 DLL(CIMSUE_SHARED — export.h), 그 외는 정적. pj 라이브러리는 DLL 안으로 정적 링크된다.
option(CIMSUE_SHARED "libcimsue 를 공유 라이브러리(cimsue.dll)로" ${WIN32})
if(CIMSUE_SHARED)
    set(_cimsue_lib_type SHARED)
else()
    set(_cimsue_lib_type STATIC)
endif()
# 코어 소스는 OBJECT 라이브러리로 한 번만 컴파일한다 — 라이브러리 cimsue(Linux 정적 / Windows DLL)와 단위시험이 같은 오브젝트를
# 나눠 쓴다. Windows DLL 은 공개 표면(CIMSUE_API)만 export 하므로 내부 심볼을 시험하는 cimsue_test 는 DLL 대신 오브젝트를 링크한다.
add_library(cimsue_objs OBJECT
    src/engine.cpp
    src/account_map.cpp
    src/types.cpp
    src/floor/floor_codec.cpp
    src/floor/floor_participant.cpp
    src/mcptt/mcptt_xml.cpp
    src/mcdata/sds_codec.cpp
    src/http/https_client.cpp
    src/csc/csc_client.cpp
    src/c_api.cpp
    ${FLOOR_DEFS_H}
)
# 사용 요구(include·매크로·옵션·pj 링크)는 INTERFACE 타깃 하나에 모아 오브젝트·라이브러리·시험이 같은 것을 본다.
add_library(cimsue_iface INTERFACE)
# pjproject 헤더는 공개 헤더에 새지 않지만(pImpl), 내부 소스·단위시험은 pjsua2 를 include 한다.
target_include_directories(cimsue_iface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include ${PJ_INC})
# pjlib 플랫폼 선택 매크로 — autoconf 빌드(Linux/Android)는 PJ_AUTOCONF, MSVC 는 PJ_WIN32 (pjproject CMake 와 동일값)
if(WIN32)
    # WIN32_LEAN_AND_MEAN·PJ_WIN32_WINNT 는 pj/compat/os_win32.h 가 무조건 정의한다 — 여기서 겹치면 C4005.
    target_compile_definitions(cimsue_iface INTERFACE PJ_WIN32=1 NOMINMAX PJ_IS_BIG_ENDIAN=0 PJ_IS_LITTLE_ENDIAN=1)
    if(MSVC)
        target_compile_options(cimsue_iface INTERFACE /utf-8)   # 소스·주석이 UTF-8 (코드 페이지 949 에서 C4819)
    endif()
else()
    target_compile_definitions(cimsue_iface INTERFACE PJ_AUTOCONF=1 PJ_IS_BIG_ENDIAN=0 PJ_IS_LITTLE_ENDIAN=1)
endif()
target_compile_features(cimsue_iface INTERFACE cxx_std_17)
if(CIMSUE_SHARED)
    target_compile_definitions(cimsue_iface INTERFACE CIMSUE_SHARED)
endif()
target_link_directories(cimsue_iface INTERFACE ${PJ_LIB_PATH})
if(DEFINED AMR_PKG_DIR)
    target_link_directories(cimsue_iface INTERFACE ${AMR_PKG_DIR}/lib ${AMRWB_PKG_DIR}/lib)
endif()
target_link_libraries(cimsue_iface INTERFACE ${PJ_LIBS} ${PJ_SYS_LIBS})

target_link_libraries(cimsue_objs PRIVATE cimsue_iface)
target_include_directories(cimsue_objs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(CIMSUE_SHARED)
    target_compile_definitions(cimsue_objs PRIVATE CIMSUE_BUILDING)   # export.h → dllexport
endif()
if(TARGET pjproject)
    add_dependencies(cimsue_objs pjproject)                          # pj 헤더가 설치된 뒤 컴파일
endif()

add_library(cimsue ${_cimsue_lib_type} $<TARGET_OBJECTS:cimsue_objs>)
target_link_libraries(cimsue PUBLIC cimsue_iface)
if(CIMSUE_SHARED)
    set_target_properties(cimsue PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
                                            ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

# 헤드리스 UE — 검증 도구 (build/bin/cimsue-cli)
add_executable(cimsue-cli cli/main.cpp)
target_link_libraries(cimsue-cli PRIVATE cimsue)
if(NOT MSVC)
    target_link_options(cimsue-cli PRIVATE -rdynamic)  # 크래시 백트레이스 심볼 (glibc execinfo)
endif()
set_target_properties(cimsue-cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 단위시험 (S1-UE-UNIT / S1-UE-FLOOR-CODEC) — googletest 는 루트(Linux)·sdk/windows 슈퍼빌드(Windows) 의 ExternalProject 산출물
if(DEFINED GTEST_DIR)
    add_executable(cimsue_test
        test/account_map_test.cpp
        test/floor_codec_test.cpp
        test/sds_codec_test.cpp
        test/csc_test.cpp
        test/engine_route_test.cpp
        test/c_api_test.cpp)
    target_include_directories(cimsue_test PRIVATE ${GTEST_INC} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_directories(cimsue_test PRIVATE ${GTEST_LIB_PATH} ${GTEST_DIR}/lib)   # 설치 산출물은 lib/ (루트 변수 lib64 는 구 경로)
    if(WIN32)
        # DLL 은 공개 표면만 export 한다 — 내부 심볼(detail::·floor::Message·mcptt::)을 시험하므로 오브젝트를 직접 링크한다.
        # 시험 TU 는 CIMSUE_API 를 비워 dllimport 선언을 내지 않는다(__imp_ 미해결 방지 — export.h 의 override). 오브젝트 쪽 dllexport 는
        # 그대로라 exe 에도 export 테이블이 생기지만 무해하다.
        target_sources(cimsue_test PRIVATE $<TARGET_OBJECTS:cimsue_objs>)
        target_compile_definitions(cimsue_test PRIVATE CIMSUE_API=)
        target_link_libraries(cimsue_test PRIVATE cimsue_iface gtest gtest_main)
    else()
        # CMP 서버 코덱과 교차 검증 — cmp/PFloorCodec.cpp 는 PMcpttGroup.h(+pasf pbase.h) 만 필요.
        # pasf 가 pthread/semaphore/sys/time 을 끌어 Linux 전용 — Windows 는 코어 코덱 시험(floor_codec_test)만.
        target_sources(cimsue_test PRIVATE test/floor_xcheck_test.cpp ${CIMS_ROOT}/cmp/PFloorCodec.cpp)
        target_include_directories(cimsue_test PRIVATE ${CIMS_ROOT}/cmp ${EXT_DIR}/pasf/include)
        target_link_libraries(cimsue_test PRIVATE cimsue gtest gtest_main pthread)
    endif()
    set_target_properties(cimsue_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    if(TARGET googletest)
        add_dependencies(cimsue_test googletest)
    endif()
endif()
