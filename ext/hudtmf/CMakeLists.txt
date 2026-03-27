#
Project("hudtmf library" C CXX)

set (HUDTMF hudtmf)
#set (HUDTMF_DIR ${CMAKE_CURRENT_SOURCE_DIR})
#set (HUDTMF_INC ${HUDTMF_DIR})
#set (HUDTMF_LIB ${HUDTMF})
set (HUDTMF_TEST ${HUDTMF}_test)

include_directories(
   ${HUDTMF_INC}
)

#add_definitions( -Wcast -Wall -W -Wextra -Wno-long-long -g )
add_definitions( -Wall -W -Wextra -Wno-long-long -g )

add_library(${HUDTMF_LIB} STATIC
   hudtmf_detect.cpp
   hudtmf_gen.cpp
   hug711.cpp
)

LINK_LIBRARIES( ${HUDTMF_LIB} )

add_executable( ${HUDTMF_TEST} main.cpp ) 


install(TARGETS ${HUDTMF_LIB} DESTINATION ${HUDTMF_LIB_PATH})


