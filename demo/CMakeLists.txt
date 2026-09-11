bdex_add_shaders(bdex_demo_shaders
  OUT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/demo_shaders.h"
  SHADERS shaders/fullscreen.vert shaders/scene.frag)

add_executable(bdex_demo main.cpp)
add_dependencies(bdex_demo bdex_demo_shaders)
target_include_directories(bdex_demo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(bdex_demo PRIVATE Vulkan::Vulkan PkgConfig::GLFW3)
target_compile_options(bdex_demo PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
install(TARGETS bdex_demo RUNTIME DESTINATION bin)
