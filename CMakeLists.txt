cmake_minimum_required(VERSION 3.20)
project(bdex_framegen VERSION 0.3.0 LANGUAGES C CXX)

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
option(BDEX_32BIT       "Build the 32-bit layer (for 32-bit games); needs a multilib toolchain (ignored on Windows, where the arch comes from the generator platform)" OFF)

# The 32-bit build gets its own library dir / manifest suffix. On Linux it is
# requested with -DBDEX_32BIT=ON (-m32); on Windows it follows the generator
# platform (-A Win32), like the Steam overlay's _32/_64 pair.
if(BDEX_32BIT OR CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(BDEX_LIBDIR lib32)
  set(BDEX_MANIFEST_SUFFIX "_32")
else()
  set(BDEX_LIBDIR lib)
  set(BDEX_MANIFEST_SUFFIX "")
endif()
if(BDEX_32BIT)
  set(BDEX_BUILD_DEMO OFF CACHE BOOL "" FORCE)
  set(BDEX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  if(NOT WIN32)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m32")
  endif()
endif()

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

if(NOT WIN32)
  install(PROGRAMS tools/bdex-framegen tools/bdex-framegen-gui DESTINATION bin)
  install(FILES tools/bdex-framegen.desktop DESTINATION share/applications)
endif()
