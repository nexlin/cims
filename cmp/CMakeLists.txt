#
#Project("pmproc library" C CXX)

include_directories(
   ./
   ${PMP_INC}
   ${PMPROC_INC}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

#   ${FFM_LIB_PATH}
message(" 111 : ${FFM_LIB_PATH}")
message(" ###> ${PMPROC_BIN_PATH}")
message(" ***> ${PMPROC_LIB_PATH}")
#message("${FFM_LIB}")


#add_executable(pmpagent pmpagent.cpp)
#add_executable(prtpsim prtpsim.cpp)
add_executable(cmp PMain.cpp PCmpServer.cpp PMcpttGroup.cpp PRtpRelay.cpp PRtpMulticast.cpp PSyncRtpRecorder.cpp)

set(TEST_SRCS
   #pmpagent 
   #prtpsim 
   cmp
)


install(TARGETS ${TEST_SRCS}  
	    RUNTIME DESTINATION ${PMPROC_BIN_PATH}
		LIBRARY DESTINATION ${PMPROC_LIB_PATH}
		ARCHIVE DESTINATION ${PMPROC_LIB_PATH})

install(FILES config/config_template.json DESTINATION ${PMPROC_BIN_PATH}/../config)


