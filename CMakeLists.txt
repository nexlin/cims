cmake_minimum_required(VERSION 3.10)
Project("TGroup MPS Projcet Base" C CXX)

set(CMAKE_CXX_STANDARD 17)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++11 -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++17 -Wall -fpermissive -fPIC")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++14 -Wall -fpermissive")
#------------------------------------------------------------------
# Global definitions and include
# All source code should be POSIX 200112L compatible, but some other extensions might be used, so:
add_definitions(-m64 ${CMAKE_C_FLAGS_DEBUG})
add_definitions(-D_GNU_SOURCE -fpermissive)
add_definitions(-D__STDC_CONSTANT_MACROS)
add_definitions(-DUSING_POLL)
add_definitions(-D_PALOGGER)
ADD_COMPILE_OPTIONS( -g -Wall -std=c++17)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH};${PROJECT_SOURCE_DIR}/cmake)

#base definitions
set(COM_LIB m z dl rt stdc++ pthread)
set(COM_LIB_PATH /lib64)
set(ARCH intel64 CACHE STRING "Set Arch [ia32|intel64]")
set(GCCVER gcc4 CACHE STRING "Set gcc version [gcc3|gcc4]")
include(ExternalProject)

set(PMP_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)


#=================================================
set(EXT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext)
set(PKG_BASE ${CMAKE_CURRENT_SOURCE_DIR}/pkg)

set(PASF     pasf)
set(PASF_DIR ${EXT_DIR}/${PASF})
set(PASF_INC ${PASF_DIR}/include)
set(PASF_LIB_PATH ${PASF_DIR}/lib)
set(PASF_BIN_PATH ${TMPA_BIN_PATH})
set(PASF_LIB ${PASF})
set(BUILD_BASE ${CMAKE_CURRENT_SOURCE_DIR}/build)

#set(PDK_EXT pdk-3.2.5)
#set(PDK_DIR ${EXT_DIR}/${PDK_EXT})  
#set(PDK_PKG_DIR ${PKG_BASE}/${PDK_EXT})  
#set(PDK_INC ${PDK_DIR}/include) 
#set(PALOG_INC ${PDK_DIR}/palogging-include) 
#set(PDK_LIB_PATH ${PDK_PKG_DIR}) 
#set(PDK_LIB_PATH ${PDK_DIR}) 
#set(PDK_LIB ${PDK_EXT} ) 
#set(PALOG_LIB palogging-pdk) 

#PDK Project 
#set(HUDTMF_EXT hudtmf)
#set(HUDTMF_DIR ${EXT_DIR}/${HUDTMF_EXT})  
#set(HUDTMF_INC ${HUDTMF_DIR}) 
#set(HUDTMF_PKG_DIR ${PKG_BASE}/${HUDTMF_EXT})  
#set(HUDTMF_LIB_PATH ${HUDTMF_PKG_DIR}/lib) 
#set(HUDTMF_LIB ${HUDTMF_EXT}) 

#RTP Project
#set(AMTRTP_EXT amtrtp)
#set(AMTRTP_DIR ${EXT_DIR}/${AMTRTP_EXT})  
#set(AMTRTP_INC ${AMTRTP_DIR}) 
#set(AMTRTP_PKG_DIR ${PKG_BASE}/${AMTRTP_EXT})
#set(AMTRTP_LIB_PATH ${AMTRTP_PKG_DIR}/lib) 
#set(AMTRTP_LIB ${AMTRTP_EXT}) 


#set(PMPROC pmproc)
#set(PMPROC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${PMPROC})
#set(PMPROC_INC ${PMPROC_DIR}/include)
#set(PMPROC_LIB_PATH ${PMPROC_DIR}/lib)
#set(PMPROC_BIN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)
#set(PMPROC_LIB ${PMPROC})


