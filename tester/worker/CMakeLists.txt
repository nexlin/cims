# cims-tester-worker — 계측기 워커 (test_instrument.md §2·§8). libcsim(cspsim/) 위의 C++17 실행 파일.
add_executable(cims-tester-worker
    src/main.cpp
    src/Worker.cpp
    src/Json.cpp
    src/HttpServer.cpp
    src/StreamClient.cpp
    src/Metrics.cpp
    src/SipCapture.cpp
)
target_compile_features(cims-tester-worker PRIVATE cxx_std_17)
target_include_directories(cims-tester-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cims-tester-worker PRIVATE csim)

# 워커 SIP 캡처 단위시험 — cims-verify S1-UNIT-TESTER 가 실행한다
add_executable(tester_sip_capture_test src/SipCaptureTest.cpp src/SipCapture.cpp)
target_compile_features(tester_sip_capture_test PRIVATE cxx_std_17)
target_include_directories(tester_sip_capture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tester_sip_capture_test PRIVATE csim)
