cmake_minimum_required(VERSION 3.20)
project(bdex_framegen VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

option(BDEX_BUILD_DEMO  "Build the GLFW demo application" ON)
option(BDEX_BUILD_TESTS "Build unit tests" ON)

include(cmake/EmbedShader.cmake)

add_subdirectory(layer)

if(BDEX_BUILD_DEMO)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(GLFW3 IMPORTED_TARGET glfw3)
  endif()
  if(GLFW3_FOUND)
    add_subdirectory(demo)
  else()
    message(STATUS "GLFW not found: demo will not be built")
  endif()
endif()

if(BDEX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(PROGRAMS tools/bdex-framegen DESTINATION bin)
