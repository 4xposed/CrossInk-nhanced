add_executable(OpdsDownloadedBookTest
  OpdsDownloadedBookTest.cpp
)

target_include_directories(OpdsDownloadedBookTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OpdsDownloadedBookTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsDownloadedBookTest)
