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
    shaders/scene_hist.comp
    shaders/splat.comp
    shaders/frame_state.comp
    shaders/downsample_chroma.comp
    shaders/flow_temporal.comp
    shaders/self_eval.comp
    shaders/upscale_temporal.comp
    shaders/flow_lk.comp
    shaders/hud_mask.comp
    shaders/flow_confidence.comp
    shaders/learned_correction.comp
    shaders/learned_flow.comp
    shaders/luma_match.comp:f32
    shaders/luma_match.comp:f16:LUMA_FMT=r16f
    shaders/flow_compose.comp
    shaders/probe_compare.comp
    shaders/interpolate.comp:u32
    shaders/interpolate.comp:u64:OUT_64
    shaders/interpolate.comp:sharp:SHARP
    shaders/upscale.comp:u32
    shaders/upscale.comp:u64:OUT_64
    shaders/upscale.comp:sharp:SHARP
    shaders/cas.comp:u32
    shaders/cas.comp:u64:OUT_64
    shaders/hud_overlay.comp:u32
    shaders/hud_overlay.comp:u64:OUT_64
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.glsl" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/learned_correction_weights.glsl" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/learned_correction_extrap_weights.glsl" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/learned_flow_weights.glsl"
)

if(MSVC)
  # MSVC hides symbols by default and warns at /W4; the layer entry points are
  # exported with __declspec(dllexport) in layer.cpp.
  set(BDEX_WARNINGS /W4)
else()
  set(BDEX_WARNINGS -Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden)
endif()

# layer/options_ui.conf as bytes, for the Windows launcher, which builds its
# option column from it (the GTK panel gets tools/options' JSON). Hex bytes
# rather than a string literal: MSVC limits literals to ~16 KB.
file(READ "${CMAKE_SOURCE_DIR}/layer/options_ui.conf" BDEX_OPTIONS_UI_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BDEX_OPTIONS_UI_BYTES "${BDEX_OPTIONS_UI_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/options_ui_conf.h"
     "#pragma once\n// Generated from layer/options_ui.conf (layer/CMakeLists.txt).\n"
     "static const unsigned char kOptionsUiConf[] = {${BDEX_OPTIONS_UI_BYTES}0x00};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/layer/options_ui.conf")
# ... and its English texts (layer/options_ui.en.conf).
file(READ "${CMAKE_SOURCE_DIR}/layer/options_ui.en.conf" BDEX_OPTIONS_UI_EN_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BDEX_OPTIONS_UI_EN_BYTES "${BDEX_OPTIONS_UI_EN_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/options_ui_en_conf.h"
     "#pragma once\n// Generated from layer/options_ui.en.conf (layer/CMakeLists.txt).\n"
     "static const unsigned char kOptionsUiEnConf[] = {${BDEX_OPTIONS_UI_EN_BYTES}0x00};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/layer/options_ui.en.conf")

# Device-independent parts, shared with the unit tests.
add_library(bdex_core STATIC
  src/config.cpp
  src/log.cpp
  src/format.cpp
  src/present_slice.cpp
  src/platform.cpp
  src/vk_util.cpp
  src/budget.cpp
  src/online_tune.cpp
  src/descriptor_planner.cpp
  src/options_ui.cpp
  src/display_clock.cpp
  src/gamescope.cpp
  src/status.cpp
  src/crash.cpp
  src/thread_priority.cpp
)
target_include_directories(bdex_core PUBLIC src)
# Threads and dladdr: part of libc since glibc 2.34, in libpthread / libdl
# before (the release build links against glibc 2.31).
find_package(Threads REQUIRED)
target_link_libraries(bdex_core PUBLIC Vulkan::Headers Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  # CommandLineToArgvW() in platform.cpp (process names for [game] sections).
  target_link_libraries(bdex_core PUBLIC shell32)
endif()
target_compile_options(bdex_core PRIVATE ${BDEX_WARNINGS})
bdex_strict(bdex_core)

# The engine (FrameGen, its optional passes, the device helpers), built into
# the layer, bdex_offline and bdex_capture from this one list.
set(BDEX_ENGINE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/device.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/framegen.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/framegen_output.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/registry.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/flow_temporal.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/online_tune.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/synthesis_probe.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/temporal_upscale.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/subpixel_lk.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/hud_mask.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/flow_confidence.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/learned_correction.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/learned_flow.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/luma_match.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/extrapolate_accel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/extrapolate_splat.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/passes/fps_hud.cpp
)
set(BDEX_ENGINE_SOURCES ${BDEX_ENGINE_SOURCES} PARENT_SCOPE)

add_library(bdex_framegen SHARED
  src/layer.cpp
  ${BDEX_ENGINE_SOURCES}
  src/swapchain.cpp
  src/swapchain_worker.cpp
)
add_dependencies(bdex_framegen bdex_layer_shaders)
target_include_directories(bdex_framegen PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" src)
target_link_libraries(bdex_framegen PRIVATE bdex_core)
target_compile_options(bdex_framegen PRIVATE ${BDEX_WARNINGS})
bdex_strict(bdex_framegen)
if(WIN32)
  # VkLayer_bdex_framegen.dll (no lib prefix). No version-script/-Bsymbolic:
  # those are ELF linker concepts; exports come from __declspec(dllexport).
  set_target_properties(bdex_framegen PROPERTIES
    OUTPUT_NAME "VkLayer_bdex_framegen"
    PREFIX "")
  target_sources(bdex_framegen PRIVATE bdex_framegen.def)
else()
  set_target_properties(bdex_framegen PROPERTIES
    OUTPUT_NAME "bdex_framegen"
    PREFIX "libVkLayer_"
    LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer.map -Wl,-Bsymbolic")
  if(NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    # Every symbol resolved at link time: a library the layer forgot to link
    # (libdl, libpthread on glibc < 2.34) fails the build, rather than the
    # game — resolved by luck through the loader's own dependencies, or not.
    target_link_options(bdex_framegen PRIVATE -Wl,--no-undefined)
  endif()
  if(BDEX_STATIC_RUNTIME)
    # The version script keeps the static runtime's symbols hidden.
    target_link_options(bdex_framegen PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()
target_compile_definitions(bdex_framegen PRIVATE
  BDEX_LAYER_NAME="${BDEX_LAYER_NAME}"
  BDEX_LAYER_VERSION_STR="${PROJECT_VERSION}")

# Manifest pointing at the build tree (VK_LAYER_PATH=<build>/layer, dev use) ...
# Multi-config generators (Visual Studio) need one manifest per configuration
# since $<TARGET_FILE> differs: it lands next to the DLL, in <build>/layer/<config>/.
set(BDEX_LAYER_LIBRARY_PATH "$<TARGET_FILE:bdex_framegen>")
configure_file(bdex_framegen.json.in "${CMAKE_CURRENT_BINARY_DIR}/bdex_framegen.json.gen" @ONLY)
get_property(BDEX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(BDEX_MULTI_CONFIG)
  set(BDEX_BUILD_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${BDEX_LAYER_NAME}.json")
else()
  set(BDEX_BUILD_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/${BDEX_LAYER_NAME}.json")
endif()
file(GENERATE OUTPUT "${BDEX_BUILD_MANIFEST}"
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
  # Shipped per-game profiles, found from the library (lib/../share/...).
  install(FILES profiles.conf DESTINATION share/bdex-framegen)
else()
  install(TARGETS bdex_framegen RUNTIME DESTINATION ${BDEX_LIBDIR})
endif()
