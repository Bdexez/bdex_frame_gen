# The loader rejects two manifests with the same layer name, so the 32-bit
# build gets its own (like the Steam overlay's _32/_64 layers).
set(BDEX_LAYER_NAME "VK_LAYER_BDEX_framegen${BDEX_MANIFEST_SUFFIX}")

bdex_add_shaders(bdex_layer_shaders
  OUT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders.h"
  SHADERS
    shaders/downsample.comp:f32
    shaders/downsample.comp:f16:LUMA_FMT=r16f
    shaders/block_match.comp:f32
    shaders/block_match.comp:f16:LUMA_FMT=r16f
    shaders/flow_smooth.comp
    shaders/flow_refine.comp:f32
    shaders/flow_refine.comp:f16:LUMA_FMT=r16f
    shaders/reduce_cost.comp
    shaders/interpolate.comp:u32
    shaders/interpolate.comp:u64:OUT_64
    shaders/interpolate.comp:sharp:SHARP
    shaders/upscale.comp:u32
    shaders/upscale.comp:u64:OUT_64
    shaders/upscale.comp:sharp:SHARP
    shaders/cas.comp:u32
    shaders/cas.comp:u64:OUT_64
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.glsl"
)

if(MSVC)
  # MSVC hides symbols by default and warns at /W4; the layer entry points are
  # exported with __declspec(dllexport) in layer.cpp.
  set(BDEX_WARNINGS /W4)
else()
  set(BDEX_WARNINGS -Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden)
endif()

# Device-independent parts, shared with the unit tests.
add_library(bdex_core STATIC
  src/config.cpp
  src/log.cpp
  src/format.cpp
  src/platform.cpp
  src/vk_util.cpp
)
target_include_directories(bdex_core PUBLIC src)
target_link_libraries(bdex_core PUBLIC Vulkan::Headers)
if(WIN32)
  # CommandLineToArgvW() in platform.cpp (process names for [game] sections).
  target_link_libraries(bdex_core PUBLIC shell32)
endif()
target_compile_options(bdex_core PRIVATE ${BDEX_WARNINGS})

add_library(bdex_framegen SHARED
  src/layer.cpp
  src/device.cpp
  src/framegen.cpp
  src/swapchain.cpp
)
add_dependencies(bdex_framegen bdex_layer_shaders)
target_include_directories(bdex_framegen PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" src)
target_link_libraries(bdex_framegen PRIVATE bdex_core)
target_compile_options(bdex_framegen PRIVATE ${BDEX_WARNINGS})
if(WIN32)
  # VkLayer_bdex_framegen.dll (no lib prefix). No version-script/-Bsymbolic:
  # those are ELF linker concepts; exports come from __declspec(dllexport).
  set_target_properties(bdex_framegen PROPERTIES
    OUTPUT_NAME "VkLayer_bdex_framegen"
    PREFIX "")
else()
  set_target_properties(bdex_framegen PROPERTIES
    OUTPUT_NAME "bdex_framegen"
    PREFIX "libVkLayer_"
    LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer.map -Wl,-Bsymbolic")
endif()
target_compile_definitions(bdex_framegen PRIVATE
  BDEX_LAYER_NAME="${BDEX_LAYER_NAME}"
  BDEX_LAYER_VERSION_STR="${PROJECT_VERSION}")

# Manifest pointing at the build tree (VK_LAYER_PATH=<build>/layer, dev use) ...
set(BDEX_LAYER_LIBRARY_PATH "$<TARGET_FILE:bdex_framegen>")
configure_file(bdex_framegen.json.in "${CMAKE_CURRENT_BINARY_DIR}/bdex_framegen.json.gen" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${BDEX_LAYER_NAME}.json"
     INPUT "${CMAKE_CURRENT_BINARY_DIR}/bdex_framegen.json.gen")

if(NOT WIN32)
  # ... and one for the install tree. On Windows the loader discovers layers
  # through the registry instead: tools/register-layer.ps1 copies the DLL plus
  # a manifest with a relative library_path into %LOCALAPPDATA% and writes the
  # ImplicitLayers registry value.
  set(BDEX_LAYER_LIBRARY_PATH "${CMAKE_INSTALL_PREFIX}/${BDEX_LIBDIR}/libVkLayer_bdex_framegen.so")
  configure_file(bdex_framegen.json.in "${CMAKE_CURRENT_BINARY_DIR}/install/${BDEX_LAYER_NAME}.json" @ONLY)

  install(TARGETS bdex_framegen LIBRARY DESTINATION ${BDEX_LIBDIR})
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/install/${BDEX_LAYER_NAME}.json"
          DESTINATION share/vulkan/implicit_layer.d)
else()
  install(TARGETS bdex_framegen RUNTIME DESTINATION ${BDEX_LIBDIR})
endif()
