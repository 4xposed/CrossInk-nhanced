add_executable(OpdsDownloadPathTest
  OpdsDownloadPathTest.cpp
  ${REPO_ROOT}/src/activities/browser/OpdsDownloadPath.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(OpdsDownloadPathTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(OpdsDownloadPathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsDownloadPathTest)
