find_package(Threads REQUIRED)
add_executable(bdex_tests test_main.cpp test_config.cpp test_format.cpp test_present_slice.cpp test_budget.cpp test_display_clock.cpp test_status.cpp test_online_tune.cpp test_descriptor_planner.cpp test_options_ui.cpp test_crash.cpp test_pacing.cpp)
target_link_libraries(bdex_tests PRIVATE bdex_core)
if(MSVC)
  target_compile_options(bdex_tests PRIVATE /W4)
else()
  target_compile_options(bdex_tests PRIVATE -Wall -Wextra)
endif()
bdex_strict(bdex_tests)
target_compile_definitions(bdex_tests PRIVATE BDEX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unit COMMAND bdex_tests)

# The real block_match shader against a C++ model of its winner selection
# (tie-breaking included). Needs a Vulkan device, not a display. Linux only,
# like the other GPU tests: the Windows runners have no GPU, and nothing else
# links the Vulkan loader there (the Win32 build found no 32-bit vulkan-1.lib).
if(NOT WIN32)
  add_executable(bdex_matcher_test test_matcher.cpp ../layer/src/device.cpp)
  add_dependencies(bdex_matcher_test bdex_layer_shaders)
  target_include_directories(bdex_matcher_test PRIVATE "${CMAKE_BINARY_DIR}/layer/generated" "${CMAKE_SOURCE_DIR}/layer/src")
  target_link_libraries(bdex_matcher_test PRIVATE bdex_core Vulkan::Vulkan)
  add_test(NAME matcher COMMAND bdex_matcher_test)
  set_tests_properties(matcher PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  # Quiet: a missing module is expected on some runners (the Python tests are
  # then skipped), and its traceback would read as an error in CI summaries.
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy, PIL"
                  RESULT_VARIABLE BDEX_EVAL_PYTHON_DEPS OUTPUT_QUIET ERROR_QUIET)
  if(BDEX_EVAL_PYTHON_DEPS EQUAL 0)
    add_test(NAME eval_quality COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_eval_quality.py")
    if(TARGET bdex_offline)
      # Headless: the engine on a synthetic sequence (needs a Vulkan device, no display).
      add_test(NAME offline COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_offline.py" "${CMAKE_BINARY_DIR}")
      set_tests_properties(offline PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)  # sanitizer builds on software Vulkan are ~10x slower
      # Quality regression on the synthetic corpus (+ $BDEX_CORPUS_DIR), headless.
      add_test(NAME corpus COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_corpus.py" "${CMAKE_BINARY_DIR}")
      set_tests_properties(corpus PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
      # Every output encoding (8/10-bit, sRGB, FP16) and the upscaling path.
      add_test(NAME formats COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_formats.py" "${CMAKE_BINARY_DIR}")
      set_tests_properties(formats PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
      # Within the limits every Vulkan GPU guarantees (4 storage images, 16 KB
      # of shared memory): small GPUs like the Raspberry Pi's.
      add_test(NAME min_limits COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_min_limits.py" "${CMAKE_BINARY_DIR}")
      set_tests_properties(min_limits PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
      # Every option gives the same frames twice (races, out-of-bounds reads).
      add_test(NAME determinism COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_determinism.py" "${CMAKE_BINARY_DIR}")
      set_tests_properties(determinism PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()
  endif()
  if(TARGET bdex_offline)
    # The GUI, the launcher and the README name only options the engine
    # knows, and the README documents all of them (no GPU, no numpy).
    add_test(NAME option_keys COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_option_keys.py" "${CMAKE_BINARY_DIR}")
    # The control panel's config model (Game Mode settings), without a window;
    # skipped where PyGObject with GTK 4 is missing.
    add_test(NAME panel_logic COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/panel_logic.py"
             "${CMAKE_SOURCE_DIR}/tools/bdex-framegen-gui")
    set_tests_properties(panel_logic PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

# The Linux install scripts (Flatpak / Snap copies, stale copies after an
# update, no in-place copy over a mapped library), in a scratch home.
if(UNIX AND NOT APPLE)
  add_test(NAME linux_scripts COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/linux_scripts.sh" "${CMAKE_SOURCE_DIR}")
endif()

# End-to-end: runs the demo through the layer and checks the output frame
# rate is doubled. Needs a display; the script skips itself otherwise.
if(TARGET bdex_demo)
  add_test(NAME integration
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/integration.sh" "$<TARGET_FILE:bdex_demo>" "${CMAKE_BINARY_DIR}/layer")
  set_tests_properties(integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  # Steam's framerate limit in Game Mode (a headless gamescope, nothing on
  # screen); skipped without gamescope and its WSI layer.
  # Input-to-screen latency (bdex_demo --latency-test, latency_log): in a
  # headless gamescope, skipped without it.
  add_test(NAME latency_probe COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/latency_probe.sh" "${CMAKE_BINARY_DIR}")
  set_tests_properties(latency_probe PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
  add_test(NAME gamescope_limit
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/gamescope_limit.sh" "$<TARGET_FILE:bdex_demo>" "${CMAKE_BINARY_DIR}/layer")
  set_tests_properties(gamescope_limit PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME validation
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/validation.sh" "$<TARGET_FILE:bdex_demo>" "${CMAKE_BINARY_DIR}/layer")
  set_tests_properties(validation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  if(NOT WIN32)
    # This build's layer for the loader, whatever its version (see
    # tools/layer_env.sh): old loaders ignore VK_IMPLICIT_LAYER_PATH but
    # always search $XDG_DATA_HOME/vulkan/implicit_layer.d.
    set(BDEX_TEST_XDG "${CMAKE_BINARY_DIR}/test-xdg")
    # Same template as the build manifest (layer/CMakeLists.txt), generated
    # at the same time; the file does not exist yet at configure time.
    file(GENERATE OUTPUT "${BDEX_TEST_XDG}/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json"
         INPUT "${CMAKE_BINARY_DIR}/layer/bdex_framegen.json.gen")
    add_executable(bdex_acquire_test test_acquire.cpp)
    target_link_libraries(bdex_acquire_test PRIVATE Vulkan::Vulkan PkgConfig::GLFW3)
    target_compile_options(bdex_acquire_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME acquire_timeout COMMAND bdex_acquire_test)
    set_tests_properties(acquire_timeout PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30
      ENVIRONMENT "BDEX_FG=1;BDEX_FG_CONFIG=/dev/null;BDEX_FG_PRESET=performance;BDEX_FG_MULTIPLIER=2;BDEX_FG_LOG=2;VK_IMPLICIT_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;XDG_DATA_HOME=${BDEX_TEST_XDG}")

    # Two swapchains presented from two threads on two queues while the
    # config file is rewritten (live reload): the settings race the TSan job
    # would report. Skips without two queues in a present-capable family.
    add_executable(bdex_multithread_test test_multithread.cpp)
    target_link_libraries(bdex_multithread_test PRIVATE Vulkan::Vulkan PkgConfig::GLFW3 Threads::Threads)
    target_compile_options(bdex_multithread_test PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    add_test(NAME multithread_config COMMAND bdex_multithread_test)
    set_tests_properties(multithread_config PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60
      ENVIRONMENT "BDEX_FG=1;BDEX_FG_PRESET=performance;VK_IMPLICIT_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;XDG_DATA_HOME=${BDEX_TEST_XDG}")
  endif()
endif()
