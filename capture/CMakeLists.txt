# bdex_capture — Windows capture proof of concept (mode 2, phase 1 of
# docs/windows-capture.md): capture a game's window with Windows.Graphics.Capture,
# interop the D3D11 texture into Vulkan, run the layer's upscale shader and
# present it in a borderless always-on-top overlay. Console app: logs go to
# stderr. MSVC only (C++/WinRT ships with the Windows SDK); the validation
# target is VS 2022, see docs/windows-port.md §3.

bdex_add_shaders(bdex_capture_shaders
  OUT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/cap_shaders.h"
  SHADERS ../layer/shaders/upscale.comp:u32
  DEPENDS "${CMAKE_SOURCE_DIR}/layer/shaders/common.glsl")

add_executable(bdex_capture
  src/main.cpp
  src/capture.cpp
  src/vkctx.cpp
)
add_dependencies(bdex_capture bdex_capture_shaders)
target_include_directories(bdex_capture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" src)
# windowsapp: C++/WinRT activation (RoGetActivationFactory & co.);
# dwmapi: extended frame bounds / cloaked state of the target window.
target_link_libraries(bdex_capture PRIVATE Vulkan::Vulkan d3d11 dxgi dwmapi windowsapp)
target_compile_definitions(bdex_capture PRIVATE
  VK_USE_PLATFORM_WIN32_KHR
  WIN32_LEAN_AND_MEAN
  NOMINMAX)
if(MSVC)
  # /Zc:__cplusplus: C++/WinRT checks the real language level; /utf-8: the
  # sources (and the usage text) are UTF-8.
  target_compile_options(bdex_capture PRIVATE /W4 /EHsc /permissive- /Zc:__cplusplus /utf-8 /bigobj)
endif()
install(TARGETS bdex_capture RUNTIME DESTINATION bin)
