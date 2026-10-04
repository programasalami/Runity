cmake_minimum_required(VERSION 3.25)

project(runity_gameserver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RUNITY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(RUNITY_BUILD_TESTS "Build the test suite" ON)

include(cmake/Warnings.cmake)

add_subdirectory(libs/core)
add_subdirectory(libs/protocol)
add_subdirectory(libs/net)
add_subdirectory(libs/persistence)
add_subdirectory(libs/session)
add_subdirectory(libs/content)
add_subdirectory(libs/sim)
add_subdirectory(libs/game)
add_subdirectory(app)

if(RUNITY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
