# Third-party dependencies: ONNX Runtime (prebuilt CPU package), GoogleTest, Google Benchmark.
include(FetchContent)
find_package(Threads REQUIRED)

# --- ONNX Runtime ------------------------------------------------------------------------------
# Use -DONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-X to skip the download.
set(HOTPATH_ORT_VERSION "1.31.1" CACHE STRING "ONNX Runtime release to download")
if(NOT ONNXRUNTIME_ROOT)
  FetchContent_Declare(onnxruntime_pkg
    URL https://github.com/microsoft/onnxruntime/releases/download/v${HOTPATH_ORT_VERSION}/onnxruntime-linux-x64-${HOTPATH_ORT_VERSION}.tgz
    URL_HASH SHA256=c849e36a1ef3e22d4ff217b9d1ce4a3cf5b9467b975610913ae959ad5b16bdc9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(onnxruntime_pkg)
  set(ONNXRUNTIME_ROOT ${onnxruntime_pkg_SOURCE_DIR})
endif()
get_filename_component(ONNXRUNTIME_ROOT "${ONNXRUNTIME_ROOT}" ABSOLUTE)
set(ONNXRUNTIME_LIB_DIR ${ONNXRUNTIME_ROOT}/lib)
add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime::onnxruntime PROPERTIES
  IMPORTED_LOCATION ${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so
  INTERFACE_INCLUDE_DIRECTORIES ${ONNXRUNTIME_ROOT}/include)
# Find libonnxruntime.so at run time from the build tree and from <prefix>/lib after install.
set(CMAKE_BUILD_RPATH ${ONNXRUNTIME_LIB_DIR})
set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib")

# --- GoogleTest / Google Benchmark ---------------------------------------------------------------
if(HOTPATH_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0 GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
if(HOTPATH_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git GIT_TAG v1.9.5 GIT_SHALLOW TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()
