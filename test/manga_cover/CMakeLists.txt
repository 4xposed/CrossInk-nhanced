add_executable(MangaCoverTest MangaCoverTest.cpp stubs/HalStorage.cpp stubs/Converters.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaCover.cpp ${REPO_ROOT}/lib/MangaPanel/MangaBook.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaFormat.cpp ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(MangaCoverTest PRIVATE stubs ${REPO_ROOT}/lib/CooperativeCancellation ${REPO_ROOT}/lib/MangaPanel
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/uzlib/src)
target_link_libraries(MangaCoverTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(MangaCoverTest PRIVATE MANGA_FIXTURE_DIR="${REPO_ROOT}/test/manga_format/fixtures")
gtest_discover_tests(MangaCoverTest)

set(COVER_CODEC_DEPS "${REPO_ROOT}/.pio/libdeps/default")
if(EXISTS "${COVER_CODEC_DEPS}/JPEGDEC/src/JPEGDEC.cpp")
 add_executable(RealCoverTest RealCoverTest.cpp real_stubs/HalStorage.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaCover.cpp ${REPO_ROOT}/lib/MangaPanel/MangaBook.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaFormat.cpp ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/lib/JpegToBmpConverter/JpegToBmpConverter.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp ${COVER_CODEC_DEPS}/JPEGDEC/src/JPEGDEC.cpp)
 target_include_directories(RealCoverTest PRIVATE real_stubs
  ${REPO_ROOT}/lib/MangaPanel ${REPO_ROOT}/lib/FsHelpers ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/JpegToBmpConverter ${REPO_ROOT}/lib/PngToBmpConverter
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/CooperativeCancellation ${COVER_CODEC_DEPS}/JPEGDEC/src)
 target_compile_definitions(RealCoverTest PRIVATE __LINUX__ ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1 COVER_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures" MANGA_FIXTURE_DIR="${REPO_ROOT}/test/manga_format/fixtures")
 target_link_libraries(RealCoverTest PRIVATE crosspoint_test_common GTest::gtest_main)
 gtest_discover_tests(RealCoverTest)
endif()
