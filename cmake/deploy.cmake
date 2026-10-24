# `cmake --install build --prefix dist` produces a self-contained folder:
#   dist/bin/                 streamline.exe, streamline-signal.exe and every DLL they need
#   dist/lib/gstreamer-1.0/   only the GStreamer plugins we use
# Keep STREAMLINE_PLUGINS in sync with kRequiredElements in src/common/runtime.cpp.

set(STREAMLINE_PLUGINS
  coreelements app audioconvert audioresample videoconvertscale
  d3d11 amfcodec videoparsersbad dav1d vpx opus wasapi2
  rtp rsrtp rtpmanager srtp dtls nice webrtc level)

set(_plugin_files "")
foreach(p IN LISTS STREAMLINE_PLUGINS)
  set(f "${GST_PLUGINS_DIR}/libgst${p}.dll")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "GStreamer plugin not found: ${f}")
  endif()
  list(APPEND _plugin_files "${f}")
endforeach()

get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)

install(TARGETS streamline streamline-signal RUNTIME DESTINATION bin)
install(FILES ${_plugin_files} DESTINATION lib/gstreamer-1.0)

install(CODE "
  if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
  endif()
  file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES \"$<TARGET_FILE:streamline>\" \"$<TARGET_FILE:streamline-signal>\"
    MODULES ${_plugin_files}
    DIRECTORIES \"${_mingw_bin}\"
    RESOLVED_DEPENDENCIES_VAR _deps
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\"
    POST_EXCLUDE_REGEXES \"[Ss][Yy][Ss][Tt][Ee][Mm]32\" \"[Ss][Yy][Ss][Ww][Oo][Ww]64\")
  foreach(_d IN LISTS _unresolved)
    message(STATUS \"Unresolved (assumed system DLL): \${_d}\")
  endforeach()
  file(INSTALL \${_deps} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\" FOLLOW_SYMLINK_CHAIN)
")
