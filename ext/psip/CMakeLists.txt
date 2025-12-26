cmake_minimum_required(VERSION 3.10)
project(psip)

# Default to C++14 or higher for modern support, but keeping 11 as per previous codebase state if compatible
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Linux Specific Definitions
add_definitions(-D__LINUX__)
add_definitions(-D_REENTRANT)

# Suppress warnings that might clutter output given legacy code
add_compile_options(-w)

# Find system dependencies
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Include Directories (Global)
# Many components rely on sibling directories being in the include path
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/SipPlatform
    ${CMAKE_CURRENT_SOURCE_DIR}/SipStack
    ${CMAKE_CURRENT_SOURCE_DIR}/SipParser
    ${CMAKE_CURRENT_SOURCE_DIR}/SdpParser
    ${CMAKE_CURRENT_SOURCE_DIR}/StunParser
    ${CMAKE_CURRENT_SOURCE_DIR}/TcpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/HttpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/HttpParser
    ${CMAKE_CURRENT_SOURCE_DIR}/XmlParser
    ${CMAKE_CURRENT_SOURCE_DIR}/ServerPlatform
    ${CMAKE_CURRENT_SOURCE_DIR}/SipUserAgent
    ${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/lib/linux/64bit/include
    # OpenH264 includes (Local in TestWebRtcVideo - Flat structure found)
    ${CMAKE_CURRENT_SOURCE_DIR}/TestWebRtcVideo/openh264
)

