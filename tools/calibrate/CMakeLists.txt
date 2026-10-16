# bdex-calibrate — the engine's cost on this GPU before any game (synthetic
# frames, headless): run by the control panel, the Windows launcher and
# `bdex-framegen --calibrate`. Linux and Windows (x64).
add_executable(bdex-calibrate
  bdex_calibrate.cpp
  ${BDEX_ENGINE_SOURCES}
  ../../layer/src/platform.cpp
)
add_dependencies(bdex-calibrate bdex_layer_shaders)
target_include_directories(bdex-calibrate PRIVATE
  "${CMAKE_SOURCE_DIR}/layer/src"
  "${CMAKE_BINARY_DIR}/layer/generated")
target_link_libraries(bdex-calibrate PRIVATE bdex_core Vulkan::Vulkan)
if(MSVC)
  target_compile_definitions(bdex-calibrate PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(bdex-calibrate PRIVATE /W4 /EHsc /permissive- /utf-8 /bigobj)
else()
  target_compile_options(bdex-calibrate PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
bdex_strict(bdex-calibrate)
if(NOT WIN32)
  install(TARGETS bdex-calibrate RUNTIME DESTINATION bin)
endif()
