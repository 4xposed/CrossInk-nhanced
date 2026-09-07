find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME builtin_font_pool
  COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR} -v
)
set_tests_properties(builtin_font_pool PROPERTIES TIMEOUT 180)