# --- 1. CONFIGURING OPENSRTP (Special Case due to conflicts) ---
# We build this as a static library to be linked by others
file(GLOB OPENSRTP_SRTP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/srtp/*.c")
file(GLOB OPENSRTP_CRYPTO_KERNEL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/crypto/kernel/*.c")
file(GLOB OPENSRTP_CRYPTO_CIPHER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/crypto/cipher/*.c")
file(GLOB OPENSRTP_CRYPTO_HASH_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/crypto/hash/*.c")
file(GLOB OPENSRTP_CRYPTO_MATH_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/crypto/math/*.c")
file(GLOB OPENSRTP_CRYPTO_REPLAY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/crypto/replay/*.c")
file(GLOB OPENSRTP_CRYPTO_RNG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/crypto/rng/*.c")
file(GLOB OPENSRTP_TABLES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/opensrtp/tables/*.c")

# Exclude kernel specific and conflicting files (Learned from previous debugging)
list(FILTER OPENSRTP_CRYPTO_RNG_SOURCES EXCLUDE REGEX "rand_linux_kernel.c$")
list(FILTER OPENSRTP_CRYPTO_MATH_SOURCES EXCLUDE REGEX "datatypes.c$") # Conflicts with math.c
list(FILTER OPENSRTP_TABLES_SOURCES EXCLUDE REGEX "aes_tables.c$")   # Contains 'main'

add_library(opensrtp STATIC
    ${OPENSRTP_SRTP_SOURCES}
    ${OPENSRTP_CRYPTO_KERNEL_SOURCES}
    ${OPENSRTP_CRYPTO_CIPHER_SOURCES}
    ${OPENSRTP_CRYPTO_HASH_SOURCES}
    ${OPENSRTP_CRYPTO_MATH_SOURCES}
    ${OPENSRTP_CRYPTO_REPLAY_SOURCES}
    ${OPENSRTP_CRYPTO_RNG_SOURCES}
    ${OPENSRTP_TABLES_SOURCES}
)

# --- 2. GENERIC LIBRARY BUILDER ---
macro(create_psip_lib lib_name dir_name)
    file(GLOB ${lib_name}_SOURCES "${dir_name}/*.cpp")
    add_library(${lib_name} STATIC ${${lib_name}_SOURCES})
    target_link_libraries(${lib_name} PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto dl)
endmacro()

# --- 3. BUILD LIBRARIES ---
create_psip_lib(SipPlatform    SipPlatform)
create_psip_lib(SipParser      SipParser)
create_psip_lib(SdpParser      SdpParser)
create_psip_lib(StunParser     StunParser)
create_psip_lib(XmlParser      XmlParser)
create_psip_lib(HttpParser     HttpParser)
create_psip_lib(TcpStack       TcpStack)
create_psip_lib(HttpStack      HttpStack)
create_psip_lib(SipStack       SipStack)
create_psip_lib(ServerPlatform ServerPlatform)
create_psip_lib(SipUserAgent   SipUserAgent) # Restored missing library

# Link dependencies (ensures correct order of linking)
target_link_libraries(SipParser PUBLIC SipPlatform)
target_link_libraries(SdpParser PUBLIC SipPlatform)
target_link_libraries(StunParser PUBLIC SipPlatform)
target_link_libraries(XmlParser PUBLIC SipPlatform)
target_link_libraries(HttpParser PUBLIC SipPlatform)

target_link_libraries(TcpStack PUBLIC SipPlatform)
target_link_libraries(HttpStack PUBLIC SipPlatform TcpStack HttpParser)

target_link_libraries(SipStack PUBLIC 
    SipPlatform 
    SipParser 
    SdpParser 
    StunParser 
    XmlParser 
    TcpStack
    opensrtp
)

target_link_libraries(ServerPlatform PUBLIC SipStack)
target_link_libraries(SipUserAgent   PUBLIC SipStack ServerPlatform) # Dependencies for SipUserAgent

# --- 4. BUILD EXECUTABLES ---
macro(create_psip_app app_name dir_name)
    file(GLOB ${app_name}_SOURCES "${dir_name}/*.cpp")
    # Filter out MFC/Windows specific files if they exist in the source list (safeguard)
    list(FILTER ${app_name}_SOURCES EXCLUDE REGEX ".*Dlg.cpp$")
    list(FILTER ${app_name}_SOURCES EXCLUDE REGEX "stdafx.cpp$")
    
    add_executable(${app_name} ${${app_name}_SOURCES})
    target_link_libraries(${app_name} PUBLIC 
        ServerPlatform 
        SipUserAgent # Updated to PascalCase target
        SipStack 
        HttpStack
        TcpStack
        SipParser 
        SdpParser 
        StunParser 
        XmlParser 
        SipPlatform
        opensrtp
        Threads::Threads 
        OpenSSL::SSL 
        OpenSSL::Crypto
        dl
    )
endmacro()

# Applications
create_psip_app(EchoSipServer     EchoSipServer)
create_psip_app(SimpleSipServer   SimpleSipServer)
# create_psip_app(SipCallDump       SipCallDump) # Requires libpcap
# Link against local ALSA library built in ext/alsa-lib-build
set(ALSA_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../alsa-lib-build/include)
set(ALSA_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/../alsa-lib-build/lib/libasound.a)

create_psip_app(SipClient         SipClient)
target_include_directories(SipClient PRIVATE ${ALSA_INCLUDE_DIR})
target_link_libraries(SipClient PRIVATE ${ALSA_LIBRARY})
# target_compile_definitions(SipClient PRIVATE NO_ALSA) # ALSA enabled now
create_psip_app(SipLoadBalancer   SipLoadBalancer)
# create_psip_app(SipSend           SipSend/SipSend)
# create_psip_app(SipSend           SipSend)     # MFC Application - Excluded
# create_psip_app(SipUserAgent      SipUserAgent) # SipUserAgent is a library, not an app (verified via Makefile)
# create_psip_app(ServerMonitor     ServerMonitor/ServerMonitor) # MFC Application - Excluded

# SipSpeedLinux is a special case (Linux specific folder)
create_psip_app(SipSpeedLinux     SipSpeedLinux)

# TestWebRtcVideo (Ported previously, assume we can build it here too using the main libraries)
# We need to reuse the main_linux.cpp we created. It's in ext/psip/TestWebRtcVideo/main_linux.cpp
add_executable(TestWebRtcVideo 
    TestWebRtcVideo/main_linux.cpp
    TestWebRtcVideo/H264Decoder.cpp
    TestWebRtcVideo/RtpThread.cpp
    TestWebRtcVideo/Server.cpp
    TestWebRtcVideo/UserMap.cpp
    TestWebRtcVideo/HttpCallBack.cpp
)
target_link_libraries(TestWebRtcVideo PUBLIC 
    SipStack 
    SipPlatform 
    SdpParser 
    StunParser 
    TcpStack 
    HttpStack 
    HttpParser 
    XmlParser 
    opensrtp
    Threads::Threads 
    OpenSSL::SSL 
    OpenSSL::Crypto
    dl
)

message(STATUS "Configured ext/psip Linux Build")
