add_definitions(-DNO_ALSA -DNO_SOUND_CARD)

# ── libcsim — 가상 단말 엔진(SimSession·RtpThread) 정적 라이브러리 (test_instrument.md §1.3).
#    cspsim CLI 와 cims-tester-worker(tester/worker) 가 같은 엔진을 쓴다. 공개 헤더 = 이 디렉터리.
add_library(csim STATIC
    SimSession.cpp
    SipClient.cpp
    SipClientSetup.cpp
    G711.cpp
    RtpThread.cpp
)
target_include_directories(csim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ext/psip/SipUserAgent
    ${CMAKE_SOURCE_DIR}/ext/psip/SipStack
    ${CMAKE_SOURCE_DIR}/ext/psip/SipParser
    ${CMAKE_SOURCE_DIR}/ext/psip/SdpParser
    ${CMAKE_SOURCE_DIR}/ext/psip/XmlParser
    ${CMAKE_SOURCE_DIR}/ext/psip/SipPlatform
    ${CMAKE_SOURCE_DIR}/ext/psip/openssl/include
    ${SRTP_INC}
    ${CMAKE_SOURCE_DIR}/include
)
add_dependencies(csim ${SRTP_EXT})
target_link_directories(csim PUBLIC ${CMAKE_SOURCE_DIR}/build/csp/psip_build)
# libsrtp2 를 psip 보다 앞에 둔다 — psip SipStack 이 끌고 오는 동봉 opensrtp 와 심볼이 겹치므로
#   (multiple definition), 먼저 오는 libsrtp2 가 srtp_* 를 해결하고 opensrtp 의 srtp.o 는 끌리지 않게 한다.
target_link_libraries(csim PUBLIC
    ${SRTP_LIB_PATH}/libsrtp2.a
    SipUserAgent
    SipStack
    SipParser
    SipPlatform
    SdpParser
    XmlParser
    ssl
    crypto
    pthread
)

add_executable(cspsim
    CspsimMain.cpp
)
target_link_libraries(cspsim PRIVATE csim mariadb)

target_include_directories(cspsim PRIVATE
    ${CMAKE_SOURCE_DIR}/ext/psip/SipUserAgent
    ${CMAKE_SOURCE_DIR}/ext/psip/SipStack
    ${CMAKE_SOURCE_DIR}/ext/psip/SipParser
    ${CMAKE_SOURCE_DIR}/ext/psip/SdpParser
    ${CMAKE_SOURCE_DIR}/ext/psip/XmlParser
    ${CMAKE_SOURCE_DIR}/ext/psip/SipPlatform
    ${CMAKE_SOURCE_DIR}/ext/psip/openssl/include
    ${SRTP_INC}
    ${CMAKE_SOURCE_DIR}/include
)

