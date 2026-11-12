# bdex-x11-trace — a development tool (not installed): records the X Present
# completion events of the windows on screen, read-only, and replays a trace
# through the layer's refresh-grid estimator (PresentTimeline).
add_executable(bdex-x11-trace bdex_x11_trace.cpp)
target_include_directories(bdex-x11-trace PRIVATE "${CMAKE_SOURCE_DIR}/layer/src")
target_link_libraries(bdex-x11-trace PRIVATE bdex_core)
target_compile_options(bdex-x11-trace PRIVATE -Wall -Wextra)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(BDEX_XCB_PRESENT IMPORTED_TARGET xcb xcb-present)
endif()
if(BDEX_XCB_PRESENT_FOUND)
  target_link_libraries(bdex-x11-trace PRIVATE PkgConfig::BDEX_XCB_PRESENT)
  target_compile_definitions(bdex-x11-trace PRIVATE BDEX_HAVE_XCB_PRESENT)
else()
  message(STATUS "xcb-present not found: bdex-x11-trace built without `watch` (replay only)")
endif()
