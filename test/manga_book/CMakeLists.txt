add_executable(MangaBookTest MangaBookTest.cpp stubs/HalStorage.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaBook.cpp ${REPO_ROOT}/lib/MangaPanel/MangaFormat.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp)
target_include_directories(MangaBookTest PRIVATE stubs ${REPO_ROOT}/lib/MangaPanel
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/FsHelpers)
target_compile_definitions(MangaBookTest PRIVATE MANGA_FIXTURES="${REPO_ROOT}/test/manga_format/fixtures")
target_link_libraries(MangaBookTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MangaBookTest)