#----------------------------------------------------
#opencore-amr Project
set(AMR_EXT opencore-amr)
set(AMR_DIR ${EXT_DIR}/${AMR_EXT})
set(AMR_PKG_DIR ${PKG_BASE}/${AMR_EXT})
set(AMR_INC ${AMR_PKG_DIR}/include/opencore-amrnb ${AMR_PKG_DIR}/include/opencore-amrwb)
set(AMR_LIB_PATH ${AMR_PKG_DIR}/lib)
set(AMR_LIB opencore-amrnb opencore-amrwb)


ExternalProject_Add(
   ${AMR_EXT}
   SOURCE_DIR ${AMR_DIR}
   PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/build/opencore-amr
   CONFIGURE_COMMAND ./configure --prefix=${AMR_PKG_DIR} --enable-static --disable-shared
   BUILD_IN_SOURCE 1 
   DOWNLOAD_COMMAND ""
   UPDATE_COMMAND ""
   BUILD_COMMAND ""
   INSTALL_COMMAND make install
)

#----------------------------------------------------
#vo-amrwbenc-0.1.3 Project
#--------------------------------------------------------
set(AMRWB_EXT      vo-amrwbenc-0.1.3)
set(AMRWB_DIR      ${EXT_DIR}/${AMRWB_EXT})
set(AMRWB_PKG_DIR  ${PKG_BASE}/${AMRWB_EXT})
set(AMRWB_INC ${AMRWB_PKG_DIR}/include/vo-amrwbenc)
set(AMRWB_LIB_PATH ${AMRWB_PKG_DIR}/lib)
set(AMRWB_LIB      vo-amrwbenc)
ExternalProject_Add(
   ${AMRWB_EXT}
   SOURCE_DIR ${AMRWB_DIR}
   PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/build/vo-amrwbenc-0.1.3
   #CONFIGURE_COMMAND ./configure --prefix=${AMRWB_PKG_DIR} --enable-shared  --enable-static
   CONFIGURE_COMMAND ./configure --prefix=${AMRWB_PKG_DIR} --enable-static --disable-shared
   BUILD_IN_SOURCE 1 
   DOWNLOAD_COMMAND ""
   UPDATE_COMMAND ""
   BUILD_COMMAND ""
   INSTALL_COMMAND make install
)


#----------------------------------------------------
#libsrtp Project — 미디어 SRTP 엔진 (docs/design/features/media_security.md §6.1)
#  upstream 2.5.x 를 ext/libsrtp 로 독립 vendoring (air-gapped 자족, OpenSSL EVP 백엔드).
#  ⚠️ ext/pjproject/third_party/srtp(동봉분)는 pre-commit 훅이 pjproject fork 에서
#  동기화하는 트리 — 서버 빌드가 참조하면 단말 빌드 동기화가 서버 빌드를 흔든다. 링크 금지.
#--------------------------------------------------------
set(SRTP_EXT libsrtp)
set(SRTP_DIR ${EXT_DIR}/${SRTP_EXT})
set(SRTP_PKG_DIR ${PKG_BASE}/${SRTP_EXT})
set(SRTP_INC ${SRTP_PKG_DIR}/include)
set(SRTP_LIB_PATH ${SRTP_PKG_DIR}/lib)
set(SRTP_LIB srtp2)
ExternalProject_Add(
   ${SRTP_EXT}
   SOURCE_DIR ${SRTP_DIR}
   PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/build/libsrtp
   CONFIGURE_COMMAND ./configure --prefix=${SRTP_PKG_DIR} --enable-openssl
   BUILD_IN_SOURCE 1
   DOWNLOAD_COMMAND ""
   UPDATE_COMMAND ""
   # 기본 all 은 test 까지 빌드 — 정적 라이브러리만 필요하다 (install 은 빌드를 안 함)
   BUILD_COMMAND make libsrtp2.a
   INSTALL_COMMAND make install
)

