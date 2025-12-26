cmake_minimum_required(VERSION 3.10)
project(TestWebRtcVideo)

set(CMAKE_CXX_STANDARD 14)

# Determine platform
if(WIN32)
    # Enable MFC
    set(CMAKE_MFC_FLAG 1)
    add_definitions(-D_WIN32)
else()
    add_definitions(-D__linux__)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/openh264
    ${CMAKE_CURRENT_SOURCE_DIR}/../SipStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../SipPlatform
    ${CMAKE_CURRENT_SOURCE_DIR}/../SipParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../StunParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../SdpParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../HttpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../HttpParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../XmlParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../TcpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/lib/linux/64bit/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../signal/SipPlatform 
)
# Note: Added ../../../../signal/SipPlatform as fallback based on find_by_name

# Source files
if(WIN32)
    file(GLOB SOURCES 
        "*.cpp" 
        "*.h"
        "*.rc"
    )
    # Exclude Linux main
    list(REMOVE_ITEM SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main_linux.cpp)
    
    add_executable(TestWebRtcVideo WIN32 ${SOURCES})
    target_link_libraries(TestWebRtcVideo PRIVATE ws2_32)
else()
    file(GLOB SIP_PLATFORM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../SipPlatform/*.cpp")
    file(GLOB SIP_STACK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../SipStack/*.cpp")
    file(GLOB SIP_PARSER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../SipParser/*.cpp")
    file(GLOB SDP_PARSER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../SdpParser/*.cpp")
    file(GLOB STUN_PARSER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../StunParser/*.cpp")
    file(GLOB TCP_STACK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../TcpStack/*.cpp")
    file(GLOB HTTP_STACK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../HttpStack/*.cpp")
    file(GLOB HTTP_PARSER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../HttpParser/*.cpp")
    file(GLOB XML_PARSER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../XmlParser/*.cpp")
    
    # OpenSRTP sources (excluding tests and kernel files)
    file(GLOB OPENSRTP_SRTP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/srtp/*.c")
    file(GLOB OPENSRTP_CRYPTO_KERNEL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/crypto/kernel/*.c")
    file(GLOB OPENSRTP_CRYPTO_CIPHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/crypto/cipher/*.c")
    file(GLOB OPENSRTP_CRYPTO_HASH_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/crypto/hash/*.c")
    file(GLOB OPENSRTP_CRYPTO_MATH_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/crypto/math/*.c")
    # Exclude datatypes.c from math sources if it conflicts (or math.c)
    # Based on error, datatypes.c and math.c conflict. Try excluding datatypes.c first?
    # Or exclude math.c? datatypes.c seems to have definitions.
    # Let's filter out datatypes.c from MATH_SOURCES if it is there?
    # Actually, GLOB picks them up.
    # I will rely on list filtering.
    
    file(GLOB OPENSRTP_CRYPTO_REPLAY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/crypto/replay/*.c")
    file(GLOB OPENSRTP_CRYPTO_RNG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/crypto/rng/*.c")
    file(GLOB OPENSRTP_TABLES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../opensrtp/tables/*.c")

    # Filter out rand_linux_kernel.c (kernel space)
    list(FILTER OPENSRTP_CRYPTO_RNG_SOURCES EXCLUDE REGEX "rand_linux_kernel.c$")
    
    # Filter out datatypes.c (conflicts with math.c?)
    # math.c seems to implement hex_char_to_nibble which datatypes.c needs?
    # But they both define octet_weight.
    # Let's try excluding datatypes.c and keeping math.c (hoping math.c covers everything or datatypes.c is redundant)
    list(FILTER OPENSRTP_CRYPTO_MATH_SOURCES EXCLUDE REGEX "datatypes.c$")
    
    # Exclude aes_tables.c (contains main)
    list(FILTER OPENSRTP_TABLES_SOURCES EXCLUDE REGEX "aes_tables.c$")

    set(SOURCES
        main_linux.cpp
        H264Decoder.cpp
        RtpThread.cpp
        Server.cpp
        UserMap.cpp
        HttpCallBack.cpp
        ${SIP_PLATFORM_SOURCES}
        ${SIP_STACK_SOURCES}
        ${SIP_PARSER_SOURCES}
        ${SDP_PARSER_SOURCES}
        ${STUN_PARSER_SOURCES}
        ${TCP_STACK_SOURCES}
        ${HTTP_STACK_SOURCES}
        ${HTTP_PARSER_SOURCES}
        ${XML_PARSER_SOURCES}
        ${OPENSRTP_SRTP_SOURCES}
        ${OPENSRTP_CRYPTO_KERNEL_SOURCES}
        ${OPENSRTP_CRYPTO_CIPHER_SOURCES}
        ${OPENSRTP_CRYPTO_HASH_SOURCES}
        ${OPENSRTP_CRYPTO_MATH_SOURCES}
        ${OPENSRTP_CRYPTO_REPLAY_SOURCES}
        ${OPENSRTP_CRYPTO_RNG_SOURCES}
        ${OPENSRTP_TABLES_SOURCES}
    )
    
    add_executable(TestWebRtcVideo ${SOURCES})

    # Link libraries
    target_link_libraries(TestWebRtcVideo PRIVATE
        dl
        pthread
        ssl
        crypto
    )
    # Check if we need to compile SipStack/SipPlatform sources or if they are libs
    # For now, let's assume they might be needed. 
    # But usually they are libraries. The user didn't specify.
    # Looking at directory structure, 'ext/psip/TestWebRtcVideo' is deep.
    # There might be a 'sipstack' library target in the parent CMake?
    
    target_link_libraries(TestWebRtcVideo PRIVATE dl pthread ssl crypto)
endif()
