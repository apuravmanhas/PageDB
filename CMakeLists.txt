cmake_minimum_required(VERSION 3.22)
project(PageDB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PAGEDB_BUILD_TESTS "Build PageDB test suite" ON)
option(PAGEDB_ENABLE_WERROR "Treat compiler warnings as errors" ON)
option(PAGEDB_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(pagedb_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pagedb_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wformat=2
    -Wundef
    -fno-strict-aliasing
    -fstack-protector-strong
  )
  if(PAGEDB_ENABLE_WERROR)
    target_compile_options(pagedb_warnings INTERFACE -Werror)
  endif()
  if(PAGEDB_ENABLE_ASAN_UBSAN)
    target_compile_options(pagedb_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pagedb_warnings INTERFACE -fsanitize=address,undefined)
  endif()
endif()

add_library(pagedb STATIC
  src/core/status.cpp
)
target_include_directories(pagedb PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(pagedb PUBLIC pagedb_warnings)

add_executable(pagedb_cli app/pagedb_cli.cpp)
target_link_libraries(pagedb_cli PRIVATE pagedb pagedb_warnings)

if(PAGEDB_BUILD_TESTS)
  enable_testing()

  add_executable(status_test tests/unit/status_test.cpp)
  target_include_directories(status_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(status_test PRIVATE pagedb pagedb_warnings)
  add_test(NAME status_test COMMAND status_test)
endif()