#----------------------------------------------------
# googletest Project
#--------------------------------------------------------
set(GTEST_EXT googletest)
set(GTEST_DIR ${EXT_DIR}/${GTEST_EXT})
set(GTEST_LIB_PATH ${GTEST_DIR}/lib64)
set(GTEST_INC ${GTEST_DIR}/src/googlemock/include/gmock ${GTEST_DIR}/src/googletest/include/gtest ${GTEST_DIR}/src/googletest/include)
set(GTEST_LIB gmock gtest)
#subdirs(${GTEST_DIR}/googletest)
ExternalProject_Add(
   ${GTEST_EXT}
   PREFIX  ${GTEST_DIR}/tmp
   #DOWNLOAD_DIR  ${EXT_DIR}
   GIT_REPOSITORY  https://github.com/google/googletest.git
   GIT_TAG release-1.10.0
   INSTALL_DIR  ${GTEST_DIR} 
   CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${GTEST_DIR}" "-DCMAKE_CXX_FLAGS=-w -include cstdint" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
   SOURCE_DIR ${GTEST_DIR}/src
   BINARY_DIR ${GTEST_DIR}
)

#----------------------------------------------------
# X264 Project
#----------------------------------------------------
#set(X264_EXT x264)
#set(X264_DIR ${PKG_BASE}/${X264_EXT})
#set(X264_SRC ${X264_DIR}/src)
#set(X264_LIB_PATH ${X264_DIR}/lib)
#set(X264_INC ${X264_DIR}/include)
#set(X264_LIB ${X264_EXT})

#ExternalProject_Add(
#	  ${X264_EXT}
#	  GIT_REPOSITORY https://code.videolan.org/videolan/x264.git
#	  DOWNLOAD_DIR  ${X264_DIR}
	  #GIT_TAG n4.3.2
#	  INSTALL_DIR  ${X264_DIR}
#	  SOURCE_DIR ${X264_SRC}
#	  BINARY_DIR ${X264_DIR}
#	  CONFIGURE_COMMAND ${X264_SRC}/configure --prefix=${X264_DIR} --disable-shared  --enable-static --disable-asm
#   INSTALL_COMMAND make install
#)



#----------------------------------------------------
# FFMPEG Project
#----------------------------------------------------
#set(FFM_EXT ffmpeg)
#set(FFM_DIR ${PKG_BASE}/l_ffmpeg)
#set(FFM_SRC ${FFM_DIR}/src)
#set(FFM_LIB_PATH ${FFM_DIR}/lib)
#set(FFM_INC
#   "${FFM_DIR}/include"
#)
#set(FFM_PKG_PATH "${AMR_PKG_DIR}/lib/pkgconfig:${AMRWB_PKG_DIR}/lib/pkgconfig:${X264_DIR}/lib/pkgconfig")
#
#
#set(FFM_LIB avformat avfilter avcodec avutil postproc swresample swscale bz2)
#set(FFM_LIB avformat avfilter avcodec avutil postproc swresample swscale )
#
#
#ExternalProject_Add(
#	  ${FFM_EXT}
#	  GIT_REPOSITORY  https://git.ffmpeg.org/ffmpeg.git
#	  DOWNLOAD_DIR  ${FFM_DIR}
#	  GIT_TAG n4.3.2
#	  INSTALL_DIR  ${FFM_DIR}
#	  SOURCE_DIR ${FFM_DIR}/src
#	  BINARY_DIR ${FFM_DIR}
#      DEPENDS ${AMR_EXT} ${AMRWB_EXT}
#	  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env "CFLAGS=-I${AMR_PKG_DIR}/include -I${AMRWB_PKG_DIR}/include" "LDFLAGS=-L${AMR_PKG_DIR}/lib -L${AMRWB_PKG_DIR}/lib" PATH=$ENV{PATH} ${FFM_SRC}/configure --prefix=${FFM_DIR} --disable-shared --enable-static  --enable-version3 --enable-gpl --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libvo-amrwbenc --disable-libx264 --disable-amf --disable-audiotoolbox --disable-cuda-nvcc --disable-cuda-llvm --disable-cuvid --disable-d3d11va --disable-dxva2 --disable-ffnvcodec --disable-libdrm --disable-libmfx --disable-libnpp --disable-nvdec --disable-nvenc --disable-v4l2-m2m --disable-vaapi --disable-vdpau --disable-videotoolbox --disable-x86asm --disable-asm --enable-nonfree --enable-pthreads
#   INSTALL_COMMAND make install
#)
#
#message("PATH=$ENV{PATH} PKG_CONFIG_PATH=${FFM_PKG_PATH} ${FFM_SRC}/configure ${FFM_SRC}/configure --prefix=${FFM_DIR} --disable-shared --enable-static  --enable-version3 --enable-gpl --enable-libopencore-amrnb --enable-libopencore-amrwb --enable-libvo-amrwbenc --enable-libx264 --disable-amf --disable-audiotoolbox --disable-cuda-nvcc --disable-cuda-llvm --disable-cuvid --disable-d3d11va --disable-dxva2 --disable-ffnvcodec --disable-libdrm --disable-libmfx --disable-libnpp --disable-nvdec --disable-nvenc --disable-v4l2-m2m --disable-vaapi --disable-vdpau --disable-videotoolbox --disable-x86asm") 



