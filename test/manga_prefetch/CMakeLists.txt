add_executable(MangaPrefetchTest MangaPrefetchTest.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaImageGeometry.cpp)
target_include_directories(MangaPrefetchTest PRIVATE ${REPO_ROOT}/lib/MangaPanel)
target_link_libraries(MangaPrefetchTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MangaPrefetchTest)
if(EXISTS "${IMAGE_CODEC_DEPS}/JPEGDEC/src/JPEGDEC.cpp")
set(CONVERTERS "${REPO_ROOT}/lib/Epub/Epub/converters")
set(PNG_SRC "${IMAGE_CODEC_DEPS}/PNGdec/src")
add_executable(MangaPrefetchWorkerTest MangaPrefetchWorkerTest.cpp
  ../image_cache_decode/stubs/HalStorage.cpp
  ${REPO_ROOT}/src/activities/reader/MangaPrefetch.cpp
  ${REPO_ROOT}/src/activities/reader/MangaPrefetchJpegBudget.cpp
  ${REPO_ROOT}/src/activities/reader/MangaPrefetchPngBudget.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaImageGeometry.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaBitmapPixels.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaPixelCache.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${CONVERTERS}/ImageToFramebufferDecoder.cpp
  ${CONVERTERS}/JpegToFramebufferConverter.cpp ${CONVERTERS}/PngToFramebufferConverter.cpp
  ${IMAGE_CODEC_DEPS}/JPEGDEC/src/JPEGDEC.cpp
  ${PNG_SRC}/PNGdec.cpp ${PNG_SRC}/adler32.c ${PNG_SRC}/crc32.c
  ${PNG_SRC}/inffast.c ${PNG_SRC}/inflate.c ${PNG_SRC}/inftrees.c ${PNG_SRC}/zutil.c)
target_include_directories(MangaPrefetchWorkerTest PRIVATE stubs ../image_cache_decode/stubs
  ${REPO_ROOT}/src/activities/reader ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/MangaPanel ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/CooperativeCancellation
  ${IMAGE_CODEC_DEPS}/JPEGDEC/src ${PNG_SRC})
target_compile_definitions(MangaPrefetchWorkerTest PRIVATE SIMULATOR __LINUX__ PNG_MAX_BUFFERED_PIXELS=16416
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../image_cache_decode/fixtures")
target_link_libraries(MangaPrefetchWorkerTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MangaPrefetchWorkerTest)
endif()
