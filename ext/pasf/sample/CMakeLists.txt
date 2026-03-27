
include_directories(include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../src/include)

add_executable(pasf_sample src/test.cpp)
target_link_libraries(pasf_sample pasf pthread rt)
