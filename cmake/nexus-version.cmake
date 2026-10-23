# Use with -DCMAKE_PROJECT_renodx_INCLUDE=<this file> in RenoDX's existing build.
# Defer until its dlss5 target exists; no upstream CMake files are modified.
function(nr_nexus_apply_version)
  get_filename_component(nexus_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  file(READ "${nexus_root}/version.h" nexus_header)
  if(NOT nexus_header MATCHES "#define NR_NEXUS_VERSION \"([^\"]+)\"")
    message(FATAL_ERROR "Missing NR_NEXUS_VERSION in version.h")
  endif()
  set(nexus_version "${CMAKE_MATCH_1}")
  if(NOT nexus_version MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)(-[0-9A-Za-z.-]+)?([+][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "Invalid NR Nexus SemVer: ${nexus_version}")
  endif()
  set(nexus_major "${CMAKE_MATCH_1}")
  set(nexus_minor "${CMAKE_MATCH_2}")
  set(nexus_patch "${CMAKE_MATCH_3}")
  if(NOT TARGET dlss5)
    message(FATAL_ERROR "The RenoDX dlss5 target is required")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nexus_root}/version.h")
  target_compile_definitions(dlss5 PRIVATE
    "VER_FILEVERSION=${nexus_major},${nexus_minor},${nexus_patch},0"
    "VER_FILEVERSION_STR=\"${nexus_version}\\0\"")
  message(STATUS "NR Nexus: v${nexus_version}; loader: renodx-dlss5.addon64")
endfunction()

cmake_language(DEFER CALL nr_nexus_apply_version)
