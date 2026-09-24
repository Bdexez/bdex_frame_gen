add_executable(bdex_tests test_main.cpp test_config.cpp test_format.cpp test_present_slice.cpp)
target_link_libraries(bdex_tests PRIVATE bdex_core)
if(MSVC)
  target_compile_options(bdex_tests PRIVATE /W4)
else()
  target_compile_options(bdex_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME unit COMMAND bdex_tests)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy, PIL"
                  RESULT_VARIABLE BDEX_EVAL_PYTHON_DEPS)
  if(BDEX_EVAL_PYTHON_DEPS EQUAL 0)
    add_test(NAME eval_quality COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_eval_quality.py")
    if(TARGET bdex_offline)
      # Headless: the engine on a synthetic sequence (needs a Vulkan device, no display).
      add_test(NAME offline COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_offline.py" "${CMAKE_BINARY_DIR}")
      set_tests_properties(offline PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      # Quality regression on the synthetic corpus (+ $BDEX_CORPUS_DIR), headless.
      add_test(NAME corpus COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_corpus.py" "${CMAKE_BINARY_DIR}")
      set_tests_properties(corpus PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()
  endif()
endif()

# End-to-end: runs the demo through the layer and checks the output frame
# rate is doubled. Needs a display; the script skips itself otherwise.
if(TARGET bdex_demo)
  add_test(NAME integration
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/integration.sh" "$<TARGET_FILE:bdex_demo>" "${CMAKE_BINARY_DIR}/layer")
  set_tests_properties(integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  if(NOT WIN32)
    add_executable(bdex_acquire_test test_acquire.cpp)
    target_link_libraries(bdex_acquire_test PRIVATE Vulkan::Vulkan PkgConfig::GLFW3)
    target_compile_options(bdex_acquire_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME acquire_timeout COMMAND bdex_acquire_test)
    set_tests_properties(acquire_timeout PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
      ENVIRONMENT "BDEX_FG=1;BDEX_FG_CONFIG=/dev/null;BDEX_FG_PRESET=performance;BDEX_FG_MULTIPLIER=2;BDEX_FG_LOG=2;VK_IMPLICIT_LAYER_PATH=${CMAKE_BINARY_DIR}/layer")
  endif()
endif()
