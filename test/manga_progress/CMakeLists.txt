add_executable(MangaProgressStoreTest
  MangaProgressStoreTest.cpp
  stubs/HalStorage.cpp
  ${REPO_ROOT}/src/activities/reader/MangaProgressStore.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)

target_include_directories(MangaProgressStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/uzlib/src
)

target_link_libraries(MangaProgressStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MangaProgressStoreTest)
