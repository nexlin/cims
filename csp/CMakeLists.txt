cmake_minimum_required(VERSION 3.10)
project(csp)

set(CMAKE_CXX_STANDARD 11)

# Find common system prerequisites
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Add the dependency libraries from ext/psip
# We assume this file is in .../csp/ and psip is in .../ext/psip/
# Adjust path if necessary. Using relative path here.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip ${CMAKE_CURRENT_BINARY_DIR}/psip_build)

# Include current directory source files
file(GLOB SRCS "*.cpp")

# Define the executable
add_executable(csp ${SRCS})

# Include directories (Propagated from targets but adding explicit just in case)
target_include_directories(csp PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipPlatform
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SdpParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/StunParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/XmlParser
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/SipUserAgent
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/ServerPlatform
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/TcpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/HttpStack
    ${CMAKE_CURRENT_SOURCE_DIR}/../ext/psip/HttpParser
)

# Link against the psip libraries (PascalCase matches ext/psip/CMakeLists.txt)
target_link_libraries(csp PUBLIC
    SipUserAgent
    SipStack
    SipParser
    SdpParser
    XmlParser
    ServerPlatform
    SipPlatform
    StunParser
    TcpStack
    HttpStack
    HttpParser
    opensrtp
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    dl
)

# --- Deployment / Install Configuration ---

# Set default install prefix to 'dist' folder in source root if not specified
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "Default install prefix" FORCE)
endif()

# 1. Binaries (bin/)
install(TARGETS csp DESTINATION bin)
install(PROGRAMS csp.sh DESTINATION bin)

# 2. Configuration (config/)
install(FILES csp.xml DESTINATION config)
install(DIRECTORY SipServerXml UserXml cdr DESTINATION config)

# 3. Certificates (cert/)
# Install csp.pem from the source directory (as per user request)
install(FILES csp.pem DESTINATION cert)

# 4. Logs (log/)
# Create empty log directory
install(DIRECTORY DESTINATION log)
