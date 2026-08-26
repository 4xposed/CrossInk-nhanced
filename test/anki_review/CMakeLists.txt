add_executable(AnkiReviewScheduleTest
  AnkiReviewScheduleTest.cpp
)

target_include_directories(AnkiReviewScheduleTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/AnkiDeck
)

target_link_libraries(AnkiReviewScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)


add_executable(AnkiCardTextTest
  AnkiCardTextTest.cpp
  ${REPO_ROOT}/src/activities/anki/AnkiCardText.cpp
)

target_include_directories(AnkiCardTextTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/AnkiDeck
)

target_link_libraries(AnkiCardTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_executable(AnkiReviewCandidatesTest
  AnkiReviewCandidatesTest.cpp
  ${REPO_ROOT}/src/activities/anki/AnkiReviewCandidates.cpp
)

target_include_directories(AnkiReviewCandidatesTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/AnkiDeck
)

target_link_libraries(AnkiReviewCandidatesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AnkiReviewCandidatesTest)

gtest_discover_tests(AnkiCardTextTest)

gtest_discover_tests(AnkiReviewScheduleTest)
