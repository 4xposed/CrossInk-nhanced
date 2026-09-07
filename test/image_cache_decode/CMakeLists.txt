# Offline only: point at already installed PlatformIO codecs, never fetch them.
set(IMAGE_CODEC_DEPS "${REPO_ROOT}/.pio/libdeps/default" CACHE PATH "Local JPEGDEC and PNGdec library directory")
if(NOT EXISTS "${IMAGE_CODEC_DEPS}/JPEGDEC/src/JPEGDEC.cpp" OR
   NOT EXISTS "${IMAGE_CODEC_DEPS}/PNGdec/src/PNGdec.cpp")
  message(STATUS "Skipping real ImageCacheDecodeTest: local JPEGDEC/PNGdec sources unavailable")
  return()
endif()
set(CONVERTERS "${REPO_ROOT}/lib/Epub/Epub/converters")
set(PNG_SRC "${IMAGE_CODEC_DEPS}/PNGdec/src")
add_executable(ImageCacheDecodeTest ImageCacheDecodeTest.cpp stubs/HalStorage.cpp
  ${CONVERTERS}/ImageToFramebufferDecoder.cpp
  ${CONVERTERS}/JpegToFramebufferConverter.cpp
  ${CONVERTERS}/PngToFramebufferConverter.cpp
  ${IMAGE_CODEC_DEPS}/JPEGDEC/src/JPEGDEC.cpp
  ${PNG_SRC}/PNGdec.cpp ${PNG_SRC}/adler32.c ${PNG_SRC}/crc32.c
  ${PNG_SRC}/inffast.c ${PNG_SRC}/inflate.c ${PNG_SRC}/inftrees.c ${PNG_SRC}/zutil.c)
target_include_directories(ImageCacheDecodeTest PRIVATE stubs ${CONVERTERS}
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/CooperativeCancellation
  ${IMAGE_CODEC_DEPS}/JPEGDEC/src ${PNG_SRC})
target_compile_definitions(ImageCacheDecodeTest PRIVATE __LINUX__ PNG_MAX_BUFFERED_PIXELS=16416
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(ImageCacheDecodeTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ImageCacheDecodeTest)
