# bdex-capture — frame generation by capture on Linux (X11 / Xwayland): the
# counterpart of the Windows bdex_capture. Built when the X11 libraries
# (Xcomposite, Xdamage, Xext's MIT-SHM, Xfixes) are installed.
add_executable(bdex-capture
  main.cpp
  x11_capture.cpp
  presenter.cpp
  ${BDEX_ENGINE_SOURCES}
  ../../layer/src/platform.cpp
)
add_dependencies(bdex-capture bdex_layer_shaders)
target_include_directories(bdex-capture PRIVATE
  "${CMAKE_SOURCE_DIR}/layer/src"
  "${CMAKE_BINARY_DIR}/layer/generated"
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bdex-capture PRIVATE bdex_core Vulkan::Vulkan PkgConfig::BDEX_X11 Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(bdex-capture PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
bdex_strict(bdex-capture)
# Native Wayland windows through the screen-cast portal and PipeWire
# (--wayland), when libpipewire and GIO are there.
pkg_check_modules(BDEX_PW IMPORTED_TARGET libpipewire-0.3 gio-2.0 gio-unix-2.0)
if(BDEX_PW_FOUND)
  target_sources(bdex-capture PRIVATE pw_capture.cpp)
  target_compile_definitions(bdex-capture PRIVATE BDEX_PIPEWIRE=1)
  target_link_libraries(bdex-capture PRIVATE PkgConfig::BDEX_PW)
else()
  message(STATUS "libpipewire-0.3 / gio-2.0 not found: bdex-capture without Wayland capture (--wayland)")
endif()
install(TARGETS bdex-capture RUNTIME DESTINATION bin)
