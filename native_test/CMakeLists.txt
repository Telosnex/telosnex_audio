file(GLOB TSNX_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
add_executable(tsnx_tests test_main.cc ${TSNX_TEST_SOURCES}
  ${PROJECT_SOURCE_DIR}/src/export/c_api.cc ${PROJECT_SOURCE_DIR}/src/export/version.cc)
target_link_libraries(tsnx_tests PRIVATE tsnx_core)
# On a device (Android), push the fixtures and point this at them.
set(TSNX_FIXTURES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures" CACHE PATH
  "Where the test executables read native_test/fixtures")
target_compile_definitions(tsnx_tests PRIVATE
  TSNX_FIXTURES="${TSNX_FIXTURES_DIR}")
add_test(NAME tsnx_tests COMMAND tsnx_tests)
