#
#Project("pmproc library" C CXX)

include_directories(
   ./
   ${PMP_INC}
   ${PMPROC_INC}
   ${SRTP_INC}
   ${AMR_INC}
   ${AMRWB_INC}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

#   ${FFM_LIB_PATH}
message(" 111 : ${FFM_LIB_PATH}")
message(" ###> ${PMPROC_BIN_PATH}")
message(" ***> ${PMPROC_LIB_PATH}")
#message("${FFM_LIB}")


#add_executable(pmpagent pmpagent.cpp)
#add_executable(prtpsim prtpsim.cpp)
add_executable(cmp PMain.cpp PCmpServer.cpp PMcpttGroup.cpp PFloorCodec.cpp PFloorCrypto.cpp PMediaCrypto.cpp PRtpRelay.cpp PRtpTap.cpp PRtpMulticast.cpp PPttMemberPort.cpp PSyncRtpRecorder.cpp PTranscoder.cpp PAnnCatalog.cpp PAnnPlayer.cpp PAnnTicker.cpp)

# 미디어 SRTP 종단 (RFC 3711/4568, e2ae) — ext/libsrtp vendoring (media_security.md §6.1).
# floor control(RTCP) SRTCP 는 자체 구현(PFloorCrypto, TS 33.180) — OpenSSL 직접 사용.
# 피어 leg 트랜스코딩(cmp.md §11) — AMR-WB 디코더 opencore-amrwb · 인코더 vo-amrwbenc (ExternalProject, 정적).
add_dependencies(cmp ${SRTP_EXT} ${AMR_EXT} ${AMRWB_EXT})
find_package(OpenSSL REQUIRED)
target_link_libraries(cmp ${SRTP_LIB_PATH}/libsrtp2.a ${AMR_LIB_PATH}/libopencore-amrwb.a ${AMRWB_LIB_PATH}/libvo-amrwbenc.a OpenSSL::Crypto)

set(TEST_SRCS
   #pmpagent 
   #prtpsim 
   cmp
)


install(TARGETS ${TEST_SRCS}  
	    RUNTIME DESTINATION ${PMPROC_BIN_PATH}
		LIBRARY DESTINATION ${PMPROC_LIB_PATH}
		ARCHIVE DESTINATION ${PMPROC_LIB_PATH})

install(FILES config/config_template.json config/fm_catalog.json DESTINATION ${PMPROC_BIN_PATH}/../config)


