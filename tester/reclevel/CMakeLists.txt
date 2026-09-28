# cims-rec-level — CMP 녹취 음성 레벨·클리핑 측정기 (ue_audio_level.md §7). 녹취 트랙(.rtp)을 디코드해
#   ITU-T P.56 활성 레벨·포화 지표를 JSON 으로, --dir --summary 면 화자별 요약 표로 낸다.
#   AMR-WB = opencore-amrwb 디코더(외부 프로젝트 정적 라이브러리), G.711 = libcsim 의 G711.cpp.
add_executable(cims-rec-level RecLevel.cpp ${CMAKE_SOURCE_DIR}/cspsim/G711.cpp)
target_compile_features(cims-rec-level PRIVATE cxx_std_17)
target_include_directories(cims-rec-level PRIVATE ${CMAKE_SOURCE_DIR}/cspsim ${CMAKE_SOURCE_DIR}/ext/psip/SipPlatform ${AMR_PKG_DIR}/include/opencore-amrwb)
add_dependencies(cims-rec-level ${AMR_EXT})
target_link_libraries(cims-rec-level PRIVATE ${AMR_LIB_PATH}/libopencore-amrwb.a m)
