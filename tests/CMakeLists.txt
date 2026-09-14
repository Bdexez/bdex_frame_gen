add_executable(bdex_tests test_main.cpp test_config.cpp test_format.cpp)
target_link_libraries(bdex_tests PRIVATE bdex_core)
if(MSVC)
  target_compile_options(bdex_tests PRIVATE /W4)
else()
  target_compile_options(bdex_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME unit COMMAND bdex_tests)

# End-to-end: runs the demo through the layer and checks the output frame
# rate is doubled. Needs a display; the script skips itself otherwise.
if(TARGET bdex_demo)
  add_test(NAME integration
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/integration.sh" "$<TARGET_FILE:bdex_demo>" "${CMAKE_BINARY_DIR}/layer")
  set_tests_properties(integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
