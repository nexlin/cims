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
# F1 에서 확정된 사실(VS 2022 17.14 / MSVC 14.44 / CMake 4.4 / vcpkg openssl 3.6):
#   - pjproject CMake 는 MSVC 에서 그대로 configure·build 된다(폴백 vs14.sln 불필요). 라이브러리 이름 = 타깃 이름(PJ_LIBS 아래).
#   - 정적 라이브러리 설치 경로가 bin/(third_party 는 bin/pjproject/third_party) — PJ_LIB_PATH 아래.
#   - config_site_sample.h 는 CMake 설치 FILE_SET 에 없다 → INSTALL_COMMAND 가 보충.
#   - CRT 는 /MD 로 명시(비우면 ExternalProject 가 /MT 로 떨어짐), pjmedia-codec 의 AMR 자동 링크 pragma 는 config_site 에서 끔.
#   - vcpkg applocal 이 OpenSSL DLL 을 실행 파일 옆에 복사한다 → sdk/bin 은 그 디렉터리를 통째로 복사.
# WMME 장치 열거·재생 라우트 이중 출력은 실기 검증 항목(ue_sdk.md §9).
cmake_minimum_required(VERSION 3.21)
project(cimsue-windows-sdk CXX C)

if(NOT WIN32)
    message(FATAL_ERROR "sdk/windows 는 Windows(MSVC) 전용 슈퍼빌드다. Linux 는 루트 CMake, Android 는 sdk/android/build-native.sh")
endif()

include(ExternalProject)
get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

# CRT 는 전 구간 동적(/MD·/MDd) — cimsue.dll·vcpkg OpenSSL(x64-windows 트리플릿)·.NET P/Invoke 앱이 한 CRT 를 공유한다.
# 비워 두면 ExternalProject 에 빈 값이 전달돼 플래그가 빠지고 cl.exe 기본(/MT)이 돼 링크 시 RuntimeLibrary 불일치(LNK2038).
if(NOT CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
set(PKG_WIN ${CMAKE_BINARY_DIR}/pkg)                 # 의존성·엔진 설치 prefix (커밋 안 함)
set(SDK_OUT ${CMAKE_BINARY_DIR}/sdk)                 # 배포 zip 의 원본 디렉터리

find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)

# ── 1. AMR-WB 코덱 ──
# ExternalProject 타깃 이름은 링크 대상 라이브러리 이름(opencore-amrwb·vo-amrwbenc — PJ_LIBS)과 겹치면 안 된다(UTILITY 타깃은 링크 불가) → ep- 접두.
ExternalProject_Add(ep-opencore-amrwb
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/opencore-amrwb
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PKG_WIN} -DOC_SRC=${REPO_ROOT}/ext/opencore-amr
               -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>)
ExternalProject_Add(ep-vo-amrwbenc
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
    DEPENDS ep-opencore-amrwb ep-vo-amrwbenc
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
               -DPJMEDIA_WITH_G7221_CODEC=OFF -DPJMEDIA_WITH_GSM_CODEC=OFF -DPJMEDIA_WITH_ILBC_CODEC=OFF
               -DPJMEDIA_WITH_G722_CODEC=OFF -DPJMEDIA_WITH_L16_CODEC=OFF
               -DPJMEDIA_WITH_SPEEX_CODEC=OFF -DPJMEDIA_WITH_VPX_CODEC=OFF -DPJMEDIA_WITH_FFMPEG=OFF
               -DPJMEDIA_WITH_AUDIODEV_WMME=ON -DPJMEDIA_WITH_AUDIODEV_WASAPI=OFF
               # 영상은 F3 (windows.h PJMEDIA_HAS_VIDEO 0 과 정합 — CMake 가 -DPJMEDIA_HAS_VIDEO 를 같이 정의하므로 옵션도 꺼야 C4005 가 없다)
               -DPJMEDIA_WITH_VIDEO=OFF -DPJMEDIA_WITH_VIDEODEV_DSHOW=OFF
               -DPJNATH_WITH_UPNP=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    # pjproject CMake 의 헤더 FILE_SET 에 config_site_sample.h 가 없다(autoconf install 은 복사) — config_site.h 가 include 하므로 보충.
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>
            COMMAND ${CMAKE_COMMAND} -E copy ${PJ_SRC}/pjlib/include/pj/config_site_sample.h ${PKG_WIN}/include/pj/config_site_sample.h)

# ── 3. 코어 libcimsue → cimsue.dll ──
# pjproject CMake 의 설치 라이브러리 이름 — 타깃 이름 그대로(pjlib·pjsua-lib, autoconf 의 `-<target>` 접미 없음). MSVC 는 순서 무관.
# third_party 는 위 옵션에서 켜진 것만 빌드된다: srtp·resample·webrtc(AEC)·webrtc_aec3. (g7221/gsm/ilbc/yuv 는 OFF 라 산출물 없음)
if(NOT DEFINED PJ_LIBS)
    set(PJ_LIBS pjsua2 pjsua-lib pjsip-ua pjsip-simple pjsip pjmedia-codec pjmedia-videodev pjmedia-audiodev
                pjmedia pjnath pjlib-util pjlib srtp resample webrtc webrtc_aec3
                opencore-amrwb vo-amrwbenc OpenSSL::SSL OpenSSL::Crypto)
endif()
set(PJ_DIR      ${PJ_SRC})
set(PJ_PKG_DIR  ${PKG_WIN})
set(PJ_INC      ${PKG_WIN}/include)
# pjproject CMake 는 정적 라이브러리(ARCHIVE)를 lib/ 가 아니라 bin/ 에, third_party(srtp·resample)는 bin/pjproject/third_party 에
# 설치한다(ext/pjproject/CMakeLists.txt·third_party/CMakeLists.txt 의 install 절). deps 래퍼(AMR-WB)는 lib/.
set(PJ_LIB_PATH ${PKG_WIN}/lib ${PKG_WIN}/bin ${PKG_WIN}/bin/pjproject/third_party)
set(PJ_TARGET   "msvc-x64")
add_subdirectory(${REPO_ROOT}/sdk/core ${CMAKE_BINARY_DIR}/core)
add_dependencies(cimsue pjproject)

# ── 4. SDK 디렉터리 (zip 원본) ──
add_custom_target(sdk-layout ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SDK_OUT}/bin ${SDK_OUT}/lib ${SDK_OUT}/include
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${REPO_ROOT}/sdk/core/include ${SDK_OUT}/include
    # bin 디렉터리 통째로 — cimsue.dll·cimsue-cli.exe 에 더해 vcpkg applocal 이 가져다 둔 OpenSSL 런타임(libcrypto-3-x64.dll·libssl-3-x64.dll)
    COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:cimsue-cli> ${SDK_OUT}/bin
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_LINKER_FILE:cimsue> ${SDK_OUT}/lib
    DEPENDS cimsue cimsue-cli
    COMMENT "sdk/ 레이아웃(bin/lib/include) 구성")
