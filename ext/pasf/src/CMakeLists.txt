include_directories(include)

file(GLOB_RECURSE PASF_SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c *.cpp *.h)

add_library(${PASF_LIB} STATIC  ${PASF_SRCS})

install (TARGETS ${PASF_LIB}
         RUNTIME DESTINATION ${PASF_BIN_PATH}
         LIBRARY DESTINATION ${PASF_LIB_PATH}
         ARCHIVE DESTINATION ${PASF_LIB_PATH})

#add_library(${TASF_TARGET} STATIC 
#        pevent.cpp
#        phandler.cpp
#        pipcaio.cpp
#        pmodule.cpp
#        prunnable.cpp
#        prunnable.h
#        psocket.cpp
#        ptcpcliaio.cpp
#        ptcpclihandler.cpp
#        ptcpsrvaio.cpp
#        ptcpsrvhandler.cpp
#        ptcpsrvsockhandler.cpp
#        pudpaio.cpp
#        pudphandler.cpp
#        pworker.cpp
#)
