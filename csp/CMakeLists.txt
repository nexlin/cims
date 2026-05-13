cmake_minimum_required(VERSION 3.10)
project(csp)

set(CMAKE_CXX_STANDARD 11)

# Find common system prerequisites
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# MariaDB / MySQL C connector
# 설치: sudo apt-get install -y libmariadb-dev
find_library(MARIADB_LIB NAMES mariadb mysqlclient
    HINTS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
if(NOT MARIADB_LIB)
    message(FATAL_ERROR "MariaDB/MySQL client library not found.\n"
            "  sudo apt-get install -y libmariadb-dev")
endif()
find_path(MARIADB_INCLUDE_DIR NAMES mysql/mysql.h mariadb/mysql.h
    HINTS /usr/include /usr/local/include)
if(NOT MARIADB_INCLUDE_DIR)
    message(FATAL_ERROR "MariaDB/MySQL header not found.\n"
            "  sudo apt-get install -y libmariadb-dev")
endif()
message(STATUS "MariaDB lib: ${MARIADB_LIB}, include: ${MARIADB_INCLUDE_DIR}")

# hiredis (Phase 1.D-2 — HA register state replication, optional).
# 미설치 시 RedisStore 가 cold-mode (Init 가 false 반환) — fail-over 시 단말 재 REGISTER 로 복원.
# 활성: sudo apt-get install -y libhiredis-dev
find_library(HIREDIS_LIB NAMES hiredis
    HINTS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h
    HINTS /usr/include /usr/local/include)
if(HIREDIS_LIB AND HIREDIS_INCLUDE_DIR)
    message(STATUS "hiredis: ${HIREDIS_LIB} (include: ${HIREDIS_INCLUDE_DIR})")
    add_compile_definitions(CIMS_HAS_HIREDIS=1)
else()
    message(STATUS "hiredis: 미설치 — RedisStore cold-mode (apt install libhiredis-dev 시 활성)")
endif()

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
    ${MARIADB_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
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
    ${MARIADB_LIB}
    dl
)

# hiredis link + include (optional — Phase 1.D-2)
if(HIREDIS_LIB AND HIREDIS_INCLUDE_DIR)
    target_include_directories(csp PUBLIC ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(csp PUBLIC ${HIREDIS_LIB})
endif()

# --- Deployment / Install Configuration ---

# Set default install prefix to 'dist' folder in source root if not specified
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "Default install prefix" FORCE)
endif()

# 1. Binaries (bin/)
install(TARGETS csp DESTINATION bin)
install(PROGRAMS csp.sh DESTINATION bin)

# 2. Configuration (config/)
#    A안 통합 — csp.json.template 는 config_template.json 으로 흡수됨.
install(FILES csp.xml config/config_template.json DESTINATION config)
install(DIRECTORY User Group DESTINATION .)
install(DIRECTORY DESTINATION config/cdr)

# 3. Certificates (cert/)
# Install csp.pem from the source directory (as per user request)
install(FILES csp.pem DESTINATION cert)

# 4. Logs (log/)
# Create empty log directory
install(DIRECTORY DESTINATION log)