#----------------------------------------------------
# oneTBB Project
#--------------------------------------------------------
set(TBB_EXT      oneTBB)
set(TBB_PFX     ${EXT_DIR}/${TBB_EXT}_down)
set(TBB_DIR     ${TBB_PFX}/src/${TBB_EXT})
set(TBB_SRC_INC ${TBB_DIR}/include)
set(TBB_BUILD_PATH ${TBB_PFX}/src/${TBB_EXT}-build)

set(TBB_PKG_DIR  ${PKG_BASE}/${TBB_EXT})
set(TBB_INC      ${TBB_PKG_DIR}/include)
set(TBB_LIB_PATH ${TBB_PKG_DIR}/lib)
#set(TBB_LIB  tbb tbbmalloc tbbmalloc_proxy)
set(TBB_LIB  tbb tbbmalloc)

include(ExternalProject)
ExternalProject_Add(
   ${TBB_EXT}
   PREFIX ${TBB_PFX}
   GIT_REPOSITORY https://github.com/oneapi-src/oneTBB.git
   GIT_TAG tbb_2020
   #BUILD_IN_SOURCE 1
   CONFIGURE_COMMAND ""
   #BUILD_COMMAND make -C src -j8 run_cmd=true {_tbb_components_cfg} tbb_build_dir=${_tbb_root}/src tbb_build_prefix=tbb
   BUILD_COMMAND  
	  make -C ${TBB_DIR} -j4 run_cmd=true tbb_build_dir=${TBB_BUILD_PATH} tbb_build_prefix=tbb
	  #make -C ${TBB_DIR} -j4 extra_inc=big_iron.inc run_cmd=true tbb_build_dir=${TBB_BUILD_PATH} tbb_build_prefix=tbb
   INSTALL_COMMAND 
      sh -c "mkdir -p ${TBB_LIB_PATH} && mkdir -p ${TBB_INC} && cp ${TBB_BUILD_PATH}/tbb_release/lib*.so* ${TBB_LIB_PATH}/ && cp -R ${TBB_SRC_INC}/tbb ${TBB_INC}/"
      #sh -c "mkdir -p ${TBB_LIB_PATH} && mkdir -p ${TBB_INC} && cp ${TBB_BUILD_PATH}/tbb_release/lib*.so* ${TBB_LIB_PATH}/ && cp ${TBB_BUILD_PATH}/tbb_release/lib*.a ${TBB_LIB_PATH}/ && cp -R ${TBB_SRC_INC}/tbb ${TBB_INC}/"
)

