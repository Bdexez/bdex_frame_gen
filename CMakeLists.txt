cmake_minimum_required(VERSION 3.20)
project(bdex_framegen VERSION 0.5.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Vulkan REQUIRED)

if(MSVC)
  # getenv / fopen / strerror: the portable standard calls, used safely here;
  # MSVC's "unsafe" deprecation (C4996) would otherwise be the bulk of the log.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
# Warnings as errors for a target (BDEX_WERROR): /WX or -Werror.
function(bdex_strict target)
  if(BDEX_WERROR)
    target_compile_options(${target} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
  endif()
endfunction()
find_program(GLSLC glslc REQUIRED)

option(BDEX_BUILD_DEMO  "Build the GLFW demo application" ON)
option(BDEX_BUILD_TESTS "Build unit tests" ON)
option(BDEX_BUILD_OFFLINE "Build bdex_offline (headless engine on PPM sequences, for quality evaluation)" ON)
option(BDEX_BUILD_CAPTURE "Build the Windows capture PoC (bdex_capture.exe, MSVC only)" ON)
option(BDEX_WERROR      "Treat compiler warnings as errors in the layer, tests and capture app (the CI turns it on)" OFF)
option(BDEX_STATIC_RUNTIME "Link libstdc++/libgcc statically into the Linux layer (release archives: games and the Steam runtime may bring an older libstdc++)" OFF)
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

if(WIN32 AND MSVC AND BDEX_BUILD_CAPTURE AND NOT BDEX_32BIT AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(capture)
endif()

if(BDEX_BUILD_OFFLINE AND NOT BDEX_32BIT AND NOT WIN32)
  add_subdirectory(tools/offline)
endif()
# The GTK panel's option description (Linux; the Windows launcher embeds the
# same layer/options_ui.conf at compile time).
if(NOT BDEX_32BIT AND NOT WIN32)
  add_subdirectory(tools/options)
endif()

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
