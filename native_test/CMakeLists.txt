file(GLOB TSNX_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
add_executable(tsnx_tests test_main.cc ${TSNX_TEST_SOURCES}
  ${PROJECT_SOURCE_DIR}/src/export/c_api.cc ${PROJECT_SOURCE_DIR}/src/export/version.cc)
target_link_libraries(tsnx_tests PRIVATE tsnx_core)
target_compile_definitions(tsnx_tests PRIVATE
  TSNX_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME tsnx_tests COMMAND tsnx_tests)
