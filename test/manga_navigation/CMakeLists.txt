add_executable(MangaNavigationTest MangaNavigationTest.cpp
  ${REPO_ROOT}/src/activities/reader/MangaNavigation.cpp)
target_include_directories(MangaNavigationTest PRIVATE ${REPO_ROOT}/src/activities/reader)
target_link_libraries(MangaNavigationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MangaNavigationTest)
