add_executable(hotpath_bench micro_bench.cpp)
target_link_libraries(hotpath_bench PRIVATE hotpath_core benchmark::benchmark)
target_compile_definitions(hotpath_bench PRIVATE
  HOTPATH_BENCH_MODEL="${PROJECT_SOURCE_DIR}/models/anomaly.onnx")
