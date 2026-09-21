# cims-sample-conv — 계측기 미디어 샘플 변환기 (test_instrument.md §4). WAV → 16 kHz PCM 마스터 + pcmu/pcma/g722/amrwb(+DTX) + P.56 측정 JSON.
#   컨트롤러 패키지(oam-cims-tester/native/)에 동봉 — 콘솔 등록 때 컨트롤러가 부른다. 생성기(tester/worker/samples/gen_samples.py)도 같은 변환기.
#   G.711 = libcsim 의 G711.cpp, G.722 = pjproject g722_enc 이식본, AMR-WB = vo-amrwbenc(외부 프로젝트 정적 라이브러리).
add_executable(cims-sample-conv SampleConv.cpp g722_enc.c ${CMAKE_SOURCE_DIR}/cspsim/G711.cpp)
target_compile_features(cims-sample-conv PRIVATE cxx_std_17)
target_include_directories(cims-sample-conv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/cspsim ${CMAKE_SOURCE_DIR}/ext/psip/SipPlatform ${AMRWB_INC})
add_dependencies(cims-sample-conv ${AMRWB_EXT})
target_link_libraries(cims-sample-conv PRIVATE ${AMRWB_LIB_PATH}/libvo-amrwbenc.a m)
