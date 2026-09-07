# These contracts run against real pure logic and, for persistence, real files.
add_executable(MangaMenuTest MangaMenuTest.cpp ${REPO_ROOT}/src/activities/reader/MangaNavigation.cpp)
target_include_directories(MangaMenuTest PRIVATE ${REPO_ROOT}/src/activities/reader)
add_test(NAME MangaMenuTest COMMAND MangaMenuTest)
add_executable(MangaStatusTest MangaStatusTest.cpp)
target_include_directories(MangaStatusTest PRIVATE ${REPO_ROOT}/src/activities/reader)
add_test(NAME MangaStatusTest COMMAND MangaStatusTest)
add_executable(MangaStatusPixelsTest MangaStatusPixelsTest.cpp ${REPO_ROOT}/src/activities/reader/MangaStatus.cpp)
target_include_directories(MangaStatusPixelsTest PRIVATE stubs ${REPO_ROOT}/src/activities/reader)
add_test(NAME MangaStatusPixelsTest COMMAND MangaStatusPixelsTest)
add_executable(MangaQrTest MangaQrTest.cpp ${REPO_ROOT}/src/activities/reader/MangaQrPayload.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaFormat.cpp)
target_include_directories(MangaQrTest PRIVATE ${REPO_ROOT}/test/japanese_dictionary/stubs
  ${REPO_ROOT}/src ${REPO_ROOT}/src/activities/reader ${REPO_ROOT}/lib/MangaPanel)
add_test(NAME MangaQrTest COMMAND MangaQrTest)
add_executable(MangaStatsCommitTest MangaStatsCommitTest.cpp ${REPO_ROOT}/src/activities/reader/MangaStatsCommit.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsSave.cpp ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingLanguageStats.cpp ${REPO_ROOT}/test/reading_language_stats/stubs/HalStorage.cpp)
target_include_directories(MangaStatsCommitTest PRIVATE ${REPO_ROOT}/test/reading_language_stats/stubs
  ${REPO_ROOT}/src/activities/reader)
add_test(NAME MangaStatsCommitTest COMMAND MangaStatsCommitTest)
foreach(target MangaMenuTest MangaStatusTest MangaStatusPixelsTest MangaQrTest MangaStatsCommitTest)
  target_compile_options(${target} PRIVATE -UNDEBUG)
endforeach()
# Use the actual pinned PlatformIO dependency; never replace capacity validation with a stub codec.
set(MANGA_QR_CODEC ${REPO_ROOT}/.pio/libdeps/default/QRCode/src)
if(EXISTS ${MANGA_QR_CODEC}/qrcode.c)
  add_executable(QrPolicyTest QrPolicyTest.cpp ${MANGA_QR_CODEC}/qrcode.c)
  target_include_directories(QrPolicyTest PRIVATE ${REPO_ROOT}/src/util ${MANGA_QR_CODEC})
  target_compile_options(QrPolicyTest PRIVATE -UNDEBUG)
  add_test(NAME QrPolicyTest COMMAND QrPolicyTest)
  add_executable(QrDrawTest QrDrawTest.cpp ${REPO_ROOT}/src/util/QrUtils.cpp
    ${REPO_ROOT}/lib/Utf8/Utf8.cpp ${MANGA_QR_CODEC}/qrcode.c)
  target_include_directories(QrDrawTest PRIVATE stubs ${REPO_ROOT}/test/japanese_dictionary/stubs
    ${REPO_ROOT}/src/util ${REPO_ROOT}/lib/Utf8 ${MANGA_QR_CODEC})
  target_compile_options(QrDrawTest PRIVATE -UNDEBUG)
  add_test(NAME QrDrawTest COMMAND QrDrawTest)
else()
  message(STATUS "Manga real-codec QR tests require the default PlatformIO QRCode dependency")
endif()

add_executable(MangaBookmarkSaveTest MangaBookmarkSaveTest.cpp ${REPO_ROOT}/src/BookmarkStore.cpp
  ${REPO_ROOT}/test/reading_language_stats/stubs/HalStorage.cpp ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(MangaBookmarkSaveTest PRIVATE ${REPO_ROOT}/test/reading_language_stats/stubs
  ${REPO_ROOT}/src ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/uzlib/src)
target_compile_options(MangaBookmarkSaveTest PRIVATE -UNDEBUG)
foreach(mode write sync close remove)
  add_test(NAME MangaBookmarkSave_${mode} COMMAND MangaBookmarkSaveTest ${mode})
endforeach()

add_executable(MangaScreenshotWriteTest MangaScreenshotWriteTest.cpp ${REPO_ROOT}/src/util/ScreenshotUtil.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp ${REPO_ROOT}/test/reading_language_stats/stubs/HalStorage.cpp)
target_include_directories(MangaScreenshotWriteTest PRIVATE screenshot_stubs ${REPO_ROOT}/test/reading_language_stats/stubs
  ${REPO_ROOT}/src/util ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Memory)
target_compile_options(MangaScreenshotWriteTest PRIVATE -UNDEBUG)
add_test(NAME MangaScreenshotWriteTest COMMAND MangaScreenshotWriteTest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME MangaSmokeValidationTest
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/MangaSmokeValidationTest.py)
