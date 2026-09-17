# Android SDK 네이티브 빌드 — libcimsue.so (JNI) (docs/design/features/ue_sdk.md §5·§8,
#                                                docs/design/features/android_dispatch_tablet.md §2.1)
#
# sdk/windows/CMakeLists.txt 와 같은 자리의 플랫폼 래퍼다. 다만 Android 는 슈퍼빌드가 아니다 —
# 엔진(ext/pjproject)은 autoconf(`configure-android`)로 짓고 그 일은 build-native.sh 가 한다.
# 여기서는 이미 지어진 엔진 산출물 위에 sdk/core 를 NDK 로 컴파일하고 SWIG 래퍼를 붙여
# **libcimsue.so 하나**를 낸다(코어 + pj 정적 링크 + JNI 진입점).
#
#   build-native.sh 가 호출한다:
#     cmake -S sdk/android -B <build> \
#           -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
#           -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared
#
# 산출물: <build>/libcimsue.so · <java-out>/com/cims/ue/sdk/jni/*.java
#
# 엔진 바인딩(org.pjsip.**, libpjsua2.so)은 여기서 만들지 않는다 — pjproject 자체 SWIG 규칙이 내고
# build-native.sh 가 :cimsue-engine 으로 배치한다(android_dispatch_tablet.md §2.2 엔진 단일화).
cmake_minimum_required(VERSION 3.22)
project(cimsue-android CXX C)

if(NOT ANDROID)
    message(FATAL_ERROR "sdk/android 는 NDK 툴체인 전용이다. Linux 는 루트 CMake, Windows 는 sdk/windows")
endif()

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(PJ_DIR ${REPO_ROOT}/ext/pjproject)

if(NOT EXISTS ${PJ_DIR}/build.mak)
    message(FATAL_ERROR "ext/pjproject/build.mak 없음 — build-native.sh 의 configure-android 단계가 먼저다")
endif()

# ── 엔진 링크 정보는 build.mak 에 물어본다 ─────────────────────────────────────
# build.mak 은 configure-android 산출물이고 툴체인·라이브러리 목록의 정본이다. 값을 여기에 베껴 두면
# 엔진 구성(코덱 on/off 등)이 바뀔 때 조용히 어긋난다. 그래서 make 로 펼쳐 읽는다.
set(_q ${CMAKE_BINARY_DIR}/pj-query.mak)
file(WRITE ${_q} "include ${PJ_DIR}/build.mak\nprint:\n\t@echo \"$(TARGET_NAME)\"\n\t@echo \"$(APP_CFLAGS)\"\n\t@echo \"$(APP_LDFLAGS)\"\n\t@echo \"$(APP_LDLIBS)\"\n\t@echo \"$(PJSUA2_LIB_LDLIB)\"\n")
execute_process(COMMAND make -s -f ${_q} print
                WORKING_DIRECTORY ${PJ_DIR}
                OUTPUT_VARIABLE _pjq OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _pjq_rc)
if(NOT _pjq_rc EQUAL 0)
    message(FATAL_ERROR "build.mak 조회 실패 (rc=${_pjq_rc})")
endif()
string(REPLACE "\n" ";" _pjq_lines "${_pjq}")
list(GET _pjq_lines 0 PJ_TARGET)
list(GET _pjq_lines 1 _pj_cflags)
list(GET _pjq_lines 2 _pj_ldflags)
list(GET _pjq_lines 3 _pj_ldlibs)
list(GET _pjq_lines 4 _pj_pjsua2)
# APP_LDLIBS 는 pjsua2(C++ 계층)를 빼고 낸다 — 앱이 선택하는 층이라서다(build.mak 의 APP_LDLIBS 정의).
# 코어는 pjsua2 위에 있으므로 맨 앞에 붙인다(정적 링크라 순서가 의미를 갖는다). 이름도 build.mak 에 묻는다.
set(_pj_ldlibs "${_pj_pjsua2} ${_pj_ldlibs}")
string(STRIP "${PJ_TARGET}" PJ_TARGET)
if(NOT PJ_TARGET MATCHES "android")
    message(FATAL_ERROR "build.mak 의 TARGET_NAME 이 Android 가 아니다: '${PJ_TARGET}' — configure-android 를 다시 돌린다")
endif()

# -L<dir> → PJ_LIB_PATH
set(PJ_LIB_PATH "")
string(REGEX MATCHALL "-L[^ ]+" _ldirs "${_pj_ldflags}")
foreach(_d IN LISTS _ldirs)
    string(SUBSTRING ${_d} 2 -1 _d)
    list(APPEND PJ_LIB_PATH ${_d})
endforeach()

# -l<name> 과 절대경로 .so(libc++_shared) → PJ_LIBS. pj·third_party·시스템 라이브러리가 섞여 있고
# 순서가 곧 링크 순서다(정적 라이브러리라 의미가 있다) — 그대로 넘긴다.
set(PJ_LIBS "")
string(REGEX MATCHALL "(-l[^ ]+|/[^ ]+\\.so)" _llibs "${_pj_ldlibs}")
foreach(_l IN LISTS _llibs)
    if(_l MATCHES "^-l(.+)$")
        list(APPEND PJ_LIBS ${CMAKE_MATCH_1})
    else()
        list(APPEND PJ_LIBS ${_l})
    endif()
endforeach()
# 시스템 라이브러리는 위 목록에 이미 들어 있다(APP_LDLIBS 가 -lOpenSLES·-llog … 까지 포함).
# sdk/core 의 기본값(Linux 용 rt·opencore-amr 등)이 끼어들지 않게 빈 값으로 못박는다.
set(PJ_SYS_LIBS "")

