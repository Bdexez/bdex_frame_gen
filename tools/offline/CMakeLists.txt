# bdex_offline — the frame-generation engine on a PPM sequence, headless (no
# window, surface or layer). Feeds tools/eval_sequence.py; see CONTRIBUTING.md.
add_executable(bdex_offline
  bdex_offline.cpp
  ../../layer/src/framegen.cpp
  ../../layer/src/framegen_tune.cpp
  ../../layer/src/framegen_output.cpp
  ../../layer/src/device.cpp
)
add_dependencies(bdex_offline bdex_layer_shaders)
target_include_directories(bdex_offline PRIVATE
  "${CMAKE_BINARY_DIR}/layer/generated"
  "${CMAKE_SOURCE_DIR}/layer/src")
target_link_libraries(bdex_offline PRIVATE bdex_core Vulkan::Vulkan)
target_compile_options(bdex_offline PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
