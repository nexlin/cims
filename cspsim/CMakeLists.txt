add_definitions(-DNO_ALSA -DNO_SOUND_CARD)

# ── libcsim — 가상 단말 엔진(SimSession·RtpThread) 정적 라이브러리 (test_instrument.md §1.3).
#    cspsim CLI 와 cims-tester-worker(tester/worker) 가 같은 엔진을 쓴다. 공개 헤더 = 이 디렉터리.
add_library(csim STATIC
    SimSession.cpp
    CsimPeer.cpp
    SipClient.cpp
    SipClientSetup.cpp
    G711.cpp
    RtpThread.cpp
    McDataSds.cpp
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

# libcsim RTP 단위시험 (RFC 4733 DTMF 루프백) — cims-verify S1-UNIT-TESTER 가 실행한다
add_executable(csim_rtp_dtmf_test CsimRtpDtmfTest.cpp)
target_link_libraries(csim_rtp_dtmf_test PRIVATE csim)
add_executable(csim_rtp_media_test CsimRtpMediaTest.cpp)
target_link_libraries(csim_rtp_media_test PRIVATE csim)
# 피어 엔진 오류 주입 후속(재전송 유실·THIG)·G.722 협상 — UDP 루프백 피어 둘
add_executable(csim_peer_fault_test CsimPeerFaultTest.cpp)
target_link_libraries(csim_peer_fault_test PRIVATE csim)
# TLS 상호인증(클라이언트 인증서 요구·제시·서버 검증) — openssl CLI 로 임시 인증서
add_executable(csim_tls_mutual_test CsimTlsMutualTest.cpp)
target_link_libraries(csim_tls_mutual_test PRIVATE csim)
# MCData SDS 코덱 왕복(TS 24.282 §15 TLV·conversation ID 호환)
add_executable(csim_sds_test CsimSdsTest.cpp)
target_link_libraries(csim_sds_test PRIVATE csim)
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

