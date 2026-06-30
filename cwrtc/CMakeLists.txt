cmake_minimum_required(VERSION 3.10)
project(cwrtc)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
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

set(SOURCES
    main.cpp
    CwrtcSetup.cpp
    SessionMap.cpp
    SipAgent.cpp
    HttpCallBack.cpp
    RtpThread.cpp
    Server.cpp
    MsgLogger.cpp
)

add_executable(cwrtc ${SOURCES})

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