#========================================
#jitter library
#set(JT_DIR ${EXT_DIR}/jitter)
#set(JT_LIB_PATH ${PKG_BASE}/jitter)
#set(JT_INC ${JT_DIR})
#set(JT_LIB jtlib)
#
#INCLUDE_DIRECTORIES(${PDK_INC} ${PALOG_INC} ${AMTRTP_INC} ${TBB_INC})
#
#message (${FFM_INC})
#message (${FFM_LIB_PATH})
#message (${FFM_LIB})
#
#INCLUDE_DIRECTORIES(${FFM_INC} ${X264_INC} ${PDK_INC} ${AMTRTP_INC} ${HUDTMF_INC} ${AMR_INC} ${AMRWB_INC} ${TBB_INC} ${JT_INC} ${PASF_INC} ${PMP_INC} ) 
#
#LINK_DIRECTORIES(${FFM_LIB_PATH} ${X264_LIB_PATH} ${GTEST_LIB_PATH} ${AMTRTP_LIB_PATH} ${PDK_LIB_PATH} ${AMR_LIB_PATH} ${AMRWB_LIB_PATH} ${HUDTMF_PATH} ${TBB_LIB_PATH} ${JT_LIB_PATH}  ${PASF_LIB_PATH} )
#
#
#message("LINK_DIRECTORIES(${FFM_LIB_PATH} ${X264_LIB_PATH} ${GTEST_LIB_PATH} ${AMTRTP_LIB_PATH} ${PDK_LIB_PATH} ${AMR_LIB_PATH} ${AMRWB_LIB_PATH} ${HUDTMF_PATH} ${TBB_LIB_PATH} ${JT_LIB_PATH}  ${PASF_LIB_PATH} )")
#
#message("LINK_LIBRARIES( ${X264_LIB} ${AMTRTP_LIB} ${GTEST_LIB} ${PDK_LIB} ${PALOG_LIB} ${AMR_LIB} ${AMRWB_LIB} ${TBB_LIB} ${HUDTMF_LIB} ${JT_LIB} ${PASF_LIB} ${PMPROC_LIB} ${FFM_LIB} pthread rt m)")
#----------------------------------------------------
# pjproject — 단말 SDK(libcimsue) 엔진 (docs/design/features/ue_sdk.md §3·§8)
#  ext/pjproject 가 Linux/Android/Windows 공통 소스 정본. 여기서는 Linux(개발 서버·CI) 빌드 —
#  libcimsue 단위시험·헤드리스 UE `cimsue-cli` 가 링크한다. Android(NDK)는 sdk/android/build-native.sh,
#  Windows(MSVC)는 pjproject-vs14.sln 이 같은 트리를 빌드한다.
#  config_site.h(pjproject 가 gitignore) 는 sdk/engine/config_site/linux.h 를 #include 하는 한 줄로 생성.
#  aconfigure 산출물(build.mak·os-auto.mak·*_auto.h·.depend·output/·lib/)은 전부 pjproject .gitignore
#  대상 — in-source 빌드가 트리를 더럽히지 않는다.
#  ⚠️ 동봉 third_party/srtp 는 단말 엔진 전용 — 서버(CMP)의 ext/libsrtp 와 서로 링크하지 않는다.
#--------------------------------------------------------
option(CIMS_UE_SDK "단말 SDK 엔진(ext/pjproject) 빌드 — ue_sdk.md" ON)
if(CIMS_UE_SDK)
set(PJ_EXT      pjproject)
set(PJ_DIR      ${EXT_DIR}/${PJ_EXT})
set(PJ_PKG_DIR  ${PKG_BASE}/${PJ_EXT})
set(PJ_INC      ${PJ_PKG_DIR}/include)
set(PJ_LIB_PATH ${PJ_PKG_DIR}/lib)
set(PJ_CONFIG_SITE ${CMAKE_CURRENT_SOURCE_DIR}/sdk/engine/config_site/linux.h)
ExternalProject_Add(
   ${PJ_EXT}
   SOURCE_DIR ${PJ_DIR}
   PREFIX ${BUILD_BASE}/pjproject
   DEPENDS ${AMR_EXT} ${AMRWB_EXT}
   DOWNLOAD_COMMAND ""
   UPDATE_COMMAND ""
   BUILD_IN_SOURCE 1
   CONFIGURE_COMMAND sh -c "printf '#include \"%s\"\\n' '${PJ_CONFIG_SITE}' > pjlib/include/pj/config_site.h"
             COMMAND env CFLAGS=-fPIC CXXFLAGS=-fPIC ./aconfigure --prefix=${PJ_PKG_DIR}
                     --disable-sound --disable-video --disable-sdl --disable-ffmpeg --disable-v4l2
                     --disable-libyuv --disable-openh264 --disable-vpx --disable-libwebrtc
                     --disable-gsm-codec --disable-ilbc-codec --disable-g722-codec --disable-g7221-codec
                     --disable-l16-codec --disable-opus --disable-bcg729 --disable-silk
                     # speex 는 configure 로 끄지 않는다 — third_party/speex 가 AEC(PJMEDIA_HAS_SPEEX_AEC) 심볼을
                     # 제공하므로 --disable-speex-codec 은 echo_common.o 링크 실패를 낳는다. 코덱 등록만
                     # config_site(common.h PJMEDIA_HAS_SPEEX_CODEC 0)로 끈다.
                     --with-opencore-amr=${AMR_PKG_DIR} --with-opencore-amrwbenc=${AMRWB_PKG_DIR}
                     --enable-epoll
   # `lib` = 라이브러리 디렉터리만(pjsip-apps 제외). dep 는 .depend 생성 — 최초·헤더 변경 시 필요
   BUILD_COMMAND make dep
         COMMAND make lib
   INSTALL_COMMAND make install
)
# 코어 libcimsue + cimsue-cli + cimsue_test — pasf 전역 LINK_LIBRARIES 가 걸리기 전에 포함한다
add_subdirectory(sdk/core)
endif()