# -I<dir> → PJ_INC. autoconf 빌드는 설치 prefix 가 없어 모듈별 include 를 그대로 쓴다. 여기에 엔진이
# 링크한 OpenSSL(Android arm64) 헤더도 들어 있다 — 코어의 https_client·sds_codec 이 같은 것을 써야 한다.
# 상대경로(-Ijni)는 pjproject 빌드 디렉터리 기준이라 버린다.
set(PJ_INC "")
string(REGEX MATCHALL "-I/[^ ]+" _idirs "${_pj_cflags}")
foreach(_d IN LISTS _idirs)
    string(SUBSTRING ${_d} 2 -1 _d)
    list(APPEND PJ_INC ${_d})
endforeach()
list(REMOVE_DUPLICATES PJ_INC)
if(NOT PJ_INC)
    message(FATAL_ERROR "build.mak APP_CFLAGS 에서 include 경로를 찾지 못했다")
endif()

message(STATUS "cimsue-android: target=${PJ_TARGET} abi=${ANDROID_ABI}")
message(STATUS "cimsue-android: inc=${PJ_INC}")

# ── 코어 ──
# cimsue-cli 는 데스크톱 전용이라 Android 기본값이 OFF, cimsue_test 는 GTEST_DIR 미정의로 빠진다.
add_subdirectory(${REPO_ROOT}/sdk/core ${CMAKE_BINARY_DIR}/core)

# ── SWIG Java 바인딩 ──
# 정본은 sdk/core/swig/cimsue.i 다(Windows C API 와 같은 위상). 공개 헤더가 바뀌면 재생성된다.
find_package(SWIG 4.0 REQUIRED)
set(SWIG_I      ${REPO_ROOT}/sdk/core/swig/cimsue.i)
set(SWIG_WRAP   ${CMAKE_BINARY_DIR}/cimsue_wrap.cpp)
# 생성 Java 의 소스셋 뿌리. 기본값은 AAR 모듈의 swig 소스셋이고 build-native.sh 가 덮을 수 있다.
# 아래 add_custom_command 가 이 디렉터리를 통째로 지우므로(낡은 클래스가 남으면 javac 가 엉뚱한 것을
# 컴파일한다) 빈 값이면 절대 안 된다 — 빈 값은 "/com/cims/..." 가 되어 루트를 가리킨다.
if(NOT JAVA_OUT_DIR)
    set(JAVA_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cimsue/src/swig/java)
endif()
# 상대 경로는 레포 루트 기준으로 푼다 — CMake 기본(CMAKE_CURRENT_SOURCE_DIR=sdk/android)으로 두면
# `sdk/android/cimsue/...` 같은 자연스러운 입력이 `sdk/android/sdk/android/...` 에 떨어진다.
get_filename_component(JAVA_OUT_DIR ${JAVA_OUT_DIR} ABSOLUTE BASE_DIR ${REPO_ROOT})
set(SWIG_JAVA_PKG_DIR ${JAVA_OUT_DIR}/com/cims/ue/sdk/jni)
file(GLOB CIMSUE_PUBLIC_HEADERS ${REPO_ROOT}/sdk/core/include/cimsue/*.h)
# Java 산출물도 빌드 그래프에 넣는다. wrap.cpp 만 OUTPUT 으로 걸면 — Java 는 별 디렉터리에 나므로 —
# 네이티브 캐시를 둔 채 Java 디렉터리만 지웠을 때(수동 정리·CI 가 빌드 디렉터리만 복원) SWIG 이 다시 돌지
# 않아 JNI 클래스 없는 AAR 이 나온다. 파일 목록이 가변이라 스탬프를 Java 디렉터리 안에 둬, 디렉터리가
# 사라지면 스탬프도 사라져 재생성이 걸리게 한다.
set(SWIG_JAVA_STAMP ${SWIG_JAVA_PKG_DIR}/.swig-stamp)
add_custom_command(
    OUTPUT ${SWIG_WRAP} ${SWIG_JAVA_STAMP}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${SWIG_JAVA_PKG_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SWIG_JAVA_PKG_DIR}
    COMMAND ${SWIG_EXECUTABLE} -c++ -java -package com.cims.ue.sdk.jni
            -outdir ${SWIG_JAVA_PKG_DIR} -o ${SWIG_WRAP}
            -I${REPO_ROOT}/sdk/core/include ${SWIG_I}
    COMMAND ${CMAKE_COMMAND} -E touch ${SWIG_JAVA_STAMP}
    DEPENDS ${SWIG_I} ${CIMSUE_PUBLIC_HEADERS}
    COMMENT "SWIG Java 바인딩 ← cimsue.i (생성물 = ${SWIG_JAVA_PKG_DIR})")

# ── JNI 라이브러리 ──
# 이름은 libcimsue.so — Kotlin 파사드가 System.loadLibrary("cimsue") 로 연다. 코어와 pj 를 정적으로
# 품으므로 이 .so 하나면 되고, 엔진 .so(libpjsua2.so)를 같이 실을 필요가 없다.
add_library(cimsue_jni SHARED ${SWIG_WRAP})
set_target_properties(cimsue_jni PROPERTIES OUTPUT_NAME cimsue)
# .so 와 Java 는 한 쌍이다 — .so 만 최신이고 Java 가 없는 상태로 빌드가 끝나지 않게 묶는다.
add_custom_target(cimsue_swig_java ALL DEPENDS ${SWIG_JAVA_STAMP})
add_dependencies(cimsue_jni cimsue_swig_java)
target_link_libraries(cimsue_jni PRIVATE cimsue)
# SWIG director(Listener) 가 예외·RTTI 를 쓴다. NDK 기본이 켜져 있지만 명시해 둔다.
target_compile_options(cimsue_jni PRIVATE -fexceptions -frtti)
