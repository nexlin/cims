cmake_minimum_required(VERSION 3.10)
project(nrtp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(SOURCE_FILES
    nrtp_packet.cpp
    nrtp_session.cpp
    nrtp_socket.cpp
    nrtp_rtcp.cpp
    mc_video_session.cpp
)

set(HEADER_FILES
    nrtp_def.h
    nrtp_packet.h
    nrtp_session.h
    nrtp_socket.h
    nrtp_rtcp.h
    nrtp_tbcp.h
    mc_video_session.h
)

add_library(nrtp STATIC ${SOURCE_FILES} ${HEADER_FILES})

if(WIN32)
    target_link_libraries(nrtp ws2_32)
endif()

# Test Executable
add_executable(nrtp_test test_main.cpp)
target_link_libraries(nrtp_test nrtp)
if(WIN32)
    target_link_libraries(nrtp_test ws2_32)
endif()

