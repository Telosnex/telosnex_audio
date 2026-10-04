cmake_minimum_required(VERSION 3.22)
project(telosnex_audio LANGUAGES C CXX)

# One shared library: vendored WebRTC audio + Sonic + minimp3 + the core in
# src/. Only tsnx_* symbols are exported (ADR D5).

option(TSNX_BUILD_TESTS "Build native tests in native_test/" OFF)
option(TSNX_BUILD_PROBE "Build the A.2 engine probe" OFF)

if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TSNX_WEBRTC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webrtc)
include(cmake/webrtc_sources.cmake)

# ---- Target platform and architecture -------------------------------------
set(TSNX_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _n)
  if(NOT _n EQUAL 1)
    message(FATAL_ERROR "Build one architecture at a time (got ${CMAKE_OSX_ARCHITECTURES})")
  endif()
  set(TSNX_ARCH "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${TSNX_ARCH}" TSNX_ARCH)
if(TSNX_ARCH MATCHES "^(arm64|aarch64)$")
  set(TSNX_ARCH arm64)
elseif(TSNX_ARCH MATCHES "^(x86_64|amd64)$")
  set(TSNX_ARCH x64)
else()
  message(FATAL_ERROR "Unsupported architecture ${TSNX_ARCH}")
endif()

set(TSNX_DEFINES
  WEBRTC_ENABLE_PROTOBUF=0
  WEBRTC_APM_DEBUG_DUMP=0
  WEBRTC_STRICT_FIELD_TRIALS=0
  WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE
  WEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=1
  RTC_DISABLE_TRACE_EVENTS
  ABSL_ALLOCATOR_NOTHROW=1
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
  list(APPEND TSNX_DEFINES NDEBUG)
endif()

set(TSNX_WEBRTC_SOURCES ${TSNX_WEBRTC_SOURCES_COMMON})
if(TSNX_ARCH STREQUAL "arm64")
  list(APPEND TSNX_DEFINES WEBRTC_ARCH_ARM64 WEBRTC_HAS_NEON)
  list(APPEND TSNX_WEBRTC_SOURCES ${TSNX_WEBRTC_SOURCES_ARM64})
else()
  list(APPEND TSNX_DEFINES WEBRTC_ARCH_X86_64 WEBRTC_ENABLE_AVX2)
  list(APPEND TSNX_WEBRTC_SOURCES ${TSNX_WEBRTC_SOURCES_X64} ${TSNX_WEBRTC_SOURCES_X64_AVX2})
  set_source_files_properties(${TSNX_WEBRTC_SOURCES_X64_AVX2} PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()

set(TSNX_LINK_LIBS)
if(APPLE)
  list(APPEND TSNX_DEFINES WEBRTC_POSIX WEBRTC_MAC)
  list(APPEND TSNX_WEBRTC_SOURCES ${TSNX_WEBRTC_SOURCES_APPLE})
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND TSNX_DEFINES WEBRTC_IOS)
    message(FATAL_ERROR "iOS is workplan step 8")
  else()
    list(APPEND TSNX_WEBRTC_SOURCES ${TSNX_WEBRTC_SOURCES_MACOS})
    foreach(fw CoreAudio AudioToolbox CoreFoundation Foundation AVFoundation CoreGraphics CoreServices)
      list(APPEND TSNX_LINK_LIBS "-framework ${fw}")
    endforeach()
  endif()
  # The vendored Objective-C must not register classes that collide with
  # another WebRTC copy in the process (ADR D5).
  list(APPEND TSNX_DEFINES RTC_OBJC_TYPE_PREFIX=TSNX)
  set_source_files_properties(
    ${TSNX_WEBRTC_DIR}/rtc_base/system/gcd_helpers.m
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
  message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} is workplan step 7-9")
endif()

set(TSNX_INCLUDES
  ${TSNX_WEBRTC_DIR}
  ${TSNX_WEBRTC_DIR}/third_party/abseil-cpp
  ${TSNX_WEBRTC_DIR}/gen
)
set(TSNX_WARN_OFF -w)

# ---- Vendored WebRTC ------------------------------------------------------
add_library(tsnx_webrtc STATIC ${TSNX_WEBRTC_SOURCES})
target_include_directories(tsnx_webrtc SYSTEM PUBLIC ${TSNX_INCLUDES})
target_compile_definitions(tsnx_webrtc PUBLIC ${TSNX_DEFINES})
target_compile_options(tsnx_webrtc PRIVATE ${TSNX_WARN_OFF}
  $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fno-exceptions>
  -fno-strict-aliasing -fno-math-errno -ffp-contract=off)
target_link_libraries(tsnx_webrtc PUBLIC ${TSNX_LINK_LIBS})

# ---- Sonic and minimp3 ----------------------------------------------------
add_library(tsnx_thirdparty STATIC
  src/third_party_impl/sonic_impl.c
  src/third_party_impl/minimp3_impl.c)
target_include_directories(tsnx_thirdparty SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sonic
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minimp3
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(tsnx_thirdparty PRIVATE -w)
if(TSNX_BUILD_TESTS)
  target_compile_definitions(tsnx_thirdparty PUBLIC TSNX_ALLOC_PROBE=1)
endif()

# ---- Core -----------------------------------------------------------------
file(GLOB TSNX_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/clock/*.cc)
if(APPLE)
  file(GLOB TSNX_CORE_APPLE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/apple/*.mm)
  list(APPEND TSNX_CORE_SOURCES ${TSNX_CORE_APPLE})
endif()
add_library(tsnx_core STATIC ${TSNX_CORE_SOURCES})
target_include_directories(tsnx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(tsnx_core PRIVATE -Wall -Wextra -Wno-unused-parameter
  -Wno-missing-field-initializers $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fno-exceptions>)
target_link_libraries(tsnx_core PUBLIC tsnx_webrtc tsnx_thirdparty)
if(TSNX_BUILD_TESTS)
  target_compile_definitions(tsnx_core PUBLIC TSNX_ALLOC_PROBE=1)
endif()

add_library(telosnex_audio SHARED src/export/c_api.cc src/export/version.cc)
target_link_libraries(telosnex_audio PRIVATE tsnx_core)
if(APPLE)
  target_link_options(telosnex_audio PRIVATE
    "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/export/exports.apple"
    "LINKER:-dead_strip")
  set_target_properties(telosnex_audio PROPERTIES
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_NAME_DIR ON)
endif()

if(TSNX_BUILD_PROBE)
  add_executable(engine_probe native_test/engine_probe.cc)
  target_link_libraries(engine_probe PRIVATE tsnx_webrtc)
  add_executable(echo_probe native_test/echo_probe.cc)
  target_link_libraries(echo_probe PRIVATE telosnex_audio)
  target_include_directories(echo_probe PRIVATE src)
  target_compile_definitions(echo_probe PRIVATE
    TSNX_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/native_test/fixtures")
endif()

if(TSNX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(native_test)
endif()
