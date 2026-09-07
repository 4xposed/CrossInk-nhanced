find_package(EXPAT REQUIRED)

add_executable(JapaneseDictionaryTest
  JapaneseDictionaryTest.cpp
  ${REPO_ROOT}/src/activities/reader/MangaPageTextSource.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaFormat.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaImageGeometry.cpp
  ${REPO_ROOT}/lib/Dict/Deinflector.cpp
  ${REPO_ROOT}/lib/Dict/DictIndex.cpp
  ${REPO_ROOT}/lib/Dict/WordLookup.cpp
  ${REPO_ROOT}/lib/Epub/Epub/RubyGlossary.cpp
  ${REPO_ROOT}/lib/DictHtmlRenderer/DictHtmlRenderer.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
  ${REPO_ROOT}/src/util/DictionaryRegistry.cpp
  ${REPO_ROOT}/src/util/DictionaryEngine.cpp
  ${REPO_ROOT}/src/util/DictionaryLookupWorker.cpp
  ${REPO_ROOT}/src/util/DictLayout.cpp
  ${REPO_ROOT}/src/util/JapaneseDictionaryBackend.cpp
  ${REPO_ROOT}/src/util/StarDictBackend.cpp
  ${REPO_ROOT}/src/activities/reader/PageTextSource.cpp
  ${REPO_ROOT}/src/activities/reader/PageWordScanCache.cpp
  ${REPO_ROOT}/src/activities/reader/PageWordScanner.cpp
  ${REPO_ROOT}/src/activities/reader/DictionaryDefinitionModel.cpp
  ${REPO_ROOT}/src/activities/reader/DictionaryLookupFlow.cpp
)

target_include_directories(JapaneseDictionaryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Dict
  ${REPO_ROOT}/lib/MangaPanel
  ${REPO_ROOT}/lib/DictHtmlRenderer
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/src/util
)

target_compile_definitions(JapaneseDictionaryTest PRIVATE
  JAPANESE_DICTIONARY_GOLDEN_DIR="${REPO_ROOT}/test/japanese_dict_converter/golden"
  JAPANESE_DICTIONARY_PARITY_CASES="${REPO_ROOT}/test/japanese_dictionary/fixtures/parity_cases.json"
  CROSSINK_DICT_TESTING=1
)

target_link_libraries(JapaneseDictionaryTest PRIVATE
  crosspoint_test_common
  EXPAT::EXPAT
  GTest::gtest_main
)

gtest_discover_tests(JapaneseDictionaryTest TEST_PREFIX "JapaneseDictionary.")
