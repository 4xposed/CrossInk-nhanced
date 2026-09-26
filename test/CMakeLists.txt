cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CROSSINK_MEMORY_ANALYSIS "Add the focused clang-tidy target" OFF)
if(CROSSINK_MEMORY_ANALYSIS)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(CROSSINK_SANITIZERS "Instrument native tests with AddressSanitizer and UBSan" OFF)
if(CROSSINK_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CROSSINK_SANITIZERS requires Clang")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_compile_options(-fsanitize-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/../scripts/memory_checks/sanitizer-ignorelist.txt)
  add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
# JPEGDEC's x86 SSE marker filter builds its 0xFF mask by comparing an
# uninitialized register with itself; clang may fold that to anything and then
# rejects every JPEG. Use the scalar filter the device runs. (NO_SIMD cannot be
# set on arm64: JPEGDEC.h still enables NEON there without its header.)
set(JPEGDEC_HOST_DEFINITIONS)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(JPEGDEC_HOST_DEFINITIONS NO_SIMD)
endif()

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/CooperativeCancellation
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(preview_block_locator)
add_subdirectory(text_pool)
add_subdirectory(clip_word_store)
add_subdirectory(clipping_preview)
add_subdirectory(dict_layout)
add_subdirectory(reader_progress_save_debouncer)
add_subdirectory(manual_page_turn_queue)
add_subdirectory(page_count_estimator)
add_subdirectory(section_page_index)
add_subdirectory(section_persistence)
add_subdirectory(compact_table_layout)
add_subdirectory(quick_lock)
add_subdirectory(frontlight_schedule)
add_subdirectory(pinch_font_resize)
add_subdirectory(font_catalog_index)
add_subdirectory(reader_status_bar_tap_target)
add_subdirectory(two_finger_swipe)
add_subdirectory(option_popup)
add_subdirectory(sleep_wake_policy)
add_subdirectory(firmware_board_tag)
add_subdirectory(ota_staging_guard)
add_subdirectory(font_cache_manager)
add_subdirectory(reader_drawer_model)
add_subdirectory(stable_page_navigation)
add_subdirectory(touch_reader_preview)
add_subdirectory(dictionary_word_parts)
add_subdirectory(ligature_guard)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(input_release_guard)
add_subdirectory(release_suppression)
add_subdirectory(bitmap_resample)
add_subdirectory(pxc_v2)
add_subdirectory(memory_policy)
add_subdirectory(epub_grayscale)
add_subdirectory(ui_symbol_fallback)
add_subdirectory(absolute_grayscale)
add_subdirectory(buffered_file)
add_subdirectory(ota_boot_switch)
add_subdirectory(http_redirect_policy)
add_subdirectory(hal_file_stream)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(fs_helpers)
add_subdirectory(persistable_store_atomic)
add_subdirectory(opds_parser)
add_subdirectory(opds_download_path)
add_subdirectory(opds_download_action)
add_subdirectory(japanese_dictionary)
add_subdirectory(manga_format)
add_subdirectory(manga_book)

add_subdirectory(manga_navigation)
add_subdirectory(manga_progress)

add_subdirectory(manga_cover)
add_subdirectory(manga_pixel_cache)

add_subdirectory(book_deletion_snapshot)

add_subdirectory(manga_geometry)
add_subdirectory(manga_bitmap_pixels)
add_subdirectory(builtin_font_pool)
add_subdirectory(image_cache_decode)

add_subdirectory(manga_prefetch)

add_subdirectory(reading_language_stats)
add_subdirectory(book_folder_mutation)

add_subdirectory(manga_menu)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME LookupPerformanceGateTest
  COMMAND ${Python3_EXECUTABLE} -B ${REPO_ROOT}/scripts/test_lookup_performance.py)
add_test(NAME MangaSourceBoundaryTest
  COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s ${REPO_ROOT}/test/source_contracts -v)
add_test(NAME MemoryStackGateTest
  COMMAND ${CMAKE_COMMAND} -DMODE=stack -DWORK=${CMAKE_CURRENT_BINARY_DIR}/memory-probes/stack
    -P ${CMAKE_CURRENT_SOURCE_DIR}/memory_checks/checks.cmake)
if(CROSSINK_SANITIZERS)
  add_executable(MemorySanitizerProbe memory_checks/sanitizer_probe.cpp)
  add_test(NAME SanitizerInstrumentationTest
    COMMAND ${CMAKE_COMMAND} -DMODE=sanitizers -DPROBE=$<TARGET_FILE:MemorySanitizerProbe>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/memory_checks/checks.cmake)
endif()
if(CROSSINK_MEMORY_ANALYSIS)
  find_program(CROSSINK_CLANG_TIDY NAMES clang-tidy)
  if(NOT CROSSINK_CLANG_TIDY)
    message(FATAL_ERROR "CROSSINK_MEMORY_ANALYSIS requires clang-tidy 21.1.6")
  endif()
  add_custom_target(memory-lint
    COMMAND bash ${REPO_ROOT}/scripts/memory_checks/check.sh tidy ${CMAKE_CURRENT_BINARY_DIR} ${CROSSINK_CLANG_TIDY}
    USES_TERMINAL VERBATIM)
  add_test(NAME MemoryClangTidyProbe
    COMMAND ${CMAKE_COMMAND} -DMODE=tidy -DTOOL=${CROSSINK_CLANG_TIDY}
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/memory-probes/tidy
      -P ${CMAKE_CURRENT_SOURCE_DIR}/memory_checks/checks.cmake)
endif()

add_subdirectory(anki_deck)
add_subdirectory(anki_review)
add_subdirectory(library)
