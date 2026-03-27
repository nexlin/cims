#
Project("jitter library" C CXX)

include_directories(
#   ${TCLIB_INC}
#   ${GTEST_INC}
#   ${FFM_INC}
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${AMR_DIR}/include #add jjpark
)

link_directories(
#   ${GTEST_LIB_PATH}
	${COM_LIB_PATH}
	${AMR_LIB_PATH}
	${FFM_LIB_PATH}
   ${CMAKE_CURRENT_SOURCE_DIR}
)

#add_definitions( -Wno-deprecated -DUNIX -D_HPUX_SOURCE   -D_REOPEN_ -D_REENTRANT )
#add_definitions( -D__SYSTEM_USAGE__ -DNEW_VERSION -D_INCLUDE_LONGLONG  -DUSING_IPS )
#add_definitions( -D_PT_LGT_ -DUSING_POLL )
#add_definitions( -D_SIMPLE_TC_ -DSLEEP_5MS -D_VIDEO_SID_ -D_DELETE_ALLSID_ )
#add_definitions( -D_DEBUG_TC_ )
#add_definitions( -D_DEBUG_TC_FRAME_ )

file(GLOB_RECURSE JT_SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c *.cpp *.h)
#EXCLUDE_FILES_FROM_DIR_IN_LIST("${JT_SRCS}" "remove_bak/" 0)

add_library(${JT_LIB}
	${JT_SRCS}
)

set_target_properties(${JT_LIB} PROPERTIES LINKER_LANGUAGE CXX)
install(TARGETS ${JT_LIB} DESTINATION ${JT_LIB_PATH})

set (CMAKE_CXX_STANDARD 11)
#########################
##test binary
#########################
set(TEST_SRCS
	jb_test
)

foreach(testsourcefile ${TEST_SRCS})
	add_executable(${testsourcefile} ${testsourcefile}.cpp)
   set_target_properties(${testsourcefile} PROPERTIES LINK_SEARCH_START_STATIC 1)
	set_target_properties(${testsourcefile} PROPERTIES LINK_SEARCH_END_STATIC 1)
	#target_link_libraries(${testsourcefile} ${TCLIB_LIB} ${FFM_LIB} ${COM_LIB} )
	#target_link_libraries(${testsourcefile} ${TCLIB_LIB} ${FFM_LIB} ${COM_LIB} ${JT_LIB} opencore-amrnb opencore-amrwb vo-amrwbenc) #add jjpark
	target_link_libraries(${testsourcefile} ${COM_LIB} ${JT_LIB} ) #add jjpark
endforeach(testsourcefile ${TEST_SRCS})
