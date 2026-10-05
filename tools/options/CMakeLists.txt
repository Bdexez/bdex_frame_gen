# bdex_options: layer/options_ui.conf + the option registry -> options_ui.json,
# read by the GTK panel (installed next to it). Run at every build, so a UI
# line naming an option the engine does not know fails the build.
# A cross build cannot run it: it installs the JSON of a native build instead
# (BDEX_OPTIONS_JSON; the file does not depend on the architecture).
set(BDEX_OPTIONS_JSON "" CACHE FILEPATH "A prebuilt options_ui.json to install (cross builds, where bdex_options cannot run)")
if(BDEX_OPTIONS_JSON)
  install(FILES "${BDEX_OPTIONS_JSON}" DESTINATION share/bdex-framegen)
  return()
endif()
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  message(WARNING "Cross build without BDEX_OPTIONS_JSON: the control panel's options_ui.json is not generated")
  return()
endif()
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
