add_executable(ReadingLanguageStatsTest
  ${REPO_ROOT}/src/activities/reader/ReadingLanguageStats.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsSave.cpp
  ReadingLanguageStatsTest.cpp stubs/HalStorage.cpp
  ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
)
target_include_directories(ReadingLanguageStatsTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/src/activities/reader)
target_compile_options(ReadingLanguageStatsTest PRIVATE -fstack-usage)
target_link_libraries(ReadingLanguageStatsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReadingLanguageStatsTest)
target_sources(ReadingLanguageStatsTest PRIVATE
  ${REPO_ROOT}/src/activities/network/NearbyStatsProtocol.cpp
  ${REPO_ROOT}/src/activities/reader/StatsBackup.cpp
  NearbyStatsProtocolTest.cpp)
target_include_directories(ReadingLanguageStatsTest PRIVATE ${REPO_ROOT}/src)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(COMPLETION_ACTION ${CMAKE_CURRENT_BINARY_DIR}/CompletionAction.inc)
add_custom_command(OUTPUT ${COMPLETION_ACTION} VERBATIM
  COMMAND ${Python3_EXECUTABLE} -B ${REPO_ROOT}/test/source_contracts/extract_functions.py ${COMPLETION_ACTION}
    ${REPO_ROOT}/src/activities/home/BookActions.cpp "bool toggleBookCompleted("
  DEPENDS ${REPO_ROOT}/test/source_contracts/extract_functions.py ${REPO_ROOT}/src/activities/home/BookActions.cpp)
add_executable(CompletionActionTest CompletionActionTest.cpp ${COMPLETION_ACTION} stubs/HalStorage.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsSave.cpp ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp ${REPO_ROOT}/src/activities/reader/ReadingLanguageStats.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp)
target_include_directories(CompletionActionTest PRIVATE stubs ${CMAKE_CURRENT_BINARY_DIR} ${REPO_ROOT}/src ${REPO_ROOT}/src/activities/reader)
target_link_libraries(CompletionActionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CompletionActionTest)
