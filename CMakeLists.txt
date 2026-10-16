cmake_minimum_required(VERSION 3.20)
project(bdex_framegen VERSION 0.5.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The layer needs only the Vulkan headers (the loader calls into it); the
# demo, bdex_offline, the capture app and some tests also link the loader.
# Headers older than 1.3.237 are completed by layer/src/vk_compat.h.
find_package(Vulkan)
if(NOT BDEX_VULKAN_INCLUDE_DIR AND NOT TARGET Vulkan::Headers)
  find_path(BDEX_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "${Vulkan_INCLUDE_DIR}")
  if(NOT BDEX_VULKAN_INCLUDE_DIR)
    message(FATAL_ERROR "Vulkan headers not found: install them (libvulkan-dev, vulkan-headers, vulkan-devel, "
                        "vulkan-loader-devel...) or pass -DBDEX_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>")
  endif()
endif()
if(BDEX_VULKAN_INCLUDE_DIR)  # found above, or given: newer headers than the system's
  if(NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
  endif()
  set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${BDEX_VULKAN_INCLUDE_DIR}")
  if(TARGET Vulkan::Vulkan)
    set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${BDEX_VULKAN_INCLUDE_DIR}")
  endif()
endif()
get_target_property(BDEX_VULKAN_INCLUDES Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
foreach(dir IN LISTS BDEX_VULKAN_INCLUDES)
  if(EXISTS "${dir}/vulkan/vulkan_core.h")
    file(STRINGS "${dir}/vulkan/vulkan_core.h" BDEX_VK_HEADER_LINE REGEX "^#define VK_HEADER_VERSION [0-9]+")
    string(REGEX REPLACE "^#define VK_HEADER_VERSION ([0-9]+).*" "\\1" BDEX_VK_HEADER_VERSION "${BDEX_VK_HEADER_LINE}")
    break()
  endif()
endforeach()
if(BDEX_VK_HEADER_VERSION AND BDEX_VK_HEADER_VERSION LESS 204)
  message(FATAL_ERROR "Vulkan headers 1.x.${BDEX_VK_HEADER_VERSION} are too old (1.3.204 or newer needed): "
                      "git clone --depth 1 --branch v1.3.275 https://github.com/KhronosGroup/Vulkan-Headers "
                      "and pass -DBDEX_VULKAN_INCLUDE_DIR=<clone>/include (or use the prebuilt archive)")
endif()
if(NOT TARGET Vulkan::Vulkan)
  message(STATUS "Vulkan loader library not found: building the layer only (no demo, bdex_offline or GPU tests)")
  set(BDEX_BUILD_DEMO OFF CACHE BOOL "" FORCE)
  set(BDEX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BDEX_BUILD_OFFLINE OFF CACHE BOOL "" FORCE)
endif()

if(MSVC)
  # getenv / fopen / strerror: the portable standard calls, used safely here;
  # MSVC's "unsafe" deprecation (C4996) would otherwise be the bulk of the log.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
# Warnings as errors for a target (BDEX_WERROR): /WX or -Werror.
function(bdex_strict target)
  if(BDEX_WERROR)
    target_compile_options(${target} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12
       AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
      # GCC 12's own std::string code trips its -Wrestrict (GCC bug 105651):
      # a false positive on Debian 12, left a warning.
      target_compile_options(${target} PRIVATE -Wno-error=restrict)
    endif()
  endif()
endfunction()
# GLSL to SPIR-V: glslc (shaderc) when there is one — the tested output —,
# else glslangValidator (glslang), which more distributions package; or
# SPIR-V compiled elsewhere (BDEX_SPIRV_DIR: a build's <build>/layer/spv), for
# a release built on an old distribution with neither.
set(BDEX_SPIRV_DIR "" CACHE PATH "Directory of precompiled SPIR-V (<symbol>.spv, as in <build>/layer/spv): no GLSL compiler needed")
if(NOT BDEX_SPIRV_DIR)
  find_program(GLSLC glslc)
  if(NOT GLSLC)
    find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)
    if(NOT GLSLANG_VALIDATOR)
      message(FATAL_ERROR "No GLSL compiler: install glslc (packages glslc / shaderc) or glslangValidator "
                          "(glslang / glslang-tools), or pass -DBDEX_SPIRV_DIR=<precompiled SPIR-V>")
    endif()
    message(STATUS "Shaders: glslangValidator (${GLSLANG_VALIDATOR}); glslc was not found")
  endif()
endif()

option(BDEX_BUILD_DEMO  "Build the GLFW demo application" ON)
option(BDEX_BUILD_TESTS "Build unit tests" ON)
option(BDEX_BUILD_OFFLINE "Build bdex_offline (headless engine on PPM sequences, for quality evaluation)" ON)
option(BDEX_BUILD_CAPTURE "Build the Windows capture PoC (bdex_capture.exe, MSVC only)" ON)
option(BDEX_WERROR      "Treat compiler warnings as errors in the layer, tests and capture app (the CI turns it on)" OFF)
option(BDEX_STATIC_RUNTIME "Link libstdc++/libgcc statically into the Linux layer (release archives: games and the Steam runtime may bring an older libstdc++)" OFF)
option(BDEX_FUZZ        "Build the parser fuzz targets with libFuzzer (Clang; tools/ci/fuzz.sh) instead of their replay driver" OFF)
option(BDEX_32BIT       "Build the 32-bit layer (for 32-bit games); needs a multilib toolchain (ignored on Windows, where the arch comes from the generator platform)" OFF)

# The 32-bit build gets its own library dir / manifest suffix. On Linux it is
# requested with -DBDEX_32BIT=ON (-m32); on Windows it follows the generator
# platform (-A Win32), like the Steam overlay's _32/_64 pair.
if(BDEX_32BIT OR CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(BDEX_LIBDIR lib32)
  set(BDEX_MANIFEST_SUFFIX "_32")
  set(BDEX_LIBRARY_ARCH 32)
else()
  set(BDEX_LIBDIR lib)
  set(BDEX_MANIFEST_SUFFIX "")
  set(BDEX_LIBRARY_ARCH 64)
endif()
# A distribution package puts the layer in its own library dir
# (lib/x86_64-linux-gnu, lib64...): the manifest follows, and the layer finds
# its profiles one or two levels up (platformBundledProfilesPath).
set(BDEX_INSTALL_LIBDIR "" CACHE STRING "Install dir of the layer library, relative to the prefix (default: lib, or lib32 for BDEX_32BIT)")
if(BDEX_INSTALL_LIBDIR)
  set(BDEX_LIBDIR "${BDEX_INSTALL_LIBDIR}")
endif()
# (library_arch in the manifest: a loader of the other width skips the
# library instead of failing to load it with an error — loaders 1.3.239 and
# newer, whatever the manifest's format version; older ones ignore the field.
# The format stays 1.1.2: loaders before 1.3 warn "invalid layer manifest file
# version" for 1.2.x.)
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

# bdex-capture on Linux (capture/linux): X11 / Xwayland capture.
if(BDEX_BUILD_CAPTURE AND NOT BDEX_32BIT AND NOT WIN32 AND NOT APPLE)
  find_package(PkgConfig QUIET)
  find_package(Threads)
  if(PkgConfig_FOUND)
    pkg_check_modules(BDEX_X11 IMPORTED_TARGET x11 xcomposite xdamage xext xfixes)
  endif()
  if(BDEX_X11_FOUND AND TARGET Vulkan::Vulkan)
    add_subdirectory(capture/linux)
  elseif(BDEX_X11_FOUND)
    message(STATUS "Vulkan loader (libvulkan) not found: bdex-capture not built")
  else()
    message(STATUS "X11 capture libraries (x11 xcomposite xdamage xext xfixes) not found: bdex-capture not built")
  endif()
endif()

if(BDEX_BUILD_OFFLINE AND NOT BDEX_32BIT AND NOT WIN32)
  add_subdirectory(tools/offline)
endif()
if(TARGET Vulkan::Vulkan AND NOT BDEX_32BIT)
  add_subdirectory(tools/calibrate)
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
  # bdex-framegen --sandboxes: the layer for Flatpak / Snap launchers.
  install(PROGRAMS packaging/linux/flatpak.sh DESTINATION share/bdex-framegen)
endif()
