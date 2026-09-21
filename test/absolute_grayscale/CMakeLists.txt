add_executable(AbsoluteGrayscaleTest AbsoluteGrayscaleTest.cpp)
target_include_directories(AbsoluteGrayscaleTest PRIVATE
  ${REPO_ROOT}/test/memory_policy/stubs
  ${REPO_ROOT}/lib/Memory)
target_link_libraries(AbsoluteGrayscaleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AbsoluteGrayscaleTest)
