cmake_minimum_required(VERSION 3.20)

project(einheit_s5 LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build the CLI framework without ZMQ.
set(EINHEIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EINHEIT_BUILD_ADAPTERS OFF CACHE BOOL "" FORCE)
set(EINHEIT_NO_ZMQ ON CACHE BOOL "Skip ZMQ transport" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../cli einheit_cli)

# Switch library — KSZ9477 via DSA + TPS23861 PoE via I2C. Split
# from the binary so the unit tests link the same objects.
add_library(s5_lib STATIC
  src/switch_adapter.cc
  src/backend.cc
  src/service.cc
  src/dsa.cc
  src/poe.cc
  src/util.cc
  src/sys.cc
)

target_include_directories(s5_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(s5_lib PUBLIC
  einheit_cli
)

add_executable(einheit_s5
  src/main.cc
)

target_link_libraries(einheit_s5 PRIVATE
  s5_lib
)

if(CMAKE_CROSSCOMPILING)
  target_link_options(einheit_s5 PRIVATE -static)
endif()

# Unit tests — native builds only (the cross toolchain has no
# GoogleTest and the tests fake the box through the util seams).
option(EINHEIT_S5_BUILD_TESTS "Build the s5 unit tests" ON)
if(EINHEIT_S5_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  if(NOT TARGET gtest_main)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  enable_testing()
  add_executable(test_s5_backend test/test_backend.cc)
  target_link_libraries(test_s5_backend PRIVATE
    s5_lib
    gtest_main
  )
  add_test(NAME test_s5_backend COMMAND test_s5_backend)
  # Operator-facing behaviour on a real pty (completion menu,
  # Ctrl-C regime, colour detection).
  add_test(NAME test_s5_pty
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test/pty_suite.py
            $<TARGET_FILE:einheit_s5>)
endif()

# ------------------------------------------------------------------
# einheit-launch — tiny setuid-root CLI launcher (plain C).
# Installed on-device as root:root mode 4755. Authenticates the
# caller, sets EINHEIT_USER/ROLE, execs the CLI as root (admin) or
# unprivileged (operator).
# ------------------------------------------------------------------
add_executable(einheit_launch src/launch.c)
set_target_properties(einheit_launch PROPERTIES
  OUTPUT_NAME einheit-launch
  C_STANDARD 11)
if(CMAKE_CROSSCOMPILING)
  target_link_options(einheit_launch PRIVATE -static)
endif()
