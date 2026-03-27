include_directories(
${TCLIB_INC}
${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(${AMTRTP_LIB} STATIC 
	amtrtcp.cpp
	CRtcp.cpp
	amtrtpcomm.cpp
#	amtatmcomm.cpp
	amrtpbuffer.cpp
	amtsock.cpp
	amtrtpdtmf.cpp
)



install(TARGETS ${AMRTP_LIB} DESTINATION ${AMRTP_LIB_PATH})

