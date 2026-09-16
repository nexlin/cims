# cims-tester-worker — 계측기 워커 (test_instrument.md §2·§8). libcsim(cspsim/) 위의 C++17 실행 파일.
add_executable(cims-tester-worker
    src/main.cpp
    src/Worker.cpp
    src/Json.cpp
    src/HttpServer.cpp
    src/StreamClient.cpp
    src/Metrics.cpp
)
target_compile_features(cims-tester-worker PRIVATE cxx_std_17)
target_include_directories(cims-tester-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cims-tester-worker PRIVATE csim)
