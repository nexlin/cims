#
# cmdp — CIMS MCData Media Plane (MSRP 종단, TS 24.282 SDS over media plane)
#

include_directories(
   ./
   ${PMP_INC}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
   ${CMAKE_CURRENT_SOURCE_DIR}/../csp
   ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipPlatform
)

# McDataCodec(TS 24.282 TLV)·Base64 는 csp/psip 소스를 직접 공유 (의존성: Base64 뿐)
add_executable(cmdp
   PMain.cpp
   PCmdpServer.cpp
   PMsrpParser.cpp
   PMsrpSession.cpp
   PMsrpConnection.cpp
   PFdStore.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/../csp/McDataCodec.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipPlatform/Base64.cpp
)

set(TEST_SRCS
   cmdp
)

install(TARGETS ${TEST_SRCS}
	    RUNTIME DESTINATION ${PMPROC_BIN_PATH}
		LIBRARY DESTINATION ${PMPROC_LIB_PATH}
		ARCHIVE DESTINATION ${PMPROC_LIB_PATH})

install(FILES config/config_template.json DESTINATION ${PMPROC_BIN_PATH}/../config)
