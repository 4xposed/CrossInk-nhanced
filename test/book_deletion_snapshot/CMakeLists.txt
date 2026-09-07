add_executable(BookDeletionSnapshotTest
  BookDeletionSnapshotTest.cpp
  ${REPO_ROOT}/test/manga_book/stubs/HalStorage.cpp
  ${REPO_ROOT}/src/util/BookDeletionSnapshot.cpp
)
target_include_directories(BookDeletionSnapshotTest PRIVATE
  ${REPO_ROOT}/test/manga_book/stubs
  ${REPO_ROOT}/src/util
)
target_link_libraries(BookDeletionSnapshotTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookDeletionSnapshotTest)
