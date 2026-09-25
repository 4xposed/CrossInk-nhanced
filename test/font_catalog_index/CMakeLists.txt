add_executable(font_catalog_index FontCatalogIndexTest.cpp ../../lib/EpdFont/SdCardFontRegistry.cpp)
target_compile_features(font_catalog_index PRIVATE cxx_std_17)
target_include_directories(font_catalog_index PRIVATE stubs ../../lib/EpdFont)
target_compile_options(font_catalog_index PRIVATE -UNDEBUG)
# Extra sanitizer coverage where the toolchain ships the runtimes; Homebrew GCC
# on macOS does not. The CI sanitizer job instruments every test regardless.
include(CheckLinkerFlag)
check_linker_flag(CXX -fsanitize=address,undefined CROSSINK_FONT_CATALOG_SANITIZERS)
if(CROSSINK_FONT_CATALOG_SANITIZERS)
  target_compile_options(font_catalog_index PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(font_catalog_index PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME font_catalog_index COMMAND font_catalog_index)
