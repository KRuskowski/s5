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

# Switch adapter — KSZ9477 via DSA + TPS23861 PoE via I2C.
add_executable(einheit_s5
  src/main.cc
  src/switch_adapter.cc
  src/dsa.cc
  src/poe.cc
  src/util.cc
  src/sys.cc
)

target_include_directories(einheit_s5 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(einheit_s5 PRIVATE
  einheit_cli
)

if(CMAKE_CROSSCOMPILING)
  target_link_options(einheit_s5 PRIVATE -static)
endif()
