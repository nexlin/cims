# cims-tester-worker — 계측기 워커 (test_instrument.md §2·§8). libcsim(cspsim/) 위의 C++17 실행 파일.
add_executable(cims-tester-worker
    src/main.cpp
    src/Worker.cpp
    src/Json.cpp
    src/HttpServer.cpp
    src/StreamClient.cpp
    src/Metrics.cpp
    src/SipCapture.cpp
    src/RealUe.cpp
)
target_compile_features(cims-tester-worker PRIVATE cxx_std_17)
target_include_directories(cims-tester-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cims-tester-worker PRIVATE csim)

# E-model(MOS) 단위시험 — cims-verify S1-UNIT-TESTER 가 실행한다
add_executable(tester_emodel_test src/EModelTest.cpp)
target_compile_features(tester_emodel_test PRIVATE cxx_std_17)
target_include_directories(tester_emodel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 워커 SIP 캡처 단위시험 — cims-verify S1-UNIT-TESTER 가 실행한다
add_executable(tester_sip_capture_test src/SipCaptureTest.cpp src/SipCapture.cpp)
target_compile_features(tester_sip_capture_test PRIVATE cxx_std_17)
target_include_directories(tester_sip_capture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tester_sip_capture_test PRIVATE csim)

# real-ue 프로세스 관리 단위시험 — 파이썬 스텁(src/real_ue_stub.py)이 cimsue-cli drive 프로토콜을 흉내 낸다. cims-verify S1-UNIT-TESTER 가 실행한다
add_executable(tester_real_ue_test src/RealUeTest.cpp src/RealUe.cpp src/Json.cpp)
target_compile_features(tester_real_ue_test PRIVATE cxx_std_17)
target_include_directories(tester_real_ue_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(tester_real_ue_test PRIVATE REAL_UE_STUB="${CMAKE_CURRENT_SOURCE_DIR}/src/real_ue_stub.py")
target_link_libraries(tester_real_ue_test PRIVATE pthread)
