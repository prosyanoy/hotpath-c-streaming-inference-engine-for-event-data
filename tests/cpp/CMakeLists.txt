include(GoogleTest)
add_executable(hotpath_tests
  test_queues.cpp
  test_histogram.cpp
  test_config.cpp
  test_generator.cpp
  test_http_hec.cpp
  test_pipeline.cpp)
target_link_libraries(hotpath_tests PRIVATE hotpath_core GTest::gtest_main)
# Integration tests use the trained model if present (make train); they skip otherwise.
target_compile_definitions(hotpath_tests PRIVATE
  HOTPATH_TEST_MODEL="${PROJECT_SOURCE_DIR}/models/anomaly.onnx")
gtest_discover_tests(hotpath_tests DISCOVERY_MODE PRE_TEST)
