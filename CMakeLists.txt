cmake_minimum_required(VERSION 3.24)
project(hotpath VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(HOTPATH_BUILD_TESTS "Build unit tests" ON)
option(HOTPATH_BUILD_BENCH "Build Google Benchmark micro-benchmarks" ON)
option(HOTPATH_SANITIZE "Thread sanitizer build (for the lock-free structures)" OFF)

include(cmake/Dependencies.cmake)

add_library(hotpath_core STATIC
  src/config.cpp
  src/generator.cpp
  src/metrics.cpp
  src/model.cpp
  src/http.cpp
  src/hec_sink.cpp
  src/tcp_source.cpp
  src/pipeline.cpp)
target_include_directories(hotpath_core PUBLIC include)
target_compile_features(hotpath_core PUBLIC cxx_std_20)
target_compile_options(hotpath_core PUBLIC -Wall -Wextra -Wpedantic)
target_link_libraries(hotpath_core PUBLIC onnxruntime::onnxruntime Threads::Threads)
if(HOTPATH_SANITIZE)
  target_compile_options(hotpath_core PUBLIC -fsanitize=thread -g -O1)
  target_link_options(hotpath_core PUBLIC -fsanitize=thread)
endif()

add_executable(hotpath src/main.cpp)
target_link_libraries(hotpath PRIVATE hotpath_core)

install(TARGETS hotpath RUNTIME DESTINATION bin)
install(DIRECTORY ${ONNXRUNTIME_LIB_DIR}/ DESTINATION lib FILES_MATCHING PATTERN "libonnxruntime*.so*")

if(HOTPATH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/cpp)
endif()
if(HOTPATH_BUILD_BENCH)
  add_subdirectory(bench)
endif()
