add_executable(MangaPixelCacheTest MangaPixelCacheTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../manga_cover/stubs/HalStorage.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaPixelCache.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(MangaPixelCacheTest PRIVATE
  ${REPO_ROOT}/lib/CooperativeCancellation
  ${CMAKE_CURRENT_SOURCE_DIR}/../manga_cover/stubs
  ${REPO_ROOT}/lib/MangaPanel ${REPO_ROOT}/lib/uzlib/src)
target_link_libraries(MangaPixelCacheTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MangaPixelCacheTest)
