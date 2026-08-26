add_executable(AnkiDeckTest
  AnkiDeckTest.cpp
  ${REPO_ROOT}/lib/AnkiDeck/AnkiDeck.cpp
  ${REPO_ROOT}/lib/AnkiDeck/ReviewStateStore.cpp
)

target_include_directories(AnkiDeckTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/AnkiDeck
  ${REPO_ROOT}/lib/Memory
)

target_compile_definitions(AnkiDeckTest PRIVATE
  ANKI_DECK_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(AnkiDeckTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AnkiDeckTest)
