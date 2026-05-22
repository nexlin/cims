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
set(PASF_LIB_PATH ${PASF_DIR}/lib})
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
add_subdirectory(${PASF_DIR})

INCLUDE_DIRECTORIES(${PASF_INC})
LINK_DIRECTORIES(${PASF_LIB_PATH})
LINK_LIBRARIES( ${PASF_LIB} pthread rt m) 

#add_subdirectory(${PMPROC_DIR})


add_subdirectory(cmp)
add_subdirectory(csp)
add_subdirectory(cspsim)
add_subdirectory(cwrtc)

# --- Distribution Target ---
add_custom_target(dist
    COMMENT "Creating distribution package..."

    # ── 1. 디렉터리 생성 ───────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cmp/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/user
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/group
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/route
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cwrtc/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cwrtc/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cwrtc/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cwrtc/html
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cwrtc/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/src
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/cert
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csc/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/ext_mnt
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/console
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/phone

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cspsim/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/cspsim/log

    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/run
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/log

    # ── 2. 바이너리 복사 ────────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/cmp      ${CMAKE_BINARY_DIR}/dist/cmp/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/csp      ${CMAKE_BINARY_DIR}/dist/csp/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/csp.sh   ${CMAKE_BINARY_DIR}/dist/csp/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/cwrtc    ${CMAKE_BINARY_DIR}/dist/cwrtc/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/bin/cspsim   ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cspsim/SipClient.xml       ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cspsim/SipClientCaller.xml ${CMAKE_BINARY_DIR}/dist/cspsim/bin/

    # ── 3. 설정 템플릿 복사 (deploy_value: "@PLACEHOLDER@" 포함, configure.sh 로 채움) ──
    #    CSP/CMP/CSC 모두 A안 통합 — config_template.json 하나가 UI 메타+deploy_value 둘 다 담당.
    #    cwrtc 만 레거시 .json.template 유지 (향후 전환 대상).
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmp/config/config_template.json   ${CMAKE_BINARY_DIR}/dist/cmp/config/config_template.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/config/config_template.json   ${CMAKE_BINARY_DIR}/dist/csp/config/config_template.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cwrtc/config/cwrtc.json.template  ${CMAKE_BINARY_DIR}/dist/cwrtc/config/cwrtc.json.template
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/config/config_template.json   ${CMAKE_BINARY_DIR}/dist/csc/config/config_template.json

    # ── 4. CSP 데이터 복사 ─────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csp/User        ${CMAKE_BINARY_DIR}/dist/csp/user
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csp/Group       ${CMAKE_BINARY_DIR}/dist/csp/group
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/dist/csp/route

    # ── 5. 인증서 복사 ─────────────────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/csp.pem  ${CMAKE_BINARY_DIR}/dist/csp/cert/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/csp.pem  ${CMAKE_BINARY_DIR}/dist/cwrtc/cert/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/csp.pem  ${CMAKE_BINARY_DIR}/dist/cspsim/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/cert/server.key ${CMAKE_BINARY_DIR}/dist/csc/cert/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/cert/server.crt ${CMAKE_BINARY_DIR}/dist/csc/cert/

    # ── 6. CSC Python 소스 복사 ────────────────────────────────
    # copy_directory 는 기존 파일을 덮어쓰지만 삭제된 소스 파일은 반영 안 됨 → 먼저 rm -rf
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/csc/src
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/csc/src ${CMAKE_BINARY_DIR}/dist/csc/src

    # ── 6b. Agent 에셋 복사 (CSC 가 /install-agent.sh 로 서빙) ──
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/agent
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/agent ${CMAKE_BINARY_DIR}/dist/agent

    # ── 6c. 컴포넌트별 pkg.json 복사 (cims.sh pkg 가 description 에 사용) ──
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csp/pkg.json          ${CMAKE_BINARY_DIR}/dist/csp/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cmp/pkg.json          ${CMAKE_BINARY_DIR}/dist/cmp/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/csc/pkg.json          ${CMAKE_BINARY_DIR}/dist/csc/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cwrtc/pkg.json        ${CMAKE_BINARY_DIR}/dist/cwrtc/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cims-console/pkg.json ${CMAKE_BINARY_DIR}/dist/console/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cims-phone/pkg.json   ${CMAKE_BINARY_DIR}/dist/phone/pkg.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cspsim/pkg.json       ${CMAKE_BINARY_DIR}/dist/cspsim/pkg.json

    # ── 7. Web UI nginx 설정 복사 ──────────────────────────────
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cims-console/nginx.conf ${CMAKE_BINARY_DIR}/dist/console/nginx.conf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/cims-phone/nginx.conf   ${CMAKE_BINARY_DIR}/dist/phone/nginx.conf

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

