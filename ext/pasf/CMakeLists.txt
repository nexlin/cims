include_directories(include)

add_subdirectory(src)
add_subdirectory(sample)
#file(GLOB_RECURSE TASF_SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/src *.cpp)

#add_library(${TASF_LIB} STATIC  ${TASF_SRCS})




#install (TARGETS ${TASF_LIB}
#         RUNTIME DESTINATION ${TASF_BIN_PATH}
#         LIBRARY DESTINATION ${TASF_LIB_PATH}
#         ARCHIVE DESTINATION ${TASF_LIB_PATH})