add_subdirectory(${PASF_DIR})

INCLUDE_DIRECTORIES(${PASF_INC})
LINK_DIRECTORIES(${PASF_LIB_PATH})
LINK_LIBRARIES( ${PASF_LIB} pthread rt m) 

#add_subdirectory(${PMPROC_DIR})


add_subdirectory(cmp)
add_subdirectory(cmdp)
add_subdirectory(csp)
add_subdirectory(cspsim)
# cwrtc — 재설계 예정. 빌드/dist/패키징 제외 (cims-phone 웹 UI 도 함께 제외).
#add_subdirectory(cwrtc)

# --- Distribution Target ---
add_custom_target(dist
    COMMENT "Creating distribution package..."

    # ── 1. 디렉터리 생성 ───────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmdp/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmdp/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmdp/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmdp/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/vendor
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/user
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/group
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/route
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/src
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/log

    # OAM 분리 Phase 1+3 — top-level oam/ (Phase 1: csc 와 같은 binary, Phase 3: oam_app.py 별도 프로세스)
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/oam/src
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/oam/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/oam/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/ext_mnt
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/console

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cspsim/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cspsim/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/run
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/log

    # ── 2. 바이너리 복사 ────────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/cmp      ${CMAKE_BINARY_DIR}/dist/cmp/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/cmdp     ${CMAKE_BINARY_DIR}/dist/cmdp/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/csp      ${CMAKE_BINARY_DIR}/dist/csp/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/csp.sh   ${CMAKE_BINARY_DIR}/dist/csp/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/cspsim   ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cspsim/SipClient.xml       ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cspsim/SipClientCaller.xml ${CMAKE_BINARY_DIR}/dist/cspsim/bin/

    # ── 3. 설정 템플릿 복사 (deploy_value: "@PLACEHOLDER@" 포함, configure.sh 로 채움) ──
    #    CSP/CMP/CSC 모두 A안 통합 — config_template.json 하나가 UI 메타+deploy_value 둘 다 담당.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmp/config/config_template.json   ${CMAKE_BINARY_DIR}/dist/cmp/config/config_template.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmdp/config/config_template.json  ${CMAKE_BINARY_DIR}/dist/cmdp/config/config_template.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/config/config_template.json   ${CMAKE_BINARY_DIR}/dist/csp/config/config_template.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/config/fm_catalog.json        ${CMAKE_BINARY_DIR}/dist/csp/config/fm_catalog.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmp/config/fm_catalog.json        ${CMAKE_BINARY_DIR}/dist/cmp/config/fm_catalog.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmdp/config/fm_catalog.json       ${CMAKE_BINARY_DIR}/dist/cmdp/config/fm_catalog.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/config/fm_catalog.json        ${CMAKE_BINARY_DIR}/dist/csc/config/fm_catalog.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/config/config_template.json   ${CMAKE_BINARY_DIR}/dist/csc/config/config_template.json

    # ── 3a. base <module>.json 생성 (config_template.json 의 field.default) ──
    #    configure 미실행 상용 흐름(build→pkg→설치→web 설정→start)에서도 모듈이
    #    기동 시 읽는 base conf 가 tarball 에 항상 포함되도록 보장. configure.sh 는
    #    이후 같은 파일을 env(@VAR@) 값으로 덮어씀(dev). 변종(psp/isp/pmp/imp)은
    #    cmd_pkg staging 이 config/<base>.json → config/<variant>.json 으로 자동 rename.
    #    cmp/csp 는 C++ 바이너리라 base conf 부재 시 hard-fail → 대상. csc(Python)는
    #    config 부재를 tolerate(soft) 하므로 비대상.
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_default_config.py ${CMAKE_BINARY_DIR}/dist/cmp/config/config_template.json ${CMAKE_BINARY_DIR}/dist/cmp/config/cmp.json
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_default_config.py ${CMAKE_BINARY_DIR}/dist/cmdp/config/config_template.json ${CMAKE_BINARY_DIR}/dist/cmdp/config/cmdp.json
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_default_config.py ${CMAKE_BINARY_DIR}/dist/csp/config/config_template.json ${CMAKE_BINARY_DIR}/dist/csp/config/csp.json

    # ── 4. CSP 데이터 복사 ─────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csp/User        ${CMAKE_BINARY_DIR}/dist/csp/user
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csp/Group       ${CMAKE_BINARY_DIR}/dist/csp/group
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/route

    # ── 4b. CSP OS 의존 deb 동봉 (air-gapped 정식 설치) ────────
    #    csp 는 C++ 바이너리라 libmariadb.so.3 가 실행 전제다(가입자·그룹 DB). apt repo 가
    #    없는 노드를 위해 deb 를 패키지에 싣고, agent 가 **모듈 설치 시점에** dpkg 로 설치한다
    #    (cims_agent `_install_module_deps` → cims-priv `module-deps-install`).
    #    옛 방식(.so 를 csp/lib/ 에 복사 + RPATH `$ORIGIN/../lib`)은 대상에서 설치가 일어나지
    #    않아 dpkg/ldconfig 가 존재를 모르고, 실행 파일의 상대 경로 규칙 하나에만 의존했다.
    #    그 규칙은 파일 capability 가 붙는 순간 무효가 된다(AT_SECURE — 커널이 $ORIGIN 무시).
    #    IPsec 용 setcap 이 걸리자 csp 가 "cannot open shared object file" 로 즉시 죽었다.
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csp/vendor ${CMAKE_BINARY_DIR}/dist/csp/vendor

    # ── 5. 인증서 복사 ─────────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/csp.pem  ${CMAKE_BINARY_DIR}/dist/csp/cert/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/csp.pem  ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/cert/server.key ${CMAKE_BINARY_DIR}/dist/csc/cert/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/cert/server.crt ${CMAKE_BINARY_DIR}/dist/csc/cert/

    # ── 6. CSC Python 소스 복사 ────────────────────────────────
    # copy_directory 는 기존 파일을 덮어쓰지만 삭제된 소스 파일은 반영 안 됨 → 먼저 rm -rf
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/csc/src
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csc/src ${CMAKE_BINARY_DIR}/dist/csc/src

    # Phase 4 vendor: private 환경 대응 — pip install --target 결과를 그대로 tarball 에.
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/csc/vendor
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csc/vendor ${CMAKE_BINARY_DIR}/dist/csc/vendor

    # ── 6a. OAM Python 소스 + config + vendor 복사 ─────────────
    #    Phase 1: csc_app.py 가 oam/src 를 sys.path mount.
    #    Phase 3: oam_app.py 별도 프로세스 (oam-tb.json / oam.json).
    #    Phase 4: oam/vendor 에 OAM 전용 + 공통 의존성.
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/oam/src
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ems/core/oam/src ${CMAKE_BINARY_DIR}/dist/oam/src
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ems/core/oam/config ${CMAKE_BINARY_DIR}/dist/oam/config
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/oam/vendor
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ems/core/oam/vendor ${CMAKE_BINARY_DIR}/dist/oam/vendor

    # ── 6b. Agent 에셋 복사 (CSC 가 /install-agent.sh 로 서빙) ──
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/agent
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/agent ${CMAKE_BINARY_DIR}/dist/agent

    # ── 6c. 컴포넌트별 pkg.json 복사 (cims.sh pkg 가 description 에 사용) ──
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/pkg.json          ${CMAKE_BINARY_DIR}/dist/csp/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmp/pkg.json          ${CMAKE_BINARY_DIR}/dist/cmp/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmdp/pkg.json         ${CMAKE_BINARY_DIR}/dist/cmdp/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/pkg.json          ${CMAKE_BINARY_DIR}/dist/csc/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/ems/core/oam/pkg.json          ${CMAKE_BINARY_DIR}/dist/oam/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/ems/core/console/pkg.json ${CMAKE_BINARY_DIR}/dist/console/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cspsim/pkg.json       ${CMAKE_BINARY_DIR}/dist/cspsim/pkg.json

    # ── 7. Web UI nginx 설정 복사 ──────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/ems/core/console/nginx.conf ${CMAKE_BINARY_DIR}/dist/console/nginx.conf

    # ── 8. 관리 스크립트 복사 ──────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/configure.sh ${CMAKE_BINARY_DIR}/dist/configure.sh
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cims.sh      ${CMAKE_BINARY_DIR}/dist/cims.sh
    COMMAND chmod +x ${CMAKE_BINARY_DIR}/dist/configure.sh
    COMMAND chmod +x ${CMAKE_BINARY_DIR}/dist/cims.sh
    COMMAND ${CMAKE_COMMAND} -E echo "dist done: ${CMAKE_BINARY_DIR}/dist"
)

