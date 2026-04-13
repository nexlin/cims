add_definitions(-DNO_ALSA -DNO_SOUND_CARD)
add_executable(cspsim
    CspsimMain.cpp
    SimSession.cpp
    SipClient.cpp
    SipClientSetup.cpp
    G711.cpp
    RtpThread.cpp
)

target_include_directories(cspsim PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/psip/SipUserAgent
    ${CMAKE_SOURCE_DIR}/ext/psip/SipStack
    ${CMAKE_SOURCE_DIR}/ext/psip/SipParser
    ${CMAKE_SOURCE_DIR}/ext/psip/SdpParser
    ${CMAKE_SOURCE_DIR}/ext/psip/XmlParser
    ${CMAKE_SOURCE_DIR}/ext/psip/SipPlatform
    ${CMAKE_SOURCE_DIR}/ext/psip/openssl/include
    ${CMAKE_SOURCE_DIR}/include
)

# Assuming libraries are built and available in build/csp/psip_build
# We might need to adjust link directories or target names depending on how the root CMake handles psip.
# If psip is an external project not added via add_subdirectory, we rely on find_library or link flags.

target_link_directories(cspsim PRIVATE
    ${CMAKE_SOURCE_DIR}/build/csp/psip_build
)

target_link_libraries(cspsim PRIVATE
    SipUserAgent
    SipStack
    SipParser
    SipPlatform
    SdpParser
    XmlParser
    ssl
    crypto
    pthread
    mariadb
)
