add_executable(BookMutationJournalTest JournalTest.cpp ${REPO_ROOT}/src/util/BookMutationJournal.cpp)
target_include_directories(BookMutationJournalTest PRIVATE ${REPO_ROOT}/src/util)
target_link_libraries(BookMutationJournalTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_options(BookMutationJournalTest PRIVATE -fstack-usage)
gtest_discover_tests(BookMutationJournalTest)
add_executable(BookMutationJsonTest JsonTest.cpp stubs/HalStorage.cpp ${REPO_ROOT}/src/util/BookMutationJson.cpp)
target_include_directories(BookMutationJsonTest PRIVATE stubs ${REPO_ROOT}/src/util)
target_link_libraries(BookMutationJsonTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_options(BookMutationJsonTest PRIVATE -fstack-usage)
gtest_discover_tests(BookMutationJsonTest)
add_executable(BookFolderMutationTest MutationTest.cpp OwnerStubs.cpp stubs/HalStorage.cpp
  ${REPO_ROOT}/src/util/BookFolderMutation.cpp ${REPO_ROOT}/src/util/BookMutationStorage.cpp
  ${REPO_ROOT}/src/util/BookMutationJournal.cpp ${REPO_ROOT}/src/util/BookMutationJson.cpp
  ${REPO_ROOT}/src/util/BookDeletionSnapshot.cpp ${REPO_ROOT}/src/BookmarkMutation.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(BookFolderMutationTest PRIVATE stubs ${REPO_ROOT}/src ${REPO_ROOT}/src/util ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/uzlib/src)
target_link_libraries(BookFolderMutationTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_options(BookFolderMutationTest PRIVATE -fstack-usage)
gtest_discover_tests(BookFolderMutationTest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RECENT_ACTIONS ${CMAKE_CURRENT_BINARY_DIR}/RecentDeleteActions.inc)
add_custom_command(OUTPUT ${RECENT_ACTIONS}
  VERBATIM
  COMMAND ${Python3_EXECUTABLE} -B ${REPO_ROOT}/test/source_contracts/extract_functions.py ${RECENT_ACTIONS}
    ${REPO_ROOT}/src/activities/home/RecentBooksActivity.cpp "void RecentBooksActivity::promptDeleteBook("
    ${REPO_ROOT}/src/activities/home/RecentBooksGridActivity.cpp "void RecentBooksGridActivity::promptDeleteBook("
  DEPENDS ${REPO_ROOT}/test/source_contracts/extract_functions.py
    ${REPO_ROOT}/src/activities/home/RecentBooksActivity.cpp ${REPO_ROOT}/src/activities/home/RecentBooksGridActivity.cpp)
add_executable(RecentDeleteTest RecentDeleteTest.cpp ${RECENT_ACTIONS} OwnerStubs.cpp stubs/HalStorage.cpp
  ${REPO_ROOT}/src/util/BookFolderMutation.cpp ${REPO_ROOT}/src/util/BookMutationStorage.cpp
  ${REPO_ROOT}/src/util/BookMutationJournal.cpp ${REPO_ROOT}/src/util/BookMutationJson.cpp
  ${REPO_ROOT}/src/util/BookDeletionSnapshot.cpp ${REPO_ROOT}/src/BookmarkMutation.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(RecentDeleteTest PRIVATE stubs ${CMAKE_CURRENT_BINARY_DIR} ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/uzlib/src)
target_link_libraries(RecentDeleteTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(RecentDeleteTest)

add_executable(CheckedDirectoryTest CheckedDirectoryTest.cpp stubs/HalStorage.cpp)
target_include_directories(CheckedDirectoryTest PRIVATE stubs ${REPO_ROOT}/lib/hal)
target_link_libraries(CheckedDirectoryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CheckedDirectoryTest)
