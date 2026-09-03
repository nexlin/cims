# Windows SDK 슈퍼빌드 — cimsue.dll + 헤더 + cimsue-cli (docs/design/features/ue_sdk.md §6·§8)
#
# 한 configure 로 의존성 → 엔진 → 코어를 순서대로 빌드한다(ExternalProject). 소스 정본은 전부 레포 안이다:
#   AMR-WB   ext/opencore-amr, ext/vo-amrwbenc-0.1.3   (deps/*/CMakeLists.txt 래퍼로 MSVC 빌드)
#   엔진     ext/pjproject                              (자체 CMake — pjlib WIN32 분기·WMME 백엔드. config_site 는 sdk/engine/config_site/windows.h)
#   코어     sdk/core                                   (CIMSUE_SHARED → cimsue.dll)
# OpenSSL 만 외부(vcpkg `openssl` 또는 CMAKE_PREFIX_PATH 로 준 설치본) — pjproject·코어 HTTPS 가 같은 OpenSSL 을 쓴다.
#
#   cmake -S sdk/windows -B build-win -G "Visual Studio 17 2022" -A x64 ^
#         -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
#   cmake --build build-win --config Release
#   산출물: build-win/sdk/{bin/cimsue.dll,bin/cimsue-cli.exe,lib/cimsue.lib,include/cimsue/*.h}
#
# F1(엔진·cimsue-cli 실빌드) 에서 확정할 것: pjproject CMake 가 설치하는 라이브러리 이름(PJ_LIBS 기본값), 링크 순서,
# WMME 장치 열거. 실패 시 폴백은 pjproject-vs14.sln(같은 트리·같은 config_site).
cmake_minimum_required(VERSION 3.21)
project(cimsue-windows-sdk CXX C)

if(NOT WIN32)
    message(FATAL_ERROR "sdk/windows 는 Windows(MSVC) 전용 슈퍼빌드다. Linux 는 루트 CMake, Android 는 sdk/android/build-native.sh")
endif()

include(ExternalProject)
get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(PKG_WIN ${CMAKE_BINARY_DIR}/pkg)                 # 의존성·엔진 설치 prefix (커밋 안 함)
set(SDK_OUT ${CMAKE_BINARY_DIR}/sdk)                 # 배포 zip 의 원본 디렉터리

find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)

# ── 1. AMR-WB 코덱 ──
ExternalProject_Add(opencore-amrwb
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/opencore-amrwb
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PKG_WIN} -DOC_SRC=${REPO_ROOT}/ext/opencore-amr
               -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>)
ExternalProject_Add(vo-amrwbenc
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/vo-amrwbenc
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PKG_WIN} -DVO_SRC=${REPO_ROOT}/ext/vo-amrwbenc-0.1.3
               -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>)

# ── 2. 엔진 pjproject ──
# config_site.h 는 pjproject 가 gitignore 하는 파일 — Linux ExternalProject 와 같은 방식으로 한 줄 생성(정본 = sdk/engine/config_site/windows.h).
set(PJ_SRC ${REPO_ROOT}/ext/pjproject)
set(PJ_CONFIG_SITE ${REPO_ROOT}/sdk/engine/config_site/windows.h)
file(TO_CMAKE_PATH "${PJ_CONFIG_SITE}" _cs)
file(WRITE ${CMAKE_BINARY_DIR}/config_site.h "#include \"${_cs}\"\n")
ExternalProject_Add(pjproject
    SOURCE_DIR ${PJ_SRC}
    DEPENDS opencore-amrwb vo-amrwbenc
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/config_site.h ${PJ_SRC}/pjlib/include/pj/config_site.h
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PKG_WIN}
               -DCMAKE_PREFIX_PATH=${PKG_WIN}
               -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
               -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
               -DPJ_SKIP_EXPERIMENTAL_NOTICE=ON
               -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
               # 헤드리스 서버 빌드와 같은 코덱·보안 결정 (sdk/engine/config_site/common.h 와 정합)
               -DPJMEDIA_WITH_SRTP=ON -DPJSIP_WITH_TLS=ON
               -DPJMEDIA_WITH_OPENCORE_AMRWB_CODEC=ON -DPJMEDIA_WITH_OPENCORE_AMRNB_CODEC=OFF
               -DPJMEDIA_WITH_SPEEX_CODEC=OFF -DPJMEDIA_WITH_VPX_CODEC=OFF -DPJMEDIA_WITH_FFMPEG=OFF
               -DPJMEDIA_WITH_AUDIODEV_WMME=ON -DPJMEDIA_WITH_AUDIODEV_WASAPI=OFF
               -DPJMEDIA_WITH_VIDEODEV_DSHOW=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>)

# ── 3. 코어 libcimsue → cimsue.dll ──
# pjproject CMake 의 설치 라이브러리 이름(F1 에서 확정). autoconf 빌드의 `-<target>` 접미는 없다.
if(NOT DEFINED PJ_LIBS)
    set(PJ_LIBS pjsua2 pjsua-lib pjsip-ua pjsip-simple pjsip pjmedia-codec pjmedia-videodev pjmedia-audiodev
                pjmedia pjnath pjlib-util pjlib srtp resample
                opencore-amrwb vo-amrwbenc OpenSSL::SSL OpenSSL::Crypto)
endif()
set(PJ_DIR      ${PJ_SRC})
set(PJ_PKG_DIR  ${PKG_WIN})
set(PJ_INC      ${PKG_WIN}/include)
set(PJ_LIB_PATH ${PKG_WIN}/lib)
set(PJ_TARGET   "msvc-x64")
add_subdirectory(${REPO_ROOT}/sdk/core ${CMAKE_BINARY_DIR}/core)
add_dependencies(cimsue pjproject)

# ── 4. SDK 디렉터리 (zip 원본) ──
add_custom_target(sdk-layout ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SDK_OUT}/bin ${SDK_OUT}/lib ${SDK_OUT}/include
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${REPO_ROOT}/sdk/core/include ${SDK_OUT}/include
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cimsue> $<TARGET_FILE:cimsue-cli> ${SDK_OUT}/bin
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_LINKER_FILE:cimsue> ${SDK_OUT}/lib
    DEPENDS cimsue cimsue-cli
    COMMENT "sdk/ 레이아웃(bin/lib/include) 구성")
