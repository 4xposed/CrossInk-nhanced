add_executable(MangaImageGeometryTest
  MangaImageGeometryTest.cpp
  ${REPO_ROOT}/lib/MangaPanel/MangaImageGeometry.cpp
)

target_include_directories(MangaImageGeometryTest PRIVATE
  ${REPO_ROOT}/lib/MangaPanel
)

target_link_libraries(MangaImageGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MangaImageGeometryTest)
