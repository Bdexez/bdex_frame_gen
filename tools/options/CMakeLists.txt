# bdex_options: layer/options_ui.conf + the option registry -> options_ui.json,
# read by the GTK panel (installed next to it). Run at every build, so a UI
# line naming an option the engine does not know fails the build.
add_executable(bdex_options bdex_options.cpp)
target_link_libraries(bdex_options PRIVATE bdex_core)
set(BDEX_OPTIONS_UI_JSON "${CMAKE_BINARY_DIR}/options_ui.json")
add_custom_command(
  OUTPUT "${BDEX_OPTIONS_UI_JSON}"
  COMMAND bdex_options "${CMAKE_SOURCE_DIR}/layer/options_ui.conf" "${BDEX_OPTIONS_UI_JSON}"
  DEPENDS bdex_options "${CMAKE_SOURCE_DIR}/layer/options_ui.conf"
  COMMENT "Generating options_ui.json")
add_custom_target(bdex_options_ui ALL DEPENDS "${BDEX_OPTIONS_UI_JSON}")
install(FILES "${BDEX_OPTIONS_UI_JSON}" DESTINATION share/bdex-framegen)
