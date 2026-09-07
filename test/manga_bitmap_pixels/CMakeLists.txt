add_executable(MangaBitmapPixelsTest MangaBitmapPixelsTest.cpp stubs/HalStorage.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaBitmapPixels.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp)
target_include_directories(MangaBitmapPixelsTest PRIVATE stubs ${REPO_ROOT}/lib/CooperativeCancellation
  ${REPO_ROOT}/lib/MangaPanel ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Memory)
target_link_libraries(MangaBitmapPixelsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MangaBitmapPixelsTest)
