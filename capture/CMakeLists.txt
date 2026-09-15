# bdex_capture — the Windows capture product (mode 2 of docs/windows-capture.md):
# capture a game's window with Windows.Graphics.Capture, interop the D3D11
# texture into Vulkan, run the layer's frame-generation engine (framegen.cpp
# and the embedded shaders, unchanged) and present real + generated frames in
# a borderless always-on-top overlay. Console app: logs go to stderr. MSVC
# only (C++/WinRT ships with the Windows SDK).

add_executable(bdex_capture
  src/main.cpp
  src/launcher.cpp
  src/capture.cpp
  src/vkctx.cpp
  # The layer's engine and device helpers, shared as source: they only
  # depend on bdex_core and the generated shaders.h. platform.cpp joins for
  # platformConfigPath() (the launcher's config-file save).
  ../layer/src/framegen.cpp
  ../layer/src/device.cpp
  ../layer/src/platform.cpp
)
add_dependencies(bdex_capture bdex_layer_shaders)
target_include_directories(bdex_capture PRIVATE
  src
  "${CMAKE_BINARY_DIR}/layer/generated"
  "${CMAKE_SOURCE_DIR}/layer/src")
# windowsapp: C++/WinRT activation (RoGetActivationFactory & co.);
# dwmapi: extended frame bounds / cloaked state of the target window.
target_link_libraries(bdex_capture PRIVATE bdex_core Vulkan::Vulkan d3d11 dxgi dwmapi windowsapp)
target_compile_definitions(bdex_capture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# Only our own sources see the Win32 Vulkan platform header (windows.h in
# tow); the layer sources compile exactly as they do in the DLL.
set_source_files_properties(src/main.cpp src/capture.cpp src/vkctx.cpp
  PROPERTIES COMPILE_DEFINITIONS VK_USE_PLATFORM_WIN32_KHR)
if(MSVC)
  # /Zc:__cplusplus: C++/WinRT checks the real language level; /utf-8: the
  # sources (and the usage text) are UTF-8.
  target_compile_options(bdex_capture PRIVATE /W4 /EHsc /permissive- /Zc:__cplusplus /utf-8 /bigobj)
endif()
install(TARGETS bdex_capture RUNTIME DESTINATION bin)