# Independent cspsim distribution target
add_custom_target(dist_cspsim
    COMMENT "Creating cspsim distribution package..."
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cspsim/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cspsim/log
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/cspsim ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cspsim/SipClient.xml ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cspsim/SipClientCaller.xml ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/csp/csp.pem ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
)

# ─────────────────────────────────────────────────────────────
# verify-scenarios — deployment/ 의 모든 env × scenario 회기 검증
#   render.py --check-only 일괄. csp .cpp 헤더 schema 와 yaml 정합 회기.
#   CI 또는 코드 변경 후 1초 내 결과 확인.
# 사용: cmake --build . --target verify-scenarios   (or `make verify-scenarios`)
# ─────────────────────────────────────────────────────────────
add_custom_target(verify-scenarios
    COMMENT "Verifying deployment scenarios (render --check-only)..."
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/deployment/bin/check-all.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deployment
)



#--------------------------------------------------------
#function define
#--------------------------------------------------------
function (EXCLUDE_FILES_FROM_DIR_IN_LIST _InFileList _excludeDirName _verbose)
  foreach (ITR ${_InFileList})
    if ("${_verbose}")
      message(STATUS "ITR=${ITR}")
    endif ("${_verbose}")

    if ("${ITR}" MATCHES "(.*)${_excludeDirName}(.*)")                   # Check if the item matches the directory name in _excludeDirName
      message(STATUS "Remove Item from List:${ITR}")
      list (REMOVE_ITEM _InFileList ${ITR})                              # Remove the item from the list
    endif ("${ITR}" MATCHES "(.*)${_excludeDirName}(.*)")

  endforeach(ITR)
  set(SOURCE_FILES ${_InFileList} PARENT_SCOPE)                          # Return the SOURCE_FILES variable to the calling parent
endfunction (EXCLUDE_FILES_FROM_DIR_IN_LIST)

