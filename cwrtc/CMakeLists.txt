cmake_minimum_required(VERSION 3.10)
project(cwrtc)

set(CMAKE_CXX_STANDARD 14)

# Find Dependencies
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Add ext/psip dependencies
# We assume this file is in .../cwrtc/ and psip is in .../ext/psip/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip ${CMAKE_CURRENT_BINARY_DIR}/psip_build)

# Include Directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/openh264
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipPlatform
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SdpParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/StunParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/TcpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/HttpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/HttpParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/XmlParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/ServerPlatform
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipUserAgent
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/opensrtp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/opensrtp/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/opensrtp/lib/linux/64bit/include
)

# Source Files
set(SOURCES
    main.cpp
    H264Decoder.cpp
    RtpThread.cpp
    Server.cpp
    UserMap.cpp
    HttpCallBack.cpp
)

# Define Executable
add_executable(cwrtc ${SOURCES})

# Link Libraries
target_link_libraries(cwrtc PUBLIC
    ServerPlatform
    SipUserAgent
    SipStack
    SipParser
    SdpParser
    StunParser
    XmlParser
    SipPlatform
    TcpStack
    HttpStack
    HttpParser
    opensrtp
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    dl
)
