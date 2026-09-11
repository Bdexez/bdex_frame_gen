# bdex_add_shaders(<target_name> OUT_HEADER <path> SHADERS <file>[:<variant>:<defines...>] ... [DEPENDS <files>])
#
# Compiles every GLSL file with glslc to SPIR-V and embeds the words in a
# generated header as `static const uint32_t <symbol>[]` + `<symbol>_size`.
# The symbol is derived from the file name (dots/dashes -> underscores) plus an
# optional variant suffix; extra defines are passed to glslc.

set(BDEX_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/embed_spv.cmake")

function(bdex_add_shaders target)
  cmake_parse_arguments(ARG "" "OUT_HEADER" "SHADERS;DEPENDS" ${ARGN})
  set(spv_files)
  set(symbols)
  foreach(entry IN LISTS ARG_SHADERS)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 src)
    list(LENGTH parts nparts)
    set(variant "")
    set(defines)
    if(nparts GREATER 1)
      list(GET parts 1 variant)
    endif()
    if(nparts GREATER 2)
      list(SUBLIST parts 2 -1 defines)
    endif()
    get_filename_component(name "${src}" NAME)
    string(MAKE_C_IDENTIFIER "${name}" sym)
    if(variant)
      set(sym "${sym}_${variant}")
    endif()
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/spv/${sym}.spv")
    set(defflags)
    foreach(d IN LISTS defines)
      list(APPEND defflags "-D${d}")
    endforeach()
    add_custom_command(
      OUTPUT "${spv}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/spv"
      COMMAND ${GLSLC} --target-env=vulkan1.1 -O ${defflags} -o "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}" ${ARG_DEPENDS}
      COMMENT "glslc ${src} ${defflags}"
      VERBATIM)
    list(APPEND spv_files "${spv}")
    list(APPEND symbols "${sym}")
  endforeach()

  add_custom_command(
    OUTPUT "${ARG_OUT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
      "-DOUT=${ARG_OUT_HEADER}"
      "-DSPV_FILES=${spv_files}"
      "-DSYMBOLS=${symbols}"
      -P "${BDEX_EMBED_SCRIPT}"
    DEPENDS ${spv_files} "${BDEX_EMBED_SCRIPT}"
    COMMENT "Embedding SPIR-V into ${ARG_OUT_HEADER}"
    VERBATIM)
  add_custom_target(${target} DEPENDS "${ARG_OUT_HEADER}")
endfunction()
